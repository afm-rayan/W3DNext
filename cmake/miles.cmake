# In-tree Miles Sound System API implementation backed by miniaudio.
# Provides the same "milesstub" (mss32.dll) target that the previously used
# FetchContent stub (TheSuperHackers/miles-sdk-stub) provided, but with a real
# audio backend: WAV (PCM/float/IMA ADPCM), FLAC, MP3 and OGG Vorbis decoding,
# 2D mixing, 3D spatialization and file/stream playback.
add_subdirectory(${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/milesaudio milesaudio)
