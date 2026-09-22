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

// D3D11Backend is the Direct3D 11 implementation of IRenderBackend
// (RENDERER_PORT.md step 3: device / swapchain / clear / present skeleton).
//
// Real implementations: the device-lifecycle slice only - Initialize,
// Shutdown, Begin_Scene, End_Scene, Flip_To_Primary, Clear, Set_Viewport,
// plus the trivially answerable queries (Is_Device_Lost, Has_Stencil,
// Get_Back_Buffer_Format). Everything else is a stub marked with the
// D3D11_STUB comment convention (see D3D11Backend.cpp) - stubs are safe to
// call (no-op / safe default return), never assert-crash.
//
// Nothing constructs this backend in the game yet; RenderBackend.cpp still
// creates DX8Backend. The smoke test (Core/Tests/d3d11_smoke) instantiates
// it directly.

#pragma once

#include "IRenderBackend.h"
#include "D3D11States.h" // render-state vector + state-object cache (step 9)
#include "matrix4.h" // Matrix4x4 stored by value for the transform constant buffer
#include "vector3.h" // Vector3 stored by value (ambient) + typed lighting setters
#include <map>       // uploaded-texture cache
#include <vector>     // vertex/index ring buffers

// D3D11 interfaces are forward-declared so this header stays as light as its
// Backend/ siblings; d3d11.h is included by D3D11Backend.cpp only.
struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;
struct ID3D11Texture2D;
struct ID3D11DepthStencilView;
struct ID3D11Buffer;
struct ID3D11InputLayout;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11RasterizerState;
struct ID3D11BlendState;
struct ID3D11DepthStencilState;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11SamplerState;
struct ID3D11Query;

// Defined in D3D11Backend.cpp. Gates the optional per-draw/per-texture-bind/
// per-upload QueryPerformanceCounter debug timing (off by default - set env
// D3D11_PROFILE=1 to enable). See the definition for why this exists.
bool Profiling_Enabled();

class D3D11Backend : public IRenderBackend
{
public:
	D3D11Backend();
	virtual ~D3D11Backend();

	// Device lifecycle (real). window is the target HWND, passed as void* to
	// keep the interface windows.h-free. Construction stays cheap; all D3D11
	// object creation happens here.
	virtual void Initialize(void * window, int width, int height) override;
	virtual void Shutdown() override;

	// True once Initialize succeeded and until Shutdown. If Initialize failed,
	// Get_Init_Result() carries the failing HRESULT (as a long, so the header
	// needs no windows.h).
	bool Is_Initialized() const { return m_device != nullptr; }
	long Get_Init_Result() const { return m_initResult; }

	// Backend-specific accessors for tests/tools that drive the D3D11 objects
	// directly (e.g. the smoke test's staging-texture readback).
	ID3D11Device * Get_Device() const { return m_device; }
	ID3D11DeviceContext * Get_Context() const { return m_context; }
	IDXGISwapChain * Get_Swap_Chain() const { return m_swapChain; }
	unsigned int Get_Viewport_Width() const { return m_viewportWidth; }
	unsigned int Get_Viewport_Height() const { return m_viewportHeight; }

	// Backend-specific direct-upload path (RENDERER_PORT.md step 4). The
	// IRenderBackend Set_Vertex_Buffer/Set_Index_Buffer overloads take W3D
	// buffer objects (VertexBufferClass etc.) whose data can only be reached
	// through the full WW3D2 header graph - unavailable to the standalone smoke
	// test, which compiles this .cpp without linking ww3d2. These two entry
	// points take raw bytes instead, do the real ID3D11Buffer creation + input
	// layout build + IA binding, and are what the smoke oracle drives. The W3D
	// overloads route through this same machinery once the backend is wired
	// into the VB/IB classes. Return false (and leave nothing bound) on failure.
	bool Upload_Vertices(const void * data, unsigned int size_bytes, unsigned int fvf);
	bool Upload_Indices16(const unsigned short * indices, unsigned int count);
	// Issues a GPU-completion fence for the buffers used by the just-issued draw
	// so the upload pool can safely reuse them (only after the fence signals).
	void Mark_Draw_Complete();

	// Releases every pooled dynamic vertex/index buffer. Called at Begin_Scene:
	// by then the previous frame's GPU work is done, so releasing is safe and
	// memory stays bounded (no per-upload leak, no in-flight reuse -> no TDR/AV).
	void Flush_Dynamic_Pools();

	// Grow (or create) the single per-frame DEFAULT staging buffer if needed.
	bool Ensure_Dyn_VB_Cap(unsigned int need);
	bool Ensure_Dyn_IB_Cap(unsigned int need);

	// Number of texture stages the combiner ubershader supports simultaneously.
	// N=2 covers the common game case (single/dual-texture); structured to 4.
	enum { RB_MAX_TEXTURE_STAGES = 4 };

	// Upper bound on a source mip chain: 16 levels covers 32768x32768 down to
	// 1x1, well past anything the game ships. Bounds the stack arrays in the
	// multi-level upload paths so no allocation is needed per bind.
	enum { RB_MAX_MIP_LEVELS = 16 };

	// Backend-specific direct texture-upload path (RENDERER_PORT.md step 6),
	// analogous to Upload_Vertices: the IRenderBackend Set_Texture overload takes a
	// TextureBaseClass whose pixels can only be reached through the full WW3D2
	// header graph (unavailable to the standalone smoke test). This entry point
	// takes raw R8G8B8A8 bytes (R first in memory), creates the ID3D11Texture2D +
	// SRV + point/clamp ID3D11SamplerState, and binds them to pixel-shader slot
	// `stage`. It is what the smoke oracle drives. Returns false on failure.
	// wrap/linear default to false = the POINT + CLAMP sampler the smoke oracle
	// relies on (exact-texel reads). The real-game texture path passes true/true so
	// tiled backdrops sample correctly and UI edges filter smoothly.
	bool Upload_Texture_RGBA(unsigned int stage, unsigned int width, unsigned int height, const void * rgba_pixels,
		bool wrap = false, bool linear = false);

	// Backend-neutral block-compressed formats, mapped to DXGI BC1/BC2/BC3_UNORM
	// (the D3D8 DXT1 / DXT2+3 / DXT4+5 families). Values are stable for logging.
	enum RenderBackendBCFormat { RB_BC1 = 1, RB_BC2 = 2, RB_BC3 = 3 };

	// Sampler mip policy for the upload paths - see Upload_Texture_*_Mips.
	enum RenderBackendMipFilter { RB_MIPF_LINEAR = 0, RB_MIPF_POINT = 1, RB_MIPF_NONE = 2 };

	// Native block-compressed upload (the DXT path of RENDERER_PORT.md step 10):
	// takes the raw compressed blocks exactly as they sit in the source surface -
	// no CPU decode - and creates a BCn ID3D11Texture2D + SRV + sampler bound to
	// pixel-shader slot `stage`, exactly like Upload_Texture_RGBA. width/height are
	// the TEXEL dimensions and must be multiples of 4 (>= 4): D3D11 requires
	// block-aligned top-level dimensions for BC resources. row_pitch_bytes is the
	// byte distance between consecutive BLOCK rows in `blocks` (pass 0 for tightly
	// packed = blocks_per_row * bytes_per_block; 8 for BC1, 16 for BC2/BC3).
	// Returns false on failure (nothing bound).
	bool Upload_Texture_BC(unsigned int stage, unsigned int width, unsigned int height,
		RenderBackendBCFormat format, const void * blocks, unsigned int row_pitch_bytes = 0,
		bool wrap = false, bool linear = false);

	// One source mip level for the multi-level upload paths below.
	//   data      - the level's bytes (R8G8B8A8 texels, or BCn blocks)
	//   width/height - the TEXEL dimensions OF THIS LEVEL
	//   row_pitch - bytes between consecutive rows (RGBA) or BLOCK rows (BCn)
	struct RenderBackendMipLevel {
		const void * data;
		unsigned int width;
		unsigned int height;
		unsigned int row_pitch;
	};

	// Multi-level (mip-mapped) uploads. THESE ARE THE REAL-GAME PATHS; the two
	// single-level entry points above now delegate here with count = 1 and remain
	// for the smoke oracle, which wants exact-texel reads from a one-level surface.
	//
	// WHY (2026-07-26, see docs/architecture/d3d11-parity-log.md): every texture
	// used to be created with MipLevels = 1 while the samplers asked for
	// D3D11_FILTER_MIN_MAG_MIP_LINEAR with MaxLOD = FLOAT32_MAX. Trilinear
	// filtering requested against a one-level chain means every sample lands on
	// level 0 no matter how minified - the terrain rendered sharp and aliased
	// where DX8 is smooth (~19 mae over 54% of pixels), and minified level-0
	// sampling has almost no texture-cache locality, which is why collapsing the
	// upload COUNT (101k -> 1 per 600f) bought no frames. Uploads were never the
	// bottleneck; sampling was.
	//
	// `levels` must be ordered largest-first and contiguous (level 0 .. count-1);
	// count == 0 or a null level fails the call. The source surfaces already carry
	// the chain (IDirect3DTexture8::GetLevelCount), so nothing is generated here.
	//
	// `mip` mirrors the engine's per-texture TextureFilterClass mip mode
	// (D3DTSS_MIPFILTER on DX8): LINEAR = trilinear, POINT = per-level snap
	// (FILTER_TYPE_FAST), NONE = sample level 0 only (D3DTEXF_NONE - the
	// MIP_LEVELS_1 alias textures, e.g. the terrain blend pass sharing the
	// base atlas's 3-level D3D texture). A blanket-trilinear sampler here is
	// what put rectangular mip seams on the terrain: blend-pass cells sampled
	// down the chain while DX8 pins them to level 0.
	bool Upload_Texture_RGBA_Mips(unsigned int stage, const RenderBackendMipLevel * levels,
		unsigned int count, bool wrap = false, bool linear = false,
		RenderBackendMipFilter mip = RB_MIPF_LINEAR);
	bool Upload_Texture_BC_Mips(unsigned int stage, RenderBackendBCFormat format,
		const RenderBackendMipLevel * levels, unsigned int count,
		bool wrap = false, bool linear = false,
		RenderBackendMipFilter mip = RB_MIPF_LINEAR);

	// W3DNEXT_D3D11_MIPS=0 clamps Set_Texture back to uploading level 0 only, i.e. the
	// pre-2026-07-26 behaviour, WITHOUT a second build. Same in-process-toggle
	// reasoning as W3DNEXT_D3D11_TEXCACHE: an A/B across two builds/runs of this scene
	// cannot be trusted (runs diverge by the same magnitude as the effect, and the
	// fps metric alone has read 11-19 for one build), so the only honest A/B is
	// one binary flipped by an env var and measured repeatedly.
	static bool Mip_Upload_Enabled();

	// Bind a deliberately-loud 4x4 magenta/black checker to `stage` so a texture
	// whose format the backend cannot upload is VISIBLY wrong instead of silently
	// black (an unbound SRV samples (0,0,0,0), which the MODULATE combiner turns
	// into solid black). Routes through Upload_Texture_RGBA.
	bool Upload_Fallback_Texture(unsigned int stage);

	// Uploaded-texture cache (see m_textureCache). Bind_Cached_Texture binds a
	// previously uploaded GPU texture to `stage` and returns true on a hit for
	// (key, version); Store_Cached_Texture records whatever the stage currently
	// holds under that key after a fresh upload. Cache entries own a reference;
	// the stage slots hold their own, so either can be released independently.
	//
	// `key` is TextureBaseClass::Get_ID() - a process-monotonic id that is NEVER
	// reused - and `version` is that texture's D3D-generation (bumped on every
	// D3DTexture mutation) folded with the copy-shadow content version. Two
	// distinct textures therefore cannot collide on a key, and a surviving entry
	// cannot outlive the bytes it was uploaded from. (The earlier revision keyed
	// on the raw IDirect3DTexture8* and DID alias when the engine reallocated a
	// freed texture at the same address - see the parity log.)
	bool Bind_Cached_Texture(unsigned int stage, unsigned int key, unsigned long long version);
	void Store_Cached_Texture(unsigned int stage, unsigned int key, unsigned long long version);
	void Evict_Cached_Texture(unsigned int key);
	void Release_Texture_Cache();
	// Drop a render-target texture's D3D11 resources when its TextureClass dies
	// (settings change / map reload recreates the RT). Keyed by the same
	// TextureClass* the engine holds, so the lookup matches Create_Render_Target.
	void Evict_Render_Target(TextureBaseClass * tex);
	unsigned int Peek_Texture_Cache_Hits() const { return m_texCacheHits; }
	unsigned int Peek_Texture_Cache_Uploads() const { return m_texCacheUploads; }

	// Bind the SHARED 4x4 all-WHITE texture to `stage`: the IDENTITY for every
	// multiply (MODULATE combiner -> texture*diffuse == diffuse; DSTCOLOR
	// framebuffer blends -> dst*1 == dst). Used for null Set_Texture binds and
	// for textures whose format IS understood but whose bytes are UNREADABLE at
	// bind time (default-pool / GPU-only surfaces, e.g. the fog-of-war shroud):
	// degrading to a no-op beats painting the whole affected pass magenta or
	// sampling stale garbage. One device-lifetime texture+SRV+sampler created
	// lazily on first use (real->null transitions happen dozens of times per
	// frame on the terrain path - creating per bind was measurable churn); the
	// stage slot AddRefs the shared objects so the normal per-stage release
	// invariants hold unchanged.
	bool Bind_Neutral_Texture(unsigned int stage);

	// Typed, D3D-ABI-free setters for the fixed-function combiner (the
	// D3DTSS_COLOROP/ALPHAOP world). The RB_TEXOP_*/RB_TEXARG_* enums live in
	// IRenderBackend.h; a DX8-facing caller translates D3DTSS_* into these before
	// they reach the backend. State is packed into cbCombiner on the next draw.
	void Set_Texture_Stage_Count(unsigned int count);
	void Set_Texture_Stage_ColorOp(unsigned int stage, RenderBackendTexOp op, RenderBackendTexArg arg1, RenderBackendTexArg arg2);
	void Set_Texture_Stage_AlphaOp(unsigned int stage, RenderBackendTexOp op, RenderBackendTexArg arg1, RenderBackendTexArg arg2);
	void Set_Texture_Stage_TexCoordIndex(unsigned int stage, unsigned int texcoord_index);
	void Set_Texture_Factor(float r, float g, float b, float a);
	// DX8 alpha test -> pixel-shader discard on the post-combiner alpha.
	// less_equal = pass when alpha <= ref (ShaderClass's INVSRCALPHA case);
	// otherwise pass when alpha >= ref. ref is normalized 0..1.
	void Set_Alpha_Test(bool enable, bool less_equal, float ref);

	// Grayscale override for the 2D UI path (disabled command-bar buttons).
	// DX8 draws these via a two-stage DOT3 luminance trick (render2d.cpp)
	// whose raw texture-stage states never reach this backend; instead the
	// combiner's monochrome post-op replays the same math: rgb is replaced by
	// dot(rgb, lum) with the DX8 trick's effective Rec.601-style weights,
	// alpha passes through. Sticky until cleared - callers bracket the draw.
	void Set_Grayscale_Override(bool enable);

	// Clean D3D11 terrain normal-mapping path. When enabled, the next draws use
	// the terrain HLSL pixel shader (kTerrainNormalPixelShaderHLSL) instead of the
	// FF combiner, with SunDir/SunColor/Ambient/flags supplied via
	// Set_Terrain_Normal_Constants. Textures (base/normal/lightmap) are bound
	// through the normal Set_Texture stage path (t0/t1/t2). Inert until called.
	void Set_Terrain_Normal_Pixel_Shader(bool enable);
	void Set_Terrain_Normal_Blend_Pixel_Shader(bool enable);
	void Set_Terrain_Normal_Constants(const Vector3 & sunDir, const Vector3 & sunColor,
	                                  const Vector3 & ambient, bool useLightMap);
	bool Is_Terrain_Normal_Active() const { return m_terrainNormalIsActive; }
	bool Is_Grade_Enabled() const { return m_gradeEnabled; }
	void Apply_Color_Grade();
	void Set_Sun_Screen(float x, float y, float intensity);

	// Clean D3D11 unit/model normal-mapping path (mirrors the terrain normal
	// path). When enabled, the next unit draws use the unit HLSL vertex+pixel
	// shaders (kUnitNormalVertexShaderHLSL / kUnitNormalPixelShaderHLSL) instead
	// of the FF pipeline, with Blinn-Phong sun/ambient/specular + tangent-space
	// normal map supplied via Set_Unit_Normal_Constants. The base color binds to
	// t0 and the normal map to t1 through the normal Set_Texture path. Inert
	// until called - the unit renderer enables it per mesh that has a normal map.
	void Set_Unit_Normal_Pixel_Shader(bool enable, float detailStrength = 0.0f, float secondaryAdd = 0.0f);
	void Set_Unit_Normal_Constants(const Vector3 & sunDir, const Vector3 & sunColor,
	                               const Vector3 & ambient, float detailStrength = 0.0f, float secondaryAdd = 0.0f);

	// Clean D3D11 water reflection/surface pixel shader. When enabled, the next
	// water draw uses kWaterPixelShaderHLSL (sun glint + planar reflection sampled
	// from the reflection RTT) instead of the FF combiner. Textures are bound by the
	// caller via Set_Texture (t0=watermap, t1=reflection RTT, t2=noise); sun direction,
	// bump strength, reflection factor, glitter and a time value ride in cbWater from
	// Set_Water_Constants. The FF combiner (m_combinerBuffer) is restored on disable so
	// terrain/unit/UI draws are untouched. Enabled/restored around each water draw in
	// W3DWater.cpp.
	void Set_Water_Pixel_Shader(bool enable);
	void Set_Water_Constants(const Vector3 & sunDir, const Vector3 & sunColor,
	                         float bumpScale, float reflectionFactor, float sunGlitter,
	                         float time, unsigned viewportW, unsigned viewportH);

	// Fixed-function texcoord generation (the D3D8 D3DTSS_TCI_CAMERASPACEPOSITION
	// + D3DTS_TEXTUREn texture-matrix slice the terrain shroud/cloud passes use).
	// Typed, D3D-ABI-free like the combiner setters above: a DX8-facing caller
	// (the RB_Mirror_* hooks in D3D11Backend_W3D.cpp) translates the D3DTSS_*
	// values before they reach here. State packs into cbTexGen on the next draw.
	// The matrix is 16 floats in D3D's row-major ROW-VECTOR convention (uv =
	// pos * M), uploaded verbatim - the shader multiplies mul(vector, matrix), so
	// no transpose happens anywhere.
	void Set_Texture_Stage_Texgen_CameraSpace(unsigned int stage, bool enable);
	void Set_Texture_Transform_Enable(unsigned int stage, bool enable);
	void Set_Texture_Transform_Matrix(unsigned int stage, const float * m16);

	// Number of directional lights the FF lighting equation evaluates. N=1 covers
	// the common game case; structured to 4 to mirror LightEnvironmentClass.
	enum { RB_MAX_LIGHTS = 4 };

	// Backend-neutral material-color source, mirroring D3DRS_*MATERIALSOURCE:
	// MATERIAL uses the material constant, VERTEX uses the interpolated vertex
	// diffuse (D3DMCS_COLOR1). A DX8-facing caller maps D3DMCS_* to these.
	enum RenderBackendColorSource { RB_MATSRC_MATERIAL = 0, RB_MATSRC_VERTEX = 1 };

	// Fixed-function fog table modes (D3DFOGTABLEMODE). LINEAR is the game's
	// common case; EXP/EXP2 are supported for completeness.
	enum RenderBackendFogMode { RB_FOG_NONE = 0, RB_FOG_LINEAR = 1, RB_FOG_EXP = 2, RB_FOG_EXP2 = 3 };

	// Typed, D3D-ABI-free setters for the FF lighting / material / fog slice
	// (RENDERER_PORT.md step 7), analogous to the combiner setters above. They
	// pack cbLighting / cbFog on the next draw. The IRenderBackend Set_Light /
	// Set_Material overloads take W3D types (LightClass / VertexMaterialClass)
	// whose members can only be reached through the full WW3D2 header graph -
	// unavailable to the standalone smoke test - so the real GPU work is driven
	// through these plain-typed entry points, exactly as Upload_Vertices drives
	// the geometry slice. A DX8-facing caller extracts the W3D objects' fields and
	// calls these.
	void Set_Lighting_Enable(bool enable);
	void Set_Light_Count(unsigned int count);
	// dir is the WORLD-space direction the light travels (as D3DLIGHT8.Direction);
	// the shader lights with saturate(dot(N, -dir)). Auto-covers this index in the
	// active light count.
	void Set_Light_Directional(unsigned int index, const Vector3 & dir, const Vector3 & diffuse);
	void Set_Material_Params(
		const Vector3 & diffuse, const Vector3 & ambient, const Vector3 & emissive,
		float opacity, RenderBackendColorSource diffuse_source, RenderBackendColorSource ambient_source);
	void Set_Fog_Params(bool enable, const Vector3 & color, RenderBackendFogMode mode, float start, float end, float density);

	// Number of bone-palette matrices cbSkinning holds - the cap on a single
	// skinned draw's bone set. 64 covers the game's skinned meshes; the smoke
	// test uses 2-3. Bounds Set_Bone_Matrices and the HLSL Bones[] array.
	enum { RB_MAX_BONES = 64 };

	// Tree-sway wave table size: entry 0 = no sway, 1..MAX_SWAY_TYPES(10) =
	// W3DTreeBuffer's per-type wave vectors. Mirrors DX8's c8..c18.
	enum { RB_MAX_SWAY_ENTRIES = 11 };

	// W3DNext tree sway: upload the wave table (count float4s, entry 0 the
	// zero vector) and toggle the FF vertex shader's sway skew. Enabled only
	// around the tree draw - the vertex "normal" is repurposed there.
	void Set_Tree_Sway(bool enable, const float * vec4s, unsigned int count);

	// Typed, D3D-ABI-free setters for GPU skinning / indexed vertex blending
	// (RENDERER_PORT.md step 8), analogous to the lighting setters above. They
	// pack cbSkinning on the next draw. There is no IRenderBackend skinning
	// virtual (the interface stays raw-D3D-free and, per upstream, skinning-free),
	// so - exactly like Upload_Vertices drives the geometry slice - the smoke
	// oracle and a future DX8-facing caller drive skinning through these plain
	// entry points. Set_Skinning_Enable(false) restores the pass-through path so
	// every non-skinned draw is byte-identical to the pre-step-8 behavior.
	void Set_Skinning_Enable(bool enable);
	// Upload up to RB_MAX_BONES bone matrices to cbSkinning (bone i feeds a vertex
	// whose packed BLENDINDICES byte == i). Matrices are WWMath Matrix4x4 (16
	// row-major floats), same convention as the WVP / lighting buffers.
	void Set_Bone_Matrices(unsigned int count, const Matrix4x4 * matrices);

	// Typed, D3D-ABI-free render-state setters for the blend / depth / rasterizer
	// slice (RENDERER_PORT.md step 9), analogous to the combiner/lighting setters.
	// They mutate the legacy render-state vector and mark it dirty; the created
	// ID3D11*State objects are built + bound (from the cache) on the next
	// Apply_Render_State_Changes. The RB_* enums live in D3D11States.h; a DX8-facing
	// caller translates D3DRS_*/D3DBLEND/D3DCMP into these before they reach here, so
	// no raw D3D state enum crosses the backend boundary. The common ShaderClass
	// cases: opaque (blend off, ONE/ZERO), alpha blend (SRCALPHA/INVSRCALPHA, Z-write
	// off), additive (ONE/ONE).
void Set_Blend_Enable(bool enable);
void Set_Blend_Func(RenderBackendBlendFactor src, RenderBackendBlendFactor dst);
	void Set_Raw_Blend_Enable(unsigned int enable) override;
	void Set_Raw_Src_Blend(unsigned int d3dblend) override;
	void Set_Raw_Dst_Blend(unsigned int d3dblend) override;
	void Set_Raw_Stencil(unsigned int d3drs, unsigned int value) override;
	void Set_Blend_Op(RenderBackendBlendOp op);
	void Set_Color_Write_Enable(unsigned int mask);
	void Set_Depth_Test_Enable(bool enable);
	void Set_Depth_Write_Enable(bool enable);
	void Set_Depth_Func(RenderBackendCmpFunc func);
	void Set_Cull_Mode(RenderBackendCullMode mode);
	void Set_Fill_Mode(RenderBackendFillMode mode);

	// Currently-bound state objects (as opaque void*, so the header stays light).
	// The smoke oracle uses these to prove cache identity: requesting the same
	// render-state vector twice returns the SAME pointer (one cached object), a
	// different vector a different pointer. Null until the first
	// Apply_Render_State_Changes.
	const void * Get_Bound_Blend_State() const;
	const void * Get_Bound_Depth_State() const;
	const void * Get_Bound_Rasterizer_State() const;
	// Distinct-object counts in the cache, for the oracle's growth assertions.
	unsigned int Get_Blend_State_Count() const { return m_stateCache.Blend_Count(); }
	unsigned int Get_Depth_State_Count() const { return m_stateCache.Depth_Count(); }
	unsigned int Get_Rasterizer_State_Count() const { return m_stateCache.Raster_Count(); }

	// True once the shaders / constant buffer / rasterizer state compiled and
	// created successfully in Initialize.
	bool Is_Pipeline_Ready() const { return m_pipelineReady; }

	virtual bool Is_Device_Lost() const override;
	virtual bool Has_Stencil() override;
	virtual WW3DFormat Get_Back_Buffer_Format() override;
	virtual SurfaceClass * Get_Back_Buffer(unsigned int num) override;
	virtual void Set_Gamma(float gamma, float bright, float contrast, bool calibrate, bool uselimit) override;

	virtual void Begin_Scene() override;
	virtual void End_Scene(bool flip_frame) override;
	virtual void Flip_To_Primary() override;

private:
	// Latches m_deviceRemoved + logs GetDeviceRemovedReason() once, then exits
	// (no recovery path exists for the D3D11 device; see the .cpp comment).
	void Handle_Present_Result(long hr);

public:
	// --- GPU per-span timestamp profiler (W3DNEXT_D3D11_GPUPROF=1) ---------------
	// Answers WHERE the D3D11 frame's GPU time goes, which fps deltas alone
	// cannot (parity log 2026-07-26: the fps gap has only ever been inferred
	// from image diffs, wrongly). Begin_Scene opens a frame (disjoint query +
	// timestamp 0), engine-side Gpu_Profile_Marker calls drop labelled
	// timestamps, and the flip End_Scene closes the frame around Present. A
	// ring of in-flight frames absorbs the GPU->CPU query latency (results are
	// polled, never spun on); resolved spans accumulate per label and an
	// averaged "[D3D11 gpuprof]" line goes to the W3DNEXT_D3D11_LOG sink every
	// RB_GPUPROF_EMIT_FRAMES flips. Span cost is attributed to the LATER
	// marker's label; repeated labels in one frame (terrain under a reflection
	// pass) sum. Off (default): zero queries created, markers no-op.
	static bool Gpu_Profile_Enabled();
	virtual void Gpu_Profile_Marker(const char * label) override;
	virtual void Clear(bool clear_color, bool clear_z_stencil, const Vector3 & color, float dest_alpha, float z, unsigned int stencil) override;
	virtual void Set_Viewport(const RenderBackendViewport & viewport) override;

	virtual void Set_Vertex_Buffer(const VertexBufferClass * vb, unsigned int stream) override;
	virtual void Set_Vertex_Buffer(const DynamicVBAccessClass & vba) override;
	virtual void Set_Index_Buffer(const IndexBufferClass * ib, unsigned short index_base_offset) override;
	virtual void Set_Index_Buffer(const DynamicIBAccessClass & iba, unsigned short index_base_offset) override;
	virtual void Set_Index_Buffer_Index_Offset(unsigned int offset) override;

	// Write-time capture of DX8 dynamic-buffer contents (see IRenderBackend). These
	// route the just-written bytes straight through the existing Upload_Vertices /
	// Upload_Indices16 path (raw bytes in, real ID3D11Buffer out), so the discard-
	// locked dynamic geometry the 2D GUI / particle paths produce is drawable even
	// though it can't be read back at bind time. Defined in D3D11Backend.cpp (they
	// take raw bytes only - no WW3D2 header graph needed).
	virtual void Stage_Dynamic_Vertices(const void * data, unsigned int size_bytes, unsigned int fvf) override;
	virtual void Stage_Dynamic_Indices(const unsigned short * indices, unsigned int count) override;

	virtual void Set_Shader(const ShaderClass & shader) override;
	virtual void Get_Shader(ShaderClass & shader) override;
	virtual void Set_Alpha_Reference(float ref) override;
	virtual void Set_Material(const VertexMaterialClass * material) override;
	virtual void Set_Texture(unsigned int stage, TextureBaseClass * texture) override;

	virtual void Apply_Render_State_Changes() override;
	virtual void Apply_Default_State() override;
	virtual void Invalidate_Cached_Render_States() override;

	virtual void Set_Transform(TransformKind transform, const Matrix4x4 & m) override;
	virtual void Set_Transform(TransformKind transform, const Matrix3D & m) override;
	virtual void Get_Transform(TransformKind transform, Matrix4x4 & m) override;
	virtual void Set_World_Identity() override;
	virtual void Set_View_Identity() override;
	virtual bool Is_World_Identity() override;
	virtual bool Is_View_Identity() override;
	virtual void Set_Projection_Transform_With_Z_Bias(const Matrix4x4 & matrix, float znear, float zfar) override;

	virtual void Set_Light(unsigned int index, const LightClass & light) override;
	virtual void Disable_Light(unsigned int index) override;
	virtual void Set_Ambient(const Vector3 & color) override;
	virtual const Vector3 & Get_Ambient() const override;
	virtual void Set_Fog(bool enable, const Vector3 & color, float start, float end) override;
	virtual bool Get_Fog_Enable() const override;
	virtual void Set_Light_Environment(LightEnvironmentClass * light_env) override;
	virtual LightEnvironmentClass * Get_Light_Environment() const override;

	virtual void Draw_Triangles(
		unsigned int start_index,
		unsigned int polygon_count,
		unsigned int min_vertex_index,
		unsigned int vertex_count) override;
	virtual void Draw_Triangles(
		unsigned int buffer_type,
		unsigned int start_index,
		unsigned int polygon_count,
		unsigned int min_vertex_index,
		unsigned int vertex_count) override;
	virtual void Draw_Strip(
		unsigned int start_index,
		unsigned int primitive_count, // TRIANGLES, not indices (see IRenderBackend.h)
		unsigned int min_vertex_index,
		unsigned int vertex_count) override;

	// The shader id is treated as an opaque unsigned long.
	virtual void Set_Vertex_Shader(unsigned long vertex_shader) override;
	virtual void Set_Pixel_Shader(unsigned long pixel_shader) override;
	virtual void Set_Vertex_Shader_Constant(int reg, const void * data, int count) override;
	virtual void Set_Pixel_Shader_Constant(int reg, const void * data, int count) override;

	virtual TextureClass * Create_Render_Target(int width, int height, WW3DFormat format) override;
	virtual void Set_Render_Target_With_Z(TextureClass * texture, ZTextureClass * ztexture) override;
	virtual bool Is_Render_To_Texture() override;
	virtual void Set_Shadow_Map(int idx, ZTextureClass * ztex) override;
	virtual ZTextureClass * Get_Shadow_Map(int idx) override;

	// Screen-filter path (see IRenderBackend): backbuffer snapshot + legacy
	// XYZRHW quad draw through the FF-emulation pipeline. Real implementations
	// in D3D11Backend.cpp (raw bytes only, smoke-drivable).
	virtual void Capture_Backbuffer() override;
	virtual void Draw_Screen_Filter_Quad(const RenderBackendFilterQuad & quad) override;
	// CPU readback of the live backbuffer (top-down RGB24) for the engine's
	// screenshot feature; the legacy path reads the dead D3D8 device under
	// D3D11 and captured pure black.
	virtual bool Read_Back_Buffer(unsigned char * rgb_dst, unsigned int & width, unsigned int & height) override;

	// Diagnostics accessors for the W3DNEXT_D3D11_DRAWLOG per-draw log (read-only;
	// no rendering decision consumes these).
	const RenderStateVector & Peek_Render_State() const { return m_renderState; }
	unsigned int Peek_Last_Shader_Bits() const { return m_lastShaderBits; }
	unsigned int Peek_Combiner_Num_Stages() const { return m_combiner.numStages; }
	unsigned int Peek_Combiner_Color0(unsigned int i) const { return m_combiner.stageColor[0][i]; }
	unsigned int Peek_Combiner_Alpha0(unsigned int i) const { return m_combiner.stageAlpha[0][i]; }
	unsigned int Peek_Stage_Tex_Format(unsigned int stage) const { return m_stageTexFormatLog[stage]; }
	// Name of the bound stage texture ("" when none) - W3D TU only (needs
	// TextureBaseClass); the smoke TU stub returns "".
	const char * Peek_Stage_Tex_Name(unsigned int stage) const;

private:
	// Bind backbuffer RTV + DSV and the full-backbuffer viewport to the
	// immediate context. Called from Initialize and Begin_Scene.
	void Bind_Back_Buffer_Targets();
	void Release_Device_Objects();

	// GPU profiler internals (see Gpu_Profile_Marker above). One GpuProfFrame
	// per in-flight frame: its queries are created once and reused; `count`
	// timestamps are live this frame. RB_GPUPROF_RING = 8 frames of latency
	// budget - a timestamp result 8 flips old is always ready in practice, and
	// a slot still unresolved when the ring wraps is dropped, never waited on.
	enum { RB_GPUPROF_MAX_MARKS = 48, RB_GPUPROF_RING = 8, RB_GPUPROF_MAX_SPANS = 64, RB_GPUPROF_EMIT_FRAMES = 50 };
	struct GpuProfFrame {
		ID3D11Query * disjoint;
		ID3D11Query * ts[RB_GPUPROF_MAX_MARKS];
		const char * label[RB_GPUPROF_MAX_MARKS];
		unsigned int count;
		bool inFlight;
	};
	void Gpu_Profile_Open_Frame();               // from Begin_Scene
	void Gpu_Profile_Close_Frame();              // from the flip End_Scene, after Present
	void Gpu_Profile_Collect();                  // poll resolved slots, accumulate, maybe emit
	GpuProfFrame m_gpuProfRing[RB_GPUPROF_RING];
	unsigned int m_gpuProfWrite;                 // ring slot currently recording / next to record
	bool m_gpuProfOpen;                          // a frame is between Open and Close
	struct GpuProfSpan { const char * label; double sumMs; unsigned int hits; };
	GpuProfSpan m_gpuProfSpans[RB_GPUPROF_MAX_SPANS];
	unsigned int m_gpuProfSpanCount;
	unsigned int m_gpuProfFramesAccum;           // resolved frames since last emit

	// Uncached-upload profiler (CPU side, same W3DNEXT_D3D11_GPUPROF gate): every
	// Set_Texture that reaches an actual upload accounts count + bytes +
	// wall-ms here, split by WHY it wasn't a cache hit. The gpuprof spans
	// showed the ~11ms/frame mip cost sits in NO draw span and the texcache
	// counters are blind to the non-cacheable path (procedural / POOL_DEFAULT
	// / shroud re-upload on EVERY bind) - these buckets are that missing
	// witness. Emitted per RB_GPUPROF_EMIT_FRAMES flips from End_Scene as
	// "[D3D11 uploadprof]" with the mips=/swap= falsifiers.
	enum {
		RB_UPLOAD_NC = 0,        // !cacheable: re-uploads every bind
		RB_UPLOAD_MISS = 1,      // cacheable, first upload (or post-eviction)
		RB_UPLOAD_SHADOW = 2,    // copy-shadow path (fog-of-war shroud dst)
		RB_UPLOAD_FALLBACK = 3,  // neutral white / magenta checker
		RB_UPLOAD_CAT_COUNT = 4
	};
	struct UploadProfBucket { unsigned int count; unsigned long long bytes; double ms; };
	UploadProfBucket m_uploadProf[RB_UPLOAD_CAT_COUNT];
	static long long Upload_Prof_Now();          // QPC ticks; 0 when profiling off
	void Upload_Prof_Account(unsigned int cat, unsigned long long bytes, long long t0);
	void Upload_Prof_Emit(unsigned int frame);   // from the flip End_Scene
	// Per-texture identity for the nc bucket: WHICH textures re-upload every
	// bind, so the fix targets the real set instead of a guess. Keyed on
	// Get_ID(); emitted as one "[D3D11 uploadprof-nc]" line per entry after
	// the bucket line, then reset with it.
	enum { RB_UPLOAD_NC_IDS = 64 };
	struct UploadProfNCEntry {
		unsigned int id; char name[96]; unsigned int pool; bool procedural;
		unsigned int w, h, fmt; unsigned int count; unsigned long long bytes;
	};
	UploadProfNCEntry m_uploadProfNC[RB_UPLOAD_NC_IDS];
	unsigned int m_uploadProfNCCount;
	void Upload_Prof_Note_NC(TextureBaseClass * texture, unsigned int w, unsigned int h,
		unsigned int fmt, unsigned long long bytes);

	// Pipeline slice (RENDERER_PORT.md steps 4/5): compile the FF-emulation
	// VS/PS, create the per-object constant buffer + a no-cull rasterizer, and
	// bind the fixed parts of the pipeline. Called from Initialize.
	bool Create_Pipeline_Resources();
	void Release_Pipeline_Resources();
	// Upload the world*view*proj matrix to the constant buffer if it changed.
	void Update_Constant_Buffer();
	// Pack the combiner state into cbCombiner if it changed. Called per draw.
	void Update_Combiner_Buffer();
	// Pack the FF lighting/material and fog state into cbLighting / cbFog if they
	// changed. Called per draw (step 7).
	void Update_Lighting_Buffer();
	void Update_Fog_Buffer();
	// Pack the bone-palette + skinning-enable into cbSkinning if it changed.
	// Called per draw (step 8).
	void Update_Skinning_Buffer();
	// Pack the texgen flags + texture matrices into cbTexGen if they changed.
	// Called per draw.
	void Update_TexGen_Buffer();
	void Release_Texture_Stages();

	ID3D11Device * m_device;
	ID3D11DeviceContext * m_context;
	IDXGISwapChain * m_swapChain;
	ID3D11RenderTargetView * m_backBufferRTV;
	ID3D11Texture2D * m_depthStencilTexture;
	ID3D11DepthStencilView * m_depthStencilView;
	int m_width;
	int m_height;
	// Live viewport extent (tracks Set_Viewport; seeds from the backbuffer).
	// Feeds the D3D8 half-pixel rasterization correction in
	// Update_Constant_Buffer - the offset is half a pixel of the CURRENT
	// viewport, so a viewport change re-dirties the transform.
	int m_viewportWidth;
	int m_viewportHeight;
	long m_initResult; // HRESULT of the last Initialize attempt
	bool m_deviceRemoved; // Present() returned DEVICE_REMOVED/RESET; surfaced via Is_Device_Lost()

	// Vertex/index data streams through a POOL of reusable DYNAMIC buffers. Each
	// buffer carries a GPU completion fence (D3D11_QUERY_EVENT); a buffer is only
	// reused (Map WRITE_DISCARD) once its fence has signalled, so a draw still in
	// flight never has its data overwritten. That corruption was causing GPU
	// hangs (TDR -> device removed). This also removes the per-upload IMMUTABLE
	// buffer allocation that tanked FPS (30 -> 2). Input layouts are cached per FVF.
	// Single DEFAULT vertex/index buffer re-used for the whole frame. Each
	// Upload_Vertices / Upload_Indices16 suballocates a disjoint slice via
	// UpdateSubresource(dst-box) at an advancing offset. CreateBuffer runs ONCE
	// per frame (not once per upload), and the buffer is released at the next
	// Begin_Scene (by then the prior frame's GPU work is done) so no slice is
	// ever overwritten while in flight - that was the Intel AV we hit with
	// per-upload buffers and with DYNAMIC+Map (TDR).
	ID3D11Buffer * m_vertexBuffer;   // current pool slot bound for the next draw
	unsigned int m_vertexFVF;         // FVF of the last uploaded/bound VB
	unsigned int m_unitNormalFVF;     // FVF captured on the first draw after
	                                  // Set_Unit_Normal_Pixel_Shader(true). Any later
	                                  // draw whose FVF differs (e.g. a shadow/decal
	                                  // quad that inherited the bound unit-normal
	                                  // pipeline) is force-reset to the FF combiner.
	ID3D11Buffer * m_indexBuffer;
	ID3D11Buffer * m_dynVB;          // frame vertex staging buffer (DEFAULT)
	unsigned int m_dynVBOffset;      // next free byte offset
	unsigned int m_dynVBCap;         // allocated capacity
	ID3D11Buffer * m_dynIB;          // frame index staging buffer (DEFAULT)
	unsigned int m_dynIBOffset;
	unsigned int m_dynIBCap;
	ID3D11Buffer * m_constantBuffer; // cbPerObject { row_major float4x4 WVP }
	std::map<unsigned int, ID3D11InputLayout *> m_inputLayoutCache;
	ID3D11VertexShader * m_vertexShader;
	ID3D11PixelShader * m_pixelShader;
	ID3D11PixelShader * m_terrainNormalPixelShader; // clean D3D11 terrain normal path (inert until enabled)
	ID3D11PixelShader * m_terrainNormalBlendPixelShader; // terrain normal BLEND (edge crossfade) variant
	ID3D11Buffer * m_terrainBuffer;                 // cbTerrain (pixel-shader register b0)
	bool m_terrainNormalIsActive;                  // tracks terrain-normal pipeline state so the unit-normal hook can skip terrain
	ID3D11PixelShader * m_gradePixelShader;         // W3DNext color-grade post-process (inert until enabled)
	ID3D11VertexShader * m_gradeVertexShader;
	ID3D11Buffer * m_gradeBuffer;                   // cbGrade (pixel-shader register b0)
	ID3D11Texture2D * m_gradeTexture;               // off-screen copy of the finished frame
	ID3D11ShaderResourceView * m_gradeSRV;
	ID3D11SamplerState * m_gradeSampler;
	unsigned int m_gradeWidth;                      // current grade-texture size (recreated on resize)
	unsigned int m_gradeHeight;
bool m_gradeEnabled;                            // master switch (W3DNEXT_GRADE=0 disables)
	bool m_gradeNightVision;                        // F6: green NV-goggle filter (needs grade pass on)
	float m_sunScreen[3];                           // god rays: sun screen uv + intensity (0 = off this frame)
	ID3D11VertexShader * m_unitNormalVertexShader;  // clean D3D11 unit normal path (inert until enabled)
	ID3D11PixelShader * m_unitNormalPixelShader;    // clean D3D11 unit normal path (inert until enabled)
	ID3D11Buffer * m_unitBuffer;                    // cbUnit (pixel-shader register b0)
	bool m_unitNormalIsActive;                      // tracks unit-normal pipeline state to avoid per-category thrash
	float m_unitLastDetail;                         // last detailStrength passed to Set_Unit_Normal_Constants
	float m_unitLastSecondary;                      // last secondaryAdd passed to Set_Unit_Normal_Constants
	ID3D11PixelShader * m_waterPixelShader;         // clean D3D11 water reflection/surface shader (inert until enabled)
	ID3D11Buffer * m_waterBuffer;                   // cbWater (pixel-shader register b0)
	ID3D11RasterizerState * m_rasterizerState;

	// Compiled VS bytecode, retained because CreateInputLayout validates the
	// layout against it. Heap copy so the header needs no ID3DBlob forward decl.
	void * m_vsBytecode;
	unsigned int m_vsBytecodeSize;

	unsigned int m_vertexStride; // bytes per vertex of the bound VB
	unsigned int m_indexCount;   // indices in the bound IB
	bool m_pipelineReady;

	// Transform state feeding the constant buffer. WWMath matrices are
	// column-vector / OpenGL-convention (see FFVertex.hlsl).
	Matrix4x4 m_world;
	Matrix4x4 m_view;
	Matrix4x4 m_proj;
	bool m_transformDirty;

	// Bind-state bookkeeping for the W3D-typed Set_*_Buffer overloads. The
	// pointers are recorded (as the DX8 path records them in its render state);
	// GPU upload from these objects lands when the backend is integrated into
	// the VB/IB classes (see Upload_Vertices / Upload_Indices16).
	const VertexBufferClass * m_boundVertexBuffer;
	unsigned int m_boundVertexStream;
	const IndexBufferClass * m_boundIndexBuffer;
	unsigned int m_indexBaseOffset;

	// --- Texture + combiner slice (RENDERER_PORT.md step 6) -----------------
	// Per-stage GPU texture objects created by Upload_Texture_RGBA and bound to
	// pixel-shader slots [stage]. m_boundTextures records the W3D-typed
	// Set_Texture pointers (bind-state, as the DX8 path records them).
	ID3D11Texture2D * m_stageTexture[RB_MAX_TEXTURE_STAGES];
	ID3D11ShaderResourceView * m_stageSRV[RB_MAX_TEXTURE_STAGES];
	ID3D11SamplerState * m_stageSampler[RB_MAX_TEXTURE_STAGES];
	TextureBaseClass * m_boundTextures[RB_MAX_TEXTURE_STAGES];
	unsigned long long m_boundTexGen[RB_MAX_TEXTURE_STAGES]; // generation of the bound texture (early-out check)
	bool m_stageNeutral[RB_MAX_TEXTURE_STAGES]; // slot holds the neutral-white fallback (null Set_Texture)

	// The shared neutral-white object behind Bind_Neutral_Texture. Device
	// lifetime: released in Release_Device_Objects only (NOT per-stage).
	ID3D11Texture2D * m_neutralTexture;
	ID3D11ShaderResourceView * m_neutralSRV;
	ID3D11SamplerState * m_neutralSampler;
	bool m_neutralCreateFailed; // creation failed once - don't retry per frame

	// --- Uploaded-texture cache ---------------------------------------------
	// Without this, Set_Texture LockRect'd the source surface and created a NEW
	// ID3D11Texture2D + SRV + sampler on EVERY bind - hundreds of GPU resource
	// creations and full texture copies per frame (measured: 12 fps vs DX8's 30).
	// Keyed by the source D3D8 texture pointer plus a content version, so static
	// textures upload once for the process lifetime while a live-updating
	// destination (the shroud) still re-uploads when its content changes.
	struct CachedTexture
	{
		ID3D11Texture2D * tex = nullptr;
		ID3D11ShaderResourceView * srv = nullptr;
		ID3D11SamplerState * sampler = nullptr;
		unsigned long long version = 0;
	};
	std::map<unsigned int, CachedTexture> m_textureCache;
	unsigned int m_texCacheHits;
	unsigned int m_texCacheUploads;
	unsigned int m_texCacheEvictions;

	unsigned int m_frameDrawCalls; // draws issued this frame (reset at End_Scene)

	// Last-bound OM/RS state objects, so Apply_Render_State_Changes can skip the
	// (expensive on Intel) OMSet*/RSSet* calls when the state did not change.
	ID3D11BlendState * m_lastBlendState;
	ID3D11DepthStencilState * m_lastDepthState;
	unsigned int m_lastStencilRef;                  // stencil ref is an OMSet arg, part of the flush key
	ID3D11RasterizerState * m_lastRasterState;

	LARGE_INTEGER m_frameStartT; // Begin_Scene QPC timestamp (render-submit timing)

	// Per-draw hot-path timing accumulators (diagnostic, printed in [frame]).
	double m_dbgApplyMs;
	double m_dbgConstMs;
	double m_dbgOtherMs;
	double m_dbgDrawMs;
	double m_dbgSetTexMs;
	double m_dbgUploadMs;       // Upload_Vertices / Upload_Indices16 cost
	unsigned int m_dbgUploads;  // dynamic buffer uploads this frame
	LARGE_INTEGER m_qpcFreq;

	// Flip-frame counter, incremented once per End_Scene(flip_frame) present. The
	// framedump and the texcache counter line both key off it, so a dumped fNNN
	// names exactly the frame whose draws produced it.
	unsigned int m_flipFrame;

	// True when W3DNEXT_D3D11_FLIP=1 selected the flip-model swapchain at
	// Initialize (see there). Logged on every gpuprof line so a run's swap
	// model is machine-checkable, mirroring the mips= falsifier.
	bool m_flipModel;

	// Cache-toggle verification mode (W3DNEXT_D3D11_TEXCACHE_TOGGLE=1): the uploaded-
	// texture cache is consulted only on even flip frames, so consecutive dumps
	// (e.g. W3DNEXT_FRAMEDUMP_FRAMES=900,901) are a cache-ON/cache-OFF pair rendered
	// ONE frame apart by ONE process.
	//
	// This exists because the obvious oracle - run the game twice, once with the
	// cache on and once off, and diff - does not work: two runs of the same scene
	// diverge on their own (measured 2026-07-25: ~54% of pixels over tol 2, mae
	// ~19, which is the same magnitude as a DX8-vs-D3D11 diff). That noise floor
	// swamps the signal the oracle is looking for. One frame apart in one process,
	// the scene is nearly static (DX8 moves 2.3% of pixels over 600 frames), so a
	// corrupting cache has nowhere to hide.
	bool m_texCacheToggleMode;

	// True when the cache is live for the draws of the frame currently being
	// built. Constant unless m_texCacheToggleMode is on; reported per dumped frame
	// so a diff never has to infer which side of the toggle a dump came from.
	bool Tex_Cache_Enabled_This_Frame() const;

	// Screen-filter backbuffer snapshot (Capture_Backbuffer). Lazily created to
	// match the swapchain backbuffer; the linear/clamp sampler mirrors the
	// sampling state endRenderToTexture forces on DX8.
	ID3D11Texture2D * m_captureTexture;
	ID3D11ShaderResourceView * m_captureSRV;
	ID3D11SamplerState * m_captureSampler;

	// --- Render-to-texture (W3DNext D3D11): water reflection + projected shadows.
	// Keyed by the TextureClass* returned to the engine; Set_Texture detects these
	// so the real D3D11 SRV is sampled instead of the (null) legacy DX8 surface.
	struct RTTTarget
	{
		ID3D11Texture2D *          tex;
		ID3D11RenderTargetView *   rtv;
		ID3D11ShaderResourceView * srv;
		ID3D11SamplerState *       sampler;
		ID3D11Texture2D *          depthTex;
		ID3D11DepthStencilView *   dsv;
		unsigned int               width, height;
	};
	std::map<TextureBaseClass *, RTTTarget> m_rtt;
	ID3D11RenderTargetView * m_savedBackBufferRTV; // non-null == currently rendering to an RT
	ID3D11RenderTargetView * m_currentRTV; // actually-bound RTV (backbuffer or an RTT)
	ID3D11DepthStencilView * m_currentDSV; // actually-bound DSV (mirrors m_currentRTV)

	// Diagnostics only (W3DNEXT_D3D11_DRAWLOG): the last ShaderClass word mirrored by
	// Set_Shader and the D3D8 format of each bound stage texture, so the per-draw
	// log can name the exact engine state a draw ran with. Not consumed by any
	// rendering decision.
	unsigned int m_lastShaderBits;
	unsigned int m_stageTexFormatLog[RB_MAX_TEXTURE_STAGES];

	// CPU-side mirror of cbCombiner (must match the FFPixel.hlsl layout exactly:
	// 16-byte aligned, uint4-packed per-stage arrays).
	struct CombinerConstants
	{
		unsigned int numStages;
		unsigned int pad0[3];
		float tfactor[4];
		unsigned int stageColor[RB_MAX_TEXTURE_STAGES][4]; // op, arg1, arg2, texcoordIndex
		unsigned int stageAlpha[RB_MAX_TEXTURE_STAGES][4]; // op, arg1, arg2, unused
		// Screen-filter monochrome post-op (the BW filter's monochrome.nvp pixel
		// shader): gray = dot(rgb, monoLum), result = lerp(current, gray*monoTint,
		// monoFade). Zero monoEnable = untouched cascade output (the default).
		float monoLum[4];
		float monoTint[4];
		float monoFade[4];
		unsigned int monoEnable;
		unsigned int padm[3];
		// DX8 alpha test (D3DRS_ALPHATESTENABLE/ALPHAREF/ALPHAFUNC), applied to
		// the post-combiner alpha as a discard in FFPixel.hlsl. lessEqual
		// mirrors ShaderClass::Apply's INVSRCALPHA special case; ref is 0..1.
		unsigned int alphaTestEnable;
		unsigned int alphaTestLessEqual;
		float alphaTestRef;
		unsigned int padA;
	};
	CombinerConstants m_combiner;
	ID3D11Buffer * m_combinerBuffer; // cbCombiner (pixel-shader register b0)
	bool m_combinerDirty;

	// --- FF lighting + material + fog slice (RENDERER_PORT.md step 7) --------
	// CPU-side mirror of cbLighting (vertex-shader register b1). Layout MUST match
	// the FFVertex.hlsl cbuffer byte-for-byte: two row_major float4x4 (world, view),
	// four float4 material/ambient colors, two float4[4] light arrays, four uints.
	struct LightingConstants
	{
		float world[16];              // row_major float4x4 World  (normal xform)
		float view[16];               // row_major float4x4 View   (fog depth)
		float globalAmbient[4];       // scene ambient (D3DRS_AMBIENT)
		float matDiffuse[4];          // material diffuse rgb + opacity a
		float matAmbient[4];          // material ambient
		float matEmissive[4];         // material emissive
		float lightDir[RB_MAX_LIGHTS][4];     // world-space travel direction
		float lightDiffuse[RB_MAX_LIGHTS][4]; // light diffuse color
		unsigned int lightingEnable;  // 0 => pass vertex diffuse through
		unsigned int numLights;
		unsigned int diffuseSource;   // RenderBackendColorSource
		unsigned int ambientSource;   // RenderBackendColorSource
	};
	LightingConstants m_lighting;
	ID3D11Buffer * m_lightingBuffer; // cbLighting (vertex-shader register b1)
	bool m_lightingDirty;

	// CPU-side mirror of cbFog (pixel-shader register b1). Matches FFPixel.hlsl.
	struct FogConstants
	{
		float color[4];
		unsigned int enable;
		unsigned int mode;      // RenderBackendFogMode
		float start;
		float end;
		float density;
		float pad[3];
	};
	FogConstants m_fog;
	ID3D11Buffer * m_fogBuffer; // cbFog (pixel-shader register b1)
	bool m_fogDirty;

	// --- GPU skinning slice (RENDERER_PORT.md step 8) -----------------------
	// CPU-side mirror of cbSkinning (vertex-shader register b2). Layout MUST match
	// the FFVertex.hlsl cbuffer byte-for-byte: RB_MAX_BONES row_major float4x4
	// bone matrices, then a uint enable + 3 pad uints (a 16-byte tail).
	struct SkinningConstants
	{
		float bones[RB_MAX_BONES][16]; // bone-palette matrices, 16 row-major floats each
		unsigned int skinningEnable;   // 0 => pass raw position/normal through
		unsigned int pad[3];
		// W3DNext tree sway (see FFVertex.hlsl cbSkinning tail): [0] is the
		// zero no-sway entry, [1..10] the per-type wave vectors. Enabled only
		// around W3DTreeBuffer's draw via Set_Tree_Sway.
		float sway[RB_MAX_SWAY_ENTRIES][4];
		unsigned int swayEnable;
		unsigned int swayPad[3];
	};
	SkinningConstants m_skinning;
	ID3D11Buffer * m_skinningBuffer; // cbSkinning (vertex-shader register b2)
	bool m_skinningDirty;

	// --- FF texgen / texture-matrix slice -----------------------------------
	// CPU-side mirror of cbTexGen (ONE buffer bound at BOTH vertex-shader b3 -
	// which consumes the matrices - and pixel-shader b2 - which reads only the
	// per-stage texgen flag to pick generated over explicit coords). Layout MUST
	// match the FFVertex.hlsl/FFPixel.hlsl cbuffer byte-for-byte: four float4x4
	// then four uint4.
	struct TexGenConstants
	{
		float texMat[RB_MAX_TEXTURE_STAGES][16];        // raw D3D row-vector matrices
		unsigned int texGen[RB_MAX_TEXTURE_STAGES][4];  // x=camera-space, y=matrix enable
	};
	TexGenConstants m_texgen;
	ID3D11Buffer * m_texgenBuffer; // cbTexGen (VS register b3 + PS register b2)
	bool m_texgenDirty;

	// Cached copies for the const Get_Ambient / Get_Fog_Enable queries.
	Vector3 m_ambient;
	bool m_fogEnable;
	// Recorded by Set_Light_Environment (bind-state, as the DX8 path records it).
	LightEnvironmentClass * m_lightEnvironment;

	// --- Blend / depth / rasterizer state-object cache (RENDERER_PORT.md step 9)
	// The legacy render-state vector is the mutable CPU-side state the typed
	// setters write; Apply_Render_State_Changes translates it (via m_stateCache)
	// into the three immutable ID3D11*State objects and binds them. The active
	// pointers are OWNED by the cache (not separately AddRef'd) - the backend only
	// records which ones are currently bound. Zero game-behavior change: nothing
	// binds a cached state until a caller drives the typed setters +
	// Apply_Render_State_Changes, so the existing pass-through draws are untouched.
	RenderStateVector m_renderState;
	bool m_renderStateDirty;
	D3D11StateCache m_stateCache;
	ID3D11BlendState * m_activeBlendState;
	ID3D11DepthStencilState * m_activeDepthState;
	ID3D11RasterizerState * m_activeRasterizerState;
};

// Mirrors a WORLD/VIEW transform into DX8Wrapper's engine-side render_state
// record (no device call - the wrapper's WORLD/VIEW cases are record-only).
// Defined in D3D11Backend_W3D.cpp with the other DX8Wrapper-coupled bodies;
// no-op for PROJECTION/texture kinds. Exists because the sorting renderer
// captures world/view from that record at Insert time.
void Mirror_Transform_To_Wrapper(TransformKind transform, const Matrix4x4 & m);
