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

// Legacy render-state vector -> D3D11 immutable state objects, with a cache
// (RENDERER_PORT.md step 9). Direct3D 11 has no per-call SetRenderState: the ~60
// D3D8 D3DRS_* blend/depth/raster states collapse into three immutable objects
// (ID3D11BlendState / ID3D11DepthStencilState / ID3D11RasterizerState) that must
// be created up-front and bound whole. Re-creating them per draw would be
// ruinous, so this translation unit keys the created objects on the legacy state
// vector and hands back a cached object on every later request for the same
// vector - the whole point of the step.
//
// Layering: this header is deliberately <d3d11.h>-free (it only forward-declares
// the ID3D11*State interfaces), exactly like D3D11FVF.h keeps its d3d8 surface
// out of the header. That lets D3D11Backend.h include it and stay as light as its
// Backend/ siblings; the actual D3D11_*_DESC translation + CreateXState calls
// live in D3D11States.cpp, which includes <d3d11.h>. The RB_* semantic enums
// below carry sequential, D3D-ABI-free values so no raw D3DRS_*/D3DBLEND/D3DCMP
// enum ever crosses into the backend-neutral interface.

#pragma once

#include <unordered_map>

struct ID3D11Device;
struct ID3D11BlendState;
struct ID3D11DepthStencilState;
struct ID3D11RasterizerState;

// --- Typed, D3D-ABI-free blend factors (the D3DBLEND / D3D11_BLEND world) -----
// Only the factors the game's ShaderClass path actually drives are modelled:
// opaque (ONE/ZERO), alpha blend (SRCALPHA/INVSRCALPHA), additive (ONE/ONE), and
// the src/dst-color variants a few materials use. Values are sequential and do
// NOT match D3DBLEND_* / D3D11_BLEND_*.
enum RenderBackendBlendFactor
{
	RB_BLEND_ZERO = 0,
	RB_BLEND_ONE,
	RB_BLEND_SRCALPHA,
	RB_BLEND_INVSRCALPHA,
	RB_BLEND_SRCCOLOR,
	RB_BLEND_INVSRCCOLOR,
	RB_BLEND_DESTALPHA,
	RB_BLEND_INVDESTALPHA,
	RB_BLEND_DESTCOLOR,
	RB_BLEND_INVDESTCOLOR,
	RB_BLEND_FACTOR_COUNT
};

// Stencil operation (D3DRS_STENCILFAIL/ZFAIL/PASS / D3D11_STENCIL_OP).
// Values are sequential and do NOT match D3DSTENCILOP_*.
enum RenderBackendStencilOp
{
	RB_STENCILOP_KEEP = 0,
	RB_STENCILOP_ZERO,
	RB_STENCILOP_REPLACE,
	RB_STENCILOP_INCRSAT,
	RB_STENCILOP_DECRSAT,
	RB_STENCILOP_INVERT,
	RB_STENCILOP_INCR,
	RB_STENCILOP_DECR,
	RB_STENCILOP_COUNT
};

// Blend equation (D3DRS_BLENDOP / D3D11_BLEND_OP). ADD covers every game case;
// the rest are supported for completeness.
enum RenderBackendBlendOp
{
	RB_BLENDOP_ADD = 0,
	RB_BLENDOP_SUBTRACT,
	RB_BLENDOP_REVSUBTRACT,
	RB_BLENDOP_MIN,
	RB_BLENDOP_MAX,
	RB_BLENDOP_COUNT
};

// Comparison function for the depth test (D3DRS_ZFUNC / D3D11_COMPARISON_FUNC).
enum RenderBackendCmpFunc
{
	RB_CMP_NEVER = 0,
	RB_CMP_LESS,
	RB_CMP_EQUAL,
	RB_CMP_LESSEQUAL,
	RB_CMP_GREATER,
	RB_CMP_NOTEQUAL,
	RB_CMP_GREATEREQUAL,
	RB_CMP_ALWAYS,
	RB_CMP_COUNT
};

// Face culling (D3DRS_CULLMODE / D3D11 rasterizer CullMode). CW mirrors
// D3DCULL_CW (cull clockwise-wound faces); see the FrontCounterClockwise note in
// D3D11States.cpp for the winding mapping.
enum RenderBackendCullMode
{
	RB_CULL_NONE = 0,
	RB_CULL_CW,
	RB_CULL_CCW,
	RB_CULL_COUNT
};

enum RenderBackendFillMode
{
	RB_FILL_SOLID = 0,
	RB_FILL_WIREFRAME,
	RB_FILL_COUNT
};

// The legacy render-state vector - the cache key spine of step 9. Two vectors
// with identical fields translate to the same three state objects. Fields are
// grouped by which object they feed (blend / depth-stencil / rasterizer); the
// per-object sub-keys below let a vector that differs only in, say, its depth
// fields still SHARE one ID3D11BlendState with another vector.
struct RenderStateVector
{
	// Blend (OMSetBlendState).
	bool blendEnable;                 // D3DRS_ALPHABLENDENABLE
	RenderBackendBlendFactor srcBlend;// D3DRS_SRCBLEND
	RenderBackendBlendFactor dstBlend;// D3DRS_DESTBLEND
	RenderBackendBlendOp blendOp;     // D3DRS_BLENDOP
	unsigned int colorWriteEnable;    // D3DRS_COLORWRITEENABLE (low 4 bits: R,G,B,A)

	// Depth-stencil (OMSetDepthStencilState).
	bool depthEnable;                 // D3DRS_ZENABLE
	bool depthWrite;                  // D3DRS_ZWRITEENABLE
	RenderBackendCmpFunc depthFunc;   // D3DRS_ZFUNC

	// Stencil (OMSetDepthStencilState) - drives the volumetric shadow passes.
	bool stencilEnable;               // D3DRS_STENCILENABLE
	RenderBackendCmpFunc stencilFunc; // D3DRS_STENCILFUNC
	RenderBackendStencilOp stencilFail;  // D3DRS_STENCILFAIL
	RenderBackendStencilOp stencilZFail; // D3DRS_STENCILZFAIL
	RenderBackendStencilOp stencilPass;  // D3DRS_STENCILPASS
	unsigned int stencilRef;             // D3DRS_STENCILREF (OMSetDepthStencilState arg)
	unsigned int stencilMask;            // D3DRS_STENCILMASK (read mask)
	unsigned int stencilWriteMask;       // D3DRS_STENCILWRITEMASK

	// Back-face (CCW) stencil ops - D3DRS_TWOSIDEDSTENCILMODE / D3DRS_CCW_STENCIL*.
	// The stencil-shadow-volume technique fundamentally needs DIFFERENT ops for
	// front- and back-facing shadow-volume polygons (e.g. increment on back-face
	// depth-fail, decrement on front-face depth-fail) to count correctly; without
	// these, D3D11's BackFace descriptor was silently mirrored from FrontFace
	// (see Get_Depth_State), which breaks the volume count and makes shadows
	// render as if stuck to the casting object instead of projected onto the
	// ground.
	bool twoSidedStencil;                  // D3DRS_TWOSIDEDSTENCILMODE
	RenderBackendCmpFunc ccwStencilFunc;    // D3DRS_CCW_STENCILFUNC
	RenderBackendStencilOp ccwStencilFail;  // D3DRS_CCW_STENCILFAIL
	RenderBackendStencilOp ccwStencilZFail; // D3DRS_CCW_STENCILZFAIL
	RenderBackendStencilOp ccwStencilPass;  // D3DRS_CCW_STENCILPASS

	// Rasterizer (RSSetState).
	RenderBackendCullMode cullMode;   // D3DRS_CULLMODE
	RenderBackendFillMode fillMode;   // D3DRS_FILLMODE

	// Default opaque state, mirroring DX8Wrapper::Apply_Default_State():
	// blend OFF (ONE/ZERO, ADD), depth test + write ON at LESSEQUAL, cull CW,
	// solid fill.
	RenderStateVector();

	// Packed sub-keys - the ONLY fields that define each object. Identical
	// blend fields => identical Blend_Key() => one shared ID3D11BlendState, even
	// when the depth or raster fields differ. Each packs into well under 32 bits.
	unsigned int Blend_Key() const;
	unsigned int Depth_Key() const;
	unsigned int Raster_Key() const;
};

// Caches ID3D11*State objects keyed by the legacy state vector's per-object
// sub-keys. First request for a given sub-key CREATES + stores the object (miss);
// every later request with an equal sub-key returns the SAME pointer (hit) - so
// there is no per-draw CreateXState. Growth is bounded by the (small) reachable
// state space; the maps are never pruned (fine for now - see step 9 notes).
// The cache does not AddRef the returned pointers for callers: it owns them and
// releases every one in Release_All(), which the backend calls before releasing
// the device.
class D3D11StateCache
{
public:
	D3D11StateCache() {}
	~D3D11StateCache() { Release_All(); }

	// Return a cached state object for rs, creating it on a miss. Returns nullptr
	// only if the underlying CreateXState fails (or device is null).
	ID3D11BlendState * Get_Blend_State(ID3D11Device * device, const RenderStateVector & rs);
	ID3D11DepthStencilState * Get_Depth_State(ID3D11Device * device, const RenderStateVector & rs);
	ID3D11RasterizerState * Get_Rasterizer_State(ID3D11Device * device, const RenderStateVector & rs);

	// Release every cached object and empty the maps. Safe to call repeatedly.
	void Release_All();

	// Distinct-object counts, for the smoke oracle's cache-growth assertions.
	unsigned int Blend_Count() const { return static_cast<unsigned int>(m_blend.size()); }
	unsigned int Depth_Count() const { return static_cast<unsigned int>(m_depth.size()); }
	unsigned int Raster_Count() const { return static_cast<unsigned int>(m_raster.size()); }

private:
	D3D11StateCache(const D3D11StateCache &);            // non-copyable (owns objects)
	D3D11StateCache & operator=(const D3D11StateCache &);

	std::unordered_map<unsigned int, ID3D11BlendState *> m_blend;
	std::unordered_map<unsigned int, ID3D11DepthStencilState *> m_depth;
	std::unordered_map<unsigned int, ID3D11RasterizerState *> m_raster;
};
