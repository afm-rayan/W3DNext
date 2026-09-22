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

// Abstract W3D-facing rendering interface so WW3D2 rendering can be
// re-targeted to other backends while the existing DX8 path stays as the
// reference implementation. Shape (names, file layout, method cut line)
// deliberately mirrors upstream TheSuperHackers PR #2613 (bobtista) so future
// merges stay mechanical.

#pragma once

#include "ww3dformat.h"

// Forward declarations keep this header includable without pulling in the full
// WW3D2 header graph. All W3D types below are passed by pointer or reference.

class ShaderClass;
class VertexMaterialClass;
class TextureBaseClass;
class TextureClass;
class ZTextureClass;
class SurfaceClass;
class VertexBufferClass;
class IndexBufferClass;
class DynamicVBAccessClass;
class DynamicIBAccessClass;
class LightClass;
class LightEnvironmentClass;
class Matrix4x4;
class Matrix3D;
class Vector3;

// Backend-neutral transform selector. Values are sequential and deliberately
// do NOT match any native API's transform ids - each backend maps these to its
// own ids internally (DX8Backend maps them to D3DTS_*).
enum TransformKind
{
	RB_TRANSFORM_WORLD = 0,
	RB_TRANSFORM_VIEW,
	RB_TRANSFORM_PROJECTION,
	RB_TRANSFORM_TEXTURE0,
	RB_TRANSFORM_TEXTURE1,
	RB_TRANSFORM_TEXTURE2,
	RB_TRANSFORM_TEXTURE3,
	RB_TRANSFORM_TEXTURE4,
	RB_TRANSFORM_TEXTURE5,
	RB_TRANSFORM_TEXTURE6,
	RB_TRANSFORM_TEXTURE7
};

// Texture-transform selector for a dynamically computed stage index (0..7).
// The RB_TRANSFORM_TEXTUREn values are contiguous, so this is a plain offset.
inline TransformKind RB_Texture_Transform(unsigned int stage)
{
	return static_cast<TransformKind>(RB_TRANSFORM_TEXTURE0 + stage);
}

struct RenderBackendViewport
{
	unsigned int x;
	unsigned int y;
	unsigned int width;
	unsigned int height;
	float min_z;
	float max_z;
};

// Backend-neutral fixed-function texture-stage combiner semantics (the D3D8
// D3DTSS_COLOROP/ALPHAOP world) expressed WITHOUT any D3D ABI values. A backend
// translates the legacy D3DTSS_* enums into these before they reach it, and maps
// these onto its own emulation (the D3D11 backend maps them 1:1 onto integer op
// codes consumed by the combiner ubershader - see FFPixel.hlsl). Values are
// sequential and deliberately do NOT match D3DTOP_*/D3DTA_*.
//
// Only the ops the real game's ShaderClass path actually drives are modelled
// (MODULATE / SELECTARG1 / ADD / MODULATE2X and the arg set TEXTURE / DIFFUSE /
// CURRENT / TFACTOR); the ~25 exotic D3DTOP_* values are intentionally absent.
enum RenderBackendTexOp
{
	RB_TEXOP_DISABLE = 0,   // stop the cascade (result = current)
	RB_TEXOP_SELECTARG1,    // result = arg1
	RB_TEXOP_SELECTARG2,    // result = arg2
	RB_TEXOP_MODULATE,      // result = arg1 * arg2
	RB_TEXOP_MODULATE2X,    // result = saturate(arg1 * arg2 * 2)
	RB_TEXOP_ADD            // result = saturate(arg1 + arg2)
};

enum RenderBackendTexArg
{
	RB_TEXARG_CURRENT = 0,  // running combiner result (== DIFFUSE at stage 0)
	RB_TEXARG_DIFFUSE,      // interpolated vertex diffuse color
	RB_TEXARG_TEXTURE,      // this stage's sampled texel
	RB_TEXARG_TFACTOR       // the constant texture-factor color
};

// One legacy pre-transformed (D3DFVF_XYZRHW) screen-filter quad, for
// Draw_Screen_Filter_Quad below. `verts` points at the DX8-layout vertices the
// filter code already builds for its raw DrawPrimitiveUP: float4 pos (x,y in
// screen pixels, z, rhw), DWORD diffuse (0xAARRGGBB), then `uv_sets` float2
// texcoord pairs, in triangle-strip order. The bool knobs express the raw
// device state the DX8 filters set around the draw (blend render states, the
// ALPHAOP SELECTARG1 override, the stage-1 mask combiner, the monochrome.nvp
// pixel shader) in backend-neutral terms.
struct RenderBackendFilterQuad
{
	enum BlendMode { BLEND_NONE = 0, BLEND_ALPHA, BLEND_ADDITIVE };
	const void * verts;
	unsigned int vertex_count;   // strip order; 3..8 supported (filters use 4)
	unsigned int stride_bytes;   // bytes per source vertex
	unsigned int uv_sets;        // float2 texcoord sets after the diffuse (1 or 2)
	int blend;                   // BlendMode (DX8: raw SRCBLEND/DESTBLEND states)
	bool use_captured_scene;     // stage 0 samples the Capture_Backbuffer snapshot
	bool alpha_from_diffuse;     // stage-0 alpha = vertex diffuse alpha, ignore texture alpha (DX8: ALPHAOP SELECTARG1(CURRENT))
	bool stage1_mask_modulate;   // stage 1 modulates a mask texture (bound via Set_Texture(1,...)) using uv set 1
	bool monochrome_enable;      // apply the monochrome.nvp post-op after the combiner cascade
	float mono_lum[4];           // luminance dot weights (monochrome.nvp c0)
	float mono_tint[4];          // tint color (c1)
	float mono_fade[4];          // fade lerp factor (c2): 0 = original, 1 = full effect
};

/**
** IRenderBackend
**
** Exposes the high-level subset of DX8Wrapper's public API: the calls that take
** and return W3D types (ShaderClass, TextureBaseClass, Matrix4x4, etc.). The
** low-level D3D8-specific entry points on DX8Wrapper are not exposed here and
** remain reachable only through DX8Wrapper's static methods.
**
** Method names intentionally match the existing DX8Wrapper names so migrating a
** caller is a mechanical DX8Wrapper::X(...) -> g_renderBackend->X(...) rewrite.
** Index/polygon/vertex counts are unsigned int here rather than DX8Wrapper's
** DX8-era unsigned short - backends narrow them as their native API requires.
*/
class IRenderBackend
{
public:
	virtual ~IRenderBackend() {}

	// Optional device lifecycle. DX8Wrapper owns the render device and calls
	// these after the backend is constructed and before it is destroyed. A
	// backend that drives its own device creates it in Initialize and releases
	// it in Shutdown; the DX8 reference backend leaves them as no-ops.
	virtual void Initialize(void * window, int width, int height) {}
	virtual void Shutdown() {}

	virtual bool Is_Device_Lost() const = 0;
	virtual bool Has_Stencil() = 0;
	virtual WW3DFormat Get_Back_Buffer_Format() = 0;
	virtual SurfaceClass * Get_Back_Buffer(unsigned int num) = 0;
	virtual void Set_Gamma(float gamma, float bright, float contrast, bool calibrate = true, bool uselimit = true) = 0;

	virtual void Begin_Scene() = 0;
	virtual void End_Scene(bool flip_frame) = 0;
	virtual void Flip_To_Primary() = 0;
	virtual void Clear(bool clear_color, bool clear_z_stencil, const Vector3 & color, float dest_alpha = 0.0f, float z = 1.0f, unsigned int stencil = 0) = 0;
	virtual void Set_Viewport(const RenderBackendViewport & viewport) = 0;

	virtual void Set_Vertex_Buffer(const VertexBufferClass * vb, unsigned int stream = 0) = 0;
	virtual void Set_Vertex_Buffer(const DynamicVBAccessClass & vba) = 0;
	virtual void Set_Index_Buffer(const IndexBufferClass * ib, unsigned short index_base_offset) = 0;
	virtual void Set_Index_Buffer(const DynamicIBAccessClass & iba, unsigned short index_base_offset) = 0;
	virtual void Set_Index_Buffer_Index_Offset(unsigned int offset) = 0;

	// Write-time capture of dynamic-buffer contents (RENDERER_PORT.md step 10). The
	// engine fills a BUFFER_TYPE_DYNAMIC_DX8 vertex/index buffer through a lock that
	// discards on re-lock, so a backend that owns a separate device (the D3D11 path)
	// cannot read the data back at Set_Vertex_Buffer time. These hooks let the DX8
	// dynamic-lock destructor hand the just-written bytes straight to the backend
	// while the CPU mapping is still valid. `fvf` is the D3DFVF bitmask of the data.
	// Default no-op: the DX8 reference backend ignores them (it draws from the DX8
	// buffer directly), so the default game path is byte-identical.
	virtual void Stage_Dynamic_Vertices(const void * data, unsigned int size_bytes, unsigned int fvf) {}
	virtual void Stage_Dynamic_Indices(const unsigned short * indices, unsigned int count) {}

	virtual void Set_Shader(const ShaderClass & shader) = 0;
	virtual void Get_Shader(ShaderClass & shader) = 0;
	// Override the alpha-test reference programmed by Set_Shader (normalized
	// 0..1, greater-equal convention - the mesh alpha-override fade path).
	// Set_Shader re-programs the shader's own reference, which is the restore.
	// Backend notes: DX8Backend writes the value straight to D3DRS_ALPHAREF
	// (raw pass-through, matching the original game code - the D3D8 runtime
	// applies whatever D3DRS_ALPHAFUNC is current, so no conversion belongs
	// there); D3D11Backend converts to its combiner's internal form, mirroring
	// Set_Shader's LESSEQUAL inversion for INVSRCALPHA blends.
	virtual void Set_Alpha_Reference(float ref) = 0;
	// OM render-target write mask (D3DRS_COLORWRITEENABLE, low 4 bits R/G/B/A).
	// Default no-op: the DX8 backend drives this through the raw D3D8 device, the
	// D3D11 backend honours it (see D3D11Backend::Set_Color_Write_Enable).
	virtual void Set_Color_Write_Enable(unsigned int /*mask*/) {}
	// W3DNext: raw DX8 blend-state writes (D3DRS_ALPHABLENDENABLE / SRCBLEND /
	// DESTBLEND - e.g. the soft-water-edge DESTALPHA blend and the water alpha
	// pass) forwarded as D3DBLEND_* codes. Default no-op: the DX8 backend
	// drives them through the real device; the D3D11 backend overrides.
	virtual void Set_Raw_Blend_Enable(unsigned int /*enable*/) {}
	virtual void Set_Raw_Src_Blend(unsigned int /*d3dblend*/) {}
	virtual void Set_Raw_Dst_Blend(unsigned int /*d3dblend*/) {}
	// Raw stencil render-state write (D3DRS_STENCILENABLE..D3DRS_STENCILWRITEMASK
	// codes) - drives the volumetric shadow passes on the D3D11 backend.
	virtual void Set_Raw_Stencil(unsigned int /*d3drs*/, unsigned int /*value*/) {}
	virtual void Set_Material(const VertexMaterialClass * material) = 0;
	virtual void Set_Texture(unsigned int stage, TextureBaseClass * texture) = 0;

	virtual void Apply_Render_State_Changes() = 0;
	virtual void Apply_Default_State() = 0;
	virtual void Invalidate_Cached_Render_States() = 0;

	virtual void Set_Transform(TransformKind transform, const Matrix4x4 & m) = 0;
	virtual void Set_Transform(TransformKind transform, const Matrix3D & m) = 0;
	virtual void Get_Transform(TransformKind transform, Matrix4x4 & m) = 0;
	virtual void Set_World_Identity() = 0;
	virtual void Set_View_Identity() = 0;
	virtual bool Is_World_Identity() = 0;
	virtual bool Is_View_Identity() = 0;
	virtual void Set_Projection_Transform_With_Z_Bias(const Matrix4x4 & matrix, float znear, float zfar) = 0;

	virtual void Set_Light(unsigned int index, const LightClass & light) = 0;
	// Turn a light slot off again. Mirrors DX8Wrapper::Set_Light(index, nullptr)
	// without letting the raw D3DLIGHT8 pointer overload cross the interface.
	virtual void Disable_Light(unsigned int index) = 0;
	virtual void Set_Ambient(const Vector3 & color) = 0;
	virtual const Vector3 & Get_Ambient() const = 0;
	virtual void Set_Fog(bool enable, const Vector3 & color, float start, float end) = 0;
	virtual bool Get_Fog_Enable() const = 0;
	virtual void Set_Light_Environment(LightEnvironmentClass * light_env) = 0;
	virtual LightEnvironmentClass * Get_Light_Environment() const = 0;

	virtual void Draw_Triangles(
		unsigned int start_index,
		unsigned int polygon_count,
		unsigned int min_vertex_index,
		unsigned int vertex_count) = 0;
	virtual void Draw_Triangles(
		unsigned int buffer_type,
		unsigned int start_index,
		unsigned int polygon_count,
		unsigned int min_vertex_index,
		unsigned int vertex_count) = 0;
	// NOTE the count contract: primitive_count is the number of TRIANGLES
	// (DX8 DrawIndexedPrimitive semantics - every caller passes indexCount-2),
	// NOT the number of indices. A strip consumes primitive_count + 2 indices.
	virtual void Draw_Strip(
		unsigned int start_index,
		unsigned int primitive_count,
		unsigned int min_vertex_index,
		unsigned int vertex_count) = 0;

	// The shader id is treated as an opaque unsigned long.
	virtual void Set_Vertex_Shader(unsigned long vertex_shader) = 0;
	virtual void Set_Pixel_Shader(unsigned long pixel_shader) = 0;
	virtual void Set_Vertex_Shader_Constant(int reg, const void * data, int count) = 0;
	virtual void Set_Pixel_Shader_Constant(int reg, const void * data, int count) = 0;

	virtual TextureClass * Create_Render_Target(int width, int height, WW3DFormat format = WW3D_FORMAT_UNKNOWN) = 0;
	virtual void Set_Render_Target_With_Z(TextureClass * texture, ZTextureClass * ztexture = nullptr) = 0;
	virtual bool Is_Render_To_Texture() = 0;
	virtual void Set_Shadow_Map(int idx, ZTextureClass * ztex) = 0;
	virtual ZTextureClass * Get_Shadow_Map(int idx) = 0;

	// --- Screen-filter support ----------------------------------------------
	// The W3DShaderManager screen filters (mission BW fade, motion blur,
	// crossfade) grab the just-rendered scene and re-draw it as pre-transformed
	// full-viewport quads with filter-specific combiner/pixel-shader state. On
	// DX8 they do this on the raw device (render-to-texture redirect + raw
	// DrawPrimitiveUP); that byte-identical path stays untouched and these
	// default to no-ops there. A backend without the raw path overrides them:
	// Capture_Backbuffer snapshots the current backbuffer into a backend-owned
	// texture, Draw_Screen_Filter_Quad draws one legacy screen-space quad with
	// the captured scene (and the requested filter behaviors) applied.
	virtual void Capture_Backbuffer() {}
	virtual void Draw_Screen_Filter_Quad(const RenderBackendFilterQuad & quad) {}

	// --- GPU profiling ------------------------------------------------------
	// Drop a named timestamp into the backend's command stream. A profiling
	// backend (D3D11, under W3DNEXT_D3D11_GPUPROF=1) resolves consecutive markers
	// into per-span GPU times; span cost is attributed to the LATER marker's
	// label. `label` must be a string literal (stored by pointer, not copied).
	// Default no-op: the DX8 reference path stays byte-identical, and the D3D11
	// path is a no-op too unless profiling is enabled - callers may mark
	// unconditionally.
	virtual void Gpu_Profile_Marker(const char * label) {}

	// Read_Back_Buffer copies the current backbuffer as tightly-packed top-down
	// RGB24 into rgb_dst (caller allocates width*height*3 bytes). Query mode:
	// rgb_dst == nullptr fills width/height and returns whether readback is
	// supported without copying. Returns false on backends without a CPU
	// readback (the DX8 path keeps its legacy D3D8 surface copy).
	virtual bool Read_Back_Buffer(unsigned char * rgb_dst, unsigned int & width, unsigned int & height)
	{
		(void)rgb_dst; (void)width; (void)height;
		return false;
	}
};
