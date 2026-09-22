/**
 * @file
 *
 * @brief Miles Sound System API implementation backed by miniaudio.
 *
 *        Drop-in replacement for mss32.dll implementing the subset of the
 *        MSS API used by the W3D engine (see mss/mss.h). Audio output and
 *        mixing are provided by miniaudio (WASAPI/DirectSound/WinMM). File
 *        decoding supports WAV (PCM / IEEE float / IMA ADPCM / extensible),
 *        FLAC and MP3 natively, and OGG Vorbis through the bundled
 *        stb_vorbis decoder. 2D sounds, 3D spatialized sounds and streams
 *        (music / speech) are all supported.
 *
 * @copyright This is free software: you can redistribute it and/or
 *            modify it under the terms of the GNU General Public License
 *            as published by the Free Software Foundation, either version
 *            3 of the License, or (at your option) any later version.
 *            A full copy of the GNU General Public License can be found in
 *            LICENSE
 */
#include "mss/mss.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include <windows.h>
#ifdef _DEBUG
#include <crtdbg.h>
#include <io.h>
#include <fcntl.h>
#endif

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA
#include "stb_vorbis.c"

// ----------------------------------------------------------------------------
// Optional diagnostic logging (set MILES_DEBUG=1 in the environment).
// ----------------------------------------------------------------------------

static FILE* g_dbg = NULL;
static int g_dbg_on = -1;

static void dbg(const char* fmt, ...)
{
    if (g_dbg_on < 0) {
        g_dbg_on = 1; /* always log during diagnosis */
    }
    if (!g_dbg_on) return;
    if (!g_dbg) {
        g_dbg = fopen("C:\\Users\\AFMRAYAN\\AppData\\Local\\Temp\\miles_debug.log", "a");
        if (g_dbg) fprintf(g_dbg, "=== mss32 session ===\n");
    }
    if (!g_dbg) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_dbg, fmt, ap);
    va_end(ap);
    fflush(g_dbg);
}

// ----------------------------------------------------------------------------
// Global state
// ----------------------------------------------------------------------------

static ma_engine g_engine;
static ma_bool32 g_engineActive = MA_FALSE;
static int g_engineRefCount = 0;

static DIG_DRIVER* g_digDriver = NULL;

static CRITICAL_SECTION g_cs;
static int g_csInit = 0;

static char g_lastError[256] = { 0 };

static AIL_file_open_callback g_fileOpen = NULL;
static AIL_file_close_callback g_fileClose = NULL;
static AIL_file_seek_callback g_fileSeek = NULL;
static AIL_file_read_callback g_fileRead = NULL;

static int g_3DProviderOpen = 0;
static int g_providerToken = 0;

// Enumerate a list of provider names the game may expect. The game stores a
// preferred 3D provider name in its options/registry and only selects one whose
// name matches what we report here, so we list the common Miles Sound System
// defaults to maximise the chance of a match.
static const char* g_providerNames[] = {
    "Miles Fast 2D Positional Audio",
    "Dolby Surround",
    "DirectSound3D Hardware",
    "A3D",
    "EAX",
    "DirectSound",
    "Miniaudio DirectSound 3D"
};
static const int g_providerNameCount = (int)(sizeof(g_providerNames) / sizeof(g_providerNames[0]));

// ----------------------------------------------------------------------------
// Thread safety
//
// The miniaudio engine runs its own mixing thread, and Sound_End_Callback fires
// on that thread. The game also creates / starts / stops / releases sounds from
// its own thread(s). Every access to the global object list and to per-object
// miniaudio handles must therefore be serialized through g_cs. Without this, a
// sound released on the game thread can be torn down underneath the mixing
// thread (or vice-versa) which corrupts the list / handles and crashes -- most
// visibly when a menu or level transition churns through many sounds at once.
// ----------------------------------------------------------------------------

static void Lock(void)
{
    if (!g_csInit) {
        InitializeCriticalSection(&g_cs);
        g_csInit = 1;
    }
    EnterCriticalSection(&g_cs);
}

static void Unlock(void)
{
    if (g_csInit) {
        LeaveCriticalSection(&g_cs);
    }
}

static void Set_Error(const char* msg)
{
    if (msg != NULL) {
        strncpy(g_lastError, msg, sizeof(g_lastError) - 1);
        g_lastError[sizeof(g_lastError) - 1] = '\0';
    } else {
        g_lastError[0] = '\0';
    }
}

// ----------------------------------------------------------------------------
// Decoding
// ----------------------------------------------------------------------------

typedef struct AudioBlob
{
    short* data;           // s16 interleaved PCM (we own this memory)
    size_t bytes;
    unsigned int rate;
    unsigned int channels;
    unsigned long long frames;
} AudioBlob;

static void Blob_Free(AudioBlob* blob)
{
    if (blob != NULL) {
        free(blob->data);
        memset(blob, 0, sizeof(*blob));
    }
}

typedef struct MemStream
{
    const unsigned char* data;
    size_t size;
    size_t pos;
} MemStream;

static ma_result MEM_Read(void* userData, void* dst, size_t bytesToRead, size_t* bytesRead)
{
    MemStream* ms = (MemStream*)userData;
    if (bytesRead != NULL) {
        *bytesRead = 0;
    }
    if (ms->pos >= ms->size) {
        return MA_AT_END;
    }
    size_t avail = ms->size - ms->pos;
    size_t n = (bytesToRead < avail) ? bytesToRead : avail;
    memcpy(dst, ms->data + ms->pos, n);
    ms->pos += n;
    if (bytesRead != NULL) {
        *bytesRead = n;
    }
    return MA_SUCCESS;
}

static ma_result MEM_Seek(void* userData, ma_int64 offset, ma_seek_origin origin)
{
    MemStream* ms = (MemStream*)userData;
    switch (origin) {
    case ma_seek_origin_start:
        if (offset < 0 || (size_t)offset > ms->size) {
            return MA_BAD_SEEK;
        }
        ms->pos = (size_t)offset;
        break;
    case ma_seek_origin_current: {
        ma_int64 next = (ma_int64)ms->pos + offset;
        if (next < 0 || (size_t)next > ms->size) {
            return MA_BAD_SEEK;
        }
        ms->pos = (size_t)next;
        break;
    }
    case ma_seek_origin_end:
        if (offset > 0 || (size_t)(-offset) > ms->size) {
            return MA_BAD_SEEK;
        }
        ms->pos = ms->size - (size_t)(-offset);
        break;
    default:
        return MA_INVALID_ARGS;
    }
    return MA_SUCCESS;
}

static int Decode_Vorbis(const unsigned char* data, int size, AudioBlob* out)
{
    int error = 0;
    stb_vorbis* vorbis = stb_vorbis_open_memory(data, size, &error, NULL);
    if (vorbis == NULL) {
        return 0;
    }

    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    out->channels = info.channels;
    out->rate = info.sample_rate;
    if (out->channels == 0 || out->rate == 0) {
        stb_vorbis_close(vorbis);
        return 0;
    }

    size_t cap = 64 * 1024;
    out->data = (short*)malloc(cap);
    if (out->data == NULL) {
        stb_vorbis_close(vorbis);
        return 0;
    }

    unsigned int chunkFrames = 4096;
    size_t shortsPerChunk = (size_t)chunkFrames * out->channels;
    for (;;) {
        if (out->bytes + shortsPerChunk * sizeof(short) > cap) {
            cap *= 2;
            short* grown = (short*)realloc(out->data, cap);
            if (grown == NULL) {
                free(out->data);
                out->data = NULL;
                stb_vorbis_close(vorbis);
                return 0;
            }
            out->data = grown;
        }
        int frames = stb_vorbis_get_frame_short_interleaved(
            vorbis, (int)out->channels, out->data + out->bytes / sizeof(short), (int)shortsPerChunk);
        if (frames <= 0) {
            break;
        }
        out->bytes += (size_t)frames * out->channels * sizeof(short);
        out->frames += (unsigned long long)frames;
    }

    stb_vorbis_close(vorbis);
    return out->frames > 0;
}

static int Decode_General(const void* image, int size, AudioBlob* out)
{
    ma_decoder decoder;
    ma_decoder_config config = ma_decoder_config_init(ma_format_s16, 0, 0);
    ma_result ri = ma_decoder_init_memory(image, (size_t)size, &config, &decoder);
    if (ri != MA_SUCCESS) {
        return 0;
    }

    out->channels = decoder.outputChannels;
    out->rate = decoder.outputSampleRate;
    if (out->channels == 0 || out->rate == 0) {
        ma_decoder_uninit(&decoder);
        return 0;
    }

    size_t cap = 64 * 1024;
    out->data = (short*)malloc(cap);
    if (out->data == NULL) {
        ma_decoder_uninit(&decoder);
        return 0;
    }

    unsigned int chunkFrames = 4096;
    for (;;) {
        if (out->bytes + (size_t)chunkFrames * out->channels * sizeof(short) > cap) {
            cap *= 2;
            short* grown = (short*)realloc(out->data, cap);
            if (grown == NULL) {
                free(out->data);
                out->data = NULL;
                ma_decoder_uninit(&decoder);
                return 0;
            }
            out->data = grown;
        }
        ma_uint64 framesRead = 0;
        if (ma_decoder_read_pcm_frames(
                &decoder, out->data + out->bytes / sizeof(short), chunkFrames, &framesRead) != MA_SUCCESS) {
            break;
        }
        if (framesRead == 0) {
            break;
        }
        out->bytes += (size_t)framesRead * out->channels * sizeof(short);
        out->frames += framesRead;
    }

    ma_decoder_uninit(&decoder);
    return out->frames > 0;
}

static int Decode_Image(const void* image, int size, AudioBlob* out)
{
    memset(out, 0, sizeof(*out));
    if (image == NULL || size <= 0) {
        return 0;
    }
    const unsigned char* p = (const unsigned char*)image;
    int result = 0;
    if (size >= 4 && p[0] == 'O' && p[1] == 'g' && p[2] == 'g' && p[3] == 'S') {
        result = Decode_Vorbis(p, size, out);
        if (!result) {
            result = Decode_General(image, size, out);
        }
    } else {
        result = Decode_General(image, size, out);
        if (!result && size >= 4 && p[0] == 'O' && p[1] == 'g' && p[2] == 'g' && p[3] == 'S') {
            result = Decode_Vorbis(p, size, out);
        }
    }
    if (!result) {
        Blob_Free(out);
        Set_Error("MSS(miniaudio): unable to decode audio file image");
    }
    return result;
}

// ----------------------------------------------------------------------------
// Shared audio object (sample / 3D sample / stream)
// ----------------------------------------------------------------------------

typedef struct SoundObject
{
    ma_sound sound;
    ma_audio_buffer buffer;
    AudioBlob blob;
    int soundValid;
    int bufferValid;
    int playing;
    float volume;    // 0..1
    float pan;       // -1..1
    int loopCount;   // 0 = infinite, N = play N times
    int loopsLeft;
    int rateRequest; // requested playback rate in Hz (0 = native)
    float minDist;
    float maxDist;
    float pos[3];
    float face[3];
    float up[3];
    float vel[3];
    float effectsLevel;
    void (*eosCallback)(void*);
    void* eosUserData;
    int eosPending;
    struct SoundObject* prev;
    struct SoundObject* next;
} SoundObject;

static SoundObject* g_objects = NULL;

static void Object_Link(SoundObject* obj)
{
    Lock();
    obj->prev = NULL;
    obj->next = g_objects;
    if (g_objects != NULL) {
        g_objects->prev = obj;
    }
    g_objects = obj;
    Unlock();
}

static void Object_Unlink(SoundObject* obj)
{
    Lock();
    if (obj->prev != NULL) {
        obj->prev->next = obj->next;
    } else {
        g_objects = obj->next;
    }
    if (obj->next != NULL) {
        obj->next->prev = obj->prev;
    }
    obj->prev = NULL;
    obj->next = NULL;
    Unlock();
}

typedef struct EOSRequest {
    void (*eosCallback)(void*);
    void* eosUserData;
} EOSRequest;

void __stdcall AIL_update(void)
{
    EOSRequest pending[256];
    int pendingCount = 0;

    Lock();
    SoundObject* obj = g_objects;
    while (obj != NULL && pendingCount < 256) {
        if (obj->eosPending && obj->eosCallback) {
            pending[pendingCount].eosCallback = obj->eosCallback;
            pending[pendingCount].eosUserData = obj->eosUserData;
            pendingCount++;
            obj->eosPending = 0;
        }
        obj = obj->next;
    }
    Unlock();

    for (int i = 0; i < pendingCount; i++) {
        dbg("ail_update eos cb=%p data=%p\n", (void*)(intptr_t)pending[i].eosCallback, pending[i].eosUserData);
        pending[i].eosCallback(pending[i].eosUserData);
    }
}

static void Object_Release_Data(SoundObject* obj)
{
    Lock();
    int hadSound = obj->soundValid;
    int hadBuffer = obj->bufferValid;
    obj->soundValid = 0;
    obj->bufferValid = 0;
    obj->eosPending = 0;
    if (hadSound) {
        ma_sound_uninit(&obj->sound);
    }
    if (hadBuffer) {
        ma_audio_buffer_uninit(&obj->buffer);
    }
    Blob_Free(&obj->blob);
    obj->playing = 0;
    obj->loopsLeft = obj->loopCount;
    Unlock();
}

// MSS uses a left-handed system (+Z forward); miniaudio is right-handed
// (-Z forward). Mirroring Z preserves the spatial relationships.
#define MAP_Z(z) (-(z))

static void Stop_All_Sounds(void);
static void Apply_Volume(SoundObject* obj)
{
    if (obj->soundValid) {
        ma_sound_set_volume(&obj->sound, obj->volume);
        ma_sound_set_pan(&obj->sound, obj->pan);
    }
}

static void Apply_Rate(SoundObject* obj, unsigned int baseRate)
{
    if (obj->soundValid) {
        float pitch = 1.0f;
        if (baseRate > 0 && obj->rateRequest > 0) {
            pitch = (float)obj->rateRequest / (float)baseRate;
            if (pitch < 0.25f) {
                pitch = 0.25f;
            }
            if (pitch > 8.0f) {
                pitch = 8.0f;
            }
        }
        ma_sound_set_pitch(&obj->sound, pitch);
    }
}

static void Apply_Loop(SoundObject* obj)
{
    if (obj->soundValid) {
        obj->loopsLeft = obj->loopCount;
        ma_sound_set_looping(&obj->sound, MA_FALSE);
    }
}

static void Sound_End_Callback(void* userData, ma_sound* sound)
{
    (void)sound;
    SoundObject* obj = (SoundObject*)userData;
    if (obj == NULL) {
        return;
    }
    Lock();
    if (!obj->soundValid) {
        Unlock();
        return;
    }
    dbg("end_callback obj=%p loopCount=%d loopsLeft=%d\n", obj, obj->loopCount, obj->loopsLeft);
    if (obj->loopCount == 0) {
        ma_sound_seek_to_pcm_frame(&obj->sound, 0);
        ma_sound_start(&obj->sound);
        Unlock();
        return;
    }
    if (obj->loopsLeft > 1) {
        obj->loopsLeft--;
        ma_sound_seek_to_pcm_frame(&obj->sound, 0);
        ma_sound_start(&obj->sound);
        Unlock();
        return;
    }
    obj->loopsLeft = 0;
    obj->playing = 0;
    obj->eosPending = 1;
    Unlock();
}

static int Object_Attach(SoundObject* obj, const void* image, int size, int spatialized)
{
    Object_Release_Data(obj);

    if (!Decode_Image(image, size, &obj->blob)) {
        return 0;
    }

    ma_audio_buffer_config config = ma_audio_buffer_config_init(
        ma_format_s16, obj->blob.channels, obj->blob.frames, obj->blob.data, NULL);
    config.sampleRate = obj->blob.rate;
    ma_result bufR = ma_audio_buffer_init(&config, &obj->buffer);
    dbg("obj_attach buf_init=%d frames=%llu rate=%u ch=%u\n", (int)bufR, (unsigned long long)obj->blob.frames, obj->blob.rate, obj->blob.channels);
    if (bufR != MA_SUCCESS) {
        Blob_Free(&obj->blob);
        Set_Error("MSS(miniaudio): audio buffer init failed");
        return 0;
    }
    obj->bufferValid = 1;

    ma_uint32 flags = 0;
    if (!spatialized) {
        flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
    }
    ma_result sndR = ma_sound_init_from_data_source(&g_engine, &obj->buffer, flags, NULL, &obj->sound);
    dbg("obj_attach snd_init=%d\n", (int)sndR);
    if (sndR != MA_SUCCESS) {
        ma_audio_buffer_uninit(&obj->buffer);
        Blob_Free(&obj->blob);
        Set_Error("MSS(miniaudio): sound init failed");
        return 0;
    }

    obj->soundValid = 1;
    ma_sound_set_end_callback(&obj->sound, Sound_End_Callback, obj);
    Apply_Volume(obj);
    Apply_Rate(obj, obj->blob.rate);
    Apply_Loop(obj);

    if (spatialized) {
        // Emulate the Miles/DirectSound3D distance model: full volume within
        // minDistance, then linear falloff to silence at maxDistance.
        ma_sound_set_attenuation_model(&obj->sound, ma_attenuation_model_linear);
        ma_sound_set_position(&obj->sound, obj->pos[0], obj->pos[1], MAP_Z(obj->pos[2]));
        ma_sound_set_min_distance(&obj->sound, obj->minDist > 0.0f ? obj->minDist : 1.0f);
        ma_sound_set_max_distance(&obj->sound, obj->maxDist);
        ma_sound_set_rolloff(&obj->sound, 1.0f);
    }
    return 1;
}

// ----------------------------------------------------------------------------
// File access (MSS callbacks with fopen fallback)
// ----------------------------------------------------------------------------

#define MAX_CACHED_FILES 64

typedef struct CachedFile {
    char filename[256];
    unsigned char* data;
    size_t size;
    int inUse;
} CachedFile;

static CachedFile g_cachedFiles[MAX_CACHED_FILES];
static int g_numCachedFiles = 0;

static int Read_Whole_File(const char* filename, unsigned char** outData, size_t* outSize)
{
    *outData = NULL;
    *outSize = 0;
    if (filename == NULL) {
        return 0;
    }

    for (int i = 0; i < g_numCachedFiles; i++) {
        if (strcmp(g_cachedFiles[i].filename, filename) == 0) {
            *outData = g_cachedFiles[i].data;
            *outSize = g_cachedFiles[i].size;
            g_cachedFiles[i].inUse++;
            return 1;
        }
    }

    void* handle = NULL;
    int usedCallbacks = 0;
    if (g_fileOpen != NULL && g_fileOpen(filename, &handle) != 0 && handle != NULL) {
        usedCallbacks = 1;
    }

    size_t cap = 256 * 1024;
    size_t len = 0;
    unsigned char* buffer = (unsigned char*)malloc(cap);
    if (buffer == NULL) {
        if (usedCallbacks) {
            g_fileClose(handle);
        }
        return 0;
    }

    if (usedCallbacks) {
        for (;;) {
            if (len + 65536 > cap) {
                cap *= 2;
                unsigned char* grown = (unsigned char*)realloc(buffer, cap);
                if (grown == NULL) {
                    break;
                }
                buffer = grown;
            }
            unsigned long n = g_fileRead(handle, buffer + len, 65536);
            if (n == 0) {
                break;
            }
            len += n;
            if (n < 65536) {
                break;
            }
        }
        g_fileClose(handle);
    } else {
        FILE* file = fopen(filename, "rb");
        if (file == NULL) {
            free(buffer);
            return 0;
        }
        for (;;) {
            if (len + 65536 > cap) {
                cap *= 2;
                unsigned char* grown = (unsigned char*)realloc(buffer, cap);
                if (grown == NULL) {
                    break;
                }
                buffer = grown;
            }
            size_t n = fread(buffer + len, 1, 65536, file);
            if (n == 0) {
                break;
            }
            len += n;
            if (n < 65536) {
                break;
            }
        }
        fclose(file);
    }

    if (len == 0) {
        free(buffer);
        return 0;
    }

    if (g_numCachedFiles < MAX_CACHED_FILES) {
        strncpy(g_cachedFiles[g_numCachedFiles].filename, filename, 255);
        g_cachedFiles[g_numCachedFiles].filename[255] = '\0';
        g_cachedFiles[g_numCachedFiles].data = buffer;
        g_cachedFiles[g_numCachedFiles].size = len;
        g_cachedFiles[g_numCachedFiles].inUse = 1;
        g_numCachedFiles++;
    }

    *outData = buffer;
    *outSize = len;
    return 1;
}

static void Release_Cached_File(unsigned char* data)
{
    for (int i = 0; i < g_numCachedFiles; i++) {
        if (g_cachedFiles[i].data == data) {
            if (g_cachedFiles[i].inUse > 0) {
                g_cachedFiles[i].inUse--;
            }
            return;
        }
    }
    free(data);
}

// ----------------------------------------------------------------------------
// Engine lifecycle
// ----------------------------------------------------------------------------

static int Ensure_Engine(void)
{
    if (g_engineActive) {
        g_engineRefCount++;
        return 1;
    }
    ma_engine_config config = ma_engine_config_init();
    config.sampleRate = 48000;
    config.channels = 2;
    ma_result r = ma_engine_init(&config, &g_engine);
    if (r != MA_SUCCESS) {
        Set_Error("MSS(miniaudio): engine init failed");
        return 0;
    }
    g_engineActive = MA_TRUE;
    g_engineRefCount = 1;
    return 1;
}

// Lazily create the digital driver handle. Some engine builds obtain the
// HDIGDRIVER exclusively via AIL_quick_handles (legacy "quick" MSS API) and
// never call AIL_waveOutOpen / AIL_open_digital_driver, so we must create it
// here or those builds see a NULL driver and skip all sample playback.
static int Ensure_DigDriver(void)
{
    if (g_digDriver != NULL) {
        return 1;
    }
    DIG_DRIVER* dig = (DIG_DRIVER*)calloc(1, sizeof(DIG_DRIVER));
    if (dig == NULL) {
        return 0;
    }
    dig->emulated_ds = 0;
    g_digDriver = dig;
    return 1;
}

static unsigned int Block_Align(const AudioBlob* blob)
{
    return (unsigned int)(blob->channels * sizeof(short));
}

static unsigned long long Frames_To_MS(const AudioBlob* blob, unsigned long long frames)
{
    if (blob->rate == 0) {
        return 0;
    }
    return frames * 1000ULL / blob->rate;
}

static unsigned long long MS_To_Frames(const AudioBlob* blob, unsigned long long ms)
{
    if (blob->rate == 0) {
        return 0;
    }
    return ms * blob->rate / 1000ULL;
}

// ----------------------------------------------------------------------------
// 2D samples
// ----------------------------------------------------------------------------

struct _SAMPLE
{
    SoundObject base;
    int allocated;
    void* userData[8];
};

HSAMPLE __stdcall AIL_allocate_sample_handle(HDIGDRIVER dig)
{
    (void)dig;
    dbg("alloc2d\n");
    if (!Ensure_Engine()) {
        dbg("alloc2d FAIL engine\n");
        return NULL;
    }
    struct _SAMPLE* sample = (struct _SAMPLE*)calloc(1, sizeof(struct _SAMPLE));
    if (sample == NULL) {
        return NULL;
    }
    sample->base.volume = 1.0f;
    sample->base.pan = 0.0f;
    sample->base.loopCount = 1;
    sample->base.minDist = 1.0f;
    sample->base.maxDist = 1000000000.0f;
    sample->allocated = 1;
    Object_Link(&sample->base);
    dbg("alloc2d OK %p\n", sample);
    return (HSAMPLE)sample;
}

void __stdcall AIL_release_sample_handle(HSAMPLE sample)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    if (s == NULL || s->allocated == 0) {
        return;
    }
    s->allocated = 0;
    Object_Unlink(&s->base);
    Object_Release_Data(&s->base);
    free(s);
}

void __stdcall AIL_init_sample(HSAMPLE sample)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    if (s == NULL) {
        return;
    }
    Object_Release_Data(&s->base);
    s->base.volume = 1.0f;
    s->base.pan = 0.0f;
    s->base.loopCount = 1;
    s->base.rateRequest = 0;
    s->base.minDist = 1.0f;
    s->base.maxDist = 1000000000.0f;
}

int __stdcall AIL_set_named_sample_file(
    HSAMPLE sample, const char* file_name, const void* file_image, int file_size, int block)
{
    (void)file_name;
    (void)block;
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    if (s == NULL) {
        return -1;
    }
    const void* img = file_image;
    int off = Find_RIFF_Offset(file_image, file_size < 65536 ? (int)file_size : 65536);
    if (off > 0) {
        img = (const char*)file_image + off;
    }
    dbg("setnamed %p img=%p sz=%d off=%d\n", s, img, file_size, off);
    if (!Object_Attach(&s->base, img, file_size - (int)((const char*)img - (const char*)file_image), 0)) {
        dbg("setnamed FAIL %p\n", s);
        return -1;
    }
    dbg("setnamed OK %p rate=%u ch=%u frames=%u\n", s, s->base.blob.rate, s->base.blob.channels, s->base.blob.frames);
    return 0;
}

static int Guess_Image_Size(const void* image)
{
    if (image == NULL) {
        return 0;
    }
    const unsigned char* p = (const unsigned char*)image;
    if (p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F') {
        size_t riffSize = (size_t)p[4] | ((size_t)p[5] << 8) | ((size_t)p[6] << 16) | ((size_t)p[7] << 24);
        return (int)(riffSize + 8);
    }
    return 0;
}

// Real Miles scans the supplied buffer for the 'RIFF' marker instead of
// assuming it sits at offset 0. Some sound buffers (e.g. sounds extracted
// from archives with a small leading header) only become valid WAV data at a
// non-zero offset. Scan a small window for the marker and return its offset,
// or -1 if not found.
static int Find_RIFF_Offset(const void* image, int max_scan)
{
    if (image == NULL || max_scan < 4) {
        return -1;
    }
    const unsigned char* p = (const unsigned char*)image;
    for (int i = 0; i <= max_scan - 4; i++) {
        if (p[i] == 'R' && p[i + 1] == 'I' && p[i + 2] == 'F' && p[i + 3] == 'F') {
            return i;
        }
    }
    return -1;
}

int __stdcall AIL_set_sample_file(HSAMPLE sample, const void* file_image, int block)
{
    (void)block;
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    if (s == NULL) {
        dbg("set_sample_file NULL sample\n");
        return -1;
    }
    const void* img = file_image;
    int off = Find_RIFF_Offset(file_image, 65536);
    if (off > 0) {
        img = (const char*)file_image + off;
    }
    int size = Guess_Image_Size(img);
    if (size <= 0) {
        dbg("set_sample_file size=0 (not RIFF) off=%d\n", off);
        return -1;
    }
    int r = Object_Attach(&s->base, img, size, 0) ? 0 : -1;
    dbg("set_sample_file %p off=%d size=%d -> %d rate=%u ch=%u frames=%u\n", s, off, size, r, s->base.blob.rate, s->base.blob.channels, s->base.blob.frames);
    return r;
}

void __stdcall AIL_start_sample(HSAMPLE sample)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    if (s == NULL) {
        return;
    }
    Lock();
    if (!s->base.soundValid) {
        Unlock();
        return;
    }
    if (s->base.playing) {
        ma_sound_stop(&s->base.sound);
    }
    dbg("start2d %p valid=%d vol=%.3f\n", s, s->base.soundValid, s->base.volume);
    ma_sound_seek_to_pcm_frame(&s->base.sound, 0);
    ma_result r = ma_sound_start(&s->base.sound);
    dbg("start2d %p ma_sound_start=%d\n", s, (int)r);
    s->base.playing = 1;
    s->base.loopsLeft = s->base.loopCount;
    Unlock();
}

void __stdcall AIL_stop_sample(HSAMPLE sample)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    if (s == NULL) {
        return;
    }
    Lock();
    if (s->base.soundValid) {
        ma_sound_stop(&s->base.sound);
        s->base.playing = 0;
    }
    Unlock();
}

void __stdcall AIL_resume_sample(HSAMPLE sample)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    if (s == NULL) {
        return;
    }
    Lock();
    if (s->base.soundValid) {
        ma_sound_start(&s->base.sound);
        s->base.playing = 1;
    }
    Unlock();
}

void __stdcall AIL_end_sample(HSAMPLE sample)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    if (s == NULL) {
        return;
    }
    Lock();
    if (s->base.soundValid) {
        ma_sound_stop(&s->base.sound);
        s->base.playing = 0;
    }
    Unlock();
}

void __stdcall AIL_set_sample_volume_pan(HSAMPLE sample, float volume, float pan)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    if (s != NULL) {
        static int vc = 0;
        if (vc < 60 || volume <= 0.001f) { dbg("vol2d %p =%.3f pan=%.3f\n", s, volume, pan); vc++; }
        s->base.volume = volume;
        s->base.pan = pan;
        Apply_Volume(&s->base);
    }
}

void __stdcall AIL_sample_volume_pan(HSAMPLE sample, float* volume, float* pan)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    if (s != NULL) {
        if (volume != NULL) {
            *volume = s->base.volume;
        }
        if (pan != NULL) {
            *pan = s->base.pan;
        }
    }
}

int __stdcall AIL_sample_volume(HSAMPLE sample)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    return s != NULL ? (int)(s->base.volume * 127.0f) : 0;
}

void __stdcall AIL_set_sample_volume(HSAMPLE sample, int volume)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    if (s != NULL) {
        float pan = s->base.pan;
        AIL_set_sample_volume_pan(sample, volume / 127.0f, pan);
    }
}

int __stdcall AIL_sample_pan(HSAMPLE sample)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    return s != NULL ? (int)(s->base.pan * 127.0f) : 0;
}

void __stdcall AIL_set_sample_pan(HSAMPLE sample, int pan)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    if (s != NULL) {
        float volume = s->base.volume;
        AIL_set_sample_volume_pan(sample, volume, pan / 127.0f);
    }
}

void __stdcall AIL_set_sample_loop_count(HSAMPLE sample, int count)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    if (s != NULL) {
        s->base.loopCount = count;
        if (count >= 1000000) {
            s->base.loopCount = 0;
        }
        Apply_Loop(&s->base);
    }
}

int __stdcall AIL_sample_loop_count(HSAMPLE sample)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    return s != NULL ? s->base.loopCount : 0;
}

void __stdcall AIL_set_sample_ms_position(HSAMPLE sample, int pos)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    if (s != NULL && s->base.soundValid) {
        unsigned long long frame = MS_To_Frames(&s->base.blob, (unsigned long long)pos);
        ma_sound_seek_to_pcm_frame(&s->base.sound, frame);
    }
}

void __stdcall AIL_sample_ms_position(HSAMPLE sample, long* total_ms, long* current_ms)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    if (s == NULL) {
        if (total_ms != NULL) {
            *total_ms = 0;
        }
        if (current_ms != NULL) {
            *current_ms = 0;
        }
        return;
    }

    unsigned long long total = Frames_To_MS(&s->base.blob, s->base.blob.frames);
    unsigned long long current = 0;
    if (s->base.soundValid) {
        ma_uint64 cursor = 0;
        if (ma_sound_get_cursor_in_pcm_frames(&s->base.sound, &cursor) == MA_SUCCESS) {
            current = Frames_To_MS(&s->base.blob, cursor);
        }
    }

    if (total_ms != NULL) {
        *total_ms = (long)total;
    }
    if (current_ms != NULL) {
        *current_ms = (long)current;
    }
}

void __stdcall AIL_set_sample_playback_rate(HSAMPLE sample, int rate)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    if (s != NULL) {
        s->base.rateRequest = rate;
        Apply_Rate(&s->base, s->base.blob.rate);
    }
}

int __stdcall AIL_sample_playback_rate(HSAMPLE sample)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    if (s == NULL) {
        return 0;
    }
    if (s->base.rateRequest > 0) {
        return s->base.rateRequest;
    }
    return (int)s->base.blob.rate;
}

void __stdcall AIL_set_sample_user_data(HSAMPLE sample, unsigned int index, void* value)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    if (s != NULL && index < 8) {
        s->userData[index] = value;
    }
}

void* __stdcall AIL_sample_user_data(HSAMPLE sample, unsigned int index)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    return (s != NULL && index < 8) ? s->userData[index] : NULL;
}

HPROVIDER __stdcall AIL_set_sample_processor(HSAMPLE sample, SAMPLESTAGE pipeline_stage, HPROVIDER provider)
{
    (void)sample;
    (void)pipeline_stage;
    (void)provider;
    return 0;
}

void __stdcall AIL_set_filter_sample_preference(HSAMPLE sample, const char* name, const void* val)
{
    (void)sample;
    (void)name;
    (void)val;
}

// ----------------------------------------------------------------------------
// 3D audio
// ----------------------------------------------------------------------------

typedef struct M3DObject
{
    SoundObject base;
    int isListener;
    int allocated;
    void* userData[8];
} M3DObject;

static void Listener_Apply(M3DObject* listener)
{
    dbg("LISTENER_APPLY engineActive=%d listeners=%u\n", g_engineActive,
        (unsigned)(g_engineActive ? g_engine.listenerCount : 0));
    ma_engine_listener_set_position(&g_engine, 0, listener->base.pos[0], listener->base.pos[1],
        MAP_Z(listener->base.pos[2]));
    dbg("LISTENER_APPLY pos done\n");
    ma_engine_listener_set_direction(&g_engine, 0, listener->base.face[0], listener->base.face[1],
        MAP_Z(listener->base.face[2]));
    dbg("LISTENER_APPLY dir done\n");
    ma_engine_listener_set_world_up(&g_engine, 0, listener->base.up[0], listener->base.up[1],
        MAP_Z(listener->base.up[2]));
    dbg("LISTENER_APPLY up done\n");
}

M3DRESULT __stdcall AIL_open_3D_provider(HPROVIDER lib)
{
    (void)lib;
    dbg("open3Dprovider\n");
    if (!Ensure_Engine()) {
        dbg("open3Dprovider FAIL engine\n");
        return -1;
    }
    g_3DProviderOpen = 1;
    dbg("open3Dprovider OK\n");
    return M3D_NOERR;
}

void __stdcall AIL_close_3D_provider(HPROVIDER lib)
{
    (void)lib;
    g_3DProviderOpen = 0;
}

int __stdcall AIL_enumerate_3D_providers(HPROENUM* next, HPROVIDER* dest, char** name)
{
    dbg("enum3D next=%p\n", next ? (void*)(size_t)*next : (void*)0);
    if (next == NULL || dest == NULL || name == NULL) {
        return 0;
    }
    if (*next >= (HPROENUM)g_providerNameCount) {
        return 0;
    }
    *dest = (HPROVIDER)&g_providerToken;
    *name = (char*)g_providerNames[*next];
    *next = (HPROENUM)(*next + 1);
    return 1;
}

void __stdcall AIL_set_3D_speaker_type(HPROVIDER lib, int speaker_type)
{
    (void)lib;
    (void)speaker_type;
}

H3DPOBJECT __stdcall AIL_open_3D_listener(HPROVIDER lib)
{
    (void)lib;
    dbg("open3Dlistener\n");
    M3DObject* listener = (M3DObject*)calloc(1, sizeof(M3DObject));
    if (listener == NULL) {
        return NULL;
    }
    listener->isListener = 1;
    listener->base.volume = 1.0f;
    listener->base.up[0] = 0.0f;
    listener->base.up[1] = 1.0f;
    listener->base.up[2] = 0.0f;
    listener->base.face[2] = 1.0f;
    dbg("open3Dlistener OK %p\n", listener);
    return (H3DPOBJECT)listener;
}

void __stdcall AIL_close_3D_listener(H3DPOBJECT listener)
{
    M3DObject* l = (M3DObject*)listener;
    free(l);
}

H3DSAMPLE __stdcall AIL_allocate_3D_sample_handle(HPROVIDER lib)
{
    (void)lib;
    dbg("alloc3d\n");
    if (!Ensure_Engine()) {
        dbg("alloc3d FAIL engine\n");
        return NULL;
    }
    M3DObject* sample = (M3DObject*)calloc(1, sizeof(M3DObject));
    if (sample == NULL) {
        return NULL;
    }
    sample->base.volume = 1.0f;
    sample->base.loopCount = 1;
    sample->base.minDist = 1.0f;
    sample->base.maxDist = 1000000000.0f;
    sample->base.face[2] = 1.0f;
    sample->allocated = 1;
    Object_Link(&sample->base);
    dbg("alloc3d OK %p\n", sample);
    return (H3DSAMPLE)sample;
}

void __stdcall AIL_release_3D_sample_handle(H3DSAMPLE sample)
{
    M3DObject* s = (M3DObject*)sample;
    if (s == NULL || s->allocated == 0) {
        return;
    }
    s->allocated = 0;
    Object_Unlink(&s->base);
    Object_Release_Data(&s->base);
    free(s);
}

int __stdcall AIL_set_3D_sample_file(H3DSAMPLE sample, const void* file_image)
{
    M3DObject* s = (M3DObject*)sample;
    if (s == NULL) {
        return -1;
    }
    const void* img = file_image;
    int off = Find_RIFF_Offset(file_image, 65536);
    if (off > 0) {
        img = (const char*)file_image + off;
    }
    int size = Guess_Image_Size(img);
    if (size <= 0) {
        dbg("set3dfile FAIL size %p off=%d\n", s, off);
        return -1;
    }
    dbg("set3dfile %p img=%p sz=%d off=%d\n", s, img, size, off);
    if (!Object_Attach(&s->base, img, size, 1)) {
        dbg("set3dfile ATTACH FAIL %p\n", s);
        return -1;
    }
    dbg("set3dfile OK %p rate=%u ch=%u frames=%u\n", s, s->base.blob.rate, s->base.blob.channels, s->base.blob.frames);
    return 0;
}

static void Stop_All_Sounds(void)
{
    Lock();
    SoundObject* obj = g_objects;
    while (obj != NULL) {
        if (obj->soundValid && obj->playing) {
            dbg("stop_all_sounds obj=%p playing=%d\n", obj, obj->playing);
            ma_sound_stop(&obj->sound);
            obj->playing = 0;
            obj->loopsLeft = 0;
            ma_sound_set_looping(&obj->sound, MA_FALSE);
        }
        obj = obj->next;
    }
    Unlock();
}

void __stdcall AIL_start_3D_sample(H3DSAMPLE sample)
{
    M3DObject* s = (M3DObject*)sample;
    if (s == NULL) {
        return;
    }
    Lock();
    if (!s->base.soundValid) {
        Unlock();
        return;
    }
    if (s->base.playing) {
        ma_sound_stop(&s->base.sound);
    }
    dbg("start3d %p valid=%d vol=%.3f\n", s, s->base.soundValid, s->base.volume);
    ma_sound_seek_to_pcm_frame(&s->base.sound, 0);
    ma_result r = ma_sound_start(&s->base.sound);
    dbg("start3d %p ma_sound_start=%d\n", s, (int)r);
    s->base.playing = 1;
    s->base.loopsLeft = s->base.loopCount;
    Unlock();
}

void __stdcall AIL_stop_3D_sample(H3DSAMPLE sample)
{
    M3DObject* s = (M3DObject*)sample;
    if (s == NULL) {
        return;
    }
    Lock();
    if (s->base.soundValid) {
        ma_sound_stop(&s->base.sound);
        s->base.playing = 0;
    }
    Unlock();
}

void __stdcall AIL_resume_3D_sample(H3DSAMPLE sample)
{
    M3DObject* s = (M3DObject*)sample;
    if (s == NULL) {
        return;
    }
    Lock();
    if (s->base.soundValid) {
        ma_sound_start(&s->base.sound);
        s->base.playing = 1;
    }
    Unlock();
}

void __stdcall AIL_end_3D_sample(H3DSAMPLE sample)
{
    M3DObject* s = (M3DObject*)sample;
    if (s == NULL) {
        return;
    }
    Lock();
    if (s->base.soundValid) {
        ma_sound_stop(&s->base.sound);
        s->base.playing = 0;
        s->base.eosPending = 0;
    }
    Unlock();
}

#ifdef MILES_NOFLOAT
int __stdcall AIL_3D_sample_volume(H3DSAMPLE sample)
{
    M3DObject* s = (M3DObject*)sample;
    return s != NULL ? (int)(s->base.volume * 127.0f) : 0;
}

void __stdcall AIL_set_3D_sample_volume(H3DSAMPLE sample, int volume)
{
    M3DObject* s = (M3DObject*)sample;
    if (s != NULL) {
        s->base.volume = volume / 127.0f;
        Apply_Volume(&s->base);
    }
}
#else
float __stdcall AIL_3D_sample_volume(H3DSAMPLE sample)
{
    M3DObject* s = (M3DObject*)sample;
    return s != NULL ? s->base.volume : 0.0f;
}

void __stdcall AIL_set_3D_sample_volume(H3DSAMPLE sample, float volume)
{
    M3DObject* s = (M3DObject*)sample;
    if (s != NULL) {
        static int vc = 0;
        if (vc < 60 || volume <= 0.001f) { dbg("vol3d %p =%.3f\n", s, volume); vc++; }
        s->base.volume = volume;
        Apply_Volume(&s->base);
    }
}
#endif

void __stdcall AIL_set_3D_sample_loop_count(H3DSAMPLE sample, unsigned int count)
{
    M3DObject* s = (M3DObject*)sample;
    if (s != NULL) {
        s->base.loopCount = (int)count;
        Apply_Loop(&s->base);
    }
}

unsigned int __stdcall AIL_3D_sample_loop_count(H3DSAMPLE sample)
{
    M3DObject* s = (M3DObject*)sample;
    return s != NULL ? (unsigned int)s->base.loopCount : 0;
}

void __stdcall AIL_set_3D_sample_offset(H3DSAMPLE sample, unsigned int offset)
{
    M3DObject* s = (M3DObject*)sample;
    if (s != NULL && s->base.soundValid) {
        unsigned int align = Block_Align(&s->base.blob);
        if (align > 0) {
            ma_sound_seek_to_pcm_frame(&s->base.sound, offset / align);
        }
    }
}

unsigned int __stdcall AIL_3D_sample_offset(H3DSAMPLE sample)
{
    M3DObject* s = (M3DObject*)sample;
    if (s == NULL || !s->base.soundValid) {
        return 0;
    }
    ma_uint64 cursor = 0;
    if (ma_sound_get_cursor_in_pcm_frames(&s->base.sound, &cursor) == MA_SUCCESS) {
        return (unsigned int)(cursor * Block_Align(&s->base.blob));
    }
    return 0;
}

int __stdcall AIL_3D_sample_length(H3DSAMPLE sample)
{
    M3DObject* s = (M3DObject*)sample;
    if (s == NULL) {
        return 0;
    }
    return (int)(s->base.blob.frames * Block_Align(&s->base.blob));
}

void __stdcall AIL_set_3D_sample_playback_rate(H3DSAMPLE sample, int playback_rate)
{
    M3DObject* s = (M3DObject*)sample;
    if (s != NULL) {
        s->base.rateRequest = playback_rate;
        Apply_Rate(&s->base, s->base.blob.rate);
    }
}

int __stdcall AIL_3D_sample_playback_rate(H3DSAMPLE sample)
{
    M3DObject* s = (M3DObject*)sample;
    if (s == NULL) {
        return 0;
    }
    return s->base.rateRequest > 0 ? s->base.rateRequest : (int)s->base.blob.rate;
}

void __stdcall AIL_set_3D_position(H3DPOBJECT obj, float X, float Y, float Z)
{
    M3DObject* s = (M3DObject*)obj;
    if (s == NULL) {
        return;
    }
    static int pc = 0;
    if (pc < 80) { dbg("%s pos=%.1f,%.1f,%.1f\n", s->isListener ? "LISTENER" : "sound", X, Y, Z); pc++; }
    s->base.pos[0] = X;
    s->base.pos[1] = Y;
    s->base.pos[2] = Z;
    if (s->isListener) {
        Listener_Apply(s);
        dbg("LISTENER apply returned\n");
    } else if (s->base.soundValid) {
        ma_sound_set_position(&s->base.sound, X, Y, MAP_Z(Z));
    }
    Unlock();
}

void __stdcall AIL_set_3D_orientation(
    H3DPOBJECT obj, float X_face, float Y_face, float Z_face, float X_up, float Y_up, float Z_up)
{
    M3DObject* s = (M3DObject*)obj;
    if (s == NULL) {
        return;
    }
    s->base.face[0] = X_face;
    s->base.face[1] = Y_face;
    s->base.face[2] = Z_face;
    s->base.up[0] = X_up;
    s->base.up[1] = Y_up;
    s->base.up[2] = Z_up;
    if (s->isListener) {
        Listener_Apply(s);
    } else if (s->base.soundValid) {
        ma_sound_set_direction(&s->base.sound, X_face, Y_face, MAP_Z(Z_face));
    }
}

void __stdcall AIL_set_3D_velocity_vector(H3DSAMPLE sample, float x, float y, float z)
{
    M3DObject* s = (M3DObject*)sample;
    if (s != NULL) {
        s->base.vel[0] = x;
        s->base.vel[1] = y;
        s->base.vel[2] = z;
    }
}

void __stdcall AIL_set_3D_sample_distances(H3DSAMPLE sample, float max_dist, float min_dist)
{
    M3DObject* s = (M3DObject*)sample;
    dbg("dist3d %p max=%f min=%f\n", s, max_dist, min_dist);
    if (s != NULL) {
        s->base.maxDist = max_dist;
        s->base.minDist = min_dist;
        if (s->base.soundValid) {
            ma_sound_set_attenuation_model(&s->base.sound, ma_attenuation_model_linear);
            ma_sound_set_min_distance(&s->base.sound, min_dist > 0.0f ? min_dist : 1.0f);
            ma_sound_set_max_distance(&s->base.sound, max_dist);
        }
    }
}

void __stdcall AIL_set_3D_sample_effects_level(H3DSAMPLE sample, float effect_level)
{
    M3DObject* s = (M3DObject*)sample;
    if (s != NULL) {
        s->base.effectsLevel = effect_level;
    }
}

void __stdcall AIL_set_3D_sample_occlusion(H3DSAMPLE sample, float occlusion)
{
    (void)sample;
    (void)occlusion;
}

void __stdcall AIL_set_3D_user_data(H3DPOBJECT obj, unsigned int index, void* value)
{
    M3DObject* s = (M3DObject*)obj;
    if (s != NULL && index < 8) {
        s->userData[index] = value;
    }
}

void* __stdcall AIL_3D_user_data(H3DSAMPLE sample, unsigned int index)
{
    M3DObject* s = (M3DObject*)sample;
    return (s != NULL && index < 8) ? s->userData[index] : NULL;
}

// ----------------------------------------------------------------------------
// Streams (music / speech)
// ----------------------------------------------------------------------------

struct _STREAM
{
    SoundObject base;
    int valid;
    int paused;
    char* filename;
    void* userData[8];
};

HSTREAM __stdcall AIL_open_stream(HDIGDRIVER dig, const char* filename, int stream_mem)
{
    (void)dig;
    (void)stream_mem;
    dbg("open_stream '%s'\n", filename ? filename : "(null)");
    if (filename == NULL) {
        return NULL;
    }
    if (!Ensure_Engine()) {
        dbg("open_stream FAIL engine\n");
        return NULL;
    }

    unsigned char* data = NULL;
    size_t size = 0;
    if (!Read_Whole_File(filename, &data, &size)) {
        Set_Error("MSS(miniaudio): stream file could not be read");
        return NULL;
    }

    struct _STREAM* stream = (struct _STREAM*)calloc(1, sizeof(struct _STREAM));
    if (stream == NULL) {
        free(data);
        return NULL;
    }
    stream->base.volume = 1.0f;
    stream->base.pan = 0.0f;
    stream->base.loopCount = 0;
    stream->filename = strdup(filename);
    Object_Link(&stream->base);

    if (!Object_Attach(&stream->base, data, (int)size, 0)) {
        dbg("open_stream ATTACH FAIL '%s'\n", filename ? filename : "");
        Object_Unlink(&stream->base);
        free(stream->filename);
        free(stream);
        Release_Cached_File(data);
        return NULL;
    }

    Release_Cached_File(data);
    stream->valid = 1;
    dbg("open_stream OK '%s' rate=%u ch=%u frames=%u\n", filename ? filename : "", stream->base.blob.rate, stream->base.blob.channels, stream->base.blob.frames);
    return (HSTREAM)stream;
}

HSTREAM __stdcall AIL_open_stream_by_sample(HDIGDRIVER driver, HSAMPLE sample, const char* file_name, int mem)
{
    (void)sample;
    return AIL_open_stream(driver, file_name, mem);
}

void __stdcall AIL_close_stream(HSTREAM stream)
{
    struct _STREAM* s = (struct _STREAM*)stream;
    if (s == NULL || s->valid == 0) {
        return;
    }
    s->valid = 0;
    Object_Unlink(&s->base);
    Object_Release_Data(&s->base);
    free(s->filename);
    free(s);
}

void __stdcall AIL_start_stream(HSTREAM stream)
{
    struct _STREAM* s = (struct _STREAM*)stream;
    if (s == NULL || !s->base.soundValid) {
        return;
    }
    if (s->base.playing) {
        ma_sound_stop(&s->base.sound);
    }
    dbg("start_stream '%s'\n", s->filename ? s->filename : "(null)");
    ma_sound_seek_to_pcm_frame(&s->base.sound, 0);
    ma_sound_start(&s->base.sound);
    s->base.playing = 1;
    s->paused = 0;
    s->base.loopsLeft = s->base.loopCount;
}

void __stdcall AIL_pause_stream(HSTREAM stream, int onoff)
{
    struct _STREAM* s = (struct _STREAM*)stream;
    if (s != NULL && s->base.soundValid) {
        if (onoff) {
            ma_sound_stop(&s->base.sound);
            s->paused = 1;
        } else {
            ma_sound_start(&s->base.sound);
            s->paused = 0;
        }
    }
}

void __stdcall AIL_stop_stream(HSTREAM stream)
{
    struct _STREAM* s = (struct _STREAM*)stream;
    if (s != NULL && s->base.soundValid) {
        ma_sound_stop(&s->base.sound);
        ma_sound_seek_to_pcm_frame(&s->base.sound, 0);
        s->base.playing = 0;
        s->base.eosPending = 0;
        s->paused = 1;
    }
}

void __stdcall AIL_set_stream_volume_pan(HSTREAM stream, float volume, float pan)
{
    struct _STREAM* s = (struct _STREAM*)stream;
    if (s != NULL) {
        s->base.volume = volume;
        s->base.pan = pan;
        Apply_Volume(&s->base);
    }
}

void __stdcall AIL_stream_volume_pan(HSTREAM stream, float* volume, float* pan)
{
    struct _STREAM* s = (struct _STREAM*)stream;
    if (s != NULL) {
        if (volume != NULL) {
            *volume = s->base.volume;
        }
        if (pan != NULL) {
            *pan = s->base.pan;
        }
    }
}

int __stdcall AIL_stream_volume(HSTREAM stream)
{
    struct _STREAM* s = (struct _STREAM*)stream;
    return s != NULL ? (int)(s->base.volume * 127.0f) : 0;
}

void __stdcall AIL_set_stream_volume(HSTREAM stream, int volume)
{
    struct _STREAM* s = (struct _STREAM*)stream;
    if (s != NULL) {
        AIL_set_stream_volume_pan(stream, volume / 127.0f, s->base.pan);
    }
}

int __stdcall AIL_stream_pan(HSTREAM stream)
{
    struct _STREAM* s = (struct _STREAM*)stream;
    return s != NULL ? (int)(s->base.pan * 127.0f) : 0;
}

void __stdcall AIL_set_stream_pan(HSTREAM stream, int pan)
{
    struct _STREAM* s = (struct _STREAM*)stream;
    if (s != NULL) {
        AIL_set_stream_volume_pan(stream, s->base.volume, pan / 127.0f);
    }
}

void __stdcall AIL_set_stream_loop_count(HSTREAM stream, int count)
{
    struct _STREAM* s = (struct _STREAM*)stream;
    if (s != NULL) {
        s->base.loopCount = count;
        Apply_Loop(&s->base);
    }
}

int __stdcall AIL_stream_loop_count(HSTREAM stream)
{
    struct _STREAM* s = (struct _STREAM*)stream;
    return s != NULL ? s->base.loopCount : 0;
}

void __stdcall AIL_set_stream_loop_block(HSTREAM stream, int loop_start, int loop_end)
{
    (void)stream;
    (void)loop_start;
    (void)loop_end;
}

void __stdcall AIL_set_stream_ms_position(HSTREAM stream, int pos)
{
    struct _STREAM* s = (struct _STREAM*)stream;
    if (s != NULL && s->base.soundValid) {
        ma_sound_seek_to_pcm_frame(&s->base.sound, MS_To_Frames(&s->base.blob, (unsigned long long)pos));
    }
}

void __stdcall AIL_stream_ms_position(HSTREAM stream, S32* total_milliseconds, S32* current_milliseconds)
{
    struct _STREAM* s = (struct _STREAM*)stream;
    unsigned long long total = 0;
    unsigned long long current = 0;
    if (s != NULL) {
        total = Frames_To_MS(&s->base.blob, s->base.blob.frames);
        if (s->base.soundValid) {
            ma_uint64 cursor = 0;
            if (ma_sound_get_cursor_in_pcm_frames(&s->base.sound, &cursor) == MA_SUCCESS) {
                current = Frames_To_MS(&s->base.blob, cursor);
            }
        }
    }
    if (total_milliseconds != NULL) {
        *total_milliseconds = (S32)total;
    }
    if (current_milliseconds != NULL) {
        *current_milliseconds = (S32)current;
    }
}

void __stdcall AIL_set_stream_playback_rate(HSTREAM stream, int rate)
{
    struct _STREAM* s = (struct _STREAM*)stream;
    if (s != NULL) {
        s->base.rateRequest = rate;
        Apply_Rate(&s->base, s->base.blob.rate);
    }
}

int __stdcall AIL_stream_playback_rate(HSTREAM stream)
{
    struct _STREAM* s = (struct _STREAM*)stream;
    if (s == NULL) {
        return 0;
    }
    return s->base.rateRequest > 0 ? s->base.rateRequest : (int)s->base.blob.rate;
}

AIL_stream_callback __stdcall AIL_register_stream_callback(HSTREAM stream, AIL_stream_callback callback)
{
    (void)stream;
    (void)callback;
    return NULL;
}

AIL_sample_callback __stdcall AIL_register_EOS_callback(HSAMPLE sample, AIL_sample_callback EOS)
{
    struct _SAMPLE* s = (struct _SAMPLE*)sample;
    if (s == NULL) {
        return NULL;
    }
    s->base.eosCallback = (void (*)(void*))EOS;
    s->base.eosUserData = sample;
    return EOS;
}

AIL_3dsample_callback __stdcall AIL_register_3D_EOS_callback(H3DSAMPLE sample, AIL_3dsample_callback EOS)
{
    M3DObject* s = (M3DObject*)sample;
    if (s == NULL) {
        return NULL;
    }
    s->base.eosCallback = (void (*)(void*))EOS;
    s->base.eosUserData = sample;
    return EOS;
}

// ----------------------------------------------------------------------------
// Core lifecycle / misc
// ----------------------------------------------------------------------------

int MSS_auto_cleanup(void)
{
    return 0;
}

int __stdcall AIL_startup(void)
{
    dbg("AIL_startup\n");
    if (!g_csInit) {
        InitializeCriticalSection(&g_cs);
        g_csInit = 1;
    }
    Set_Error(NULL);
    return AIL_NO_ERROR;
}

void __stdcall AIL_shutdown(void)
{
    dbg("AIL_shutdown\n");
    while (g_objects != NULL) {
        SoundObject* obj = g_objects;
        Object_Unlink(obj);
        Object_Release_Data(obj);
        // The owning structs (_SAMPLE / M3DObject) are leaked intentionally
        // here; the engine is going away and the game frees its handles.
    }
    if (g_engineActive) {
        ma_engine_uninit(&g_engine);
        g_engineActive = MA_FALSE;
        g_engineRefCount = 0;
    }
    if (g_digDriver != NULL) {
        free(g_digDriver);
        g_digDriver = NULL;
    }
    g_3DProviderOpen = 0;
    if (g_csInit) {
        DeleteCriticalSection(&g_cs);
        g_csInit = 0;
    }
}

int __stdcall AIL_set_preference(unsigned int number, int value)
{
    (void)number;
    (void)value;
    return 0;
}

int __stdcall AIL_waveOutOpen(HDIGDRIVER* driver, LPHWAVEOUT* waveout, int id, LPWAVEFORMAT format)
{
    (void)waveout;
    (void)id;
    (void)format;
    dbg("waveOutOpen\n");
    if (driver == NULL) {
        return 1;
    }
    *driver = NULL;
    if (!Ensure_Engine()) {
        dbg("waveOutOpen FAIL engine\n");
        return 1;
    }
    DIG_DRIVER* dig = (DIG_DRIVER*)calloc(1, sizeof(DIG_DRIVER));
    if (dig == NULL) {
        return 1;
    }
    dig->emulated_ds = 0;
    g_digDriver = dig;
    *driver = dig;
    dbg("waveOutOpen OK %p\n", dig);
    return AIL_NO_ERROR;
}

void __stdcall AIL_waveOutClose(HDIGDRIVER driver)
{
    if (driver != NULL && driver == g_digDriver) {
        free(driver);
        g_digDriver = NULL;
    }
}

void __stdcall AIL_lock(void)
{
    Lock();
}

void __stdcall AIL_unlock(void)
{
    Unlock();
}

char* __stdcall AIL_last_error(void)
{
    return g_lastError;
}

int __stdcall AIL_enumerate_filters(HPROENUM* next, HPROVIDER* dest, char** name)
{
    (void)next;
    (void)dest;
    (void)name;
    return 0;
}

void __stdcall AIL_set_file_callbacks(AIL_file_open_callback opencb, AIL_file_close_callback closecb,
    AIL_file_seek_callback seekcb, AIL_file_read_callback readcb)
{
    g_fileOpen = opencb;
    g_fileClose = closecb;
    g_fileSeek = seekcb;
    g_fileRead = readcb;
}

int __stdcall AIL_WAV_info(const void* data, AILSOUNDINFO* info)
{
    if (data == NULL || info == NULL) {
        return 0;
    }
    const unsigned char* p = (const unsigned char*)data;
    if (p[0] != 'R' || p[1] != 'I' || p[2] != 'F' || p[3] != 'F' || p[8] != 'W' || p[9] != 'A' || p[10] != 'V' ||
        p[11] != 'E') {
        return 0;
    }

    int fmtTag = 0;
    int channels = 0;
    unsigned int rate = 0;
    unsigned int blockAlign = 0;
    int bits = 0;
    const unsigned char* dataPtr = NULL;
    unsigned int dataLen = 0;

    size_t riffSize = (size_t)p[4] | ((size_t)p[5] << 8) | ((size_t)p[6] << 16) | ((size_t)p[7] << 24);
    size_t fileLimit = riffSize + 8;
    size_t off = 12;
    int haveFmt = 0;

    while (off + 8 <= fileLimit) {
        unsigned int chunkSize = (unsigned int)p[off + 4] | ((unsigned int)p[off + 5] << 8) |
            ((unsigned int)p[off + 6] << 16) | ((unsigned int)p[off + 7] << 24);
        if (memcmp(p + off, "fmt ", 4) == 0 && chunkSize >= 16) {
            const unsigned char* f = p + off + 8;
            fmtTag = (int)f[0] | ((int)f[1] << 8);
            channels = (int)f[2] | ((int)f[3] << 8);
            rate = (unsigned int)f[4] | ((unsigned int)f[5] << 8) | ((unsigned int)f[6] << 16) |
                ((unsigned int)f[7] << 24);
            blockAlign = (unsigned int)f[12] | ((unsigned int)f[13] << 8);
            bits = (int)f[14] | ((int)f[15] << 8);
            if (fmtTag == 0xFFFE && chunkSize >= 40) {
                fmtTag = (int)f[24] | ((int)f[25] << 8);
            }
            haveFmt = 1;
        } else if (memcmp(p + off, "data", 4) == 0) {
            dataPtr = p + off + 8;
            dataLen = chunkSize;
        }
        off += 8 + chunkSize + (chunkSize & 1);
    }

    if (!haveFmt || dataPtr == NULL) {
        return 0;
    }

    info->format = fmtTag;
    info->data_ptr = dataPtr;
    info->data_len = dataLen;
    info->rate = rate;
    info->bits = bits;
    info->channels = channels;
    info->block_size = blockAlign;
    // For IMA ADPCM, block_size is the compressed block size.
    // The number of PCM samples per block is (block_size - header) * 2 + 1.
    // For PCM, samples = data_len / (block_align * channels * 2).
    if (fmtTag == WAVE_FORMAT_IMA_ADPCM) {
        unsigned int headerSize = (unsigned int)(channels * 4);
        if (blockAlign > headerSize) {
            unsigned int samplesPerBlock = (blockAlign - headerSize) * 2 + channels;
            unsigned int numBlocks = dataLen / blockAlign;
            info->samples = numBlocks * samplesPerBlock;
            /* Add samples from partial last block */
            unsigned int remainingBytes = dataLen % blockAlign;
            if (remainingBytes > headerSize) {
                info->samples += (remainingBytes - headerSize) * 2 + channels;
            }
        } else {
            info->samples = 0;
        }
    } else {
        info->samples = (blockAlign > 0) ? (dataLen / blockAlign) : 0;
    }
    info->initial_ptr = info->data_ptr;
    return 1;
}

void __stdcall AIL_stop_timer(HTIMER timer)
{
    (void)timer;
}

void __stdcall AIL_release_timer_handle(HTIMER timer)
{
    (void)timer;
}

unsigned long __stdcall AIL_get_timer_highest_delay(void)
{
    return 0;
}

void __stdcall AIL_mem_free_lock(void* ptr)
{
    if (ptr != NULL) {
        free(ptr);
    }
}

static const int IMA_stepTable[89] = {
     7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int IMA_indexTable[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static int decode_ima_adpcm_block(const unsigned char* src, int srcLen, int channels,
                                    short* out, int* predictor, int* index)
{
    if (srcLen < 4) return 0;

    predictor[0] = (short)(src[0] | (src[1] << 8));
    index[0] = src[2];
    if (index[0] < 0) index[0] = 0;
    if (index[0] > 88) index[0] = 88;
    if (channels == 2 && srcLen >= 8) {
        predictor[1] = (short)(src[4] | (src[5] << 8));
        index[1] = src[6];
        if (index[1] < 0) index[1] = 0;
        if (index[1] > 88) index[1] = 88;
    }

    out[0] = (short)predictor[0];
    int outPos = 1;
    int inPos = (channels == 2) ? 8 : 4;

    while (inPos < srcLen) {
        unsigned char byte = src[inPos++];

        for (int nibbleIdx = 0; nibbleIdx < 2; nibbleIdx++) {
            int nibble = (nibbleIdx == 0) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);

            int step = IMA_stepTable[index[0]];
            int diff = 0;
            if (nibble & 1) diff += step >> 2;
            if (nibble & 2) diff += step >> 1;
            if (nibble & 4) diff += step;
            if (nibble & 8) diff = -diff;

            predictor[0] += diff;
            if (predictor[0] > 32767) predictor[0] = 32767;
            if (predictor[0] < -32768) predictor[0] = -32768;

            out[outPos++] = (short)predictor[0];

            index[0] += IMA_indexTable[nibble & 7];
            if (index[0] < 0) index[0] = 0;
            if (index[0] > 88) index[0] = 88;

            if (channels == 2) {
                int step2 = IMA_stepTable[index[1]];
                int diff2 = 0;
                if (nibble & 1) diff2 += step2 >> 2;
                if (nibble & 2) diff2 += step2 >> 1;
                if (nibble & 4) diff2 += step2;
                if (nibble & 8) diff2 = -diff2;

                predictor[1] += diff2;
                if (predictor[1] > 32767) predictor[1] = 32767;
                if (predictor[1] < -32768) predictor[1] = -32768;

                out[outPos++] = (short)predictor[1];

                index[1] += IMA_indexTable[nibble & 7];
                if (index[1] < 0) index[1] = 0;
                if (index[1] > 88) index[1] = 88;
            }
        }
    }

    return outPos;
}

int __stdcall AIL_decompress_ADPCM(const AILSOUNDINFO* info, void** outdata, unsigned long* outsize)
{
    if (info == NULL || outdata == NULL || outsize == NULL) {
        if (outdata) *outdata = NULL;
        if (outsize) *outsize = 0;
        return -1;
    }
    *outdata = NULL;
    *outsize = 0;

    const unsigned char* src = (const unsigned char*)info->data_ptr;
    unsigned int dataLen = info->data_len;
    int channels = info->channels > 0 ? info->channels : 1;
    unsigned int rate = info->rate;
    unsigned int blockAlign = info->block_size > 0 ? info->block_size : 512;

    unsigned int headerSize = (unsigned int)(channels * 4);

    /* First pass: count total samples */
    unsigned int totalSamples = 0;
    unsigned int pos = 0;
    while (pos + headerSize <= dataLen) {
        unsigned int blockBytes = blockAlign;
        if (pos + blockBytes > dataLen) blockBytes = dataLen - pos;
        unsigned int nibbleBytes = blockBytes - headerSize;
        totalSamples += nibbleBytes * 2 + channels;
        pos += blockBytes;
    }

    if (totalSamples == 0) return -1;

    /* Allocate output buffer for PCM data */
    unsigned int pcmDataSize = totalSamples * sizeof(short);
    unsigned int totalSize = 44 + pcmDataSize;
    unsigned char* buf = (unsigned char*)malloc(totalSize);
    if (buf == NULL) return -1;

    /* Decode ADPCM to PCM */
    short* pcmOut = (short*)(buf + 44);
    unsigned int pcmPos = 0;
    pos = 0;
    int predictor[2] = {0, 0};
    int index[2] = {0, 0};

    while (pos + headerSize <= dataLen) {
        unsigned int blockBytes = blockAlign;
        if (pos + blockBytes > dataLen) blockBytes = dataLen - pos;

        int decoded = decode_ima_adpcm_block(src + pos, (int)blockBytes, channels,
                                              pcmOut + pcmPos, predictor, index);
        pcmPos += decoded;
        pos += blockBytes;
    }

    /* Build WAV header with PCM format (0x01) */
    unsigned char* wav = buf;
    wav[0]='R'; wav[1]='I'; wav[2]='F'; wav[3]='F';
    wav[4]=(unsigned char)(totalSize-8); wav[5]=(unsigned char)((totalSize-8)>>8);
    wav[6]=(unsigned char)((totalSize-8)>>16); wav[7]=(unsigned char)((totalSize-8)>>24);
    wav[8]='W'; wav[9]='A'; wav[10]='V'; wav[11]='E';

    wav[12]='f'; wav[13]='m'; wav[14]='t'; wav[15]=' ';
    wav[16]=16; wav[17]=0; wav[18]=0; wav[19]=0;
    wav[20]=0x01; wav[21]=0;
    wav[22]=(unsigned char)channels; wav[23]=0;
    wav[24]=(unsigned char)(rate&0xFF); wav[25]=(unsigned char)((rate>>8)&0xFF);
    wav[26]=(unsigned char)((rate>>16)&0xFF); wav[27]=(unsigned char)((rate>>24)&0xFF);

    unsigned int byteRate = rate * channels * sizeof(short);
    wav[28]=(unsigned char)(byteRate&0xFF); wav[29]=(unsigned char)((byteRate>>8)&0xFF);
    wav[30]=(unsigned char)((byteRate>>16)&0xFF); wav[31]=(unsigned char)((byteRate>>24)&0xFF);

    unsigned short blockAlignPcm = (unsigned short)(channels * sizeof(short));
    wav[32]=(unsigned char)(blockAlignPcm&0xFF); wav[33]=(unsigned char)(blockAlignPcm>>8);
    wav[34]=16; wav[35]=0;

    wav[36]='d'; wav[37]='a'; wav[38]='t'; wav[39]='a';
    wav[40]=(unsigned char)(pcmDataSize&0xFF); wav[41]=(unsigned char)((pcmDataSize>>8)&0xFF);
    wav[42]=(unsigned char)((pcmDataSize>>16)&0xFF); wav[43]=(unsigned char)((pcmDataSize>>24)&0xFF);

    *outdata = buf;
    *outsize = totalSize;
    return 0;
}

void __stdcall AIL_get_DirectSound_info(HSAMPLE sample, AILLPDIRECTSOUND* lplpDS, AILLPDIRECTSOUNDBUFFER* lplpDSB)
{
    (void)sample;
    dbg("getDSinfo sample=%p\n", sample);
    if (lplpDS != NULL) {
        *lplpDS = NULL;
    }
    if (lplpDSB != NULL) {
        *lplpDSB = NULL;
    }
}

char* __stdcall AIL_set_redist_directory(const char* dir)
{
    return (char*)dir;
}

// ----------------------------------------------------------------------------
// Quick API (minimal, unused by the W3D engine)
// ----------------------------------------------------------------------------

int __stdcall AIL_quick_startup(
    int use_digital, int use_MIDI, unsigned int output_rate, int output_bits, int output_channels)
{
    dbg("quick_startup dig=%d midi=%d rate=%u bits=%d ch=%d\n", use_digital, use_MIDI, output_rate, output_bits, output_channels);
    (void)use_digital;
    (void)use_MIDI;
    (void)output_rate;
    (void)output_bits;
    (void)output_channels;
    Ensure_DigDriver();
    int r = Ensure_Engine() ? 1 : 0;
    dbg("quick_startup -> %d engineActive=%d dig=%p\n", r, g_engineActive, g_digDriver);
    return r;
}

HAUDIO __stdcall AIL_quick_load_and_play(const char* filename, unsigned int loop_count, int wait_request)
{
    (void)wait_request;
    HSTREAM stream = AIL_open_stream(NULL, filename, 1);
    if (stream == NULL) {
        return NULL;
    }
    AIL_set_stream_loop_count(stream, (int)loop_count);
    AIL_start_stream(stream);
    return (HAUDIO)stream;
}

void __stdcall AIL_quick_set_volume(HAUDIO audio, float volume, float extravol)
{
    (void)extravol;
    AIL_set_stream_volume_pan((HSTREAM)audio, volume, 0.0f);
}

void __stdcall AIL_quick_unload(HAUDIO audio)
{
    AIL_close_stream((HSTREAM)audio);
}

void __stdcall AIL_quick_handles(HDIGDRIVER* pdig, HMDIDRIVER* pmdi, HDLSDEVICE* pdls)
{
    Ensure_DigDriver();
    dbg("quick_handles dig=%p\n", g_digDriver);
    if (pdig != NULL) {
        *pdig = g_digDriver;
    }
    if (pmdi != NULL) {
        *pmdi = NULL;
    }
    if (pdls != NULL) {
        *pdls = NULL;
    }
}

#ifdef _WIN32

// ----------------------------------------------------------------------------
// Crash diagnostics
//
// The mixing thread runs inside miniaudio and a crash there does not go through
// any of our functions, so it is invisible in the ordinary debug log. Install a
// process-wide unhandled-exception filter that dumps the faulting address, the
// owning module and a stack backtrace (with modules) to a separate file using
// only WriteFile (no heap allocations) so we can see exactly where it dies.
// ----------------------------------------------------------------------------

static void Crash_Log(const char* text, DWORD len)
{
    HANDLE h = CreateFileA("C:\\Users\\AFMRAYAN\\AppData\\Local\\Temp\\miles_crash.log",
        GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    SetFilePointer(h, 0, NULL, FILE_END);
    DWORD written = 0;
    WriteFile(h, text, len, &written, NULL);
    CloseHandle(h);
}

static LONG WINAPI Miles_ExceptionFilter(EXCEPTION_POINTERS* p)
{
    char buf[512];
    DWORD code = p->ExceptionRecord->ExceptionCode;
    PVOID addr = p->ExceptionRecord->ExceptionAddress;
    int len = wsprintfA(buf, "=== CRASH === code=0x%08X addr=%p\n", code, addr);
    Crash_Log(buf, (DWORD)len);

    HMODULE mod = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)addr, &mod) && mod) {
        char path[MAX_PATH] = { 0 };
        if (GetModuleFileNameA(mod, path, sizeof(path))) {
            len = wsprintfA(buf, "faulting module: %s (base=%p offset=0x%p)\n", path, (void*)mod,
                (void*)((char*)addr - (char*)mod));
            Crash_Log(buf, (DWORD)len);
        }
    }

    void* stack[48];
    USHORT count = CaptureStackBackTrace(0, 48, stack, NULL);
    for (USHORT i = 0; i < count; i++) {
        HMODULE m = NULL;
        char path[MAX_PATH] = { 0 };
        DWORD off = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)stack[i], &m) && m) {
            GetModuleFileNameA(m, path, sizeof(path));
            off = (DWORD)((char*)stack[i] - (char*)m);
        }
        len = wsprintfA(buf, "  #%02u %p +0x%08X %s\n", i, stack[i], off, path);
        Crash_Log(buf, (DWORD)len);
    }
    Crash_Log((char*)"=== END CRASH ===\n", 19);
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    (void)hModule;
    (void)lpReserved;
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
#ifdef _DEBUG
        {
            int fh = _open("C:\\Users\\AFMRAYAN\\AppData\\Local\\Temp\\miles_heap.txt",
                _O_WRONLY | _O_CREAT | _O_APPEND | _O_TEXT, _S_IREAD | _S_IWRITE);
            if (fh != -1) {
                _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
                _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
                _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
                _CrtSetReportFile(_CRT_WARN, (HFILE)fh);
                _CrtSetReportFile(_CRT_ERROR, (HFILE)fh);
                _CrtSetReportFile(_CRT_ASSERT, (HFILE)fh);
                _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_ALWAYS_DF | _CRTDBG_DELAY_FREE_MEM_DF);
            }
        }
#endif
        // Vectored handlers run before any top-level or frame-based handler and
        // are never replaced by the host application's own filter, so we reliably
        // get the crash location even if the game installs its own handler.
        AddVectoredExceptionHandler(1, Miles_ExceptionFilter);
        dbg("DllMain DLL_PROCESS_ATTACH -- BUILD MARKER: locking-fix + open_stream-cache-fix v2\n");
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        dbg("DllMain DLL_PROCESS_DETACH\n");
    }
    return TRUE;
}
#endif
