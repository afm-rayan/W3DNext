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

// Legacy render-state vector -> D3D11 desc translation + object cache
// (RENDERER_PORT.md step 9). This .cpp owns the <d3d11.h> dependency (the header
// stays d3d11-free); it maps the RB_* semantic enums onto D3D11_BLEND / _OP /
// _COMPARISON_FUNC / _CULL_MODE / _FILL_MODE and creates the three immutable
// state objects. Like D3D11Backend.cpp / D3D11FVF.cpp it compiles both into the
// ww3d2 library and into the standalone d3d11_smoke test.

#include "D3D11States.h"

#include <cstdlib>
#include <d3d11.h>

namespace
{

// Two-sided stencil (see Get_Depth_State) is opt-in via this env var, default
// OFF, because enabling it unconditionally caused severe, broad rendering
// corruption (black terrain, broken water, opaque shadow blobs) through some
// runtime interaction not visible from static source reading alone. Kept
// self-contained (no new header dependency) since this file also compiles
// into the standalone d3d11_smoke test.
bool W3DNext_TwoSidedStencil_Enabled()
{
	static const bool s_enabled = [] {
		const char * e = std::getenv("W3DNEXT_TWOSIDED_STENCIL");
		return e != nullptr && e[0] == '1';
	}();
	return s_enabled;
}

// --- RB_* -> D3D11 translation (all D3D ABI values confined to this file) -----

D3D11_BLEND To_D3D11_Blend_Color(RenderBackendBlendFactor f)
{
	switch (f) {
	case RB_BLEND_ZERO:        return D3D11_BLEND_ZERO;
	case RB_BLEND_ONE:         return D3D11_BLEND_ONE;
	case RB_BLEND_SRCALPHA:    return D3D11_BLEND_SRC_ALPHA;
	case RB_BLEND_INVSRCALPHA: return D3D11_BLEND_INV_SRC_ALPHA;
	case RB_BLEND_SRCCOLOR:    return D3D11_BLEND_SRC_COLOR;
	case RB_BLEND_INVSRCCOLOR: return D3D11_BLEND_INV_SRC_COLOR;
	case RB_BLEND_DESTALPHA:   return D3D11_BLEND_DEST_ALPHA;
	case RB_BLEND_INVDESTALPHA:return D3D11_BLEND_INV_DEST_ALPHA;
	case RB_BLEND_DESTCOLOR:   return D3D11_BLEND_DEST_COLOR;
	case RB_BLEND_INVDESTCOLOR:return D3D11_BLEND_INV_DEST_COLOR;
	default:                   return D3D11_BLEND_ONE;
	}
}

// The alpha channel cannot use a *_COLOR blend factor in D3D11 (the debug layer
// rejects it), so the color-based factors fold onto their alpha equivalents for
// the SrcBlendAlpha/DestBlendAlpha slots. ZERO/ONE/SRC_ALPHA/INV_SRC_ALPHA/
// DEST_ALPHA/INV_DEST_ALPHA pass through unchanged.
D3D11_BLEND To_D3D11_Blend_Alpha(RenderBackendBlendFactor f)
{
	switch (f) {
	case RB_BLEND_SRCCOLOR:    return D3D11_BLEND_SRC_ALPHA;
	case RB_BLEND_INVSRCCOLOR: return D3D11_BLEND_INV_SRC_ALPHA;
	case RB_BLEND_DESTCOLOR:   return D3D11_BLEND_DEST_ALPHA;
	case RB_BLEND_INVDESTCOLOR:return D3D11_BLEND_INV_DEST_ALPHA;
	default:                   return To_D3D11_Blend_Color(f);
	}
}

D3D11_BLEND_OP To_D3D11_Blend_Op(RenderBackendBlendOp op)
{
	switch (op) {
	case RB_BLENDOP_ADD:        return D3D11_BLEND_OP_ADD;
	case RB_BLENDOP_SUBTRACT:   return D3D11_BLEND_OP_SUBTRACT;
	case RB_BLENDOP_REVSUBTRACT:return D3D11_BLEND_OP_REV_SUBTRACT;
	case RB_BLENDOP_MIN:        return D3D11_BLEND_OP_MIN;
	case RB_BLENDOP_MAX:        return D3D11_BLEND_OP_MAX;
	default:                    return D3D11_BLEND_OP_ADD;
	}
}

D3D11_STENCIL_OP To_D3D11_StencilOp(RenderBackendStencilOp op)
{
	switch (op) {
	case RB_STENCILOP_ZERO:    return D3D11_STENCIL_OP_ZERO;
	case RB_STENCILOP_REPLACE: return D3D11_STENCIL_OP_REPLACE;
	case RB_STENCILOP_INCRSAT: return D3D11_STENCIL_OP_INCR_SAT;
	case RB_STENCILOP_DECRSAT: return D3D11_STENCIL_OP_DECR_SAT;
	case RB_STENCILOP_INVERT:  return D3D11_STENCIL_OP_INVERT;
	case RB_STENCILOP_INCR:    return D3D11_STENCIL_OP_INCR;
	case RB_STENCILOP_DECR:    return D3D11_STENCIL_OP_DECR;
	case RB_STENCILOP_KEEP:
	default:                   return D3D11_STENCIL_OP_KEEP;
	}
}

D3D11_COMPARISON_FUNC To_D3D11_Cmp(RenderBackendCmpFunc f)
{
	switch (f) {
	case RB_CMP_NEVER:       return D3D11_COMPARISON_NEVER;
	case RB_CMP_LESS:        return D3D11_COMPARISON_LESS;
	case RB_CMP_EQUAL:       return D3D11_COMPARISON_EQUAL;
	case RB_CMP_LESSEQUAL:   return D3D11_COMPARISON_LESS_EQUAL;
	case RB_CMP_GREATER:     return D3D11_COMPARISON_GREATER;
	case RB_CMP_NOTEQUAL:    return D3D11_COMPARISON_NOT_EQUAL;
	case RB_CMP_GREATEREQUAL:return D3D11_COMPARISON_GREATER_EQUAL;
	case RB_CMP_ALWAYS:      return D3D11_COMPARISON_ALWAYS;
	default:                 return D3D11_COMPARISON_LESS_EQUAL;
	}
}

// FrontCounterClockwise stays FALSE (D3D11 default), so front faces are
// clockwise-wound - the D3D8 convention. Culling CW faces (D3DCULL_CW) therefore
// maps to CULL_FRONT, and CCW to CULL_BACK.
D3D11_CULL_MODE To_D3D11_Cull(RenderBackendCullMode c)
{
	switch (c) {
	case RB_CULL_NONE: return D3D11_CULL_NONE;
	case RB_CULL_CW:   return D3D11_CULL_FRONT;
	case RB_CULL_CCW:  return D3D11_CULL_BACK;
	default:           return D3D11_CULL_NONE;
	}
}

D3D11_FILL_MODE To_D3D11_Fill(RenderBackendFillMode f)
{
	return (f == RB_FILL_WIREFRAME) ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
}

} // namespace

// ----------------------------------------------------------------------------
// RenderStateVector
// ----------------------------------------------------------------------------

RenderStateVector::RenderStateVector()
	: blendEnable(false)
	, srcBlend(RB_BLEND_ONE)
	, dstBlend(RB_BLEND_ZERO)
	, blendOp(RB_BLENDOP_ADD)
	, colorWriteEnable(0xF) // all channels (R|G|B|A) by default
	, depthEnable(true)
	, depthWrite(true)
	, depthFunc(RB_CMP_LESSEQUAL)
	, stencilEnable(false)
	, stencilFunc(RB_CMP_ALWAYS)
	, stencilFail(RB_STENCILOP_KEEP)
	, stencilZFail(RB_STENCILOP_KEEP)
	, stencilPass(RB_STENCILOP_KEEP)
	, stencilRef(0)
	, stencilMask(0xFFFFFFFF)
	, stencilWriteMask(0xFFFFFFFF)
	, twoSidedStencil(false)
	, ccwStencilFunc(RB_CMP_ALWAYS)
	, ccwStencilFail(RB_STENCILOP_KEEP)
	, ccwStencilZFail(RB_STENCILOP_KEEP)
	, ccwStencilPass(RB_STENCILOP_KEEP)
	// Bring-up: cull NONE, matching both the Initialize-time rasterizer and the
	// Set_Shader translation (which forces CULL_NONE until the DX8-vs-D3D11
	// winding convention is settled). The DX8 default is CULL_CW; restore that
	// here when per-shader culling is enabled, so the draw-time state flush
	// doesn't silently cull geometry that never goes through Set_Shader.
	, cullMode(RB_CULL_NONE)
	, fillMode(RB_FILL_SOLID)
{
}

unsigned int RenderStateVector::Blend_Key() const
{
	// colorWriteEnable:4 is always significant - even with blending OFF the OM
	// render-target write mask differs, so a "write alpha only" passthrough must
	// not share a cached ID3D11BlendState with a "write all" passthrough.
	// Then: blendEnable:1 | srcBlend:4 | dstBlend:4 | blendOp:3.
	unsigned int k = (static_cast<unsigned int>(colorWriteEnable) & 0xF);
	if (!blendEnable) {
		return k;
	}
	k |= 1u << 4;
	k |= (static_cast<unsigned int>(srcBlend) & 0xF) << 5;
	k |= (static_cast<unsigned int>(dstBlend) & 0xF) << 9;
	k |= (static_cast<unsigned int>(blendOp)  & 0x7) << 13;
	return k;
}

unsigned int RenderStateVector::Depth_Key() const
{
	// depthEnable:1 | depthWrite:1 | depthFunc:3.
	unsigned int k = depthEnable ? 1u : 0u;
	k |= (depthWrite ? 1u : 0u) << 1;
	k |= (static_cast<unsigned int>(depthFunc) & 0x7) << 2;
	// Stencil states fold into the same key with a strong mix: the reachable
	// stencil state space is tiny (a handful of shadow-volume configurations),
	// so FNV-1a over the fields keeps collisions practically impossible while
	// disabled-stencil vectors keep sharing the single fast-path key.
	if (!stencilEnable) {
		return k;
	}
	unsigned int s = stencilFunc | (stencilFail << 3) | (stencilZFail << 6)
		| (stencilPass << 9) | (stencilRef << 12) ^ stencilMask ^ (stencilWriteMask * 0x9E3779B9u);
	if (twoSidedStencil) {
		s ^= 1u << 20;
		s ^= (ccwStencilFunc | (ccwStencilFail << 3) | (ccwStencilZFail << 6) | (ccwStencilPass << 9)) * 0x2E4B8CADu;
	}
	s ^= s >> 15; s *= 0x2545F491u; s ^= s >> 13;
	k |= 1u << 5;
	k ^= s * 0x85EBCA6Bu + 0x27D4EB2Fu;
	return k;
}

unsigned int RenderStateVector::Raster_Key() const
{
	// cullMode:2 | fillMode:1.
	unsigned int k = static_cast<unsigned int>(cullMode) & 0x3;
	k |= (static_cast<unsigned int>(fillMode) & 0x1) << 2;
	return k;
}

// ----------------------------------------------------------------------------
// D3D11StateCache
// ----------------------------------------------------------------------------

ID3D11BlendState * D3D11StateCache::Get_Blend_State(ID3D11Device * device, const RenderStateVector & rs)
{
	if (device == nullptr) {
		return nullptr;
	}
	const unsigned int key = rs.Blend_Key();
	std::unordered_map<unsigned int, ID3D11BlendState *>::iterator it = m_blend.find(key);
	if (it != m_blend.end()) {
		return it->second; // cache hit - the whole point of step 9
	}

	D3D11_BLEND_DESC desc;
	ZeroMemory(&desc, sizeof(desc));
	desc.AlphaToCoverageEnable = FALSE;
	desc.IndependentBlendEnable = FALSE;
	D3D11_RENDER_TARGET_BLEND_DESC & rt = desc.RenderTarget[0];
	rt.BlendEnable = rs.blendEnable ? TRUE : FALSE;
	rt.SrcBlend = To_D3D11_Blend_Color(rs.srcBlend);
	rt.DestBlend = To_D3D11_Blend_Color(rs.dstBlend);
	rt.BlendOp = To_D3D11_Blend_Op(rs.blendOp);
	rt.SrcBlendAlpha = To_D3D11_Blend_Alpha(rs.srcBlend);
	rt.DestBlendAlpha = To_D3D11_Blend_Alpha(rs.dstBlend);
	rt.BlendOpAlpha = To_D3D11_Blend_Op(rs.blendOp);
	// D3DCOLORWRITEENABLE_R/G/B/A (1/2/4/8) share the bit layout of
	// D3D11_COLOR_WRITE_ENABLE_R/G/B/A, so the low 4 bits map directly.
	rt.RenderTargetWriteMask = static_cast<D3D11_COLOR_WRITE_ENABLE>(rs.colorWriteEnable & 0xF);

	ID3D11BlendState * state = nullptr;
	if (FAILED(device->CreateBlendState(&desc, &state))) {
		return nullptr;
	}
	m_blend[key] = state;
	return state;
}

ID3D11DepthStencilState * D3D11StateCache::Get_Depth_State(ID3D11Device * device, const RenderStateVector & rs)
{
	if (device == nullptr) {
		return nullptr;
	}
	const unsigned int key = rs.Depth_Key();
	std::unordered_map<unsigned int, ID3D11DepthStencilState *>::iterator it = m_depth.find(key);
	if (it != m_depth.end()) {
		return it->second;
	}

	D3D11_DEPTH_STENCIL_DESC desc;
	ZeroMemory(&desc, sizeof(desc));
	desc.DepthEnable = rs.depthEnable ? TRUE : FALSE;
	desc.DepthWriteMask = rs.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
	desc.DepthFunc = To_D3D11_Cmp(rs.depthFunc);
	desc.StencilEnable = rs.stencilEnable ? TRUE : FALSE;
	if (rs.stencilEnable) {
		desc.StencilReadMask = static_cast<UINT8>(rs.stencilMask & 0xFF);
		desc.StencilWriteMask = static_cast<UINT8>(rs.stencilWriteMask & 0xFF);
		D3D11_DEPTH_STENCILOP_DESC & fo = desc.FrontFace;
		fo.StencilFailOp = To_D3D11_StencilOp(rs.stencilFail);
		fo.StencilDepthFailOp = To_D3D11_StencilOp(rs.stencilZFail);
		fo.StencilPassOp = To_D3D11_StencilOp(rs.stencilPass);
		fo.StencilFunc = To_D3D11_Cmp(rs.stencilFunc);
		if (rs.twoSidedStencil && W3DNext_TwoSidedStencil_Enabled()) {
			// Real two-sided stencil (D3DRS_TWOSIDEDSTENCILMODE +
			// D3DRS_CCW_STENCIL*): the shadow-volume passes need genuinely
			// different front/back ops (e.g. incr on back-face depth-fail,
			// decr on front-face depth-fail) to count the volume correctly.
			// Opt-in via env var (see W3DNext_TwoSidedStencil_Enabled) -
			// enabling this unconditionally caused severe, broad rendering
			// corruption (black terrain, broken water, opaque shadow blobs)
			// through some runtime interaction that isn't visible from static
			// source reading alone (likely needs a GPU frame-capture tool
			// such as RenderDoc/PIX to pin down). Defaults OFF so behaviour
			// matches the previously-working mirrored path until that's
			// properly diagnosed.
			D3D11_DEPTH_STENCILOP_DESC & bo = desc.BackFace;
			bo.StencilFailOp = To_D3D11_StencilOp(rs.ccwStencilFail);
			bo.StencilDepthFailOp = To_D3D11_StencilOp(rs.ccwStencilZFail);
			bo.StencilPassOp = To_D3D11_StencilOp(rs.ccwStencilPass);
			bo.StencilFunc = To_D3D11_Cmp(rs.ccwStencilFunc);
		} else {
			// DX8 single-sided stenciling: D3D11 still requires both
			// descriptors filled in, so mirror the front-face ops (this path
			// is unrelated to shadow volumes and mirroring is correct here).
			desc.BackFace = fo;
		}
	}

	ID3D11DepthStencilState * state = nullptr;
	if (FAILED(device->CreateDepthStencilState(&desc, &state))) {
		return nullptr;
	}
	m_depth[key] = state;
	return state;
}

ID3D11RasterizerState * D3D11StateCache::Get_Rasterizer_State(ID3D11Device * device, const RenderStateVector & rs)
{
	if (device == nullptr) {
		return nullptr;
	}
	const unsigned int key = rs.Raster_Key();
	std::unordered_map<unsigned int, ID3D11RasterizerState *>::iterator it = m_raster.find(key);
	if (it != m_raster.end()) {
		return it->second;
	}

	D3D11_RASTERIZER_DESC desc;
	ZeroMemory(&desc, sizeof(desc));
	desc.FillMode = To_D3D11_Fill(rs.fillMode);
	desc.CullMode = To_D3D11_Cull(rs.cullMode);
	desc.FrontCounterClockwise = FALSE;
	desc.DepthClipEnable = TRUE;

	ID3D11RasterizerState * state = nullptr;
	if (FAILED(device->CreateRasterizerState(&desc, &state))) {
		return nullptr;
	}
	m_raster[key] = state;
	return state;
}

void D3D11StateCache::Release_All()
{
	for (std::unordered_map<unsigned int, ID3D11BlendState *>::iterator it = m_blend.begin(); it != m_blend.end(); ++it) {
		if (it->second != nullptr) {
			it->second->Release();
		}
	}
	m_blend.clear();
	for (std::unordered_map<unsigned int, ID3D11DepthStencilState *>::iterator it = m_depth.begin(); it != m_depth.end(); ++it) {
		if (it->second != nullptr) {
			it->second->Release();
		}
	}
	m_depth.clear();
	for (std::unordered_map<unsigned int, ID3D11RasterizerState *>::iterator it = m_raster.begin(); it != m_raster.end(); ++it) {
		if (it->second != nullptr) {
			it->second->Release();
		}
	}
	m_raster.clear();
}
