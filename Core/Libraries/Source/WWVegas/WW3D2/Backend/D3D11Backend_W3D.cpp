/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// D3D11Backend_W3D - the ww3d2-ONLY half of the D3D11 backend
// (RENDERER_PORT.md step 10). It holds the D3D11Backend virtual methods whose
// bodies must dereference the W3D handle classes (VertexBufferClass,
// IndexBufferClass, the dynamic-access classes, ShaderClass, VertexMaterialClass,
// TextureBaseClass) - i.e. the full WW3D2 + d3d8 header graph.
//
// It is built ONLY into the game (the corei_ww3d2 interface library, which
// defines W3DNEXT_D3D11_W3D_TU); it is deliberately NOT part of the standalone
// w3d_d3d11_smoke target, which links no ww3d2. The matching stub bodies in
// D3D11Backend.cpp are #ifndef W3DNEXT_D3D11_W3D_TU'd out here and left in for the
// smoke build, so exactly one definition of each method exists in each link and
// the smoke oracle keeps compiling against the raw-bytes Upload_* entry points.
//
// Every method here reads bytes / parameters out of the W3D objects and calls
// D3D11Backend's OWN core methods (Upload_Vertices / Upload_Indices16 /
// Upload_Texture_RGBA / Set_Material_Params / the blend-depth-cull + combiner
// setters) - the same machinery the smoke test drives. No raw D3D11 type is
// touched here (d3d8 is the SOURCE side); the D3D11 device work stays in
// D3D11Backend.cpp behind those typed entry points.

#include "D3D11Backend.h"

#include <d3d11.h>           // full ID3D11DeviceContext def (RTT SRV bind in Set_Texture)
#include "RenderBackend.h"   // Is_D3D11_Backend_Active (copy-shadow gate)
#include "dx8wrapper.h"      // BUFFER_TYPE_* enum
#include "dx8fvf.h"          // FVFInfoClass (+ transitively <d3d8.h>)
#include "dx8vertexbuffer.h" // VertexBufferClass / DynamicVBAccessClass + lock classes
#include "dx8indexbuffer.h"  // IndexBufferClass / DynamicIBAccessClass + lock classes
#include "shader.h"          // ShaderClass
#include "vertmaterial.h"    // VertexMaterialClass
#include "texture.h"         // TextureBaseClass / TextureClass
#include "vector3.h"

#include <d3d8.h>
#include <cstring>
#include <map>
#include <new>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <utility>
#include <vector>

// ----------------------------------------------------------------------------
// ShaderClass -> backend-neutral blend / depth / combiner translation
// ----------------------------------------------------------------------------
namespace
{

RenderBackendBlendFactor Map_Src_Blend(ShaderClass::SrcBlendFuncType s)
{
	switch (s) {
	case ShaderClass::SRCBLEND_ZERO:                 return RB_BLEND_ZERO;
	case ShaderClass::SRCBLEND_ONE:                  return RB_BLEND_ONE;
	case ShaderClass::SRCBLEND_SRC_ALPHA:            return RB_BLEND_SRCALPHA;
	case ShaderClass::SRCBLEND_ONE_MINUS_SRC_ALPHA:  return RB_BLEND_INVSRCALPHA;
	default:                                         return RB_BLEND_ONE;
	}
}

RenderBackendBlendFactor Map_Dst_Blend(ShaderClass::DstBlendFuncType d)
{
	switch (d) {
	case ShaderClass::DSTBLEND_ZERO:                 return RB_BLEND_ZERO;
	case ShaderClass::DSTBLEND_ONE:                  return RB_BLEND_ONE;
	case ShaderClass::DSTBLEND_SRC_COLOR:            return RB_BLEND_SRCCOLOR;
	case ShaderClass::DSTBLEND_ONE_MINUS_SRC_COLOR:  return RB_BLEND_INVSRCCOLOR;
	case ShaderClass::DSTBLEND_SRC_ALPHA:            return RB_BLEND_SRCALPHA;
	case ShaderClass::DSTBLEND_ONE_MINUS_SRC_ALPHA:  return RB_BLEND_INVSRCALPHA;
	default:                                         return RB_BLEND_ZERO;
	}
}

RenderBackendCmpFunc Map_Depth_Cmp(ShaderClass::DepthCompareType c)
{
	switch (c) {
	case ShaderClass::PASS_NEVER:     return RB_CMP_NEVER;
	case ShaderClass::PASS_LESS:      return RB_CMP_LESS;
	case ShaderClass::PASS_EQUAL:     return RB_CMP_EQUAL;
	case ShaderClass::PASS_LEQUAL:    return RB_CMP_LESSEQUAL;
	case ShaderClass::PASS_GREATER:   return RB_CMP_GREATER;
	case ShaderClass::PASS_NOTEQUAL:  return RB_CMP_NOTEQUAL;
	case ShaderClass::PASS_GEQUAL:    return RB_CMP_GREATEREQUAL;
	case ShaderClass::PASS_ALWAYS:    return RB_CMP_ALWAYS;
	default:                          return RB_CMP_LESSEQUAL;
	}
}

// One-shot (per distinct format+reason) diagnostic for textures that bind the
// magenta fallback, into the same W3DNEXT_D3D11_LOG sink the stub trace uses - so
// the remaining format gaps are self-announcing in the log as well as on
// screen. Recon-grade: not thread-safe, one set lookup per fallback.
void Trace_Texture_Fallback(unsigned int format, unsigned int w, unsigned int h, const char * reason)
{
	static std::set<std::pair<unsigned int, const char *> > seen;
	if (!seen.insert(std::make_pair(format, reason)).second) {
		return;
	}
	char fourcc[5] = { 0, 0, 0, 0, 0 };
	if (format > 0x100) { // FOURCC-style format code
		fourcc[0] = static_cast<char>(format & 0xFF);
		fourcc[1] = static_cast<char>((format >> 8) & 0xFF);
		fourcc[2] = static_cast<char>((format >> 16) & 0xFF);
		fourcc[3] = static_cast<char>((format >> 24) & 0xFF);
	}
	char buf[256];
	std::snprintf(buf, sizeof(buf),
		"[D3D11 texture-fallback] format=%u('%s') %ux%u (%s)", format, fourcc, w, h, reason);
	const char * path = W3DNext_GetEnv("D3D11_LOG");
	FILE * f = std::fopen(path != nullptr ? path : "d3d11_backend.log", "a");
	if (f != nullptr) {
		std::fputs(buf, f);
		std::fputc('\n', f);
		std::fclose(f);
	}
}

// Diagnostics (W3DNEXT_D3D11_DRAWLOG set): one-shot per texture name, log the alpha
// range of the uploaded level-0 bytes into the W3DNEXT_D3D11_LOG sink. Directly
// tests the "menu art loses alpha in upload" hypothesis class: an opaque
// button fill must report aMin=255.
static bool Draw_Log_Enabled()
{
	static bool s_checked = false, s_on = false;
	if (!s_checked) {
		s_checked = true;
		const char * p = W3DNext_GetEnv("D3D11_DRAWLOG");
		s_on = (p != nullptr && p[0] != '\0');
	}
	return s_on;
}

static void Trace_Texture_Alpha(const char * name, unsigned int format,
	unsigned int w, unsigned int h, unsigned int aMin, unsigned int aMax)
{
	static std::set<std::string> seen;
	if (!Draw_Log_Enabled() || !seen.insert(name != nullptr ? name : "").second) {
		return;
	}
	const char * path = W3DNext_GetEnv("D3D11_LOG");
	FILE * f = std::fopen(path != nullptr ? path : "d3d11_backend.log", "a");
	if (f != nullptr) {
		std::fprintf(f, "[D3D11 tex-alpha] %s fmt=%u %ux%u aMin=%u aMax=%u\n",
			name != nullptr ? name : "?", format, w, h, aMin, aMax);
		std::fclose(f);
	}
}

// ----------------------------------------------------------------------------
// CopyRects CPU shadow (the fog-of-war shroud path)
//
// The shroud dst texture is POOL_DEFAULT: its content is composed GPU-side by
// DX8Wrapper::_Copy_DX8_Rects from a lockable sysmem surface, and LockRect on
// it fails at bind time - so Set_Texture used to bind neutral white (no shroud
// darkening at all). _Copy_DX8_Rects now mirrors each copy (D3D11 mode only)
// into a CPU shadow keyed by the DESTINATION IDirect3DSurface8*, and the
// LockRect-failed branch of Set_Texture uploads from that shadow instead.
// Destinations that ARE lockable (screenshot sysmem targets, managed font
// surfaces) are probed once and skipped - Set_Texture reads those directly.
// Entries persist for the process (never erased): a dst surface freed and
// reallocated at the same address is caught by the size/format recheck, and
// the buffers are tiny (the shroud dst is 64x128x2 = 16 KB per map load).
// ----------------------------------------------------------------------------

struct CopySurfaceShadow
{
	bool decided = false;      // probe result recorded
	bool dstLockable = false;  // dst readable at bind time -> no shadow kept
	unsigned int width = 0, height = 0, format = 0, bpp = 0;
	std::vector<unsigned char> pixels; // tightly packed, pitch = width*bpp
	// Bumped on every mirrored copy. The uploaded-texture cache keys on it so a
	// live-updating destination (the shroud, rewritten every frame) re-uploads
	// while every static texture stays a permanent cache hit.
	unsigned long long version = 0;
};

std::map<IDirect3DSurface8 *, CopySurfaceShadow> s_copyShadows;

// Bytes per pixel of the surface formats the shadow (and the RGBA decode
// below) understands; 0 = not shadowable.
unsigned int Surface_Format_Bpp(unsigned int format)
{
	switch (format) {
	case D3DFMT_A8R8G8B8:
	case D3DFMT_X8R8G8B8:
		return 4;
	case D3DFMT_R5G6B5:
	case D3DFMT_A4R4G4B4:
	case D3DFMT_X1R5G5B5:
	case D3DFMT_A1R5G5B5:
		return 2;
	default:
		return 0;
	}
}

// Swizzle a 16/32-bit D3D8 surface row-block into R8G8B8A8 (R first in
// memory). Exactly the decode Set_Texture always did; hoisted so the
// copy-shadow path shares it. False for formats outside the supported six.
bool Decode_Surface_To_RGBA(unsigned int format, unsigned int w, unsigned int h,
	const unsigned char * src, unsigned int pitch, unsigned char * rgba)
{
	const bool is32 = (format == D3DFMT_A8R8G8B8 || format == D3DFMT_X8R8G8B8);
	const bool is16 = (format == D3DFMT_A4R4G4B4 || format == D3DFMT_R5G6B5 ||
		format == D3DFMT_X1R5G5B5 || format == D3DFMT_A1R5G5B5);
	if (!is32 && !is16) {
		return false;
	}
	for (unsigned int y = 0; y < h; ++y) {
		const unsigned char * sp = src + static_cast<size_t>(y) * pitch;
		unsigned char * dp = rgba + static_cast<size_t>(y) * w * 4;
		for (unsigned int x = 0; x < w; ++x) {
			unsigned char r, g, b, a;
			if (is32) {
				// memory order B,G,R,A
				b = sp[x * 4 + 0]; g = sp[x * 4 + 1]; r = sp[x * 4 + 2];
				a = (format == D3DFMT_X8R8G8B8) ? 255 : sp[x * 4 + 3];
			} else {
				const unsigned short p =
					static_cast<unsigned short>(sp[x * 2 + 0] | (sp[x * 2 + 1] << 8));
				if (format == D3DFMT_R5G6B5) {
					const unsigned int R = (p >> 11) & 0x1F, G = (p >> 5) & 0x3F, B = p & 0x1F;
					r = static_cast<unsigned char>((R << 3) | (R >> 2));
					g = static_cast<unsigned char>((G << 2) | (G >> 4));
					b = static_cast<unsigned char>((B << 3) | (B >> 2));
					a = 255;
				} else {
					// 1555 / X555 / 4444 all have 5- or 4-bit R,G,B; branch on alpha.
					const unsigned int R = (p >> 10) & 0x1F, G = (p >> 5) & 0x1F, B = p & 0x1F;
					if (format == D3DFMT_A4R4G4B4) {
						const unsigned int A4 = (p >> 12) & 0xF, R4 = (p >> 8) & 0xF, G4 = (p >> 4) & 0xF, B4 = p & 0xF;
						r = static_cast<unsigned char>((R4 << 4) | R4);
						g = static_cast<unsigned char>((G4 << 4) | G4);
						b = static_cast<unsigned char>((B4 << 4) | B4);
						a = static_cast<unsigned char>((A4 << 4) | A4);
					} else {
						r = static_cast<unsigned char>((R << 3) | (R >> 2));
						g = static_cast<unsigned char>((G << 3) | (G >> 2));
						b = static_cast<unsigned char>((B << 3) | (B >> 2));
						a = (format == D3DFMT_A1R5G5B5) ? ((p & 0x8000) ? 255 : 0) : 255;
					}
				}
			}
			dp[x * 4 + 0] = r; dp[x * 4 + 1] = g; dp[x * 4 + 2] = b; dp[x * 4 + 3] = a;
		}
	}
	return true;
}

// One-shot W3DNEXT_D3D11_LOG line the first time a copy-shadow actually feeds a
// bind (per format+dims), mirroring Trace_Texture_Fallback's sink - so the
// shroud path's recovery is machine-checkable in the same log the old
// "LockRect failed -> neutral white" fallback used to announce itself in.
void Trace_Copy_Shadow_Bind(unsigned int format, unsigned int w, unsigned int h)
{
	static std::set<unsigned long long> seen;
	const unsigned long long key =
		(static_cast<unsigned long long>(format) << 32) | (w << 16) | h;
	if (!seen.insert(key).second) {
		return;
	}
	char buf[256];
	std::snprintf(buf, sizeof(buf),
		"[D3D11 copy-shadow] bind format=%u %ux%u (CopyRects-mirrored content)", format, w, h);
	const char * path = W3DNext_GetEnv("D3D11_LOG");
	FILE * f = std::fopen(path != nullptr ? path : "d3d11_backend.log", "a");
	if (f != nullptr) {
		std::fputs(buf, f);
		std::fputc('\n', f);
		std::fclose(f);
	}
}

// Bind-time consumer: if level 0 of `tex` is a shadowed CopyRects destination,
// decode the shadow to RGBA and upload it. False -> caller falls back.
bool Upload_From_Copy_Shadow(D3D11Backend * backend, unsigned int stage,
	IDirect3DTexture8 * tex, const D3DSURFACE_DESC & desc)
{
	IDirect3DSurface8 * surf = nullptr;
	if (FAILED(tex->GetSurfaceLevel(0, &surf)) || surf == nullptr) {
		return false;
	}
	surf->Release(); // used as a map key only; the texture keeps the real ref

	std::map<IDirect3DSurface8 *, CopySurfaceShadow>::const_iterator it = s_copyShadows.find(surf);
	if (it == s_copyShadows.end()) {
		return false;
	}
	const CopySurfaceShadow & sh = it->second;
	if (sh.dstLockable || sh.pixels.empty() ||
		sh.width != desc.Width || sh.height != desc.Height ||
		sh.format != static_cast<unsigned int>(desc.Format)) {
		return false;
	}

	std::vector<unsigned char> rgba(static_cast<size_t>(sh.width) * sh.height * 4);
	if (!Decode_Surface_To_RGBA(sh.format, sh.width, sh.height,
			sh.pixels.data(), sh.width * sh.bpp, rgba.data())) {
		return false;
	}
	Trace_Copy_Shadow_Bind(sh.format, sh.width, sh.height);
	// wrap=false = CLAMP addressing - what the shroud sets on its dst texture
	// (TEXTURE_ADDRESS_CLAMP); linear matches its FILTER_TYPE_DEFAULT.
	return backend->Upload_Texture_RGBA(stage, sh.width, sh.height, rgba.data(),
		/*wrap*/false, /*linear*/true);
}

} // namespace

// Diagnostics accessor for the W3DNEXT_D3D11_DRAWLOG per-draw log (W3D TU: needs
// TextureBaseClass). Smoke-TU stub lives in D3D11Backend.cpp.
const char * D3D11Backend::Peek_Stage_Tex_Name(unsigned int stage) const
{
	if (stage >= RB_MAX_TEXTURE_STAGES || m_boundTextures[stage] == nullptr) {
		return "";
	}
	// Static storage is fine: the caller prints the string before returning.
	static StringClass s_name;
	s_name = m_boundTextures[stage]->Get_Texture_Name();
	return s_name.Peek_Buffer();
}

// Texture-cache eviction (declared in dx8wrapper.h). Called from
// ~TextureBaseClass so a dying texture's uploaded D3D11 copy is released with
// it. Hygiene only: ids are never reused, so a missed eviction cannot alias.
void D3D11_Evict_Cached_Texture(unsigned texture_id)
{
	if (!Is_D3D11_Backend_Active()) {
		return;
	}
	static_cast<D3D11Backend *>(g_renderBackend)->Evict_Cached_Texture(texture_id);
}

// W3DNext: drop a dying render-target texture's D3D11 resources (mirror of the
// uploaded-texture eviction above) so a recreated RT gets a fresh m_rtt entry.
void D3D11_Evict_Render_Target(TextureBaseClass * tex)
{
	if (!Is_D3D11_Backend_Active()) {
		return;
	}
	static_cast<D3D11Backend *>(g_renderBackend)->Evict_Render_Target(tex);
}

// The _Copy_DX8_Rects mirror (declared in dx8wrapper.h). Records the copied
// bytes CPU-side for destinations that can't be read back at bind time. Runs
// AFTER the real CopyRects; no-op on the default DX8 backend.
void D3D11_Mirror_Copy_Rects(
	IDirect3DSurface8 * pSourceSurface,
	CONST RECT * pSourceRectsArray,
	UINT cRects,
	IDirect3DSurface8 * pDestinationSurface,
	CONST POINT * pDestPointsArray)
{
	if (!Is_D3D11_Backend_Active() || pSourceSurface == nullptr || pDestinationSurface == nullptr) {
		return;
	}

	D3DSURFACE_DESC dstDesc;
	if (FAILED(pDestinationSurface->GetDesc(&dstDesc))) {
		return;
	}
	const unsigned int bpp = Surface_Format_Bpp(dstDesc.Format);
	if (bpp == 0) {
		return; // format the bind-time decode couldn't use anyway
	}

	CopySurfaceShadow & shadow = s_copyShadows[pDestinationSurface];
	if (!shadow.decided ||
		shadow.width != dstDesc.Width || shadow.height != dstDesc.Height ||
		shadow.format != static_cast<unsigned int>(dstDesc.Format)) {
		// New (or reallocated) destination: record geometry and probe whether it
		// is CPU-lockable. Lockable destinations need no shadow - Set_Texture
		// reads their bytes directly.
		shadow.decided = true;
		shadow.width = dstDesc.Width;
		shadow.height = dstDesc.Height;
		shadow.format = static_cast<unsigned int>(dstDesc.Format);
		shadow.bpp = bpp;
		D3DLOCKED_RECT probe;
		shadow.dstLockable =
			SUCCEEDED(pDestinationSurface->LockRect(&probe, nullptr, D3DLOCK_READONLY));
		if (shadow.dstLockable) {
			pDestinationSurface->UnlockRect();
			shadow.pixels.clear();
		} else {
			shadow.pixels.assign(static_cast<size_t>(shadow.width) * shadow.height * bpp, 0);
		}
	}
	if (shadow.dstLockable) {
		return;
	}

	D3DSURFACE_DESC srcDesc;
	if (FAILED(pSourceSurface->GetDesc(&srcDesc)) ||
		Surface_Format_Bpp(srcDesc.Format) != bpp) {
		return; // cross-format CopyRects is invalid in D3D8 anyway
	}

	D3DLOCKED_RECT lr;
	if (FAILED(pSourceSurface->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
		return; // source unreadable (GPU->GPU copy) - can't mirror
	}
	const unsigned char * srcBase = static_cast<const unsigned char *>(lr.pBits);

	// D3D8 CopyRects semantics: a null rect array means one whole-surface copy
	// to (0,0); a null point array means each rect lands at (0,0).
	const UINT count = (pSourceRectsArray != nullptr) ? cRects : 1;
	for (UINT i = 0; i < count; ++i) {
		LONG sx = 0, sy = 0;
		LONG w = static_cast<LONG>(srcDesc.Width), h = static_cast<LONG>(srcDesc.Height);
		if (pSourceRectsArray != nullptr) {
			sx = pSourceRectsArray[i].left;
			sy = pSourceRectsArray[i].top;
			w = pSourceRectsArray[i].right - sx;
			h = pSourceRectsArray[i].bottom - sy;
		}
		LONG dx = 0, dy = 0;
		if (pDestPointsArray != nullptr) {
			dx = pDestPointsArray[i].x;
			dy = pDestPointsArray[i].y;
		}
		if (sx < 0 || sy < 0 || dx < 0 || dy < 0) {
			continue;
		}
		// Clamp to both surfaces so a malformed rect can't run off a buffer.
		if (w > static_cast<LONG>(srcDesc.Width) - sx) w = static_cast<LONG>(srcDesc.Width) - sx;
		if (h > static_cast<LONG>(srcDesc.Height) - sy) h = static_cast<LONG>(srcDesc.Height) - sy;
		if (w > static_cast<LONG>(shadow.width) - dx) w = static_cast<LONG>(shadow.width) - dx;
		if (h > static_cast<LONG>(shadow.height) - dy) h = static_cast<LONG>(shadow.height) - dy;
		for (LONG y = 0; y < h; ++y) {
			if (w > 0) {
				std::memcpy(
					&shadow.pixels[(static_cast<size_t>(dy + y) * shadow.width + dx) * bpp],
					srcBase + static_cast<size_t>(sy + y) * lr.Pitch + static_cast<size_t>(sx) * bpp,
					static_cast<size_t>(w) * bpp);
			}
		}
	}
	pSourceSurface->UnlockRect();
	// Content changed: invalidate any cached GPU upload of this destination.
	++shadow.version;
}

// ----------------------------------------------------------------------------
// Geometry bind: read the W3D buffer bytes and push them through Upload_*.
//
// The DX8 *dynamic* buffers (BUFFER_TYPE_DYNAMIC_DX8 - the 2D GUI / particle
// path) discard-lock, so they can't be read here; they are captured at write
// time by Stage_Dynamic_Vertices / Stage_Dynamic_Indices instead (see
// D3D11Backend.cpp) and these overloads are a no-op for them. The static and
// SORTING variants keep their data in a readable (system-memory or lockable)
// store and are uploaded here.
// ----------------------------------------------------------------------------

void D3D11Backend::Set_Vertex_Buffer(const VertexBufferClass * vb, unsigned int stream)
{
	m_boundVertexBuffer = vb;
	m_boundVertexStream = stream;
	if (vb != nullptr) {
		const unsigned int fvf = vb->FVF_Info().Get_FVF();
		const unsigned int fvf_size = vb->FVF_Info().Get_FVF_Size();
		const unsigned int count = vb->Get_Vertex_Count();
		// The WriteLockClass ctor asserts the buffer holds no engine ref; skip if
		// one is held rather than trip the assert. With the DX8Wrapper mirror below
		// the only engine-ref holder at bind time is render_state itself, i.e. a
		// consecutive re-bind of the still-recorded buffer - its bytes are already
		// the current upload, so skipping is content-correct, not lossy.
		if (fvf_size != 0 && count != 0 && vb->Engine_Refs() == 0) {
			VertexBufferClass::WriteLockClass lock(const_cast<VertexBufferClass *>(vb), 0);
			const void * src = lock.Get_Vertex_Array();
			if (src != nullptr) {
				Upload_Vertices(src, fvf_size * count, fvf);
			}
		}
	}
	// Mirror the bind into DX8Wrapper's engine-side render_state record, exactly
	// as DX8Backend does. SortingRendererClass::Insert_Triangles captures the
	// current VB/IB via DX8Wrapper::Get_Render_State(); without the mirror the
	// captured vertex_buffers[0] is null and Get_Vertex_Count() null-derefs -
	// the D3D11 world-entry crash. Mirrored AFTER the upload so the engine ref
	// taken by the record does not block the WriteLock read above.
	DX8Wrapper::Set_Vertex_Buffer(vb, stream);
}

void D3D11Backend::Set_Vertex_Buffer(const DynamicVBAccessClass & vba)
{
	// DX8-dynamic is staged at write time; only the readable sorting slice here.
	if (vba.Get_Type() == BUFFER_TYPE_DYNAMIC_SORTING) {
		const unsigned int fvf = vba.FVF_Info().Get_FVF();
		const unsigned int fvf_size = vba.FVF_Info().Get_FVF_Size();
		const unsigned int count = vba.Get_Vertex_Count();
		if (fvf_size != 0 && count != 0) {
			DynamicVBAccessClass::WriteLockClass lock(const_cast<DynamicVBAccessClass *>(&vba));
			const void * src = lock.Get_Formatted_Vertex_Array();
			if (src != nullptr) {
				Upload_Vertices(src, fvf_size * count, fvf);
			}
		}
	}
	m_boundVertexStream = 0;
	// Mirror into the engine-side render_state record (see the static overload).
	DX8Wrapper::Set_Vertex_Buffer(vba);
}

void D3D11Backend::Set_Index_Buffer(const IndexBufferClass * ib, unsigned short index_base_offset)
{
	m_boundIndexBuffer = ib;
	m_indexBaseOffset = index_base_offset;
	if (ib != nullptr) {
		const unsigned int count = ib->Get_Index_Count();
		// Engine-ref guard: same reasoning as Set_Vertex_Buffer above.
		if (count != 0 && ib->Engine_Refs() == 0) {
			IndexBufferClass::WriteLockClass lock(const_cast<IndexBufferClass *>(ib), 0);
			const unsigned short * src = lock.Get_Index_Array();
			if (src != nullptr) {
				Upload_Indices16(src, count);
			}
		}
	}
	// Mirror into the engine-side render_state record (see Set_Vertex_Buffer).
	DX8Wrapper::Set_Index_Buffer(ib, index_base_offset);
}

void D3D11Backend::Set_Index_Buffer(const DynamicIBAccessClass & iba, unsigned short index_base_offset)
{
	// The uploaded slice is 0-based within itself, so the DrawIndexed base-vertex
	// is 0 for the dynamic path (the DX8 SetIndices base only applies to shared
	// static index buffers, which come through the IndexBufferClass* overload).
	m_indexBaseOffset = 0;
	if (iba.Get_Type() == BUFFER_TYPE_DYNAMIC_SORTING) {
		const unsigned int count = iba.Get_Index_Count();
		if (count != 0) {
			DynamicIBAccessClass::WriteLockClass lock(const_cast<DynamicIBAccessClass *>(&iba));
			const unsigned short * src = lock.Get_Index_Array();
			if (src != nullptr) {
				Upload_Indices16(src, count);
			}
		}
	} // else: DX8-dynamic captured by Stage_Dynamic_Indices
	// Mirror into the engine-side render_state record; the ENGINE-side
	// index_base_offset is passed through unchanged (Flush uses it as a CPU-side
	// vertex offset), only the GPU DrawIndexed base above stays 0.
	DX8Wrapper::Set_Index_Buffer(iba, index_base_offset);
}

// Real body here (not D3D11Backend.cpp) because the render_state mirror needs
// dx8wrapper.h: the packed static-mesh path re-bases shared IBs through this,
// and the sorting renderer reads the recorded offset back out of render_state.
void D3D11Backend::Set_Index_Buffer_Index_Offset(unsigned int offset)
{
	m_indexBaseOffset = offset;
	DX8Wrapper::Set_Index_Buffer_Index_Offset(offset);
}

// Real body here (not D3D11Backend.cpp) for the same reason: the DX8Wrapper
// mirror records the light environment (and its per-light render_state entries)
// that the sorting renderer re-applies at flush time.
void D3D11Backend::Set_Light_Environment(LightEnvironmentClass * light_env)
{
	m_lightEnvironment = light_env;
	DX8Wrapper::Set_Light_Environment(light_env);

	// Unpack the environment into cbLighting, mirroring the device-side unpack
	// DX8Wrapper::Set_Light_Environment performs on the DX8 path (ambient +
	// up-to-4 lights). Without this every mesh rendered only its ambient term.
	// Direction convention: the FF VS treats lightDir as the TRAVEL direction
	// (L = normalize(-lightDir)), same as D3DLIGHT8.Direction, so negate
	// Get_Light_Direction exactly like the DX8 unpack does.
	if (light_env != nullptr) {
		Set_Ambient(light_env->Get_Equivalent_Ambient());
		const int light_count = light_env->Get_Light_Count();
		int l = 0;
		for (; l < light_count && l < (int)RB_MAX_LIGHTS; ++l) {
			if (light_env->isPointLight(l)) {
				// The FF shaders have no point-light term yet. A point light's
				// diffuse/attenuation dropped here under-lights meshes near it
				// (glows); directional sun/fill lights dominate in practice.
				static bool logged_point = false;
				if (!logged_point) {
					logged_point = true;
					const char * path = W3DNext_GetEnv("D3D11_LOG");
					FILE * f = std::fopen(path != nullptr ? path : "d3d11_backend.log", "a");
					if (f != nullptr) {
						std::fputs("[D3D11 light-env] partial: point light dropped (no FF point term)\n", f);
						std::fclose(f);
					}
				}
				Disable_Light(l);
				continue;
			}
			Set_Light_Directional(l, -light_env->Get_Light_Direction(l), light_env->Get_Light_Diffuse(l));
		}
		for (; l < (int)RB_MAX_LIGHTS; ++l) {
			Disable_Light(l);
		}
		Set_Light_Count(light_count < (int)RB_MAX_LIGHTS ? light_count : (int)RB_MAX_LIGHTS);
	}
}

// ----------------------------------------------------------------------------
// ShaderClass -> blend / depth / cull state + combiner
// ----------------------------------------------------------------------------

void D3D11Backend::Set_Shader(const ShaderClass & shader)
{
	// Mirror into DX8Wrapper's engine-side render_state record (as DX8Backend
	// does): the sorting renderer captures shader/material/textures via
	// DX8Wrapper::Get_Render_State and re-applies them at flush time.
	DX8Wrapper::Set_Shader(shader);

	// Fixed-function (combiner) shader request: the terrain-normal pixel shader
	// is bound eagerly (PSSetShader) and stays bound until explicitly released.
	// Decal/projected shadows (and any other FF draw) set their shader through
	// this path WITHOUT going through W3DShaderManager::setShader, so the leak
	// guard there never fires for them. If the terrain-normal shader is still
	// active, drop it back to the FF combiner so shadows/decals render with the
	// correct alpha blend instead of the opaque normal shader (black square
	// corners + flicker). Safe for the terrain pass itself: that path re-binds
	// the normal shader via Set_Terrain_Normal_Pixel_Shader AFTER any Set_Shader.
	if (m_terrainNormalIsActive) {
		Set_Terrain_Normal_Pixel_Shader(false);
	}

	ShaderClass & s = const_cast<ShaderClass &>(shader);
	m_lastShaderBits = s.Get_Bits();

	const ShaderClass::SrcBlendFuncType src = s.Get_Src_Blend_Func();
	const ShaderClass::DstBlendFuncType dst = s.Get_Dst_Blend_Func();
	// ONE/ZERO is the opaque no-blend case; anything else enables blending.
	const bool blend_on = !(src == ShaderClass::SRCBLEND_ONE && dst == ShaderClass::DSTBLEND_ZERO);
	Set_Blend_Enable(blend_on);
	Set_Blend_Func(Map_Src_Blend(src), Map_Dst_Blend(dst));
	Set_Blend_Op(RB_BLENDOP_ADD);

	// Alpha test, mirroring ShaderClass::Apply exactly (shader.cpp): ref 0x60
	// GREATEREQUAL, inverted to (0xff-0x60) LESSEQUAL under INVSRCALPHA source
	// blend. Unhandled before 2026-07-28: every alpha-tested cutout (prop
	// grass tufts, building pad decals like the supply-center jet arrow) drew
	// its full quad - opaque black/straw rectangles where DX8 clips.
	if (s.Get_Alpha_Test() == ShaderClass::ALPHATEST_ENABLE) {
		if (src == ShaderClass::SRCBLEND_ONE_MINUS_SRC_ALPHA) {
			Set_Alpha_Test(true, /*less_equal*/true, (255.0f - 0x60) / 255.0f);
		} else {
			Set_Alpha_Test(true, /*less_equal*/false, (float)0x60 / 255.0f);
		}
	} else {
		Set_Alpha_Test(false, false, 0.0f);
	}

	// ShaderClass carries no separate depth-test-enable; the compare func governs
	// (PASS_ALWAYS == effectively no rejection), so the test stage is always on and
	// the write bit + compare come from the shader.
	Set_Depth_Test_Enable(true);
	Set_Depth_Write_Enable(s.Get_Depth_Mask() == ShaderClass::DEPTH_WRITE_ENABLE);
	Set_Depth_Func(Map_Depth_Cmp(s.Get_Depth_Compare()));

	// First-frame bring-up: cull NONE regardless of the shader's cull bit, so no
	// geometry disappears on a winding mismatch between the DX8 convention and the
	// D3D11 rasterizer. Correct per-shader culling is a later refinement.
	Set_Cull_Mode(RB_CULL_NONE);
	Set_Fill_Mode(RB_FILL_SOLID);

	// Texturing -> single-stage TEXTURE*DIFFUSE modulate (the game's default 2D /
	// prelit-diffuse combiner). Untextured shaders pass the vertex diffuse through.
	if (s.Get_Texturing() == ShaderClass::TEXTURING_ENABLE) {
		// When the unit-normal pipeline is active we borrow stage 1 for the normal
		// map. The sorting renderer re-applies textures at flush time up to the
		// shader's stage count, so a count of 1 would leave stage 1 unbound (the
		// pink neutral fallback) on units that have no native stage-1 detail layer.
		// Bumping the count to 2 makes the flush re-bind the normal map we pinned
		// in textures[1]; the custom pixel shader ignores the FF combiner result,
		// so enabling stage 1 only affects which SRV slot survives to draw time.
		// The unit-normal pipeline borrows stage 1 for the normal map. The stage
		// count here reads the backend sticky flag m_unitNormalIsActive. That flag
		// is (a) cleared to false at the top of every category
		// (dx8renderer.cpp:1692) so it is false at insert time for non-unit
		// categories, and (b) re-driven PER polygon by the sort flush
		// (sortingrenderer.cpp:378) via Set_Unit_Normal_Pixel_Shader(render_state.
		// UnitNormalEnable) just before this Set_Shader runs. That per-task re-drive
		// is what stops a stale "true" from one category leaking into terrain/
		// water/shroud and baking count 2 (which MODULATEs the category's stage-1
		// texture into colour -> the rainbow/black halo). The count is re-baked by
		// the insert-time Set_Shader re-issue in the unit branch.
		const unsigned int unitCount = (m_unitNormalIsActive ? 2u : 1u);
		Set_Texture_Stage_Count(unitCount);
		Set_Texture_Stage_ColorOp(0, RB_TEXOP_MODULATE, RB_TEXARG_TEXTURE, RB_TEXARG_DIFFUSE);
		Set_Texture_Stage_AlphaOp(0, RB_TEXOP_MODULATE, RB_TEXARG_TEXTURE, RB_TEXARG_DIFFUSE);
		Set_Texture_Stage_TexCoordIndex(0, 0);
		if (unitCount >= 2) {
			Set_Texture_Stage_ColorOp(1, RB_TEXOP_MODULATE, RB_TEXARG_TEXTURE, RB_TEXARG_DIFFUSE);
			Set_Texture_Stage_AlphaOp(1, RB_TEXOP_MODULATE, RB_TEXARG_TEXTURE, RB_TEXARG_DIFFUSE);
			Set_Texture_Stage_TexCoordIndex(1, 0);
		}
	} else {
		Set_Texture_Stage_Count(0);
	}
}

// ----------------------------------------------------------------------------
// VertexMaterialClass -> material constant buffer
// ----------------------------------------------------------------------------

void D3D11Backend::Set_Material(const VertexMaterialClass * material)
{
	// Mirror into the engine-side render_state record (see Set_Shader): without
	// it the sorting renderer's captured material is null and Flush_Sorting_Pool
	// null-derefs it (Get_Lighting) - part of the D3D11 world-entry crash.
	DX8Wrapper::Set_Material(material);

	if (material == nullptr) {
		Set_Lighting_Enable(false);
		return;
	}
	VertexMaterialClass * m = const_cast<VertexMaterialClass *>(material);

	Vector3 diffuse(1.0f, 1.0f, 1.0f);
	Vector3 ambient(0.0f, 0.0f, 0.0f);
	Vector3 emissive(0.0f, 0.0f, 0.0f);
	m->Get_Diffuse(&diffuse);
	m->Get_Ambient(&ambient);
	m->Get_Emissive(&emissive);
	const float opacity = m->Get_Opacity();

	const VertexMaterialClass::ColorSourceType ds = m->Get_Diffuse_Color_Source();
	const VertexMaterialClass::ColorSourceType as = m->Get_Ambient_Color_Source();
	const RenderBackendColorSource diffuse_src =
		(ds == VertexMaterialClass::MATERIAL) ? RB_MATSRC_MATERIAL : RB_MATSRC_VERTEX;
	const RenderBackendColorSource ambient_src =
		(as == VertexMaterialClass::MATERIAL) ? RB_MATSRC_MATERIAL : RB_MATSRC_VERTEX;

	Set_Material_Params(diffuse, ambient, emissive, opacity, diffuse_src, ambient_src);
	Set_Lighting_Enable(m->Get_Lighting());
}

// ----------------------------------------------------------------------------
// TextureBaseClass -> SRV upload
//
// Covered formats: DXT1-5 (native BC1/BC2/BC3 upload, no CPU decode), 32-bit
// A8/X8R8G8B8 and the 16-bit families (CPU-swizzled to RGBA8). Anything else
// binds the 4x4 magenta/black fallback checker so remaining gaps are visible
// on screen instead of sampling black through an unbound SRV.
// ----------------------------------------------------------------------------

void D3D11Backend::Set_Texture(unsigned int stage, TextureBaseClass * texture)
{
	// Mirror into the engine-side render_state record (see Set_Shader).
	DX8Wrapper::Set_Texture(stage, texture);

	if (stage >= RB_MAX_TEXTURE_STAGES) {
		return;
	}
	// Timing is opt-in (D3D11_PROFILE=1, see Profiling_Enabled() in
	// D3D11Backend.cpp/.h). This runs once per texture stage per draw call, so
	// by default we skip the QueryPerformanceCounter pair entirely rather than
	// pay it just to fill in a debug stat nobody is reading.
	const bool profiling = Profiling_Enabled();
	LARGE_INTEGER st0;
	if (profiling) { QueryPerformanceCounter(&st0); }
	m_boundTextures[stage] = texture;

	// Render-to-texture targets (water reflection / projected shadows): bind the
	// pre-made D3D11 SRV + sampler directly so the scene samples the real RT
	// instead of the (null) legacy DX8 surface. Early-out before the DX8 upload
	// path, whose base-texture lookup would otherwise bind neutral-white for
	// these POOL_DEFAULT render targets.
	if (texture != nullptr) {
		auto it = m_rtt.find(texture);
		if (it != m_rtt.end()) {
			m_stageNeutral[stage] = false;
			m_stageSRV[stage] = it->second.srv;
			m_stageSampler[stage] = it->second.sampler;
			m_context->PSSetShaderResources(stage, 1, &it->second.srv);
			m_context->PSSetSamplers(stage, 1, &it->second.sampler);
			return;
		}
	}

	if (texture == nullptr) {
		// A null bind must not leave the previous draw's SRV live in the slot:
		// the combiner still samples slot texture whenever the shader's
		// TEXTURING_ENABLE bit is set (Set_Shader programs MODULATE(TEXTURE,
		// DIFFUSE) with no null-texture branch), so a stale SRV renders the
		// WRONG texture. Bind the neutral-white fallback rather than a null
		// SRV: D3D8's observed no-texture combiner behavior is "sample white"
		// (multiplicative identity), while a D3D11 null SRV samples (0,0,0,0)
		// - black geometry instead of untextured-lit geometry.
		Bind_Neutral_Texture(stage); // memoized; cheap on repeat null binds
		return;
	}
	m_stageNeutral[stage] = false;

	// The per-draw texture bind reaches this backend straight from
	// MaterialPassClass (dx8renderer.cpp:1705 -> g_renderBackend->Set_Texture),
	// bypassing TextureClass::Apply - whose first act (texture.cpp:936-938) is
	// the lazy reload `if (!Initialized) Init();`. We therefore must do that
	// reload ourselves: a detail switch / device reset invalidates textures
	// (TextureBaseClass::Invalidate releases the D3D8 surface - texture.cpp:199
	// - leaving Peek_D3D_Base_Texture()==null) and without an Init() nothing
	// ever recreates the surface, so every bind would fall through to
	// Bind_Neutral_Texture (white halo) forever. Mirror Apply: repopulate the
	// D3D8 surface, then drop through to the normal upload path - the
	// generation bump (Set_D3D_Base_Texture/Poke_Texture) invalidates any stale
	// cache entry so Store_Cached_Texture refreshes it from the new bytes.
	IDirect3DBaseTexture8 * base = texture->Peek_D3D_Base_Texture();
	if (base == nullptr && !texture->Is_Initialized()) {
		texture->Init();
		base = texture->Peek_D3D_Base_Texture();
	}
	if (base == nullptr) {
		// Still null: surface not ready (background thumbnail load in flight),
		// or the texture genuinely has no bytes for this draw. Same stale-SRV
		// hazard as the null bind above - neutral-white (not the previous
		// draw's texture) is what this draw must sample.
		Bind_Neutral_Texture(stage);
		return;
	}
	if (base->GetType() != D3DRTYPE_TEXTURE) {
		return; // cube / volume textures are not on the menu path
	}
	IDirect3DTexture8 * tex = static_cast<IDirect3DTexture8 *>(base);

	D3DSURFACE_DESC desc;
	if (FAILED(tex->GetLevelDesc(0, &desc))) {
		return;
	}
	m_stageTexFormatLog[stage] = static_cast<unsigned int>(desc.Format);

	// Sampler mip policy resolved through the SAME mode tables the DX8 path
	// applies as D3DTSS_MIPFILTER (TextureFilterClass::Apply): the texture's
	// FilterType indexes _Init_Filters' tables, so the live texture-filter
	// OPTION participates - under the default TEXTURE_FILTER_BILINEAR even
	// FILTER_TYPE_BEST resolves to D3DTEXF_POINT, and MIP_LEVELS_1 alias
	// textures (terrain blend pass) resolve to D3DTEXF_NONE = level 0 only.
	// The previous blanket-trilinear sampler is what made D3D11 terrain
	// sample down the chain where DX8 (mip point / none) does not.
	RenderBackendMipFilter mipf = RB_MIPF_LINEAR;
	{
		TextureClass * tc = texture->As_TextureClass();
		if (tc != nullptr) {
			const unsigned d3dMip = TextureFilterClass::_Get_Resolved_Mip_Filter(
				tc->Get_Filter().Get_Mip_Mapping());
			mipf = (d3dMip == D3DTEXF_NONE)  ? RB_MIPF_NONE
			     : (d3dMip == D3DTEXF_POINT) ? RB_MIPF_POINT
			     :                             RB_MIPF_LINEAR;
		}
	}

	// Uploaded-texture cache - ON by default (W3DNEXT_D3D11_TEXCACHE=0 disables).
	//
	// Every path below LockRects the source surface and creates a fresh
	// ID3D11Texture2D + SRV + sampler, i.e. an upload on EVERY bind: measured
	// ~101k uploads per 600 frames (~170/frame), and D3D11 ran 12 fps where DX8
	// runs at its 30 cap on the same Debug build and scene.
	//
	// IDENTITY (why this is sound; the first revision was NOT and shipped off):
	//  - key     = TextureBaseClass::Get_ID(), a process-monotonic counter that is
	//              never recycled, so two distinct textures can never share a key.
	//              (Keying on the raw IDirect3DTexture8* aliased exactly here: the
	//              engine reallocates a freed texture at the same address and the
	//              stale entry was served as a hit -> striped-garbage world.)
	//  - version = Get_D3D_Generation(), bumped at EVERY mutation of the texture's
	//              D3DTexture pointer (reload, mip reduction, thumbnail->full,
	//              invalidate, destruction), so an entry cannot outlive its bytes.
	//  - evicted from ~TextureBaseClass, but only as hygiene: because ids are never
	//              reused, a MISSED eviction leaks, it can never alias.
	//
	// CACHEABILITY: POOL_MANAGED textures whose pixels change only through a
	// D3DTexture swap (which the generation catches). Procedural textures are
	// excluded UNLESS they declare Is_Procedural_Cacheable() - the create-once
	// terrain/tree composites (uploadprof convicted them at 33.6 of 34.6
	// MB/frame of per-bind re-upload = ~40 ms/frame CPU, 2026-07-26; their
	// bytes are written only between creation and first bind, and every
	// refresh path recreates the instance under a fresh never-reused id).
	// Still excluded and re-uploading per bind: procedural textures mutated
	// in place mid-life (video buffers, radar), POOL_DEFAULT render targets,
	// and the GPU-composed fog-of-war shroud dst that _Copy_DX8_Rects
	// rewrites every frame.
	// W3DNEXT_D3D11_TEXCACHE=0 disables outright; W3DNEXT_D3D11_TEXCACHE_PROC=0 reverts
	// procedural textures to per-bind upload (staleness kill switch);
	// W3DNEXT_D3D11_TEXCACHE_TOGGLE=1 alternates per flip frame so one process can
	// dump a cache-ON/cache-OFF pair one frame apart (the two-process A/B is
	// swamped by run-to-run divergence - see the m_texCacheToggleMode comment
	// in D3D11Backend.h).
	static const bool s_procCacheEnabled = [] {
		const char * e = W3DNext_GetEnv("D3D11_TEXCACHE_PROC");
		return !(e != nullptr && e[0] == '0');
	}();
	const bool cacheable = Tex_Cache_Enabled_This_Frame() &&
		texture->Get_Pool() == TextureBaseClass::POOL_MANAGED &&
		(!texture->Is_Procedural() ||
			(s_procCacheEnabled && texture->Is_Procedural_Cacheable()));
	const unsigned int cache_key = texture->Get_ID();
	const unsigned long long content_version = texture->Get_D3D_Generation();
	if (cacheable && Bind_Cached_Texture(stage, cache_key, content_version)) {
		if (profiling) {
			LARGE_INTEGER st1;
			QueryPerformanceCounter(&st1);
			m_dbgSetTexMs += static_cast<double>(st1.QuadPart - st0.QuadPart) /
				static_cast<double>(m_qpcFreq.QuadPart) * 1000.0;
		}
		return;
	}

	// Everything past this point is an actual upload; account it (count/bytes/
	// wall-ms, W3DNEXT_D3D11_GPUPROF-gated). A cacheable texture landing here is a
	// one-time "miss"; a non-cacheable one lands here on EVERY bind ("nc").
	const long long up_t0 = Upload_Prof_Now();
	const unsigned int up_cat = cacheable ? RB_UPLOAD_MISS : RB_UPLOAD_NC;

	// Compressed (DXT) surfaces - the dominant world-texture format (terrain,
	// units, trees) - upload NATIVELY as D3D11 BC1/BC2/BC3: LockRect on a
	// managed-pool DXT surface returns the raw compressed blocks (pBits) with
	// Pitch = bytes per BLOCK row, which is exactly what Upload_Texture_BC's
	// SysMemPitch wants. No CPU decode, lossless. DXT2/DXT4 are the premultiplied-
	// alpha variants of DXT3/DXT5 - same block layout, same BC2/BC3 upload.
	const bool isDXT = (desc.Format == D3DFMT_DXT1 || desc.Format == D3DFMT_DXT2 ||
		desc.Format == D3DFMT_DXT3 || desc.Format == D3DFMT_DXT4 ||
		desc.Format == D3DFMT_DXT5);
	// Honor the texture's own U/V address mode on EVERY upload path: clamp
	// textures (shadow decals, shroud, UI) must not tile. World-aligned shadow
	// decal UVs intentionally exceed [0,1] and relied on the DX8 texture's
	// TEXTURE_ADDRESS_CLAMP - blanket WRAP repeated the blob as quarter copies
	// around every shadow quad.
	bool wrapUV = true;
	{
		TextureClass * tcc = texture->As_TextureClass();
		if (tcc != nullptr
			&& (tcc->Get_Filter().Get_U_Addr_Mode() == TextureFilterClass::TEXTURE_ADDRESS_CLAMP
				|| tcc->Get_Filter().Get_V_Addr_Mode() == TextureFilterClass::TEXTURE_ADDRESS_CLAMP)) {
			wrapUV = false;
		}
	}
	if (isDXT) {
		bool uploaded = false;
		// D3D11 requires block-aligned (multiple-of-4, >= 4) top-level dimensions
		// for BC resources; game DXT textures are power-of-2 so this always holds -
		// anything smaller falls through to the visible fallback below.
		if (desc.Width >= 4 && desc.Height >= 4 &&
			(desc.Width % 4) == 0 && (desc.Height % 4) == 0) {
			D3DLOCKED_RECT lr;
			if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) {
				const RenderBackendBCFormat bc =
					(desc.Format == D3DFMT_DXT1) ? RB_BC1 :
					(desc.Format == D3DFMT_DXT2 || desc.Format == D3DFMT_DXT3) ? RB_BC2 : RB_BC3;
				if (Draw_Log_Enabled()) {
					// DXT1 alpha recon: a block encodes punch-through (alpha 0) texels
					// only in the color0<=color1 mode with an index of 3. Exact for
					// aMin; DXT3/5 are left to the uncompressed/endpoint paths.
					if (desc.Format == D3DFMT_DXT1) {
						unsigned int aMin = 255;
						const unsigned char * bytes = static_cast<const unsigned char *>(lr.pBits);
						for (unsigned int by = 0; by < desc.Height / 4 && aMin == 255; ++by) {
							const unsigned char * row = bytes + static_cast<size_t>(by) * lr.Pitch;
							for (unsigned int bx = 0; bx < desc.Width / 4; ++bx) {
								const unsigned char * blk = row + bx * 8;
								const unsigned short c0 = static_cast<unsigned short>(blk[0] | (blk[1] << 8));
								const unsigned short c1 = static_cast<unsigned short>(blk[2] | (blk[3] << 8));
								if (c0 <= c1) {
									const unsigned int idx = static_cast<unsigned int>(blk[4]) | (blk[5] << 8) | (blk[6] << 16) | (static_cast<unsigned int>(blk[7]) << 24);
									for (unsigned int t = 0; t < 16; ++t) {
										if (((idx >> (t * 2)) & 3u) == 3u) { aMin = 0; break; }
									}
									if (aMin == 0) break;
								}
							}
						}
						Trace_Texture_Alpha(static_cast<const char *>(texture->Get_Texture_Name()),
							desc.Format, desc.Width, desc.Height, aMin, 255);
					}
				}
				// Upload the FULL mip chain, not just level 0. The source surface
				// already carries one; taking only level 0 is what made D3D11
				// terrain render sharp and aliased where DX8 is smooth, and made
				// minified sampling cache-hostile (parity log, 2026-07-26). Levels
				// 1..N-1 lock alongside the already-held level 0 - LockRect is
				// per-level - and all of them stay locked until CreateTexture2D
				// has copied them, because an IMMUTABLE resource reads every
				// subresource pointer during creation.
				RenderBackendMipLevel mips[RB_MAX_MIP_LEVELS];
				D3DLOCKED_RECT extra[RB_MAX_MIP_LEVELS];
				mips[0].data = lr.pBits;
				mips[0].width = desc.Width;
				mips[0].height = desc.Height;
				mips[0].row_pitch = static_cast<unsigned int>(lr.Pitch);
				unsigned int nlev = 1;
				const unsigned int have_levels = Mip_Upload_Enabled() ? tex->GetLevelCount() : 1u;
				const unsigned int want_levels =
					(have_levels < RB_MAX_MIP_LEVELS) ? have_levels : RB_MAX_MIP_LEVELS;
				for (unsigned int i = 1; i < want_levels; ++i) {
					D3DSURFACE_DESC ld;
					if (FAILED(tex->GetLevelDesc(i, &ld))) break;
					if (FAILED(tex->LockRect(i, &extra[i], nullptr, D3DLOCK_READONLY))) break;
					mips[i].data = extra[i].pBits;
					mips[i].width = ld.Width;
					mips[i].height = ld.Height;
					mips[i].row_pitch = static_cast<unsigned int>(extra[i].Pitch);
					nlev = i + 1;
				}
				uploaded = Upload_Texture_BC_Mips(stage, bc, mips, nlev, wrapUV, /*linear*/true, mipf);
				for (unsigned int i = nlev; i-- > 1; ) {
					tex->UnlockRect(i);
				}
				tex->UnlockRect(0);
				if (uploaded && cacheable) {
					Store_Cached_Texture(stage, cache_key, content_version);
				}
				if (uploaded) {
					// BC bytes: row_pitch is per BLOCK row, one block row covers 4 texel rows.
					unsigned long long up_bytes = 0;
					for (unsigned int i = 0; i < nlev; ++i) {
						up_bytes += static_cast<unsigned long long>(mips[i].row_pitch) * ((mips[i].height + 3u) / 4u);
					}
					Upload_Prof_Account(up_cat, up_bytes, up_t0);
					if (up_t0 != 0 && up_cat == RB_UPLOAD_NC) {
						Upload_Prof_Note_NC(texture, desc.Width, desc.Height, desc.Format, up_bytes);
					}
				}
			}
		}
		if (!uploaded) {
			// Known format, but the bytes were unreadable (default-pool surface) or
			// the dims are non-block-aligned: bind the neutral WHITE identity so the
			// draw degrades to a no-op instead of corrupting the pass. Logged.
			Trace_Texture_Fallback(desc.Format, desc.Width, desc.Height, "DXT lock/upload failed -> neutral white");
			if (stage == 1) { FILE * fl = fopen("unitnorm.log", "a"); if (fl) { fprintf(fl, "STG1 NEUTRAL bc-fail fmt=%u\n", (unsigned)desc.Format); fclose(fl); } }
			Bind_Neutral_Texture(stage);
			Upload_Prof_Account(RB_UPLOAD_FALLBACK, 64, up_t0);
		}
		return;
	}

	// Decode the common WW3D uncompressed surface formats: 32-bit A8/X8 R8G8B8 and
	// the 16-bit families (A4R4G4B4, R5G6B5, X1/A1 R5G5B5). Any OTHER format (bump
	// maps, luminance, palettized...) binds the loud magenta fallback - a
	// wrong-but-visible surface beats the silent black an unbound SRV samples
	// through the MODULATE combiner, and makes the remaining gaps self-announcing.
	const bool is32 = (desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8);
	const bool is16 = (desc.Format == D3DFMT_A4R4G4B4 || desc.Format == D3DFMT_R5G6B5 ||
		desc.Format == D3DFMT_X1R5G5B5 || desc.Format == D3DFMT_A1R5G5B5);
	if (!is32 && !is16) {
		{
			static int s_fbLog = 0;
			if (s_fbLog < 120) {
				const char * nm = "(unknown)";
				if (texture != nullptr) nm = (const char *)texture->Get_Texture_Name();
				FILE * dbg = fopen("unitnorm.log", "a");
				if (dbg) { fprintf(dbg, "FALLBACK stage=%u fmt=%u w=%u h=%u tex=%s\n",
					stage, (unsigned)desc.Format, (unsigned)desc.Width, (unsigned)desc.Height, nm); fclose(dbg); }
				s_fbLog++;
			}
		}
		Trace_Texture_Fallback(desc.Format, desc.Width, desc.Height, "unhandled format");
		Upload_Fallback_Texture(stage);
		Upload_Prof_Account(RB_UPLOAD_FALLBACK, 64, up_t0);
		return;
	}

	D3DLOCKED_RECT lr;
	if (FAILED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) {
		// Known format, unreadable bytes: the default-pool / GPU-only case (the
		// fog-of-war shroud dst texture, render targets). Every engine write to
		// such a surface goes through DX8Wrapper::_Copy_DX8_Rects, which mirrors
		// the bytes into the CPU copy-shadow - upload from that when it exists.
		if (Upload_From_Copy_Shadow(this, stage, tex, desc)) {
			// Deliberately NOT cached: these are the GPU-composed default-pool
			// surfaces (fog-of-war shroud dst) that _Copy_DX8_Rects rewrites in
			// place, which no D3DTexture-generation bump would catch.
			// Bytes approximated from the level-0 desc (the shadow is RGBA8 of
			// the surface's own dims, which match desc for these textures).
			Upload_Prof_Account(RB_UPLOAD_SHADOW,
				static_cast<unsigned long long>(desc.Width) * desc.Height * 4u, up_t0);
			return;
		}
		// No shadow (render targets, pre-first-copy binds): the neutral WHITE
		// identity renders the multiplying shroud pass as a no-op rather than
		// painting the whole terrain magenta/black.
		Trace_Texture_Fallback(desc.Format, desc.Width, desc.Height, "LockRect failed -> neutral white");
		if (stage == 1) { FILE * fl = fopen("unitnorm.log", "a"); if (fl) { fprintf(fl, "STG1 NEUTRAL lockrect fmt=%u\n", (unsigned)desc.Format); fclose(fl); } }
		Bind_Neutral_Texture(stage);
		Upload_Prof_Account(RB_UPLOAD_FALLBACK, 64, up_t0);
		return;
	}

	const unsigned int w = desc.Width;
	const unsigned int h = desc.Height;
	unsigned char * rgba = new (std::nothrow) unsigned char[static_cast<size_t>(w) * h * 4];
	if (rgba != nullptr) {
		Decode_Surface_To_RGBA(desc.Format, w, h,
			static_cast<const unsigned char *>(lr.pBits),
			static_cast<unsigned int>(lr.Pitch), rgba);
		tex->UnlockRect(0);
		if (Draw_Log_Enabled()) {
			unsigned int aMin = 255, aMax = 0;
			const size_t n = static_cast<size_t>(w) * h;
			for (size_t i = 0; i < n; ++i) {
				const unsigned int a = rgba[i * 4 + 3];
				if (a < aMin) aMin = a;
				if (a > aMax) aMax = a;
			}
			Trace_Texture_Alpha(static_cast<const char *>(texture->Get_Texture_Name()),
				desc.Format, w, h, aMin, aMax);
		}
		// Decode the rest of the source mip chain, same reason as the DXT path
		// above. Unlike that path these are OUR buffers rather than the surface's
		// own bytes, so each level unlocks as soon as it is decoded and only the
		// decoded copies need to outlive the loop.
		unsigned char * mipbuf[RB_MAX_MIP_LEVELS];
		RenderBackendMipLevel mips[RB_MAX_MIP_LEVELS];
		for (unsigned int i = 0; i < RB_MAX_MIP_LEVELS; ++i) {
			mipbuf[i] = nullptr;
		}
		mips[0].data = rgba;
		mips[0].width = w;
		mips[0].height = h;
		mips[0].row_pitch = w * 4;
		unsigned int nlev = 1;
		const unsigned int have_levels = Mip_Upload_Enabled() ? tex->GetLevelCount() : 1u;
		const unsigned int want_levels =
			(have_levels < RB_MAX_MIP_LEVELS) ? have_levels : RB_MAX_MIP_LEVELS;
		for (unsigned int i = 1; i < want_levels; ++i) {
			D3DSURFACE_DESC ld;
			if (FAILED(tex->GetLevelDesc(i, &ld))) break;
			D3DLOCKED_RECT llr;
			if (FAILED(tex->LockRect(i, &llr, nullptr, D3DLOCK_READONLY))) break;
			unsigned char * buf =
				new (std::nothrow) unsigned char[static_cast<size_t>(ld.Width) * ld.Height * 4];
			if (buf == nullptr) {
				tex->UnlockRect(i);
				break; // out of memory: ship the levels gathered so far
			}
			Decode_Surface_To_RGBA(ld.Format, ld.Width, ld.Height,
				static_cast<const unsigned char *>(llr.pBits),
				static_cast<unsigned int>(llr.Pitch), buf);
			tex->UnlockRect(i);
			mipbuf[i] = buf;
			mips[i].data = buf;
			mips[i].width = ld.Width;
			mips[i].height = ld.Height;
			mips[i].row_pitch = ld.Width * 4;
			nlev = i + 1;
		}
		if (Upload_Texture_RGBA_Mips(stage, mips, nlev, wrapUV, /*linear*/true, mipf) && cacheable) {
			Store_Cached_Texture(stage, cache_key, content_version);
		}
		{
			unsigned long long up_bytes = 0;
			for (unsigned int i = 0; i < nlev; ++i) {
				up_bytes += static_cast<unsigned long long>(mips[i].row_pitch) * mips[i].height;
			}
			Upload_Prof_Account(up_cat, up_bytes, up_t0);
			if (up_t0 != 0 && up_cat == RB_UPLOAD_NC) {
				Upload_Prof_Note_NC(texture, w, h, desc.Format, up_bytes);
			}
		}
		for (unsigned int i = 1; i < RB_MAX_MIP_LEVELS; ++i) {
			delete[] mipbuf[i];
		}
		delete[] rgba;
	} else {
		tex->UnlockRect(0);
	}
}

// ----------------------------------------------------------------------------
// Fixed-function texgen mirror hooks (declared in dx8wrapper.h)
//
// The terrain shroud/cloud shaders drive texgen through DX8Wrapper's inline
// funnels: Set_DX8_Texture_Stage_State(D3DTSS_TEXCOORDINDEX /
// D3DTSS_TEXTURETRANSFORMFLAGS) and _Set_DX8_Transform(D3DTS_TEXTUREn). On the
// DX8 backend those land in the fixed-function pipeline directly; the D3D11
// combiner has no fixed-function pipeline, so these hooks forward the same
// state into its cbTexGen slice. Self-gated: single active-backend check, no
// work at all while DX8Backend is drawing.
// ----------------------------------------------------------------------------

void RB_Mirror_Texgen_Stage_State(unsigned stage, unsigned state, unsigned value)
{
	if (!Is_D3D11_Backend_Active()) {
		return;
	}
	D3D11Backend * backend = static_cast<D3D11Backend *>(g_renderBackend);
	if (state == D3DTSS_TEXCOORDINDEX) {
		// High 16 bits carry the D3DTSS_TCI_* generation mode; low bits the
		// explicit texcoord-set index. Only CAMERASPACEPOSITION is generated
		// (the mode the engine's shroud/cloud passes use); the other TCI modes
		// fall back to explicit coords, the pre-texgen behavior.
		const unsigned tci_mode = value & 0xFFFF0000u;
		backend->Set_Texture_Stage_Texgen_CameraSpace(stage, tci_mode == D3DTSS_TCI_CAMERASPACEPOSITION);
		if (tci_mode == 0u) {
			backend->Set_Texture_Stage_TexCoordIndex(stage, value & 0xFFFFu);
		}
	} else if (state == D3DTSS_TEXTURETRANSFORMFLAGS) {
		// Low byte is the D3DTTFF_COUNTn value (0 = DISABLE); D3DTTFF_PROJECTED
		// lives at 0x100 and is not used by the game's texgen passes.
		backend->Set_Texture_Transform_Enable(stage, (value & 0xFFu) != D3DTTFF_DISABLE);
	}
}

void RB_Mirror_Grayscale2D(bool enable)
{
	if (!Is_D3D11_Backend_Active()) {
		return;
	}
	static_cast<D3D11Backend *>(g_renderBackend)->Set_Grayscale_Override(enable);
}

// TerrainShader2Stage's per-pass device pokes, translated into the typed
// combiner + blend state (see the dx8wrapper.h declaration for why). The
// TEXCOORDINDEX / texture-matrix half of each pass already arrives through the
// texgen mirror; this covers the COLOROP/ALPHAOP and ALPHABLENDENABLE/
// SRCBLEND/DESTBLEND half. Depth state is deliberately untouched - the DX8
// path leaves it to the surrounding Set_Shader, and so does this.
void RB_Mirror_Tree_Sway(bool enable, const float * vec4s, unsigned int count)
{
	if (!Is_D3D11_Backend_Active()) {
		return;
	}
	static_cast<D3D11Backend *>(g_renderBackend)->Set_Tree_Sway(enable, vec4s, count);
}

void RB_Mirror_Terrain_FF_Pass(int pass, int noise_stages)
{
	if (!Is_D3D11_Backend_Active()) {
		return;
	}
	D3D11Backend * backend = static_cast<D3D11Backend *>(g_renderBackend);
	switch (pass) {
	case 0: // base tiles: opaque TEXTURE*DIFFUSE via UV set 0
		backend->Set_Texture_Stage_Count(1);
		backend->Set_Texture_Stage_ColorOp(0, RB_TEXOP_MODULATE, RB_TEXARG_TEXTURE, RB_TEXARG_DIFFUSE);
		backend->Set_Texture_Stage_AlphaOp(0, RB_TEXOP_SELECTARG2, RB_TEXARG_TEXTURE, RB_TEXARG_DIFFUSE);
		backend->Set_Blend_Enable(false);
		break;
	case 1: // blend tiles: same color math via UV set 1; alpha = the atlas's
	        // 1-bit blend shape (bilinear-feathered) * the per-vertex blend
	        // alpha, lerped into the base pass by the framebuffer blend.
		backend->Set_Texture_Stage_Count(1);
		backend->Set_Texture_Stage_ColorOp(0, RB_TEXOP_MODULATE, RB_TEXARG_TEXTURE, RB_TEXARG_DIFFUSE);
		backend->Set_Texture_Stage_AlphaOp(0, RB_TEXOP_MODULATE, RB_TEXARG_TEXTURE, RB_TEXARG_DIFFUSE);
		backend->Set_Blend_Enable(true);
		backend->Set_Blend_Func(RB_BLEND_SRCALPHA, RB_BLEND_INVSRCALPHA);
		backend->Set_Blend_Op(RB_BLENDOP_ADD);
		break;
	case 2: // cloud/noise: texture-only color multiplied onto the frame
	        // (DESTCOLOR/ZERO); NOISE12 modulates the second texture in.
		backend->Set_Texture_Stage_Count(noise_stages >= 2 ? 2 : 1);
		backend->Set_Texture_Stage_ColorOp(0, RB_TEXOP_SELECTARG1, RB_TEXARG_TEXTURE, RB_TEXARG_DIFFUSE);
		backend->Set_Texture_Stage_AlphaOp(0, RB_TEXOP_SELECTARG2, RB_TEXARG_TEXTURE, RB_TEXARG_DIFFUSE);
		if (noise_stages >= 2) {
			backend->Set_Texture_Stage_ColorOp(1, RB_TEXOP_MODULATE, RB_TEXARG_TEXTURE, RB_TEXARG_CURRENT);
			backend->Set_Texture_Stage_AlphaOp(1, RB_TEXOP_SELECTARG2, RB_TEXARG_TEXTURE, RB_TEXARG_CURRENT);
		}
		backend->Set_Blend_Enable(true);
		backend->Set_Blend_Func(RB_BLEND_DESTCOLOR, RB_BLEND_ZERO);
		backend->Set_Blend_Op(RB_BLENDOP_ADD);
		break;
	}
}

// RoadShader2Stage's pass-0 device pokes, translated into the typed combiner +
// blend state (see the dx8wrapper.h declaration). Stage 0 modulates the road
// texture with vertex diffuse (color AND alpha - the alpha carries the strip's
// feathered edge); the optional noise stage modulates the cloud/lightmap in;
// the framebuffer blend lerps the strip onto the terrain by that alpha.
// NOISE12's second pass (BLENDCURRENTALPHA + ALPHAREPLICATE) is not
// expressible in the combiner emulation and is skipped under D3D11 (pass count
// forced to 1 in RoadShader2Stage::init) - ledgered in the parity log.
void RB_Mirror_Road_FF_Pass(int noise_stage_active)
{
	if (!Is_D3D11_Backend_Active()) {
		return;
	}
	D3D11Backend * backend = static_cast<D3D11Backend *>(g_renderBackend);
	backend->Set_Texture_Stage_Count(noise_stage_active ? 2 : 1);
	backend->Set_Texture_Stage_ColorOp(0, RB_TEXOP_MODULATE, RB_TEXARG_TEXTURE, RB_TEXARG_DIFFUSE);
	backend->Set_Texture_Stage_AlphaOp(0, RB_TEXOP_MODULATE, RB_TEXARG_TEXTURE, RB_TEXARG_DIFFUSE);
	if (noise_stage_active) {
		backend->Set_Texture_Stage_ColorOp(1, RB_TEXOP_MODULATE, RB_TEXARG_TEXTURE, RB_TEXARG_CURRENT);
		backend->Set_Texture_Stage_AlphaOp(1, RB_TEXOP_MODULATE, RB_TEXARG_TEXTURE, RB_TEXARG_CURRENT);
	}
	backend->Set_Blend_Enable(true);
	backend->Set_Blend_Func(RB_BLEND_SRCALPHA, RB_BLEND_INVSRCALPHA);
	backend->Set_Blend_Op(RB_BLENDOP_ADD);
}

// Backend -> wrapper record (declared in D3D11Backend.h): keep DX8Wrapper's
// render_state.world/view true while the D3D11 backend owns the transforms, so
// SortingRendererClass::Insert_Triangles captures real matrices instead of
// stale ones. The wrapper's WORLD/VIEW cases only write the record + dirty
// flags - no D3D8 device call - so this is safe on any pipeline state.
void Mirror_Transform_To_Wrapper(TransformKind transform, const Matrix4x4 & m)
{
	if (transform == RB_TRANSFORM_WORLD) {
		DX8Wrapper::Set_Transform(D3DTS_WORLD, m);
	} else if (transform == RB_TRANSFORM_VIEW) {
		DX8Wrapper::Set_Transform(D3DTS_VIEW, m);
	}
}

// Wrapper -> backend (declared in dx8wrapper.h): the flush half of the sorted-
// translucent fix. SortingRendererClass's Apply_Render_State re-applies each
// node's captured WORLD/VIEW through DX8Wrapper::_Set_DX8_Transform, which on
// the DX8 path talks straight to the device; the D3D11 backend never saw those
// sets, so every sorted draw (particles, rocket sprites, rotor blur discs) ran
// with whatever transforms the last opaque object left bound - geometry
// rasterized anywhere-but-right, i.e. invisible. W3DWater's raw sets ride the
// same funnel. Self-gated no-op while the DX8 backend is drawing.
void RB_Mirror_World_View_Transform(unsigned transform, const D3DMATRIX & m)
{
	if (!Is_D3D11_Backend_Active()) {
		return;
	}
	// Device row-vector layout -> WWMath column-vector Matrix4x4 is the same
	// transpose RB_Get_Backend_Transform performs on the read side.
	Matrix4x4 ww(true);
	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			ww[j][i] = m.m[i][j];
		}
	}
	g_renderBackend->Set_Transform(
		transform == D3DTS_WORLD ? RB_TRANSFORM_WORLD : RB_TRANSFORM_VIEW, ww);
}

void RB_Mirror_Texture_Transform(unsigned stage, const D3DMATRIX & m)
{
	if (!Is_D3D11_Backend_Active()) {
		return;
	}
	// D3DMATRIX is 16 contiguous floats, row-major, D3D's row-vector convention -
	// exactly the layout cbTexGen stores and the shader consumes (mul(v, M)), so
	// the bytes pass through verbatim.
	static_cast<D3D11Backend *>(g_renderBackend)->Set_Texture_Transform_Matrix(stage, &m.m[0][0]);
}

bool RB_Get_Backend_Transform(D3DTRANSFORMSTATETYPE transform, D3DMATRIX & m)
{
	if (!Is_D3D11_Backend_Active()) {
		return false;
	}
	TransformKind kind;
	switch (transform) {
	case D3DTS_WORLD:      kind = RB_TRANSFORM_WORLD; break;
	case D3DTS_VIEW:       kind = RB_TRANSFORM_VIEW; break;
	case D3DTS_PROJECTION: kind = RB_TRANSFORM_PROJECTION; break;
	default:               return false; // texture transforms: nobody reads them back
	}
	Matrix4x4 ww(true);
	g_renderBackend->Get_Transform(kind, ww);
	// WWMath column-vector -> device row-vector layout is a transpose, the same
	// conversion To_D3DMATRIX performs on the set side.
	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			m.m[i][j] = ww[j][i];
		}
	}
	return true;
}
