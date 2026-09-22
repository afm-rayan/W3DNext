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

// Direct3D 11 backend skeleton (RENDERER_PORT.md step 3). Real device
// lifecycle - device + immediate context + swapchain + backbuffer RTV +
// depth-stencil + viewport - and real Clear / Begin_Scene / End_Scene /
// Present. Every rendering-state and draw virtual is a stub for later steps.
//
// Stub convention: a stub body is exactly
//     D3D11_STUB();
// (optionally followed by a safe default return). D3D11_STUB expands to
// nothing in release; with WWDEBUG it can be flipped to a WWDEBUG_SAY trace.
// Stubs must never assert-crash - the skeleton stays linkable and callable.

#include "D3D11Backend.h"

#include "D3D11FVF.h"
#include "RenderBackend.h"   // W3DNext_GetEnv (diagnostic env knobs)
#include "Shaders/D3D11FFShaders.h"

#include "vector3.h"
#include "matrix3d.h"
#include "texture.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

// d3dcompiler is also wired into the CMake link inputs (ww3d2 + the smoke
// target); the pragma keeps the dependency self-documenting and covers any
// consumer that compiles this .cpp directly.
#pragma comment(lib, "d3dcompiler.lib")

// ----------------------------------------------------------------------------
// Recon instrumentation (RENDERER_PORT.md step 10): one-shot stub-hit tracing.
//
// When this backend is the live one (only under -gfxBackend d3d11 - the game
// never constructs it otherwise), each distinct stubbed / partial virtual is
// recorded the FIRST time it fires, in first-hit order, to a capturable sink:
// the file named by env W3DNEXT_D3D11_LOG (else "d3d11_backend.log" in the process
// CWD) plus OutputDebugString. This turns "which IRenderBackend methods does the
// real bring-up path actually hit" into a machine-collectable list. Recon-only,
// not thread-safe; costs one std::set lookup per call after the first.
// ----------------------------------------------------------------------------
namespace
{
void D3D11_Log_Line(const char * line)
{
	const char * path = W3DNext_GetEnv("D3D11_LOG");
	// Diagnostics are opt-in. The implicit default file caused End_Scene's
	// per-frame statistics to open, write, flush and close a file every frame.
	if (path == nullptr || path[0] == '\0') {
		return;
	}
	FILE * f = std::fopen(path, "a");
	if (f != nullptr) {
		std::fputs(line, f);
		std::fputc('\n', f);
		std::fclose(f);
	}
	OutputDebugStringA(line);
	OutputDebugStringA("\n");
}

// Record method `name` once (deduped, first-hit order). `note` classifies it:
// "stub" (D3D11_STUB no-op), "partial" (records bind-state but no GPU work),
// "no-op" (real method that early-returns because a prerequisite is missing).
void D3D11_Trace_Once(const char * name, const char * note)
{
	static std::set<std::string> seen;
	if (seen.insert(name).second) {
		char buf[320];
		std::snprintf(buf, sizeof(buf), "[D3D11 hit #%u] %s (%s)",
			static_cast<unsigned>(seen.size()), name, note);
		D3D11_Log_Line(buf);
	}
}
}

// Gates the per-draw/per-texture-bind/per-upload QueryPerformanceCounter
// instrumentation (m_dbgApplyMs, m_dbgConstMs, m_dbgSetTexMs, m_dbgUploadMs,
// etc.). Those timers only ever get READ once a frame in End_Scene's stats
// line, and that line is itself only written when D3D11_LOG is set - so for
// everyone NOT actively debugging, every one of those QueryPerformanceCounter
// calls was pure overhead, and it scales with draw-call count: a busy battle
// with hundreds of units can issue thousands of draws + texture binds a
// frame, i.e. tens of thousands of QPC calls purely to fill in numbers nobody
// reads. Checked once (lazy static), so the disabled (default) path costs a
// single already-resolved branch per call site - no getenv, no QPC. Declared
// in D3D11Backend.h (not static) so D3D11Backend_W3D.cpp's Set_Texture can
// use the same gate.
bool Profiling_Enabled()
{
	static const bool s_enabled = [] {
		const char * e = W3DNext_GetEnv("D3D11_PROFILE");
		return e != nullptr && e[0] == '1';
	}();
	return s_enabled;
}

// Stub marker (see file header): now traces the enclosing method name once when
// a not-yet-implemented virtual is called on the live D3D11 backend.
#define D3D11_STUB() D3D11_Trace_Once(__FUNCTION__, "stub")

// Trace for a method that DOES run but only records bind-state (no GPU work yet)
// or early-returns for a missing prerequisite - so the inventory captures the
// real gaps the D3D11_STUB() macro alone would miss.
#define D3D11_TRACE_PARTIAL() D3D11_Trace_Once(__FUNCTION__, "partial: records bind-state, no GPU work")
#define D3D11_TRACE_NOOP(reason) D3D11_Trace_Once(__FUNCTION__, reason)

namespace
{
template <typename T> void Safe_Release(T *& ptr)
{
	if (ptr != nullptr) {
		ptr->Release();
		ptr = nullptr;
	}
}

// True if the layout already carries an element with this semantic name+index.
bool Layout_Has_Semantic(const D3D11InputLayoutDesc & layout, const char * name, unsigned int index)
{
	for (unsigned int i = 0; i < layout.num_elements; ++i) {
		if (layout.elements[i].SemanticIndex == index &&
			layout.elements[i].SemanticName != nullptr &&
			std::strcmp(layout.elements[i].SemanticName, name) == 0) {
			return true;
		}
	}
	return false;
}

// Append a placeholder input element aliased onto offset 0. The FF-emulation VS
// input signature is fixed (POSITION, COLOR0, TEXCOORD0, TEXCOORD1), but
// CreateInputLayout requires the layout to supply EVERY input the VS reads - and
// a given FVF may omit the color or some texcoords. These aliases satisfy the
// validator; the aliased components are never consumed (flat geometry ignores
// the sampled path, and the combiner samples only active stages), so reusing the
// position bytes is harmless. Does not touch the real vertex stride.
void Append_Alias_Element(D3D11InputLayoutDesc & layout, const char * name, unsigned int index, DXGI_FORMAT format)
{
	if (layout.num_elements >= W3D_FVF_MAX_INPUT_ELEMENTS) {
		return;
	}
	D3D11_INPUT_ELEMENT_DESC & e = layout.elements[layout.num_elements++];
	e.SemanticName = name;
	e.SemanticIndex = index;
	e.Format = format;
	e.InputSlot = 0;
	e.AlignedByteOffset = 0; // alias the position bytes; value is unused
	e.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
	e.InstanceDataStepRate = 0;
}

bool Matrix_Is_Identity(const Matrix4x4 & m)
{
	static const Matrix4x4 identity(true); // Matrix4x4(true) == identity
	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			if (m[i][j] != identity[i][j]) {
				return false;
			}
		}
	}
	return true;
}
}

D3D11Backend::D3D11Backend()
	: m_device(nullptr)
	, m_context(nullptr)
	, m_swapChain(nullptr)
	, m_backBufferRTV(nullptr)
	, m_depthStencilTexture(nullptr)
	, m_depthStencilView(nullptr)
	, m_width(0)
	, m_height(0)
	, m_viewportWidth(0)
	, m_viewportHeight(0)
	, m_initResult(S_OK)
	, m_deviceRemoved(false)
	, m_vertexBuffer(nullptr)
	, m_vertexFVF(0u)
	, m_unitNormalFVF(0xFFFFFFFFu)  // sentinel: not yet captured
	, m_indexBuffer(nullptr)
	, m_constantBuffer(nullptr)
	, m_vertexShader(nullptr)
	, m_pixelShader(nullptr)
	, m_terrainNormalPixelShader(nullptr)
	, m_terrainNormalBlendPixelShader(nullptr)
	, m_terrainBuffer(nullptr)
	, m_terrainNormalIsActive(false)
	, m_gradePixelShader(nullptr)
	, m_gradeVertexShader(nullptr)
	, m_gradeBuffer(nullptr)
	, m_gradeTexture(nullptr)
	, m_gradeSRV(nullptr)
	, m_gradeSampler(nullptr)
	, m_gradeWidth(0)
	, m_gradeHeight(0)
	, m_gradeEnabled(false)
	, m_gradeNightVision(false)
	, m_sunScreen{ 0.5f, 0.5f, 0.0f }
	, m_lastStencilRef(0)
	, m_unitNormalVertexShader(nullptr)
	, m_unitNormalPixelShader(nullptr)
	, m_unitBuffer(nullptr)
	, m_unitNormalIsActive(false)
	, m_unitLastDetail(0.0f)
	, m_unitLastSecondary(0.0f)
	, m_waterPixelShader(nullptr)
	, m_waterBuffer(nullptr)
	, m_rasterizerState(nullptr)
	, m_vsBytecode(nullptr)
	, m_vsBytecodeSize(0)
	, m_vertexStride(0)
	, m_indexCount(0)
	, m_pipelineReady(false)
	, m_world(true)   // identity
	, m_view(true)    // identity
	, m_proj(true)    // identity
	, m_transformDirty(true)
	, m_boundVertexBuffer(nullptr)
	, m_boundVertexStream(0)
	, m_boundIndexBuffer(nullptr)
	, m_indexBaseOffset(0)
	, m_combinerBuffer(nullptr)
	, m_combinerDirty(true)
	, m_lightingBuffer(nullptr)
	, m_lightingDirty(true)
	, m_fogBuffer(nullptr)
	, m_fogDirty(true)
	, m_skinningBuffer(nullptr)
	, m_skinningDirty(true)
	, m_texgenBuffer(nullptr)
	, m_texgenDirty(true)
	, m_ambient(0.0f, 0.0f, 0.0f)
	, m_fogEnable(false)
	, m_lightEnvironment(nullptr)
	, m_texCacheHits(0)
	, m_frameDrawCalls(0)
	, m_frameStartT()
	, m_dbgApplyMs(0.0)
	, m_dbgConstMs(0.0)
	, m_dbgOtherMs(0.0)
	, m_dbgDrawMs(0.0)
	, m_dbgSetTexMs(0.0)
	, m_dbgUploadMs(0.0)
	, m_dbgUploads(0)
	, m_dynVB(nullptr)
	, m_dynVBOffset(0)
	, m_dynVBCap(0)
	, m_dynIB(nullptr)
	, m_dynIBOffset(0)
	, m_dynIBCap(0)
	, m_lastBlendState(nullptr)
	, m_lastDepthState(nullptr)
	, m_lastRasterState(nullptr)
	, m_texCacheUploads(0)
	, m_texCacheEvictions(0)
	, m_flipFrame(0)
	, m_flipModel(false)
	, m_texCacheToggleMode([] {
		const char * e = W3DNext_GetEnv("D3D11_TEXCACHE_TOGGLE");
		return e != nullptr && e[0] == '1';
	}())
	, m_renderStateDirty(true)
	, m_activeBlendState(nullptr)
	, m_activeDepthState(nullptr)
	, m_activeRasterizerState(nullptr)
	, m_savedBackBufferRTV(nullptr)
	, m_currentRTV(nullptr)
	, m_currentDSV(nullptr)
{
	QueryPerformanceFrequency(&m_qpcFreq);
	m_lastShaderBits = 0;
	for (int i = 0; i < RB_MAX_TEXTURE_STAGES; ++i) {
		m_stageTexture[i] = nullptr;
		m_stageSRV[i] = nullptr;
		m_stageSampler[i] = nullptr;
		m_boundTextures[i] = nullptr;
		m_boundTexGen[i] = 0;
		m_stageNeutral[i] = false;
		m_stageTexFormatLog[i] = 0;
	}
	m_neutralTexture = nullptr;
	m_neutralSRV = nullptr;
	m_neutralSampler = nullptr;
	m_neutralCreateFailed = false;
	m_captureTexture = nullptr;
	m_captureSRV = nullptr;
	m_captureSampler = nullptr;
	std::memset(m_gpuProfRing, 0, sizeof(m_gpuProfRing));
	std::memset(m_gpuProfSpans, 0, sizeof(m_gpuProfSpans));
	m_gpuProfWrite = 0;
	m_gpuProfOpen = false;
	m_gpuProfSpanCount = 0;
	m_gpuProfFramesAccum = 0;
	std::memset(m_uploadProf, 0, sizeof(m_uploadProf));
	std::memset(m_uploadProfNC, 0, sizeof(m_uploadProfNC));
	m_uploadProfNCCount = 0;
	// Default combiner: no stages -> the ubershader passes vertex diffuse through
	// (the flat/green-triangle path). Args default to the DX8 idle state.
	std::memset(&m_combiner, 0, sizeof(m_combiner));
	m_combiner.numStages = 0;
	for (int i = 0; i < RB_MAX_TEXTURE_STAGES; ++i) {
		m_combiner.stageColor[i][0] = RB_TEXOP_DISABLE;
		m_combiner.stageColor[i][1] = RB_TEXARG_TEXTURE;
		m_combiner.stageColor[i][2] = RB_TEXARG_DIFFUSE;
		m_combiner.stageColor[i][3] = static_cast<unsigned int>(i); // texcoord index
		m_combiner.stageAlpha[i][0] = RB_TEXOP_DISABLE;
		m_combiner.stageAlpha[i][1] = RB_TEXARG_TEXTURE;
		m_combiner.stageAlpha[i][2] = RB_TEXARG_DIFFUSE;
		m_combiner.stageAlpha[i][3] = 0;
	}

	// Default FF lighting state: DISABLED, so the VS passes the vertex diffuse
	// through unchanged (the pre-step-7 behavior every existing assert relies on).
	// World/View seed to identity so the fog depth is valid before any transform
	// is set. Material sources default to the DX8 Apply_Default_State() values:
	// diffuse from COLOR1 (vertex), ambient from the material constant.
	std::memset(&m_lighting, 0, sizeof(m_lighting));
	for (int i = 0; i < 4; ++i) {
		m_lighting.world[i * 4 + i] = 1.0f; // identity
		m_lighting.view[i * 4 + i] = 1.0f;  // identity
	}
	m_lighting.matDiffuse[3] = 1.0f; // opaque by default
	m_lighting.lightingEnable = 0;
	m_lighting.numLights = 0;
	m_lighting.diffuseSource = RB_MATSRC_VERTEX;   // D3DMCS_COLOR1
	m_lighting.ambientSource = RB_MATSRC_MATERIAL; // D3DMCS_MATERIAL

	// Default fog: DISABLED, linear.
	std::memset(&m_fog, 0, sizeof(m_fog));
	m_fog.enable = 0;
	m_fog.mode = RB_FOG_LINEAR;
	m_fog.start = 0.0f;
	m_fog.end = 1.0f;
	m_fog.density = 1.0f;

	// Default skinning: DISABLED, so the VS passes the raw position/normal through
	// (the pre-step-8 behavior every existing assert relies on). Seed every bone to
	// identity so an accidental enable can't collapse geometry to the origin.
	std::memset(&m_skinning, 0, sizeof(m_skinning));
	m_skinning.skinningEnable = 0;
	for (int b = 0; b < RB_MAX_BONES; ++b) {
		for (int i = 0; i < 4; ++i) {
			m_skinning.bones[b][i * 4 + i] = 1.0f; // identity
		}
	}

	// Default texgen: every stage OFF (explicit uv0/uv1 pass through, the
	// pre-texgen behavior every existing assert relies on), matrices identity.
	std::memset(&m_texgen, 0, sizeof(m_texgen));
	for (int t = 0; t < RB_MAX_TEXTURE_STAGES; ++t) {
		for (int i = 0; i < 4; ++i) {
			m_texgen.texMat[t][i * 4 + i] = 1.0f; // identity
		}
	}
}

D3D11Backend::~D3D11Backend()
{
	Release_Device_Objects();
}

// ----------------------------------------------------------------------------
//
// Device lifecycle (real)
//
// ----------------------------------------------------------------------------

void D3D11Backend::Initialize(void * window, int width, int height)
{
	if (m_device != nullptr) {
		return; // already initialized
	}

	HWND hwnd = static_cast<HWND>(window);
	m_width = width;
	m_height = height;

	DXGI_SWAP_CHAIN_DESC scd;
	ZeroMemory(&scd, sizeof(scd));
	scd.BufferDesc.Width = width;
	scd.BufferDesc.Height = height;
	// B8G8R8A8 matches the byte layout the engine calls WW3D_FORMAT_A8R8G8B8.
	scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	scd.SampleDesc.Count = 1;
	scd.SampleDesc.Quality = 0;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.BufferCount = 1;
	scd.OutputWindow = hwnd;
	scd.Windowed = TRUE;
	scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	// Flip-model swapchain (default ON since 2026-08-20): the legacy single-
	// buffered blt DISCARD below serializes Present against DWM composition -
	// measured (gpuprof, 2026-07-26): ~85% of the D3D11 frame's wall time sits
	// INSIDE Present (~41-52ms of a 48-60ms frame at 2560x1440 windowed). That
	// makes the whole port feel GPU-bound at ~10-15 FPS even on trivial scenes.
	// Flip-discard + 2 buffers removes the DWM stall; it was env-gated
	// (W3DNEXT_D3D11_FLIP=1) for A/B parity testing and is now the default.
	// W3DNEXT_D3D11_FLIP=0 keeps the legacy blt path.
	{
		const char * e = W3DNext_GetEnv("D3D11_FLIP");
		m_flipModel = (e == nullptr || e[0] == '\0' || e[0] != '0');
		if (m_flipModel) {
			scd.BufferCount = 2;   // flip model requires >= 2 buffers
			scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		}
	}

	// Feature level 10_0+ is acceptable for the fixed-function-emulation port.
	const D3D_FEATURE_LEVEL feature_levels[] = {
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
	};

	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

	D3D_FEATURE_LEVEL got_level = D3D_FEATURE_LEVEL_10_0;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(
		nullptr,                    // default adapter
		D3D_DRIVER_TYPE_HARDWARE,
		nullptr,                    // no software rasterizer module
		flags,
		feature_levels,
		ARRAYSIZE(feature_levels),
		D3D11_SDK_VERSION,
		&scd,
		&m_swapChain,
		&m_device,
		&got_level,
		&m_context);
	m_initResult = hr;
	if (FAILED(hr)) {
		Release_Device_Objects();
		return;
	}

	// Backbuffer render-target view.
	ID3D11Texture2D * back_buffer = nullptr;
	hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back_buffer));
	if (SUCCEEDED(hr)) {
		hr = m_device->CreateRenderTargetView(back_buffer, nullptr, &m_backBufferRTV);
		back_buffer->Release();
	}
	if (FAILED(hr)) {
		m_initResult = hr;
		Release_Device_Objects();
		return;
	}

	// Depth-stencil texture + view. D24S8 mirrors the DX8 path's expectations
	// (Has_Stencil answers true on the strength of this).
	D3D11_TEXTURE2D_DESC dsd;
	ZeroMemory(&dsd, sizeof(dsd));
	dsd.Width = width;
	dsd.Height = height;
	dsd.MipLevels = 1;
	dsd.ArraySize = 1;
	dsd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	dsd.SampleDesc.Count = 1;
	dsd.SampleDesc.Quality = 0;
	dsd.Usage = D3D11_USAGE_DEFAULT;
	dsd.BindFlags = D3D11_BIND_DEPTH_STENCIL;

	hr = m_device->CreateTexture2D(&dsd, nullptr, &m_depthStencilTexture);
	if (SUCCEEDED(hr)) {
		hr = m_device->CreateDepthStencilView(m_depthStencilTexture, nullptr, &m_depthStencilView);
	}
	if (FAILED(hr)) {
		m_initResult = hr;
		Release_Device_Objects();
		return;
	}

	Bind_Back_Buffer_Targets();

	// Steps 4/5: compile the FF-emulation shaders and create the geometry
	// pipeline. A failure here leaves the device usable for the clear-only
	// path (step 3); m_pipelineReady gates the draw path.
	m_pipelineReady = Create_Pipeline_Resources();
}

void D3D11Backend::Shutdown()
{
	Release_Device_Objects();
}

void D3D11Backend::Bind_Back_Buffer_Targets()
{
	if (m_context == nullptr) {
		return;
	}

	m_context->OMSetRenderTargets(1, &m_backBufferRTV, m_depthStencilView);
	m_currentRTV = m_backBufferRTV;
	m_currentDSV = m_depthStencilView;

	D3D11_VIEWPORT vp;
	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.Width = static_cast<float>(m_width);
	vp.Height = static_cast<float>(m_height);
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	m_context->RSSetViewports(1, &vp);
	if (m_viewportWidth != m_width || m_viewportHeight != m_height) {
		m_viewportWidth = m_width;
		m_viewportHeight = m_height;
		m_transformDirty = true; // half-pixel offset depends on the viewport extent
	}
}

void D3D11Backend::Release_Device_Objects()
{
	if (m_context != nullptr) {
		m_context->ClearState();
	}
	Release_Pipeline_Resources();
	Release_Texture_Cache();
	for (unsigned int s = 0; s < RB_GPUPROF_RING; ++s) {
		Safe_Release(m_gpuProfRing[s].disjoint);
		for (unsigned int i = 0; i < RB_GPUPROF_MAX_MARKS; ++i) {
			Safe_Release(m_gpuProfRing[s].ts[i]);
		}
		m_gpuProfRing[s].count = 0;
		m_gpuProfRing[s].inFlight = false;
	}
	m_gpuProfOpen = false;
	Safe_Release(m_neutralSampler);
	Safe_Release(m_neutralSRV);
	Safe_Release(m_neutralTexture);
	m_neutralCreateFailed = false;
	Safe_Release(m_captureSampler);
	Safe_Release(m_captureSRV);
	Safe_Release(m_captureTexture);
	for (auto & kv : m_rtt) {
		RTTTarget & t = kv.second;
		Safe_Release(t.sampler);
		Safe_Release(t.dsv);
		Safe_Release(t.depthTex);
		Safe_Release(t.srv);
		Safe_Release(t.rtv);
		Safe_Release(t.tex);
	}
	m_rtt.clear();
	m_savedBackBufferRTV = nullptr;
	m_currentRTV = nullptr;
	m_currentDSV = nullptr;
	Safe_Release(m_depthStencilView);
	Safe_Release(m_depthStencilTexture);
	Safe_Release(m_backBufferRTV);
	Safe_Release(m_swapChain);
	Safe_Release(m_context);
	Safe_Release(m_device);
}

// ----------------------------------------------------------------------------
//
// Geometry + transform pipeline (RENDERER_PORT.md steps 4/5)
//
// ----------------------------------------------------------------------------

bool D3D11Backend::Create_Pipeline_Resources()
{
	if (m_device == nullptr) {
		return false;
	}

	// --- Compile the fixed-function-emulation VS + PS at runtime -------------
	UINT compile_flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG) || defined(WWDEBUG)
	compile_flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

	ID3DBlob * vs_blob = nullptr;
	ID3DBlob * ps_blob = nullptr;
	ID3DBlob * errors = nullptr;

	HRESULT hr = D3DCompile(
		kFFVertexShaderHLSL, std::strlen(kFFVertexShaderHLSL),
		"FFVertex.hlsl", nullptr, nullptr,
		"main", "vs_4_0", compile_flags, 0, &vs_blob, &errors);
	if (FAILED(hr)) {
		Safe_Release(errors);
		Safe_Release(vs_blob);
		return false;
	}
	Safe_Release(errors);

	hr = D3DCompile(
		kFFPixelShaderHLSL, std::strlen(kFFPixelShaderHLSL),
		"FFPixel.hlsl", nullptr, nullptr,
		"main", "ps_4_0", compile_flags, 0, &ps_blob, &errors);
	if (FAILED(hr)) {
		Safe_Release(errors);
		Safe_Release(ps_blob);
		Safe_Release(vs_blob);
		return false;
	}
	Safe_Release(errors);

	hr = m_device->CreateVertexShader(
		vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_vertexShader);
	if (SUCCEEDED(hr)) {
		hr = m_device->CreatePixelShader(
			ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_pixelShader);
	}
	if (FAILED(hr)) {
		Safe_Release(ps_blob);
		Safe_Release(vs_blob);
		return false;
	}

	// Retain a heap copy of the VS bytecode - CreateInputLayout validates the
	// FVF-derived layout against it (once per uploaded vertex format).
	m_vsBytecodeSize = static_cast<unsigned int>(vs_blob->GetBufferSize());
	m_vsBytecode = new (std::nothrow) unsigned char[m_vsBytecodeSize];
	if (m_vsBytecode != nullptr) {
		std::memcpy(m_vsBytecode, vs_blob->GetBufferPointer(), m_vsBytecodeSize);
	}
	Safe_Release(ps_blob);
	Safe_Release(vs_blob);

	// --- Terrain normal-mapping pixel shader (clean D3D11 path) --------------
	// Compiled from kTerrainNormalPixelShaderHLSL. Failure is non-fatal: the
	// terrain normal path simply stays inert and the FF combiner keeps driving
	// terrain (the same behaviour as the DX8 token path, which this backend
	// stubs). The game-side terrain normal feature enables it explicitly.
	ID3DBlob * tps_blob = nullptr;
	ID3DBlob * tps_err = nullptr;
	HRESULT tps_hr = D3DCompile(
		kTerrainNormalPixelShaderHLSL, std::strlen(kTerrainNormalPixelShaderHLSL),
		"TerrainNormalPixel.hlsl", nullptr, nullptr,
		// ps_4_1: CalculateLevelOfDetail (used to mip-match the blend-edge
		// neighbour samples) requires shader model 4.1; every FL10.1+ GPU has it.
		"main", "ps_4_1", compile_flags, 0, &tps_blob, &tps_err);
	if (FAILED(tps_hr) && tps_err != nullptr) {
		// Silent failure here kills terrain relief AND the god-ray sun feed.
		D3D11_Log_Line("[D3D11] TerrainNormal ps_4_1 compile FAILED:");
		D3D11_Log_Line(static_cast<const char *>(tps_err->GetBufferPointer()));
	}
	Safe_Release(tps_err);
	if (SUCCEEDED(tps_hr)) {
		tps_hr = m_device->CreatePixelShader(
			tps_blob->GetBufferPointer(), tps_blob->GetBufferSize(), nullptr, &m_terrainNormalPixelShader);
	}
	Safe_Release(tps_blob);
	if (FAILED(tps_hr)) {
		m_terrainNormalPixelShader = nullptr;
	}

	// --- Terrain normal-mapping BLEND (edge crossfade) pixel shader ---------
	// Compiled from kTerrainNormalBlendPixelShaderHLSL. Same as the normal
	// terrain shader but crossfades the normal (and base colour) across texture
	// boundaries and outputs the per-vertex blend weight as alpha. Used by the
	// separate renderNormalEdgeBlend() pass.
	ID3DBlob * tpsb_blob = nullptr;
	ID3DBlob * tpsb_err = nullptr;
	HRESULT tpsb_hr = D3DCompile(
		kTerrainNormalBlendPixelShaderHLSL, std::strlen(kTerrainNormalBlendPixelShaderHLSL),
		"TerrainNormalBlendPixel.hlsl", nullptr, nullptr,
		"main", "ps_4_0", compile_flags, 0, &tpsb_blob, &tpsb_err);
	Safe_Release(tpsb_err);
	if (SUCCEEDED(tpsb_hr)) {
		tpsb_hr = m_device->CreatePixelShader(
			tpsb_blob->GetBufferPointer(), tpsb_blob->GetBufferSize(), nullptr, &m_terrainNormalBlendPixelShader);
	}
	Safe_Release(tpsb_blob);
	if (FAILED(tpsb_hr)) {
		m_terrainNormalBlendPixelShader = nullptr;
	}

	// --- W3DNext color-grading post-process ----------------------------------
	// Full-screen pass run at End_Frame before Present. Non-fatal on failure.
	// ON by default; W3DNEXT_GRADE=0 disables entirely. The '5' key toggles it
	// live at runtime for instant A/B comparison (edge-detected).
	{
		const char * ge = W3DNext_GetEnv("W3DNEXT_GRADE");
		m_gradeEnabled = !(ge != nullptr && ge[0] == '0');
		ID3DBlob * gvs_blob = nullptr;
		ID3DBlob * gps_blob = nullptr;
		ID3DBlob * g_err = nullptr;
		HRESULT g_hr = D3DCompile(
			kColorGradeVertexShaderHLSL, std::strlen(kColorGradeVertexShaderHLSL),
			"ColorGradeVertex.hlsl", nullptr, nullptr,
			"main", "vs_4_0", compile_flags, 0, &gvs_blob, &g_err);
		Safe_Release(g_err);
		if (SUCCEEDED(g_hr)) {
			g_hr = m_device->CreateVertexShader(
				gvs_blob->GetBufferPointer(), gvs_blob->GetBufferSize(), nullptr, &m_gradeVertexShader);
		}
		Safe_Release(gvs_blob);
		g_hr = D3DCompile(
			kColorGradePixelShaderHLSL, std::strlen(kColorGradePixelShaderHLSL),
			"ColorGradePixel.hlsl", nullptr, nullptr,
			"main", "ps_4_0", compile_flags, 0, &gps_blob, &g_err);
		if (FAILED(g_hr) && g_err != nullptr) {
			// Surface HLSL compile errors - a silent failure here kills the
			// whole grade pass (no grading, no NV, no god rays).
			D3D11_Log_Line("[D3D11] ColorGradePixel compile FAILED:");
			D3D11_Log_Line(static_cast<const char *>(g_err->GetBufferPointer()));
		}
		Safe_Release(g_err);
		if (SUCCEEDED(g_hr)) {
			g_hr = m_device->CreatePixelShader(
				gps_blob->GetBufferPointer(), gps_blob->GetBufferSize(), nullptr, &m_gradePixelShader);
		}
		Safe_Release(gps_blob);
		if (FAILED(g_hr)) {
			m_gradePixelShader = nullptr;
			m_gradeVertexShader = nullptr;
		}
		D3D11_BUFFER_DESC gcbd;
		ZeroMemory(&gcbd, sizeof(gcbd));
		gcbd.ByteWidth = sizeof(float) * 20; // 80 bytes (5 vec4), a 16-byte multiple
		gcbd.Usage = D3D11_USAGE_DEFAULT;
		gcbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		if (FAILED(m_device->CreateBuffer(&gcbd, nullptr, &m_gradeBuffer))) {
			m_gradeBuffer = nullptr;
			m_gradeEnabled = false;
		}
		D3D11_SAMPLER_DESC gsd;
		ZeroMemory(&gsd, sizeof(gsd));
		gsd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		gsd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		gsd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		gsd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		gsd.MaxAnisotropy = 1;
		gsd.ComparisonFunc = D3D11_COMPARISON_NEVER;
		gsd.MinLOD = 0.0f;
		gsd.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(m_device->CreateSamplerState(&gsd, &m_gradeSampler))) {
			m_gradeSampler = nullptr;
			m_gradeEnabled = false;
		}
	}

	// --- Unit/model normal-mapping shaders (clean D3D11 path) ----------------
	// Compiled from kUnitNormalVertexShaderHLSL + kUnitNormalPixelShaderHLSL.
	// Failure is non-fatal: the unit normal path stays inert and the FF combiner
	// keeps driving units (the same graceful fallback as the terrain normal
	// path). The game-side unit normal feature enables it per mesh explicitly.
	ID3DBlob * unvs_blob = nullptr;
	ID3DBlob * unvs_err = nullptr;
	HRESULT unvs_hr = D3DCompile(
		kUnitNormalVertexShaderHLSL, std::strlen(kUnitNormalVertexShaderHLSL),
		"UnitNormalVertex.hlsl", nullptr, nullptr,
		"main", "vs_4_0", compile_flags, 0, &unvs_blob, &unvs_err);
	if (FAILED(unvs_hr)) {
		if (unvs_err != nullptr) {
			const char * msg = (const char *)unvs_err->GetBufferPointer();
			FILE * dbg = fopen("unitnorm.log", "a");
			if (dbg) { fprintf(dbg, "UNIT_VS_COMPILE_ERROR: %s\n", msg); fclose(dbg); }
		}
		Safe_Release(unvs_err);
		m_unitNormalVertexShader = nullptr;
	} else {
		unvs_hr = m_device->CreateVertexShader(
			unvs_blob->GetBufferPointer(), unvs_blob->GetBufferSize(), nullptr, &m_unitNormalVertexShader);
		if (FAILED(unvs_hr)) {
			m_unitNormalVertexShader = nullptr;
		}
		Safe_Release(unvs_blob);
	}
	ID3DBlob * unps_blob = nullptr;
	ID3DBlob * unps_err = nullptr;
	HRESULT unps_hr = D3DCompile(
		kUnitNormalPixelShaderHLSL, std::strlen(kUnitNormalPixelShaderHLSL),
		"UnitNormalPixel.hlsl", nullptr, nullptr,
		"main", "ps_4_0", compile_flags, 0, &unps_blob, &unps_err);
	if (FAILED(unps_hr) && unps_err != nullptr) {
		const char * msg = (const char *)unps_err->GetBufferPointer();
		FILE * dbg = fopen("unitnorm.log", "a");
		if (dbg) { fprintf(dbg, "UNIT_PS_COMPILE_ERROR: %s\n", msg); fclose(dbg); }
	}
	Safe_Release(unps_err);
	if (SUCCEEDED(unps_hr)) {
		unps_hr = m_device->CreatePixelShader(
			unps_blob->GetBufferPointer(), unps_blob->GetBufferSize(), nullptr, &m_unitNormalPixelShader);
	}
	Safe_Release(unps_blob);
	if (FAILED(unps_hr)) {
		m_unitNormalPixelShader = nullptr;
	}
	{
		FILE * dbg = fopen("unitnorm.log", "a");
		if (dbg) { fprintf(dbg, "UNIT_SHADER_STATUS VS=%d PS=%d\n",
			m_unitNormalVertexShader ? 1 : 0, m_unitNormalPixelShader ? 1 : 0); fclose(dbg); }
	}
	// --- Water reflection/surface pixel shader (clean D3D11 path) -------------
	// Compiled from kWaterPixelShaderHLSL (sun glint + planar reflection).
	// Non-fatal: on failure m_waterPixelShader stays null and W3DWater.cpp leaves
	// the FF combiner active for water, so water stays flat-but-visible (matching the
	// terrain/unit normal shader fallback).
	ID3DBlob * wps_blob = nullptr;
	ID3DBlob * wps_err = nullptr;
	HRESULT wps_hr = D3DCompile(
		kWaterPixelShaderHLSL, std::strlen(kWaterPixelShaderHLSL),
		"WaterPixel.hlsl", nullptr, nullptr,
		"main", "ps_4_0", compile_flags, 0, &wps_blob, &wps_err);
	Safe_Release(wps_err);
	if (SUCCEEDED(wps_hr)) {
		wps_hr = m_device->CreatePixelShader(
			wps_blob->GetBufferPointer(), wps_blob->GetBufferSize(), nullptr, &m_waterPixelShader);
	}
	Safe_Release(wps_blob);
	if (FAILED(wps_hr)) {
		m_waterPixelShader = nullptr;
	}
	{
		FILE * dbg = fopen("water.log", "a");
		if (dbg) { fprintf(dbg, "WATER_SHADER_STATUS compile=%d create=%d PS=%d buffer=%d\n", (int)wps_hr, (m_waterPixelShader ? 1 : 0), (m_waterPixelShader ? 1 : 0), (m_waterBuffer ? 1 : 0)); fclose(dbg); }
	}

	if (m_vsBytecode == nullptr) {
		return false;
	}

	// --- Per-object constant buffer (row_major float4x4 WorldViewProj) -------
	D3D11_BUFFER_DESC cbd;
	ZeroMemory(&cbd, sizeof(cbd));
	cbd.ByteWidth = sizeof(float) * 16; // 64 bytes, a 16-byte multiple
	cbd.Usage = D3D11_USAGE_DEFAULT;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	hr = m_device->CreateBuffer(&cbd, nullptr, &m_constantBuffer);
	if (FAILED(hr)) {
		return false;
	}

	// --- Combiner constant buffer (cbCombiner, pixel-shader register b0) -----
	// CPU-side CombinerConstants mirrors the FFPixel.hlsl cbuffer byte-for-byte.
	D3D11_BUFFER_DESC ccd;
	ZeroMemory(&ccd, sizeof(ccd));
	ccd.ByteWidth = sizeof(CombinerConstants); // 16-byte multiple by construction
	ccd.Usage = D3D11_USAGE_DEFAULT;
	ccd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	hr = m_device->CreateBuffer(&ccd, nullptr, &m_combinerBuffer);
	if (FAILED(hr)) {
		return false;
	}

	// --- Lighting constant buffer (cbLighting, vertex-shader register b1) -----
	// CPU-side LightingConstants mirrors the FFVertex.hlsl cbuffer byte-for-byte
	// (step 7). Drives the FF lighting equation.
	D3D11_BUFFER_DESC lcd;
	ZeroMemory(&lcd, sizeof(lcd));
	lcd.ByteWidth = sizeof(LightingConstants); // 336 bytes, a 16-byte multiple
	lcd.Usage = D3D11_USAGE_DEFAULT;
	lcd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	hr = m_device->CreateBuffer(&lcd, nullptr, &m_lightingBuffer);
	if (FAILED(hr)) {
		return false;
	}

	// --- Fog constant buffer (cbFog, pixel-shader register b1) ---------------
	D3D11_BUFFER_DESC fcd;
	ZeroMemory(&fcd, sizeof(fcd));
	fcd.ByteWidth = sizeof(FogConstants); // 48 bytes, a 16-byte multiple
	fcd.Usage = D3D11_USAGE_DEFAULT;
	fcd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	hr = m_device->CreateBuffer(&fcd, nullptr, &m_fogBuffer);
	if (FAILED(hr)) {
		return false;
	}

	// --- Skinning constant buffer (cbSkinning, vertex-shader register b2) -----
	// CPU-side SkinningConstants mirrors the FFVertex.hlsl cbuffer byte-for-byte
	// (step 8): RB_MAX_BONES row_major float4x4 + a uint4 tail (enable + pad).
	D3D11_BUFFER_DESC skd;
	ZeroMemory(&skd, sizeof(skd));
	skd.ByteWidth = sizeof(SkinningConstants); // RB_MAX_BONES*64 + 16, a 16-byte multiple
	skd.Usage = D3D11_USAGE_DEFAULT;
	skd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	hr = m_device->CreateBuffer(&skd, nullptr, &m_skinningBuffer);
	if (FAILED(hr)) {
		return false;
	}

	// --- TexGen constant buffer (cbTexGen, VS register b3 + PS register b2) ---
	// CPU-side TexGenConstants mirrors the FFVertex.hlsl/FFPixel.hlsl cbuffer
	// byte-for-byte. One buffer, bound at both stages below.
	D3D11_BUFFER_DESC tgd;
	ZeroMemory(&tgd, sizeof(tgd));
	tgd.ByteWidth = sizeof(TexGenConstants); // 320 bytes, a 16-byte multiple
	tgd.Usage = D3D11_USAGE_DEFAULT;
	tgd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	hr = m_device->CreateBuffer(&tgd, nullptr, &m_texgenBuffer);
	if (FAILED(hr)) {
		return false;
	}

	// --- Terrain normal constant buffer (cbTerrain, pixel-shader register b0) -
	// Carries sun direction, sun color, ambient and flags to the terrain HLSL
	// shader. Non-fatal: if it fails the terrain normal path is disabled.
	D3D11_BUFFER_DESC tcbd;
	ZeroMemory(&tcbd, sizeof(tcbd));
	tcbd.ByteWidth = sizeof(float) * 16; // 64 bytes, a 16-byte multiple
	tcbd.Usage = D3D11_USAGE_DEFAULT;
	tcbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	hr = m_device->CreateBuffer(&tcbd, nullptr, &m_terrainBuffer);
	if (FAILED(hr)) {
		m_terrainBuffer = nullptr;
		m_terrainNormalPixelShader = nullptr;
	}

	// --- Unit normal constant buffer (cbUnit, pixel-shader register b0) ------
	// Carries sun direction/color, ambient, camera position and Blinn-Phong
	// params to the unit HLSL shader. Non-fatal: if it fails the unit normal
	// path is disabled and units fall back to the FF pipeline.
	D3D11_BUFFER_DESC ucbd;
	ZeroMemory(&ucbd, sizeof(ucbd));
	ucbd.ByteWidth = sizeof(float) * 16 * 8; // 128 bytes (8 vec4), a 16-byte multiple
	ucbd.Usage = D3D11_USAGE_DEFAULT;
	ucbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	hr = m_device->CreateBuffer(&ucbd, nullptr, &m_unitBuffer);
	if (FAILED(hr)) {
		m_unitBuffer = nullptr;
		m_unitNormalVertexShader = nullptr;
		m_unitNormalPixelShader = nullptr;
	}

	// --- Water constant buffer (cbWater, pixel-shader register b0) ------------
	// Carries sun direction/color, wave bump strength, reflection factor, sun
	// glitter and a time value to kWaterPixelShaderHLSL. Mirrors the unit buffer
	// (which over-allocates); non-fatal - on failure water falls back to the FF
	// combiner (flat, but visible).
	{
		D3D11_BUFFER_DESC wcbd;
		ZeroMemory(&wcbd, sizeof(wcbd));
		wcbd.ByteWidth = sizeof(float) * 16 * 4; // 256 bytes (Params, ViewportSize, SunDir, SunColor)
		wcbd.Usage = D3D11_USAGE_DEFAULT;
		wcbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		hr = m_device->CreateBuffer(&wcbd, nullptr, &m_waterBuffer);
		if (FAILED(hr)) {
			m_waterBuffer = nullptr;
			m_waterPixelShader = nullptr;
		}
	}

	// --- No-cull solid rasterizer (skeleton draws either winding) -----------
	D3D11_RASTERIZER_DESC rsd;
	ZeroMemory(&rsd, sizeof(rsd));
	rsd.FillMode = D3D11_FILL_SOLID;
	rsd.CullMode = D3D11_CULL_NONE;
	rsd.FrontCounterClockwise = FALSE;
	rsd.DepthClipEnable = TRUE;
	hr = m_device->CreateRasterizerState(&rsd, &m_rasterizerState);
	if (FAILED(hr)) {
		return false;
	}

	// Bind the fixed parts of the pipeline once. Input layout + VB/IB are bound
	// per upload; primitive topology per draw.
	m_context->VSSetShader(m_vertexShader, nullptr, 0);
	m_context->PSSetShader(m_pixelShader, nullptr, 0);
	m_context->VSSetConstantBuffers(0, 1, &m_constantBuffer);
	m_context->VSSetConstantBuffers(1, 1, &m_lightingBuffer);
	m_context->VSSetConstantBuffers(2, 1, &m_skinningBuffer);
	m_context->VSSetConstantBuffers(3, 1, &m_texgenBuffer);
	m_context->PSSetConstantBuffers(0, 1, &m_combinerBuffer);
	m_context->PSSetConstantBuffers(1, 1, &m_fogBuffer);
	m_context->PSSetConstantBuffers(2, 1, &m_texgenBuffer);
	m_context->RSSetState(m_rasterizerState);

	m_transformDirty = true;
	m_combinerDirty = true;
	m_lightingDirty = true;
	m_fogDirty = true;
	m_skinningDirty = true;
	m_texgenDirty = true;
	Update_Combiner_Buffer(); // seed the default (pass-through) combiner state
	Update_Lighting_Buffer(); // seed the default (lighting-off) state
	Update_Fog_Buffer();      // seed the default (fog-off) state
	Update_Skinning_Buffer(); // seed the default (skinning-off, identity bones) state
	Update_TexGen_Buffer();   // seed the default (texgen-off, identity matrices) state
	return true;
}

void D3D11Backend::Release_Texture_Stages()
{
	for (int i = 0; i < RB_MAX_TEXTURE_STAGES; ++i) {
		Safe_Release(m_stageSampler[i]);
		Safe_Release(m_stageSRV[i]);
		Safe_Release(m_stageTexture[i]);
		m_boundTextures[i] = nullptr;
		m_stageNeutral[i] = false;
	}
}

void D3D11Backend::Release_Pipeline_Resources()
{
	// Release every cached blend/depth/raster object before the device goes away
	// (the cache owns them). Null the active pointers - they aliased cache-owned
	// objects, so they must not be released again here.
	m_stateCache.Release_All();
	m_activeBlendState = nullptr;
	m_activeDepthState = nullptr;
	m_activeRasterizerState = nullptr;
	m_renderStateDirty = true;
	Release_Texture_Stages();
	Safe_Release(m_texgenBuffer);
	Safe_Release(m_skinningBuffer);
	Safe_Release(m_fogBuffer);
	Safe_Release(m_lightingBuffer);
	Safe_Release(m_combinerBuffer);
	Safe_Release(m_rasterizerState);
	Safe_Release(m_constantBuffer);
	Safe_Release(m_indexBuffer);
	Safe_Release(m_vertexBuffer);
	Safe_Release(m_dynVB);
	m_dynVBCap = 0;
	m_dynVBOffset = 0;
	Safe_Release(m_dynIB);
	m_dynIBCap = 0;
	m_dynIBOffset = 0;
	for (auto & kv : m_inputLayoutCache) {
		Safe_Release(kv.second);
	}
	m_inputLayoutCache.clear();
	Safe_Release(m_pixelShader);
	Safe_Release(m_terrainNormalPixelShader);
	Safe_Release(m_terrainNormalBlendPixelShader);
	Safe_Release(m_terrainBuffer);
	Safe_Release(m_gradePixelShader);
	Safe_Release(m_gradeVertexShader);
	Safe_Release(m_gradeBuffer);
	Safe_Release(m_gradeSRV);
	Safe_Release(m_gradeTexture);
	Safe_Release(m_gradeSampler);
	Safe_Release(m_unitNormalVertexShader);
	Safe_Release(m_unitNormalPixelShader);
	Safe_Release(m_unitBuffer);
	Safe_Release(m_waterPixelShader);
	Safe_Release(m_waterBuffer);
	Safe_Release(m_vertexShader);
	if (m_vsBytecode != nullptr) {
		delete[] static_cast<unsigned char *>(m_vsBytecode);
		m_vsBytecode = nullptr;
	}
	m_vsBytecodeSize = 0;
	m_vertexStride = 0;
	m_indexCount = 0;
	m_pipelineReady = false;
}

void D3D11Backend::Update_Constant_Buffer()
{
	if (!m_transformDirty || m_constantBuffer == nullptr || m_context == nullptr) {
		return;
	}

	// Column-vector concatenation: clip = Proj * View * World * v. WWMath stores
	// element (i,j) at float offset i*4+j (row-major), which the HLSL side reads
	// as a row_major float4x4 and multiplies mul(M, v) - the same math.
	Matrix4x4 wvp = m_proj * m_view * m_world;

	// D3D8 half-pixel rasterization convention. The engine's screen-space
	// coordinates were authored against D3D8's viewport mapping, which places
	// the same clip-space geometry half a pixel LEFT/UP of where D3D10+ puts
	// it - measured directly on frame-matched menu framedumps (parity log,
	// 95a959d round + the sign experiment in this round: shifting the D3D11
	// image RIGHT/DOWN by 0.5px is what converges on DX8). Cancel it with a
	// post-projection nudge of +half a pixel in screen space, expressed as a
	// clip-space offset scaled by clip.w: clip.x += w/W (right), clip.y -= w/H
	// (clip y is up, screen y is down). Folding it into the WVP rows costs
	// nothing per-vertex and covers every draw - 3D and the 2D menu path share
	// this one matrix.
	if (m_viewportWidth > 0 && m_viewportHeight > 0) {
		const float ox =  1.0f / static_cast<float>(m_viewportWidth);
		const float oy = -1.0f / static_cast<float>(m_viewportHeight);
		for (int j = 0; j < 4; ++j) {
			wvp[0][j] += ox * wvp[3][j];
			wvp[1][j] += oy * wvp[3][j];
		}
	}
	m_context->UpdateSubresource(m_constantBuffer, 0, nullptr, &wvp, 0, 0);
	m_transformDirty = false;
}

void D3D11Backend::Set_Terrain_Normal_Pixel_Shader(bool enable)
{
	m_terrainNormalIsActive = enable;
	if (m_context == nullptr) {
		return;
	}
	// Rainbow-artifact fix: this function only ever swaps the PIXEL shader.
	// If a unit-normal draw ran just before this (VSSetShader(m_unitNormalVertexShader)
	// + m_unitNormalIsActive=true), leaving unit-normal mode "on" while we bind the
	// terrain/FF pixel shader here desyncs the VS/PS pair: the VS still outputs
	// worldPos/worldNormal in TEXCOORD0/1, but the newly bound PS reads TEXCOORD0
	// as a UV -> texture sampled at world-space coordinates -> chaotic per-pixel
	// colours ("rainbow"). Force the unit-normal pair fully off first (no-op if
	// it wasn't active) so VS and PS are always a matched pair.
	Set_Unit_Normal_Pixel_Shader(false, 0.0f, 0.0f);
	if (enable && m_terrainNormalPixelShader != nullptr) {
		m_context->PSSetShader(m_terrainNormalPixelShader, nullptr, 0);
		m_context->PSSetConstantBuffers(0, 1, &m_terrainBuffer);
		m_context->PSSetConstantBuffers(1, 1, &m_fogBuffer);
	} else {
		// Restore the FF combiner pipeline (mirrors the Create_Pipeline_Resources bind).
		m_context->PSSetShader(m_pixelShader, nullptr, 0);
		m_context->PSSetConstantBuffers(0, 1, &m_combinerBuffer);
		m_context->PSSetConstantBuffers(1, 1, &m_fogBuffer);
		m_context->PSSetConstantBuffers(2, 1, &m_texgenBuffer);
	}
}

void D3D11Backend::Set_Terrain_Normal_Blend_Pixel_Shader(bool enable)
{
	m_terrainNormalIsActive = enable;
	if (m_context == nullptr) {
		return;
	}
	// Same VS/PS desync fix as Set_Terrain_Normal_Pixel_Shader above - see the
	// comment there. This path binds a different terrain PS but has the exact
	// same "PS-only swap while unit-normal VS is still bound" hazard.
	Set_Unit_Normal_Pixel_Shader(false, 0.0f, 0.0f);
	if (enable && m_terrainNormalBlendPixelShader != nullptr) {
		m_context->PSSetShader(m_terrainNormalBlendPixelShader, nullptr, 0);
		m_context->PSSetConstantBuffers(0, 1, &m_terrainBuffer);
		m_context->PSSetConstantBuffers(1, 1, &m_fogBuffer);
	} else {
		// Restore the FF combiner pipeline.
		m_context->PSSetShader(m_pixelShader, nullptr, 0);
		m_context->PSSetConstantBuffers(0, 1, &m_combinerBuffer);
		m_context->PSSetConstantBuffers(1, 1, &m_fogBuffer);
		m_context->PSSetConstantBuffers(2, 1, &m_texgenBuffer);
	}
}

void D3D11Backend::Set_Terrain_Normal_Constants(const Vector3 & sunDir, const Vector3 & sunColor,
                                                const Vector3 & ambient, bool useLightMap)
{
	if (m_context == nullptr || m_terrainBuffer == nullptr) {
		return;
	}
	struct TerrainConstants { float SunDir[4]; float SunColor[4]; float Ambient[4]; float Params[4]; };
	TerrainConstants tc;
	tc.SunDir[0] = sunDir.X;   tc.SunDir[1] = sunDir.Y;   tc.SunDir[2] = sunDir.Z;   tc.SunDir[3] = 0.0f;
	tc.SunColor[0] = sunColor.X; tc.SunColor[1] = sunColor.Y; tc.SunColor[2] = sunColor.Z; tc.SunColor[3] = 1.0f;
	tc.Ambient[0] = ambient.X;  tc.Ambient[1] = ambient.Y;  tc.Ambient[2] = ambient.Z;  tc.Ambient[3] = 1.0f;
	tc.Params[0] = useLightMap ? 1.0f : 0.0f;
	// TerrainParams.y/z/w drive the enhanced terrain look (specular sun glint +
	// micro-shadow). Kept here so they can be tuned without touching the HLSL.
	static const float kSpecStrength = 0.55f;   // y: overall glint intensity
	static const float kMicroShadow  = 0.45f;   // z: crevice darkening strength
	static const float kSpecShininess = 56.0f;  // w: glint softness (higher = wider, calmer sheen)
	tc.Params[1] = kSpecStrength; tc.Params[2] = kMicroShadow; tc.Params[3] = kSpecShininess;
	m_context->UpdateSubresource(m_terrainBuffer, 0, nullptr, &tc, 0, 0);
}

void D3D11Backend::Set_Unit_Normal_Pixel_Shader(bool enable, float detailStrength, float secondaryAdd)
{
	if (m_context == nullptr) {
		return;
	}
	if (enable && m_unitNormalVertexShader != nullptr && m_unitNormalPixelShader != nullptr) {
		// Only perform the (relatively expensive) VS/PS pipeline switch when
		// transitioning INTO unit-normal mode. Consecutive unit categories therefore
		// share one switch per frame instead of thrashing VS/PS state. The constant
		// buffer (sun/ambient/alpha-test) IS refreshed on every enabling call so each
		// unit category gets its own up-to-date alpha-test state.
		if (!m_unitNormalIsActive) {
			m_context->VSSetShader(m_unitNormalVertexShader, nullptr, 0);
			m_context->PSSetShader(m_unitNormalPixelShader, nullptr, 0);
			m_context->VSSetConstantBuffers(0, 1, &m_constantBuffer);
			m_context->VSSetConstantBuffers(1, 1, &m_lightingBuffer);
			m_context->VSSetConstantBuffers(2, 1, &m_skinningBuffer);
			m_context->VSSetConstantBuffers(3, 1, &m_texgenBuffer);
			m_context->PSSetConstantBuffers(0, 1, &m_unitBuffer);
			m_context->PSSetConstantBuffers(1, 1, &m_fogBuffer);
		}
		// Derive sun direction / color / ambient from the configured FF lighting
		// (the same source that currently lights units), so the unit normal path
		// is consistent with the rest of the scene. Camera position is taken from
		// the view matrix inside Set_Unit_Normal_Constants.
		Vector3 sunDir(-m_lighting.lightDir[0][0], -m_lighting.lightDir[0][1], -m_lighting.lightDir[0][2]);
		float sunLen = sunDir.X * sunDir.X + sunDir.Y * sunDir.Y + sunDir.Z * sunDir.Z;
		if (sunLen < 1e-6f) { sunDir.Set(0.5f, -0.8f, 0.3f); }
		sunDir.Normalize();
		Vector3 sunColor(m_lighting.lightDiffuse[0][0], m_lighting.lightDiffuse[0][1], m_lighting.lightDiffuse[0][2]);
		float sunColLen = sunColor.X * sunColor.X + sunColor.Y * sunColor.Y + sunColor.Z * sunColor.Z;
		if (sunColLen < 1e-6f) { sunColor.Set(1.0f, 1.0f, 1.0f); }
		Vector3 ambient(m_lighting.globalAmbient[0], m_lighting.globalAmbient[1], m_lighting.globalAmbient[2]);
		float ambLen = ambient.X * ambient.X + ambient.Y * ambient.Y + ambient.Z * ambient.Z;
		if (ambLen < 1e-6f) { ambient.Set(0.3f, 0.3f, 0.3f); }

		Set_Unit_Normal_Constants(sunDir, sunColor, ambient, detailStrength, secondaryAdd);
		m_unitNormalIsActive = true;
		// Mark the captured unit-normal FVF as "not yet known". The first Draw_Triangles
		// while active captures the real FVF; any later draw with a different FVF
		// (shadow/decal quads that inherited the bound pipeline) is force-reset.
		m_unitNormalFVF = 0xFFFFFFFFu;
	} else {
		// Restore the FF combiner pipeline (mirrors the Create_Pipeline_Resources bind).
		// Guarded so non-unit categories between unit groups do not re-thrash state.
		if (m_unitNormalIsActive) {
			m_context->VSSetShader(m_vertexShader, nullptr, 0);
			m_context->PSSetShader(m_pixelShader, nullptr, 0);
			m_context->VSSetConstantBuffers(0, 1, &m_constantBuffer);
			m_context->VSSetConstantBuffers(1, 1, &m_lightingBuffer);
			m_context->VSSetConstantBuffers(2, 1, &m_skinningBuffer);
			m_context->VSSetConstantBuffers(3, 1, &m_texgenBuffer);
			m_context->PSSetConstantBuffers(0, 1, &m_combinerBuffer);
			m_context->PSSetConstantBuffers(1, 1, &m_fogBuffer);
			m_context->PSSetConstantBuffers(2, 1, &m_texgenBuffer);
			m_unitNormalIsActive = false;
			m_unitNormalFVF = 0u;
		}
	}
}

void D3D11Backend::Set_Unit_Normal_Constants(const Vector3 & sunDir, const Vector3 & sunColor,
                                             const Vector3 & ambient, float detailStrength, float secondaryAdd)
{
	if (m_context == nullptr || m_unitBuffer == nullptr) {
		return;
	}
	// Derive the camera world position from the current view matrix
	// (world->view). For V = R * w + t with t = V(0), the camera position c
	// satisfies V(c) = 0 => c = -R^T * t. WWMath stores the matrix row-major.
	float * v = reinterpret_cast<float *>(&m_view);
	float tx = v[12], ty = v[13], tz = v[14];
	float camX = -(v[0] * tx + v[1] * ty + v[2] * tz);
	float camY = -(v[4] * tx + v[5] * ty + v[6] * tz);
	float camZ = -(v[8] * tx + v[9] * ty + v[10] * tz);
	struct UnitConstants {
		float SunDir[4]; float SunColor[4]; float AmbientColor[4]; float CameraPos[4];
		float SpecularColor[4]; float Params[4]; float MatTint[4]; float Params2[4];
		float Alpha[4];  // x=enable, y=lessEqual, z=ref (mirrors DX8 alpha test)
	};
	UnitConstants uc;
	uc.SunDir[0] = sunDir.X; uc.SunDir[1] = sunDir.Y; uc.SunDir[2] = sunDir.Z; uc.SunDir[3] = 0.0f;
	uc.SunColor[0] = sunColor.X; uc.SunColor[1] = sunColor.Y; uc.SunColor[2] = sunColor.Z; uc.SunColor[3] = 1.0f;
	uc.AmbientColor[0] = ambient.X; uc.AmbientColor[1] = ambient.Y; uc.AmbientColor[2] = ambient.Z; uc.AmbientColor[3] = 1.0f;
	uc.CameraPos[0] = camX; uc.CameraPos[1] = camY; uc.CameraPos[2] = camZ; uc.CameraPos[3] = 1.0f;
	uc.SpecularColor[0] = 1.0f; uc.SpecularColor[1] = 1.0f; uc.SpecularColor[2] = 1.0f; uc.SpecularColor[3] = 1.0f;
	uc.Params[0] = 32.0f;  // specular power (shininess)
	uc.Params[1] = 1.0f;   // normal intensity (full perturbation)
	uc.Params[2] = 1.0f;   // enable specular
	uc.Params[3] = 1.0f;   // enable normal map (caller binds it to t2)
	uc.MatTint[0] = 1.0f; uc.MatTint[1] = 1.0f; uc.MatTint[2] = 1.0f; uc.MatTint[3] = 1.0f;
	uc.Params2[0] = detailStrength;  // post-detail multiplicative strength (0 = off)
	uc.Params2[1] = secondaryAdd;    // secondary gradient (team color) add enable
	uc.Params2[2] = 0.0f; uc.Params2[3] = 0.0f;
	m_unitLastDetail = detailStrength;
	m_unitLastSecondary = secondaryAdd;
	// Mirror the current DX8 alpha-test state so the unit-normal pixel shader can
	// discard cutout texels exactly like the FF combiner (fixes transparent fences
	// and other alpha-tested geometry showing solid under the normal pipeline).
	uc.Alpha[0] = (float)m_combiner.alphaTestEnable;
	uc.Alpha[1] = (float)m_combiner.alphaTestLessEqual;
	uc.Alpha[2] = m_combiner.alphaTestRef;
	uc.Alpha[3] = 0.0f;
	// W3DNext: avoid re-uploading the unit-normal constant buffer every time this
	// is called (once per unit category per frame -> a GPU upload stall that tanks
	// FPS). Only push to the GPU when the contents actually changed.
	static UnitConstants s_lastUnit;
	static bool s_unitInit = false;
	if (s_unitInit && std::memcmp(&uc, &s_lastUnit, sizeof(UnitConstants)) == 0) {
		return;
	}
	s_lastUnit = uc;
	s_unitInit = true;
	m_context->UpdateSubresource(m_unitBuffer, 0, nullptr, &uc, 0, 0);
}

void D3D11Backend::Set_Water_Pixel_Shader(bool enable)
{
	if (m_context == nullptr) {
		return;
	}
	// Same VS/PS desync fix as Set_Terrain_Normal_Pixel_Shader - see the comment
	// there. Water draws use the FF vertex shader, so if a unit-normal draw ran
	// immediately before and left m_unitNormalIsActive true, this PS-only swap
	// would leave the unit-normal VS bound underneath the water PS.
	Set_Unit_Normal_Pixel_Shader(false, 0.0f, 0.0f);
	if (enable && m_waterPixelShader != nullptr) {
		// Activate the water reflection/surface pixel shader. cbWater is bound at b3
		// (NOT b0): Draw_Triangles internally re-uploads the WVP/combiner buffers to
		// b0/b1/b2 for EVERY draw, so binding cbWater at b0 would be clobbered right
		// before the water tile draws -> all-zero Params -> flat reflection. b3 is
		// untouched by the FF constant updates, so the water params survive.
		m_context->PSSetShader(m_waterPixelShader, nullptr, 0);
		m_context->PSSetConstantBuffers(3, 1, &m_waterBuffer);
		m_context->PSSetConstantBuffers(1, 1, &m_fogBuffer);
		m_context->PSSetConstantBuffers(2, 1, &m_texgenBuffer);
	} else {
		// Restore the FF combiner pipeline so the next (non-water) draw - including the
		// post-water shroud pass - samples its own textures with the FF PS.
		m_context->PSSetShader(m_pixelShader, nullptr, 0);
		m_context->PSSetConstantBuffers(0, 1, &m_combinerBuffer);
		m_context->PSSetConstantBuffers(1, 1, &m_fogBuffer);
		m_context->PSSetConstantBuffers(2, 1, &m_texgenBuffer);
	}
}

void D3D11Backend::Set_Water_Constants(const Vector3 & sunDir, const Vector3 & sunColor,
                                       float bumpScale, float reflectionFactor, float sunGlitter,
                                       float time, unsigned viewportW, unsigned viewportH)
{
	if (m_context == nullptr || m_waterBuffer == nullptr) {
		return;
	}
	struct WaterConstants
	{
		float Params[4];
		float ViewportSize[4];
		float SunDir[4];
		float SunColor[4];
	};
	WaterConstants wc;
	wc.Params[0] = bumpScale;
	wc.Params[1] = reflectionFactor;
	wc.Params[2] = sunGlitter;
	wc.Params[3] = time;
	wc.ViewportSize[0] = static_cast<float>(viewportW);
	wc.ViewportSize[1] = static_cast<float>(viewportH);
	wc.ViewportSize[2] = viewportW > 0 ? (1.0f / static_cast<float>(viewportW)) : 0.0f;
	wc.ViewportSize[3] = viewportH > 0 ? (1.0f / static_cast<float>(viewportH)) : 0.0f;
	wc.SunDir[0] = sunDir.X; wc.SunDir[1] = sunDir.Y; wc.SunDir[2] = sunDir.Z; wc.SunDir[3] = 0.0f;
	wc.SunColor[0] = sunColor.X; wc.SunColor[1] = sunColor.Y; wc.SunColor[2] = sunColor.Z; wc.SunColor[3] = 1.0f;
	m_context->UpdateSubresource(m_waterBuffer, 0, nullptr, &wc, 0, 0);
}

void D3D11Backend::Update_Combiner_Buffer()
{
	if (!m_combinerDirty || m_combinerBuffer == nullptr || m_context == nullptr) {
		return;
	}
	m_context->UpdateSubresource(m_combinerBuffer, 0, nullptr, &m_combiner, 0, 0);
	m_combinerDirty = false;
}

void D3D11Backend::Update_Lighting_Buffer()
{
	if (!m_lightingDirty || m_lightingBuffer == nullptr || m_context == nullptr) {
		return;
	}
	// Refresh the world/view matrices the FF lighting (normal transform) and fog
	// (view-space depth) read. WWMath Matrix4x4 is 16 row-major floats, matching
	// the row_major float4x4 declaration - same convention as the WVP buffer.
	std::memcpy(m_lighting.world, &m_world, sizeof(float) * 16);
	std::memcpy(m_lighting.view, &m_view, sizeof(float) * 16);
	m_context->UpdateSubresource(m_lightingBuffer, 0, nullptr, &m_lighting, 0, 0);
	m_lightingDirty = false;
}

void D3D11Backend::Update_Fog_Buffer()
{
	if (!m_fogDirty || m_fogBuffer == nullptr || m_context == nullptr) {
		return;
	}
	m_context->UpdateSubresource(m_fogBuffer, 0, nullptr, &m_fog, 0, 0);
	m_fogDirty = false;
}

void D3D11Backend::Update_Skinning_Buffer()
{
	if (!m_skinningDirty || m_skinningBuffer == nullptr || m_context == nullptr) {
		return;
	}
	m_context->UpdateSubresource(m_skinningBuffer, 0, nullptr, &m_skinning, 0, 0);
	m_skinningDirty = false;
}

void D3D11Backend::Update_TexGen_Buffer()
{
	if (!m_texgenDirty || m_texgenBuffer == nullptr || m_context == nullptr) {
		return;
	}
	m_context->UpdateSubresource(m_texgenBuffer, 0, nullptr, &m_texgen, 0, 0);
	m_texgenDirty = false;
}

// Bounded texgen state-change trace, enabled by env W3DNEXT_D3D11_TEXGENLOG=1
// (lines go to the W3DNEXT_D3D11_LOG sink). First 96 changes only - enough to see
// the per-frame set/reset cadence and matrix values without flooding.
static bool D3D11_Texgen_Trace_Budget()
{
	static int budget = -1;
	if (budget < 0) {
		const char * e = W3DNext_GetEnv("D3D11_TEXGENLOG");
		budget = (e != nullptr && e[0] == '1') ? 96 : 0;
	}
	if (budget > 0) {
		--budget;
		return true;
	}
	return false;
}

void D3D11Backend::Set_Texture_Stage_Texgen_CameraSpace(unsigned int stage, bool enable)
{
	if (stage >= RB_MAX_TEXTURE_STAGES) {
		return;
	}
	const unsigned int v = enable ? 1u : 0u;
	if (m_texgen.texGen[stage][0] != v) {
		m_texgen.texGen[stage][0] = v;
		m_texgenDirty = true;
		if (D3D11_Texgen_Trace_Budget()) {
			char buf[128];
			std::snprintf(buf, sizeof(buf), "[texgen] stage=%u camera-space=%u", stage, v);
			D3D11_Log_Line(buf);
		}
	}
}

void D3D11Backend::Set_Texture_Transform_Enable(unsigned int stage, bool enable)
{
	if (stage >= RB_MAX_TEXTURE_STAGES) {
		return;
	}
	const unsigned int v = enable ? 1u : 0u;
	if (m_texgen.texGen[stage][1] != v) {
		m_texgen.texGen[stage][1] = v;
		m_texgenDirty = true;
		if (D3D11_Texgen_Trace_Budget()) {
			char buf[128];
			std::snprintf(buf, sizeof(buf), "[texgen] stage=%u matrix-enable=%u", stage, v);
			D3D11_Log_Line(buf);
		}
	}
}

void D3D11Backend::Set_Texture_Transform_Matrix(unsigned int stage, const float * m16)
{
	if (stage >= RB_MAX_TEXTURE_STAGES || m16 == nullptr) {
		return;
	}
	if (std::memcmp(m_texgen.texMat[stage], m16, sizeof(float) * 16) != 0) {
		std::memcpy(m_texgen.texMat[stage], m16, sizeof(float) * 16);
		m_texgenDirty = true;
		if (D3D11_Texgen_Trace_Budget()) {
			char buf[256];
			std::snprintf(buf, sizeof(buf),
				"[texgen] stage=%u mat r0=(%.6f %.6f %.6f %.6f) r1=(%.6f %.6f) r3=(%.3f %.3f %.3f %.3f)",
				stage, m16[0], m16[1], m16[2], m16[3], m16[4], m16[5], m16[12], m16[13], m16[14], m16[15]);
			D3D11_Log_Line(buf);
			const float * v = reinterpret_cast<const float *>(&m_view);
			std::snprintf(buf, sizeof(buf),
				"[texgen]   view r0=(%.4f %.4f %.4f %.4f) r1=(%.4f %.4f %.4f %.4f) r2=(%.4f %.4f %.4f %.4f)",
				v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
			D3D11_Log_Line(buf);
		}
	}
}

void D3D11Backend::Set_Skinning_Enable(bool enable)
{
	const unsigned int v = enable ? 1u : 0u;
	if (m_skinning.skinningEnable != v) {
		m_skinning.skinningEnable = v;
		m_skinningDirty = true;
	}
}

void D3D11Backend::Set_Bone_Matrices(unsigned int count, const Matrix4x4 * matrices)
{
	if (matrices == nullptr) {
		return;
	}
	if (count > RB_MAX_BONES) {
		count = RB_MAX_BONES;
	}
	// Matrix4x4 is 16 row-major floats (no vtable) - the same raw form the WVP /
	// lighting buffers upload - so each bone copies straight into the cbSkinning
	// mirror. The HLSL reads them row_major and does mul(Bones[i], pos), matching
	// WWMath's column-vector transform (see Update_Constant_Buffer).
	for (unsigned int i = 0; i < count; ++i) {
		std::memcpy(m_skinning.bones[i], &matrices[i], sizeof(float) * 16);
	}
	m_skinningDirty = true;
}

// W3DNEXT_D3D11_MIPS=0 -> level 0 only (pre-2026-07-26 behaviour). Read once.
bool D3D11Backend::Mip_Upload_Enabled()
{
	static bool s_checked = false;
	static bool s_enabled = true;
	if (!s_checked) {
		s_checked = true;
		const char * e = W3DNext_GetEnv("D3D11_MIPS");
		s_enabled = !(e != nullptr && e[0] == '0');
	}
	return s_enabled;
}

bool D3D11Backend::Upload_Texture_RGBA(unsigned int stage, unsigned int width, unsigned int height, const void * rgba_pixels,
	bool wrap, bool linear)
{
	// Single-level convenience wrapper (the smoke oracle's entry point).
	RenderBackendMipLevel lvl;
	lvl.data = rgba_pixels;
	lvl.width = width;
	lvl.height = height;
	lvl.row_pitch = width * 4; // 4 bytes/texel, tightly packed
	return Upload_Texture_RGBA_Mips(stage, &lvl, 1, wrap, linear);
}

// Experiment lever for the terrain mip-seam study (W3DNEXT_D3D11_LODBIAS=<float>,
// default 0): a constant sampler LOD bias applied to every mip-enabled
// sampler. Exists because DX8 (mip POINT, bias 0) and D3D11 compute per-pixel
// LOD near the 0.5 rounding boundary on the RTS camera and can land on
// opposite levels; the A/B runs tune this against the DX8 framedump truth.
static float RB_Mip_LOD_Bias()
{
	static bool s_read = false;
	static float s_bias = 0.0f;
	if (!s_read) {
		s_read = true;
		const char * e = W3DNext_GetEnv("D3D11_LODBIAS");
		if (e != nullptr && e[0] != '\0') {
			s_bias = static_cast<float>(std::atof(e));
		}
	}
	return s_bias;
}

bool D3D11Backend::Upload_Texture_RGBA_Mips(unsigned int stage, const RenderBackendMipLevel * levels,
	unsigned int count, bool wrap, bool linear, RenderBackendMipFilter mip)
{
	if (m_device == nullptr || m_context == nullptr) {
		return false;
	}
	if (stage >= RB_MAX_TEXTURE_STAGES || levels == nullptr || count == 0 || count > RB_MAX_MIP_LEVELS) {
		return false;
	}
	if (levels[0].width == 0 || levels[0].height == 0) {
		return false;
	}

	// Validate and pack the per-level descriptors BEFORE touching stage state, so
	// a malformed chain leaves whatever was bound alone rather than unbinding it.
	D3D11_SUBRESOURCE_DATA srd[RB_MAX_MIP_LEVELS];
	ZeroMemory(srd, sizeof(srd));
	for (unsigned int i = 0; i < count; ++i) {
		const RenderBackendMipLevel & L = levels[i];
		if (L.data == nullptr || L.width == 0 || L.height == 0) {
			return false;
		}
		const unsigned int packed = L.width * 4;
		const unsigned int pitch = (L.row_pitch != 0) ? L.row_pitch : packed;
		if (pitch < packed) {
			return false; // a shorter-than-packed pitch cannot hold a texel row
		}
		srd[i].pSysMem = L.data;
		srd[i].SysMemPitch = pitch;
	}

	// Replace any texture already living in this stage.
	Safe_Release(m_stageSampler[stage]);
	Safe_Release(m_stageSRV[stage]);
	Safe_Release(m_stageTexture[stage]);

	// R8G8B8A8_UNORM (linear, NOT _SRGB): a texel byte 128 samples as 128/255 so
	// the modulate math is an exact UNORM product - no gamma in the middle.
	D3D11_TEXTURE2D_DESC td;
	ZeroMemory(&td, sizeof(td));
	td.Width = levels[0].width;
	td.Height = levels[0].height;
	td.MipLevels = count;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_IMMUTABLE;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	HRESULT hr = m_device->CreateTexture2D(&td, srd, &m_stageTexture[stage]);
	if (FAILED(hr)) {
		return false;
	}

	hr = m_device->CreateShaderResourceView(m_stageTexture[stage], nullptr, &m_stageSRV[stage]);
	if (FAILED(hr)) {
		Safe_Release(m_stageTexture[stage]);
		return false;
	}

	// Default POINT + CLAMP: a flat-color texture reads back its exact texel
	// regardless of UV, so the smoke combiner assert measures the op, not filtering.
	// The real game passes wrap=true/linear=true so tiled backdrops and filtered UI
	// look right. The mip mode mirrors the texture's own D3DTSS_MIPFILTER: POINT
	// snaps between levels, NONE pins sampling to level 0 via MaxLOD.
	const D3D11_TEXTURE_ADDRESS_MODE addr = wrap ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
	D3D11_SAMPLER_DESC sd;
	ZeroMemory(&sd, sizeof(sd));
	if (linear) {
		sd.Filter = (mip == RB_MIPF_LINEAR) ? D3D11_FILTER_MIN_MAG_MIP_LINEAR
		                                    : D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	} else {
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	}
	sd.AddressU = addr;
	sd.AddressV = addr;
	sd.AddressW = addr;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MaxLOD = (mip == RB_MIPF_NONE) ? 0.0f : D3D11_FLOAT32_MAX;
	sd.MipLODBias = (mip == RB_MIPF_NONE) ? 0.0f : RB_Mip_LOD_Bias();
	hr = m_device->CreateSamplerState(&sd, &m_stageSampler[stage]);
	if (FAILED(hr)) {
		Safe_Release(m_stageSRV[stage]);
		Safe_Release(m_stageTexture[stage]);
		return false;
	}

	// Bind to the matching pixel-shader slot.
	m_context->PSSetShaderResources(stage, 1, &m_stageSRV[stage]);
	m_context->PSSetSamplers(stage, 1, &m_stageSampler[stage]);
	return true;
}

bool D3D11Backend::Upload_Texture_BC(unsigned int stage, unsigned int width, unsigned int height,
	RenderBackendBCFormat format, const void * blocks, unsigned int row_pitch_bytes,
	bool wrap, bool linear)
{
	if (m_device == nullptr || m_context == nullptr) {
		return false;
	}
	if (stage >= RB_MAX_TEXTURE_STAGES || blocks == nullptr) {
		return false;
	}
	// Single-level convenience wrapper (the smoke oracle's entry point).
	RenderBackendMipLevel lvl;
	lvl.data = blocks;
	lvl.width = width;
	lvl.height = height;
	lvl.row_pitch = row_pitch_bytes; // 0 => tightly packed, resolved below
	return Upload_Texture_BC_Mips(stage, format, &lvl, 1, wrap, linear);
}

bool D3D11Backend::Upload_Texture_BC_Mips(unsigned int stage, RenderBackendBCFormat format,
	const RenderBackendMipLevel * levels, unsigned int count, bool wrap, bool linear,
	RenderBackendMipFilter mip)
{
	if (m_device == nullptr || m_context == nullptr) {
		return false;
	}
	if (stage >= RB_MAX_TEXTURE_STAGES || levels == nullptr || count == 0 || count > RB_MAX_MIP_LEVELS) {
		return false;
	}
	// D3D11 requires block-aligned TOP-LEVEL dimensions for BC resources. Lower
	// levels legitimately shrink past a block (2x2, 1x1) and are stored as a
	// single partially-used block - that is the standard chain, not an error, so
	// the alignment rule is asserted on level 0 only.
	if (levels[0].width < 4 || levels[0].height < 4 ||
		(levels[0].width % 4) != 0 || (levels[0].height % 4) != 0) {
		return false;
	}

	DXGI_FORMAT dxgi;
	unsigned int block_bytes;
	switch (format) {
	case RB_BC1: dxgi = DXGI_FORMAT_BC1_UNORM; block_bytes = 8;  break;
	case RB_BC2: dxgi = DXGI_FORMAT_BC2_UNORM; block_bytes = 16; break;
	case RB_BC3: dxgi = DXGI_FORMAT_BC3_UNORM; block_bytes = 16; break;
	default:     return false;
	}

	// Validate and pack every level BEFORE touching stage state, so a malformed
	// chain leaves whatever was bound alone rather than unbinding it.
	D3D11_SUBRESOURCE_DATA srd[RB_MAX_MIP_LEVELS];
	ZeroMemory(srd, sizeof(srd));
	for (unsigned int i = 0; i < count; ++i) {
		const RenderBackendMipLevel & L = levels[i];
		if (L.data == nullptr || L.width == 0 || L.height == 0) {
			return false;
		}
		// Round UP to whole blocks: a 2x2 or 1x1 level still occupies one block.
		const unsigned int blocks_per_row = (L.width + 3) / 4;
		const unsigned int packed_pitch = blocks_per_row * block_bytes;
		const unsigned int pitch = (L.row_pitch != 0) ? L.row_pitch : packed_pitch;
		if (pitch < packed_pitch) {
			return false; // a shorter-than-packed pitch cannot hold a block row
		}
		// SysMemPitch for a BC format is the byte distance between BLOCK rows.
		srd[i].pSysMem = L.data;
		srd[i].SysMemPitch = pitch;
	}

	// Replace any texture already living in this stage.
	Safe_Release(m_stageSampler[stage]);
	Safe_Release(m_stageSRV[stage]);
	Safe_Release(m_stageTexture[stage]);

	// BCn_UNORM (linear, NOT _SRGB) for the same reason as the RGBA path: the
	// decoded texel feeds the modulate math as a plain UNORM product.
	D3D11_TEXTURE2D_DESC td;
	ZeroMemory(&td, sizeof(td));
	td.Width = levels[0].width;
	td.Height = levels[0].height;
	td.MipLevels = count;
	td.ArraySize = 1;
	td.Format = dxgi;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_IMMUTABLE;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	HRESULT hr = m_device->CreateTexture2D(&td, srd, &m_stageTexture[stage]);
	if (FAILED(hr)) {
		return false;
	}

	hr = m_device->CreateShaderResourceView(m_stageTexture[stage], nullptr, &m_stageSRV[stage]);
	if (FAILED(hr)) {
		Safe_Release(m_stageTexture[stage]);
		return false;
	}

	// Same sampler policy as the RGBA path: POINT+CLAMP default for exact-texel
	// smoke reads; the real-game path passes wrap/linear true and the texture's
	// own mip mode (POINT snaps levels, NONE pins level 0 via MaxLOD).
	const D3D11_TEXTURE_ADDRESS_MODE addr = wrap ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
	D3D11_SAMPLER_DESC sd;
	ZeroMemory(&sd, sizeof(sd));
	if (linear) {
		sd.Filter = (mip == RB_MIPF_LINEAR) ? D3D11_FILTER_MIN_MAG_MIP_LINEAR
		                                    : D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	} else {
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	}
	sd.AddressU = addr;
	sd.AddressV = addr;
	sd.AddressW = addr;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MaxLOD = (mip == RB_MIPF_NONE) ? 0.0f : D3D11_FLOAT32_MAX;
	sd.MipLODBias = (mip == RB_MIPF_NONE) ? 0.0f : RB_Mip_LOD_Bias();
	hr = m_device->CreateSamplerState(&sd, &m_stageSampler[stage]);
	if (FAILED(hr)) {
		Safe_Release(m_stageSRV[stage]);
		Safe_Release(m_stageTexture[stage]);
		return false;
	}

	m_context->PSSetShaderResources(stage, 1, &m_stageSRV[stage]);
	m_context->PSSetSamplers(stage, 1, &m_stageSampler[stage]);
	return true;
}

bool D3D11Backend::Bind_Cached_Texture(unsigned int stage, unsigned int key, unsigned long long version)
{
	if (stage >= RB_MAX_TEXTURE_STAGES || m_context == nullptr) {
		return false;
	}
	std::map<unsigned int, CachedTexture>::iterator it = m_textureCache.find(key);
	if (it == m_textureCache.end() || it->second.version != version || it->second.srv == nullptr) {
		return false; // miss, or the content changed since it was uploaded
	}
	Safe_Release(m_stageTexture[stage]);
	Safe_Release(m_stageSRV[stage]);
	Safe_Release(m_stageSampler[stage]);
	m_stageTexture[stage] = it->second.tex;
	m_stageSRV[stage] = it->second.srv;
	m_stageSampler[stage] = it->second.sampler;
	if (m_stageTexture[stage] != nullptr) m_stageTexture[stage]->AddRef();
	if (m_stageSRV[stage] != nullptr) m_stageSRV[stage]->AddRef();
	if (m_stageSampler[stage] != nullptr) m_stageSampler[stage]->AddRef();
	m_context->PSSetShaderResources(stage, 1, &m_stageSRV[stage]);
	m_context->PSSetSamplers(stage, 1, &m_stageSampler[stage]);
	++m_texCacheHits;
	return true;
}

void D3D11Backend::Store_Cached_Texture(unsigned int stage, unsigned int key, unsigned long long version)
{
	if (stage >= RB_MAX_TEXTURE_STAGES) {
		return;
	}
	CachedTexture & c = m_textureCache[key];
	// Replacing an older generation of the same source texture (content changed).
	Safe_Release(c.tex);
	Safe_Release(c.srv);
	Safe_Release(c.sampler);
	c.tex = m_stageTexture[stage];
	c.srv = m_stageSRV[stage];
	c.sampler = m_stageSampler[stage];
	if (c.tex != nullptr) c.tex->AddRef();
	if (c.srv != nullptr) c.srv->AddRef();
	if (c.sampler != nullptr) c.sampler->AddRef();
	c.version = version;
	++m_texCacheUploads;
}

void D3D11Backend::Evict_Cached_Texture(unsigned int key)
{
	std::map<unsigned int, CachedTexture>::iterator it = m_textureCache.find(key);
	if (it == m_textureCache.end()) {
		return;
	}
	Safe_Release(it->second.tex);
	Safe_Release(it->second.srv);
	Safe_Release(it->second.sampler);
	m_textureCache.erase(it);
	++m_texCacheEvictions;
}

void D3D11Backend::Release_Texture_Cache()
{
	for (std::map<unsigned int, CachedTexture>::iterator it = m_textureCache.begin();
		it != m_textureCache.end(); ++it) {
		Safe_Release(it->second.tex);
		Safe_Release(it->second.srv);
		Safe_Release(it->second.sampler);
	}
	m_textureCache.clear();
}

void D3D11Backend::Evict_Render_Target(TextureBaseClass * tex)
{
	if (tex == nullptr) {
		return;
	}
	auto it = m_rtt.find(tex);
	if (it == m_rtt.end()) {
		return;
	}
	RTTTarget & t = it->second;
	// If this RT is currently bound, drop back to the backbuffer first.
	if (m_currentRTV == t.rtv || m_savedBackBufferRTV == t.rtv) {
		Bind_Back_Buffer_Targets();
		m_savedBackBufferRTV = nullptr;
	}
	Safe_Release(t.sampler);
	Safe_Release(t.dsv);
	Safe_Release(t.depthTex);
	Safe_Release(t.srv);
	Safe_Release(t.rtv);
	Safe_Release(t.tex);
	m_rtt.erase(it);
}

bool D3D11Backend::Upload_Fallback_Texture(unsigned int stage)
{
	// 4x4 magenta/black 2x2-checker (R,G,B,A memory order): wrong-but-visible for
	// any texture whose source format the backend cannot upload yet. Magenta is
	// this codebase's canonical "look at me" color (the smoke clear color).
	unsigned char px[4 * 4 * 4];
	for (unsigned int y = 0; y < 4; ++y) {
		for (unsigned int x = 0; x < 4; ++x) {
			const bool magenta = (((x / 2) + (y / 2)) & 1u) == 0u;
			unsigned char * p = px + (y * 4 + x) * 4;
			p[0] = magenta ? 255 : 0; // R
			p[1] = 0;                 // G
			p[2] = magenta ? 255 : 0; // B
			p[3] = 255;               // A
		}
	}
	return Upload_Texture_RGBA(stage, 4, 4, px, /*wrap*/true, /*linear*/false);
}

bool D3D11Backend::Bind_Neutral_Texture(unsigned int stage)
{
	if (m_device == nullptr || m_context == nullptr || stage >= RB_MAX_TEXTURE_STAGES) {
		return false;
	}
	{
		static int s_neutralLog = 0;
		if (s_neutralLog < 200) {
			const char * nm = "(unknown)";
			if (m_boundTextures[stage] != nullptr) {
				nm = (const char *)m_boundTextures[stage]->Get_Texture_Name();
			}
			FILE * dbg = fopen("unitnorm.log", "a");
			if (dbg) { fprintf(dbg, "NEUTRAL stage=%u tex=%s\n", stage, nm); fclose(dbg); }
			s_neutralLog++;
		}
	}

	// Lazily create the shared 4x4 all-white texture (multiplicative identity,
	// see the header comment) ONCE per device; a failed creation is latched so
	// the per-frame null-bind storm doesn't retry it.
	if (m_neutralSRV == nullptr) {
		if (m_neutralCreateFailed) {
			return false;
		}
		unsigned char px[4 * 4 * 4];
		for (unsigned int i = 0; i < 4 * 4 * 4; ++i) {
			px[i] = 255;
		}
		D3D11_TEXTURE2D_DESC td;
		ZeroMemory(&td, sizeof(td));
		td.Width = 4;
		td.Height = 4;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_IMMUTABLE;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA srd;
		srd.pSysMem = px;
		srd.SysMemPitch = 4 * 4;
		srd.SysMemSlicePitch = 0;
		HRESULT hr = m_device->CreateTexture2D(&td, &srd, &m_neutralTexture);
		if (SUCCEEDED(hr)) {
			hr = m_device->CreateShaderResourceView(m_neutralTexture, nullptr, &m_neutralSRV);
		}
		if (SUCCEEDED(hr)) {
			// POINT + WRAP, mirroring what the per-stage neutral upload used.
			D3D11_SAMPLER_DESC sd;
			ZeroMemory(&sd, sizeof(sd));
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
			sd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
			sd.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
			sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
			sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
			hr = m_device->CreateSamplerState(&sd, &m_neutralSampler);
		}
		if (FAILED(hr)) {
			Safe_Release(m_neutralSampler);
			Safe_Release(m_neutralSRV);
			Safe_Release(m_neutralTexture);
			m_neutralCreateFailed = true;
			return false;
		}
	}

	if (m_stageNeutral[stage] && m_stageSRV[stage] == m_neutralSRV) {
		return true; // already bound to this slot
	}

	// The slot takes its own refs on the shared objects, so every existing
	// per-stage Safe_Release path (real bind replace, Release_Texture_Stages)
	// works unchanged and can never destroy the shared copy.
	Safe_Release(m_stageSampler[stage]);
	Safe_Release(m_stageSRV[stage]);
	Safe_Release(m_stageTexture[stage]); // slot does not alias the shared texture object
	m_stageSRV[stage] = m_neutralSRV;
	m_stageSRV[stage]->AddRef();
	m_stageSampler[stage] = m_neutralSampler;
	m_stageSampler[stage]->AddRef();
	m_context->PSSetShaderResources(stage, 1, &m_stageSRV[stage]);
	m_context->PSSetSamplers(stage, 1, &m_stageSampler[stage]);
	m_stageNeutral[stage] = true;
	return true;
}

void D3D11Backend::Set_Texture_Stage_Count(unsigned int count)
{
	if (count > RB_MAX_TEXTURE_STAGES) {
		count = RB_MAX_TEXTURE_STAGES;
	}
	if (m_combiner.numStages != count) {
		m_combiner.numStages = count;
		m_combinerDirty = true;
	}
}

void D3D11Backend::Set_Texture_Stage_ColorOp(unsigned int stage, RenderBackendTexOp op, RenderBackendTexArg arg1, RenderBackendTexArg arg2)
{
	if (stage >= RB_MAX_TEXTURE_STAGES) {
		return;
	}
	m_combiner.stageColor[stage][0] = static_cast<unsigned int>(op);
	m_combiner.stageColor[stage][1] = static_cast<unsigned int>(arg1);
	m_combiner.stageColor[stage][2] = static_cast<unsigned int>(arg2);
	m_combinerDirty = true;
}

void D3D11Backend::Set_Texture_Stage_AlphaOp(unsigned int stage, RenderBackendTexOp op, RenderBackendTexArg arg1, RenderBackendTexArg arg2)
{
	if (stage >= RB_MAX_TEXTURE_STAGES) {
		return;
	}
	m_combiner.stageAlpha[stage][0] = static_cast<unsigned int>(op);
	m_combiner.stageAlpha[stage][1] = static_cast<unsigned int>(arg1);
	m_combiner.stageAlpha[stage][2] = static_cast<unsigned int>(arg2);
	m_combinerDirty = true;
}

void D3D11Backend::Set_Texture_Stage_TexCoordIndex(unsigned int stage, unsigned int texcoord_index)
{
	if (stage >= RB_MAX_TEXTURE_STAGES) {
		return;
	}
	m_combiner.stageColor[stage][3] = texcoord_index;
	m_combinerDirty = true;
}

void D3D11Backend::Set_Texture_Factor(float r, float g, float b, float a)
{
	m_combiner.tfactor[0] = r;
	m_combiner.tfactor[1] = g;
	m_combiner.tfactor[2] = b;
	m_combiner.tfactor[3] = a;
	m_combinerDirty = true;
}

void D3D11Backend::Set_Tree_Sway(bool enable, const float * vec4s, unsigned int count)
{
	const unsigned int en = enable ? 1u : 0u;
	if (m_skinning.swayEnable != en) {
		m_skinning.swayEnable = en;
		m_skinningDirty = true;
	}
	if (enable && vec4s != nullptr) {
		const unsigned int n = count < (unsigned int)RB_MAX_SWAY_ENTRIES
			? count : (unsigned int)RB_MAX_SWAY_ENTRIES;
		std::memcpy(m_skinning.sway, vec4s, n * 4 * sizeof(float));
		m_skinningDirty = true;
	}
}

void D3D11Backend::Set_Alpha_Test(bool enable, bool less_equal, float ref)
{
	const unsigned int en = enable ? 1u : 0u;
	const unsigned int le = less_equal ? 1u : 0u;
	if (m_combiner.alphaTestEnable == en && m_combiner.alphaTestLessEqual == le &&
		m_combiner.alphaTestRef == ref) {
		return;
	}
	m_combiner.alphaTestEnable = en;
	m_combiner.alphaTestLessEqual = le;
	m_combiner.alphaTestRef = ref;
	m_combinerDirty = true;
	// The unit-normal pixel shader reads its own alpha-test state from cbUnit
	// (not the FF combiner). Re-derive it here whenever the combiner changes so
	// alpha-tested cutouts (tree canopies, prop grass, fence pickets) keep their
	// discard while the unit normal pass is bound.
	if (m_unitNormalIsActive && m_unitBuffer != nullptr) {
		Vector3 sd(-m_lighting.lightDir[0][0], -m_lighting.lightDir[0][1], -m_lighting.lightDir[0][2]);
		float sl = sd.X * sd.X + sd.Y * sd.Y + sd.Z * sd.Z;
		if (sl < 1e-6f) { sd.Set(0.5f, -0.8f, 0.3f); } else { sd.Normalize(); }
		Vector3 sc(m_lighting.lightDiffuse[0][0], m_lighting.lightDiffuse[0][1], m_lighting.lightDiffuse[0][2]);
		if (sc.X * sc.X + sc.Y * sc.Y + sc.Z * sc.Z < 1e-6f) { sc.Set(1.0f, 1.0f, 1.0f); }
		Vector3 amb(m_lighting.globalAmbient[0], m_lighting.globalAmbient[1], m_lighting.globalAmbient[2]);
		if (amb.X * amb.X + amb.Y * amb.Y + amb.Z * amb.Z < 1e-6f) { amb.Set(0.3f, 0.3f, 0.3f); }
		Set_Unit_Normal_Constants(sd, sc, amb, m_unitLastDetail, m_unitLastSecondary);
	}
}

void D3D11Backend::Set_Alpha_Reference(float ref)
{
	// Caller convention is the base greater-equal reference (DX8's ALPHAREF).
	// Set_Shader may have programmed the inverted LESSEQUAL form (INVSRCALPHA
	// source blend); mirror the same inversion so the override tests the same
	// alpha population the shader's own reference did.
	const float effective = (m_combiner.alphaTestLessEqual != 0u) ? (1.0f - ref) : ref;
	if (m_combiner.alphaTestRef == effective) {
		return;
	}
	m_combiner.alphaTestRef = effective;
	m_combinerDirty = true;
	// Mirror into cbUnit for the active unit-normal pass (see Set_Alpha_Test).
	if (m_unitNormalIsActive && m_unitBuffer != nullptr) {
		Vector3 sd(-m_lighting.lightDir[0][0], -m_lighting.lightDir[0][1], -m_lighting.lightDir[0][2]);
		float sl = sd.X * sd.X + sd.Y * sd.Y + sd.Z * sd.Z;
		if (sl < 1e-6f) { sd.Set(0.5f, -0.8f, 0.3f); } else { sd.Normalize(); }
		Vector3 sc(m_lighting.lightDiffuse[0][0], m_lighting.lightDiffuse[0][1], m_lighting.lightDiffuse[0][2]);
		if (sc.X * sc.X + sc.Y * sc.Y + sc.Z * sc.Z < 1e-6f) { sc.Set(1.0f, 1.0f, 1.0f); }
		Vector3 amb(m_lighting.globalAmbient[0], m_lighting.globalAmbient[1], m_lighting.globalAmbient[2]);
		if (amb.X * amb.X + amb.Y * amb.Y + amb.Z * amb.Z < 1e-6f) { amb.Set(0.3f, 0.3f, 0.3f); }
		Set_Unit_Normal_Constants(sd, sc, amb, m_unitLastDetail, m_unitLastSecondary);
	}
}

void D3D11Backend::Set_Grayscale_Override(bool enable)
{
	if ((m_combiner.monoEnable != 0u) == enable) {
		return;
	}
	m_combiner.monoEnable = enable ? 1u : 0u;
	if (enable) {
		// Effective weights of the DX8 grayscale trick (render2d.cpp): stage 0
		// computes 0.502 + 0.502*tex (MULTIPLYADD with TFACTOR 0x80A5CA8E alpha-
		// replicated), stage 1 DOT3s against 4*(TFACTOR.rgb - 0.5), which folds
		// to dot(tex.rgb, (0.295, 0.587, 0.114)) replicated to RGB (the ~0.004
		// constant term is under 1/255 and dropped). Fade of 0 on the alpha lane
		// leaves alpha at the cascade's output, matching the opaque DX8 draw.
		m_combiner.monoLum[0] = 0.295f;
		m_combiner.monoLum[1] = 0.587f;
		m_combiner.monoLum[2] = 0.114f;
		m_combiner.monoLum[3] = 0.0f;
		for (int c = 0; c < 4; ++c) {
			m_combiner.monoTint[c] = 1.0f;
			m_combiner.monoFade[c] = (c < 3) ? 1.0f : 0.0f;
		}
	}
	m_combinerDirty = true;
}

// ----------------------------------------------------------------------------
//
// FF lighting / material / fog typed setters (RENDERER_PORT.md step 7)
//
// ----------------------------------------------------------------------------

void D3D11Backend::Set_Lighting_Enable(bool enable)
{
	const unsigned int v = enable ? 1u : 0u;
	if (m_lighting.lightingEnable != v) {
		m_lighting.lightingEnable = v;
		m_lightingDirty = true;
	}
}

void D3D11Backend::Set_Light_Count(unsigned int count)
{
	if (count > RB_MAX_LIGHTS) {
		count = RB_MAX_LIGHTS;
	}
	if (m_lighting.numLights != count) {
		m_lighting.numLights = count;
		m_lightingDirty = true;
	}
}

void D3D11Backend::Set_Light_Directional(unsigned int index, const Vector3 & dir, const Vector3 & diffuse)
{
	if (index >= RB_MAX_LIGHTS) {
		return;
	}
	m_lighting.lightDir[index][0] = dir.X;
	m_lighting.lightDir[index][1] = dir.Y;
	m_lighting.lightDir[index][2] = dir.Z;
	m_lighting.lightDir[index][3] = 0.0f;
	m_lighting.lightDiffuse[index][0] = diffuse.X;
	m_lighting.lightDiffuse[index][1] = diffuse.Y;
	m_lighting.lightDiffuse[index][2] = diffuse.Z;
	m_lighting.lightDiffuse[index][3] = 1.0f;
	// Auto-cover this slot in the active count (mirrors LightEnable turning a slot on).
	if (index + 1 > m_lighting.numLights) {
		m_lighting.numLights = index + 1;
	}
	m_lightingDirty = true;
}

void D3D11Backend::Set_Material_Params(
	const Vector3 & diffuse, const Vector3 & ambient, const Vector3 & emissive,
	float opacity, RenderBackendColorSource diffuse_source, RenderBackendColorSource ambient_source)
{
	m_lighting.matDiffuse[0] = diffuse.X;
	m_lighting.matDiffuse[1] = diffuse.Y;
	m_lighting.matDiffuse[2] = diffuse.Z;
	m_lighting.matDiffuse[3] = opacity;
	m_lighting.matAmbient[0] = ambient.X;
	m_lighting.matAmbient[1] = ambient.Y;
	m_lighting.matAmbient[2] = ambient.Z;
	m_lighting.matAmbient[3] = 1.0f;
	m_lighting.matEmissive[0] = emissive.X;
	m_lighting.matEmissive[1] = emissive.Y;
	m_lighting.matEmissive[2] = emissive.Z;
	m_lighting.matEmissive[3] = 1.0f;
	m_lighting.diffuseSource = static_cast<unsigned int>(diffuse_source);
	m_lighting.ambientSource = static_cast<unsigned int>(ambient_source);
	m_lightingDirty = true;
}

void D3D11Backend::Set_Fog_Params(bool enable, const Vector3 & color, RenderBackendFogMode mode, float start, float end, float density)
{
	m_fog.color[0] = color.X;
	m_fog.color[1] = color.Y;
	m_fog.color[2] = color.Z;
	m_fog.color[3] = 1.0f;
	m_fog.enable = enable ? 1u : 0u;
	m_fog.mode = static_cast<unsigned int>(mode);
	m_fog.start = start;
	m_fog.end = end;
	m_fog.density = density;
	m_fogDirty = true;
	m_fogEnable = enable;
}

bool D3D11Backend::Upload_Vertices(const void * data, unsigned int size_bytes, unsigned int fvf)
{
	if (m_device == nullptr || m_context == nullptr || m_vsBytecode == nullptr) {
		return false;
	}
	if (data == nullptr || size_bytes == 0) {
		return false;
	}

	D3D11InputLayoutDesc layout;
	if (!FVF_To_Input_Layout(fvf, layout) || layout.stride == 0) {
		return false;
	}
	m_vertexFVF = fvf;

	// Ensure the layout supplies every input the FF-emulation VS reads (POSITION
	// is always present; BLENDWEIGHT / BLENDINDICES / COLOR0 / TEXCOORD0 / TEXCOORD1
	// / NORMAL may be missing for a given FVF). Missing ones are aliased onto offset
	// 0 - unused (the skinning branch only runs when SkinningEnable != 0, which a
	// non-skinned FVF never sets), so harmless.
	if (!Layout_Has_Semantic(layout, "BLENDWEIGHT", 0)) {
		Append_Alias_Element(layout, "BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32_FLOAT);
	}
	if (!Layout_Has_Semantic(layout, "BLENDINDICES", 0)) {
		Append_Alias_Element(layout, "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT);
	}
	if (!Layout_Has_Semantic(layout, "NORMAL", 0)) {
		Append_Alias_Element(layout, "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT);
	}
	if (!Layout_Has_Semantic(layout, "COLOR", 0)) {
		Append_Alias_Element(layout, "COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM);
	}
	if (!Layout_Has_Semantic(layout, "TEXCOORD", 0)) {
		Append_Alias_Element(layout, "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT);
	}
	if (!Layout_Has_Semantic(layout, "TEXCOORD", 1)) {
		Append_Alias_Element(layout, "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT);
	}

	// Input layout, validated against the VS bytecode. Cached per FVF - do NOT
	// rebuild it on the hot upload path (driver input-assembly validation is
	// expensive and was the source of the 1-2 FPS collapse).
	ID3D11InputLayout *& cachedLayout = m_inputLayoutCache[fvf];
	if (cachedLayout == nullptr) {
		HRESULT hr = m_device->CreateInputLayout(
			layout.elements, layout.num_elements,
			m_vsBytecode, m_vsBytecodeSize, &cachedLayout);
		if (FAILED(hr)) {
			return false;
		}
	}

	// Suballocate a disjoint slice from the single per-frame DEFAULT staging
	// buffer (created once in Ensure_Dyn_VB_Cap, released next Begin_Scene).
	// One CreateBuffer per frame instead of one per upload - this removed the
	// ~300 driver allocations/frame that dropped FPS from ~30 to ~2. The slice
	// is written once and never overwritten until the buffer is released, so no
	// in-flight reuse (the Intel AV we hit with per-upload buffers / DYNAMIC+Map).
	const UINT stride = layout.stride;
	// Align each slice to its own stride so vertex[0] fetches land on a stride
	// boundary (non-pow2 strides like 12 would mis-read under a fixed 16-byte
	// align). The buffer is private per slice, so the slice itself is read-safe.
	const UINT off = ((m_dynVBOffset + stride - 1u) / stride) * stride;
	if (m_dynVB == nullptr || (off + size_bytes) > m_dynVBCap) {
		if (!Ensure_Dyn_VB_Cap(off + size_bytes)) {
			return false;
		}
	}
	const bool profiling = Profiling_Enabled();
	LARGE_INTEGER ut0;
	if (profiling) { QueryPerformanceCounter(&ut0); }
	m_dynVBOffset = off + size_bytes;
	m_vertexBuffer = m_dynVB;
	D3D11_BOX box;
	box.left = off; box.top = 0; box.front = 0;
	box.right = off + size_bytes; box.bottom = 1; box.back = 1;
	m_context->UpdateSubresource(m_vertexBuffer, 0, &box, data, size_bytes, size_bytes);

	m_vertexStride = stride;
	const UINT ioff = off;
	m_context->IASetInputLayout(cachedLayout);
	m_context->IASetVertexBuffers(0, 1, &m_vertexBuffer, &stride, &ioff);
	// Timing is opt-in (D3D11_PROFILE=1) - see Profiling_Enabled(). Skipping
	// QueryPerformanceCounter here by default removes 2 QPC calls from every
	// dynamic vertex upload (UI/particles), which can happen many times a frame.
	if (profiling) {
		LARGE_INTEGER ut1;
		QueryPerformanceCounter(&ut1);
		m_dbgUploadMs += static_cast<double>(ut1.QuadPart - ut0.QuadPart) /
			static_cast<double>(m_qpcFreq.QuadPart) * 1000.0;
		++m_dbgUploads;
	}
	return true;
}

bool D3D11Backend::Upload_Indices16(const unsigned short * indices, unsigned int count)
{
	if (m_device == nullptr || m_context == nullptr || indices == nullptr || count == 0) {
		return false;
	}

	const UINT size_bytes = count * sizeof(unsigned short);
	// Suballocate a disjoint slice from the single per-frame DEFAULT index buffer
	// (created once in Ensure_Dyn_IB_Cap, released next Begin_Scene). See
	// Upload_Vertices for the single-buffer / no-in-flight-reuse rationale.
	const UINT sizeAligned = (size_bytes + 15u) & ~15u;
	if (m_dynIB == nullptr || (m_dynIBOffset + sizeAligned) > m_dynIBCap) {
		if (!Ensure_Dyn_IB_Cap(m_dynIBOffset + sizeAligned)) {
			return false;
		}
	}
	const bool profiling = Profiling_Enabled();
	LARGE_INTEGER ut0;
	if (profiling) { QueryPerformanceCounter(&ut0); }
	const UINT off = m_dynIBOffset;
	m_dynIBOffset += sizeAligned;
	m_indexBuffer = m_dynIB;
	D3D11_BOX box;
	box.left = off; box.top = 0; box.front = 0;
	box.right = off + size_bytes; box.bottom = 1; box.back = 1;
	m_context->UpdateSubresource(m_indexBuffer, 0, &box, indices, size_bytes, size_bytes);

	m_indexCount = count;
	m_context->IASetIndexBuffer(m_indexBuffer, DXGI_FORMAT_R16_UINT, off);
	if (profiling) {
		LARGE_INTEGER ut1;
		QueryPerformanceCounter(&ut1);
		m_dbgUploadMs += static_cast<double>(ut1.QuadPart - ut0.QuadPart) /
			static_cast<double>(m_qpcFreq.QuadPart) * 1000.0;
		++m_dbgUploads;
	}
	return true;
}

// Write-time capture of the DX8 dynamic buffers (RENDERER_PORT.md step 10). The
// 2D GUI / particle paths fill a BUFFER_TYPE_DYNAMIC_DX8 buffer through a lock
// that discards on re-lock, so the D3D11 backend (a separate device) cannot read
// it back at Set_Vertex_Buffer time. The engine's dynamic-lock destructor hands
// the freshly-written bytes here, straight into a real ID3D11Buffer, while the
// mapping is still valid. Raw bytes only - no WW3D2 header graph, so these live
// in this shared translation unit rather than the ww3d2-only one.
void D3D11Backend::Stage_Dynamic_Vertices(const void * data, unsigned int size_bytes, unsigned int fvf)
{
	if (data != nullptr && size_bytes != 0) {
		Upload_Vertices(data, size_bytes, fvf);
	}
}

void D3D11Backend::Stage_Dynamic_Indices(const unsigned short * indices, unsigned int count)
{
	if (indices != nullptr && count != 0) {
		Upload_Indices16(indices, count);
	}
}

// ----------------------------------------------------------------------------
//
// Frame + clear + viewport (real)
//
// ----------------------------------------------------------------------------

// Forward decl (defined above Draw_Triangles); null unless W3DNEXT_D3D11_DRAWLOG set.
static FILE * Draw_Log_File();

// ----------------------------------------------------------------------------
//
// GPU per-span timestamp profiler (W3DNEXT_D3D11_GPUPROF=1) - see D3D11Backend.h
//
// ----------------------------------------------------------------------------

bool D3D11Backend::Gpu_Profile_Enabled()
{
	static bool s_checked = false;
	static bool s_enabled = false;
	if (!s_checked) {
		s_checked = true;
		const char * e = W3DNext_GetEnv("D3D11_GPUPROF");
		s_enabled = (e != nullptr && e[0] != '0');
	}
	return s_enabled;
}

void D3D11Backend::Gpu_Profile_Open_Frame()
{
	if (!Gpu_Profile_Enabled() || m_device == nullptr || m_context == nullptr || m_gpuProfOpen) {
		return;
	}
	GpuProfFrame & fr = m_gpuProfRing[m_gpuProfWrite];
	if (fr.inFlight) {
		// Ring wrapped onto a slot the GPU has not resolved after 8 flips
		// (abnormal). Drop that frame's data rather than stall the pipeline.
		fr.inFlight = false;
	}
	if (fr.disjoint == nullptr) {
		D3D11_QUERY_DESC qd;
		qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
		qd.MiscFlags = 0;
		if (FAILED(m_device->CreateQuery(&qd, &fr.disjoint)) || fr.disjoint == nullptr) {
			return;
		}
	}
	fr.count = 0;
	m_context->Begin(fr.disjoint);
	m_gpuProfOpen = true;
	Gpu_Profile_Marker("begin");
}

void D3D11Backend::Gpu_Profile_Marker(const char * label)
{
	// m_gpuProfOpen is only ever true when profiling is enabled and a frame is
	// recording, so the disabled/default path is this one branch.
	if (!m_gpuProfOpen || label == nullptr) {
		return;
	}
	GpuProfFrame & fr = m_gpuProfRing[m_gpuProfWrite];
	if (fr.count >= RB_GPUPROF_MAX_MARKS) {
		return;
	}
	if (fr.ts[fr.count] == nullptr) {
		D3D11_QUERY_DESC qd;
		qd.Query = D3D11_QUERY_TIMESTAMP;
		qd.MiscFlags = 0;
		if (FAILED(m_device->CreateQuery(&qd, &fr.ts[fr.count])) || fr.ts[fr.count] == nullptr) {
			return;
		}
	}
	// Timestamp queries take End() only (Begin is invalid for them).
	m_context->End(fr.ts[fr.count]);
	fr.label[fr.count] = label;
	++fr.count;
}

void D3D11Backend::Gpu_Profile_Close_Frame()
{
	if (!m_gpuProfOpen) {
		return;
	}
	Gpu_Profile_Marker("present");
	GpuProfFrame & fr = m_gpuProfRing[m_gpuProfWrite];
	m_context->End(fr.disjoint);
	fr.inFlight = fr.count >= 2;  // one lone timestamp spans nothing
	m_gpuProfOpen = false;
	m_gpuProfWrite = (m_gpuProfWrite + 1u) % RB_GPUPROF_RING;
	Gpu_Profile_Collect();
}

void D3D11Backend::Gpu_Profile_Collect()
{
	auto accumulate = [this](const char * label, double ms) {
		unsigned int k = 0;
		for (; k < m_gpuProfSpanCount; ++k) {
			if (std::strcmp(m_gpuProfSpans[k].label, label) == 0) {
				break;
			}
		}
		if (k == m_gpuProfSpanCount) {
			if (m_gpuProfSpanCount >= RB_GPUPROF_MAX_SPANS) {
				return;
			}
			m_gpuProfSpans[k].label = label;
			m_gpuProfSpans[k].sumMs = 0.0;
			m_gpuProfSpans[k].hits = 0;
			++m_gpuProfSpanCount;
		}
		m_gpuProfSpans[k].sumMs += ms;
		++m_gpuProfSpans[k].hits;
	};

	// Poll every in-flight slot; DONOTFLUSH so a profile read never forces the
	// pipeline. Accumulation is order-free, so slot order doesn't matter.
	for (unsigned int s = 0; s < RB_GPUPROF_RING; ++s) {
		GpuProfFrame & fr = m_gpuProfRing[s];
		if (!fr.inFlight) {
			continue;
		}
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
		if (m_context->GetData(fr.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
			continue;
		}
		unsigned long long t[RB_GPUPROF_MAX_MARKS];
		bool ready = true;
		for (unsigned int i = 0; i < fr.count && ready; ++i) {
			ready = m_context->GetData(fr.ts[i], &t[i], sizeof(t[i]), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
		}
		if (!ready) {
			continue;
		}
		fr.inFlight = false;
		if (dj.Disjoint || dj.Frequency == 0) {
			continue;  // clock glitch (power transition etc.): drop the frame
		}
		const double toMs = 1000.0 / static_cast<double>(dj.Frequency);
		for (unsigned int i = 1; i < fr.count; ++i) {
			accumulate(fr.label[i], static_cast<double>(t[i] - t[i - 1]) * toMs);
		}
		accumulate("total", static_cast<double>(t[fr.count - 1] - t[0]) * toMs);
		++m_gpuProfFramesAccum;
	}

	if (m_gpuProfFramesAccum >= RB_GPUPROF_EMIT_FRAMES) {
		char line[512];
		int n = std::snprintf(line, sizeof(line), "[D3D11 gpuprof] f%u frames=%u mips=%s swap=%s",
			m_flipFrame, m_gpuProfFramesAccum, Mip_Upload_Enabled() ? "on" : "off",
			m_flipModel ? "flip" : "blt");
		for (unsigned int k = 0; k < m_gpuProfSpanCount && n > 0 && n < static_cast<int>(sizeof(line)); ++k) {
			n += std::snprintf(line + n, sizeof(line) - n, " %s=%.3fms",
				m_gpuProfSpans[k].label, m_gpuProfSpans[k].sumMs / static_cast<double>(m_gpuProfFramesAccum));
		}
		D3D11_Log_Line(line);
		m_gpuProfSpanCount = 0;
		m_gpuProfFramesAccum = 0;
	}
}

// Uncached-upload profiler (see the bucket declarations in the header). Wall
// time is QPC, measured from the t0 Set_Texture takes right after the cache
// check to the moment the upload path finishes - so it covers the LockRect
// chain, any CPU decode, and CreateTexture2D+SRV+sampler creation.
long long D3D11Backend::Upload_Prof_Now()
{
	if (!Gpu_Profile_Enabled()) {
		return 0;
	}
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

void D3D11Backend::Upload_Prof_Account(unsigned int cat, unsigned long long bytes, long long t0)
{
	if (t0 == 0 || cat >= RB_UPLOAD_CAT_COUNT) {
		return;  // t0 == 0: profiling off, Set_Texture paid no QPC either
	}
	static const double s_msPerTick = [] {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return 1000.0 / static_cast<double>(f.QuadPart);
	}();
	LARGE_INTEGER t1;
	QueryPerformanceCounter(&t1);
	m_uploadProf[cat].count += 1;
	m_uploadProf[cat].bytes += bytes;
	m_uploadProf[cat].ms += static_cast<double>(t1.QuadPart - t0) * s_msPerTick;
}

void D3D11Backend::Upload_Prof_Emit(unsigned int frame)
{
	static const char * const s_catName[RB_UPLOAD_CAT_COUNT] = { "nc", "miss", "shadow", "fallback" };
	const double inv = 1.0 / static_cast<double>(RB_GPUPROF_EMIT_FRAMES);
	char line[512];
	int n = std::snprintf(line, sizeof(line), "[D3D11 uploadprof] f%u frames=%u mips=%s swap=%s",
		frame, static_cast<unsigned int>(RB_GPUPROF_EMIT_FRAMES),
		Mip_Upload_Enabled() ? "on" : "off", m_flipModel ? "flip" : "blt");
	for (unsigned int c = 0; c < RB_UPLOAD_CAT_COUNT && n > 0 && n < static_cast<int>(sizeof(line)); ++c) {
		n += std::snprintf(line + n, sizeof(line) - n, " %s=%.1f/f %s_kb=%.1f/f %s_ms=%.3f/f",
			s_catName[c], static_cast<double>(m_uploadProf[c].count) * inv,
			s_catName[c], static_cast<double>(m_uploadProf[c].bytes) * inv / 1024.0,
			s_catName[c], m_uploadProf[c].ms * inv);
	}
	D3D11_Log_Line(line);
	std::memset(m_uploadProf, 0, sizeof(m_uploadProf));
	for (unsigned int i = 0; i < m_uploadProfNCCount; ++i) {
		const UploadProfNCEntry & e = m_uploadProfNC[i];
		std::snprintf(line, sizeof(line),
			"[D3D11 uploadprof-nc] id=%u name=%s pool=%u proc=%d %ux%u fmt=%u n=%.1f/f kb=%.1f/f",
			e.id, e.name[0] ? e.name : "(unnamed)", e.pool, e.procedural ? 1 : 0,
			e.w, e.h, e.fmt, static_cast<double>(e.count) * inv,
			static_cast<double>(e.bytes) * inv / 1024.0);
		D3D11_Log_Line(line);
	}
	std::memset(m_uploadProfNC, 0, sizeof(m_uploadProfNC));
	m_uploadProfNCCount = 0;
}

void D3D11Backend::Upload_Prof_Note_NC(TextureBaseClass * texture, unsigned int w, unsigned int h,
	unsigned int fmt, unsigned long long bytes)
{
	const unsigned int id = texture->Get_ID();
	unsigned int i = 0;
	for (; i < m_uploadProfNCCount; ++i) {
		if (m_uploadProfNC[i].id == id) {
			break;
		}
	}
	if (i == m_uploadProfNCCount) {
		if (m_uploadProfNCCount >= RB_UPLOAD_NC_IDS) {
			return;
		}
		UploadProfNCEntry & e = m_uploadProfNC[i];
		e.id = id;
		const char * nm = static_cast<const char *>(texture->Get_Texture_Name());
		std::snprintf(e.name, sizeof(e.name), "%s", nm != nullptr ? nm : "");
		e.pool = static_cast<unsigned int>(texture->Get_Pool());
		e.procedural = texture->Is_Procedural();
		e.w = w; e.h = h; e.fmt = fmt;
		e.count = 0; e.bytes = 0;
		++m_uploadProfNCCount;
	}
	m_uploadProfNC[i].count += 1;
	m_uploadProfNC[i].bytes += bytes;
}

void D3D11Backend::Begin_Scene()
{
	QueryPerformanceCounter(&m_frameStartT);
	// God rays: the sun screen-position is refreshed by the terrain shader
	// setup each frame; zero the intensity so a stale position can never glow
	// (e.g. when the terrain normal path is inactive).
	m_sunScreen[2] = 0.0f;
	// Release the previous frame's dynamic buffers. By the time the next frame
	// begins the prior frame's GPU work is done, so this is safe and keeps memory
	// bounded (no per-upload leak, no in-flight reuse -> no TDR/AV).
	Flush_Dynamic_Pools();

	// Per-subsystem diagnostic hotkeys (see g_w3dnextDiagMode in RenderBackend.h).
	// Each key disables exactly one suspect so the halo's source can be found in a
	// single press instead of cycling F11 four times. Edge-triggered; works even
	// from the menu before a map loads. Plain '1'-'4' is fine here (no modifier):
	//   1 -> unit normalmap OFF, 2 -> screen filters OFF, 3 -> water OFF,
	//   4 -> ALL suspects OFF, F11 -> NORMAL (all on / baseline).
	struct DiagKey { int vk; int mode; };
	static const DiagKey s_diagKeys[] = {
		{ '1', 3 }, { '2', 2 }, { '3', 1 }, { '4', 4 }, { VK_F11, 0 }
	};
	static bool s_prevKey[5] = {};
	for (int i = 0; i < 5; ++i) {
		bool down = (GetAsyncKeyState(s_diagKeys[i].vk) & 0x8000) != 0;
		if (down && !s_prevKey[i]) {
			Set_W3DNext_Diag_Mode(s_diagKeys[i].mode);
		}
		s_prevKey[i] = down;
	}

	// D3D11 has no BeginScene; just make sure the backbuffer targets are bound
	// (a later render-to-texture pass may have unbound them).
	Bind_Back_Buffer_Targets();
	Gpu_Profile_Open_Frame();
	if (FILE * f = Draw_Log_File()) {
		std::fprintf(f, "--- Begin_Scene ---\n");
		std::fflush(f);
	}
}

// Env-gated backbuffer dump (W3DNEXT_D3D11_FRAMEDUMP=<path-prefix>): at frames
// 300/600/900 after backend construction, copy the backbuffer to a staging
// texture and write <prefix>_fNNN.ppm (binary P6), logging mean luminance to
// the W3DNEXT_D3D11_LOG sink. Window/focus-independent ground truth for what the
// backend actually rendered - PrintWindow/screen grabs need the game window
// focused and unoccluded, which no headless A/B run can guarantee.
// The caller owns the frame counter (D3D11Backend::m_flipFrame) so the dumped
// fNNN and the texcache state reported for it name the same frame.
static void Dump_Back_Buffer(ID3D11Device * device, ID3D11DeviceContext * context,
	IDXGISwapChain * swapChain, unsigned int frame, bool texCacheWasOn)
{
	static const char * s_prefix = nullptr;
	static bool s_checked = false;
	// Dump frames: default 300/600/900; W3DNEXT_FRAMEDUMP_FRAMES="900,2700" overrides
	// (shared with the DX8 twin) so in-world runs can dump past the load screen.
	static unsigned int s_frames[8] = { 300, 600, 900, 0, 0, 0, 0, 0 };
	// Cadence mode for video assembly / flicker hunts: W3DNEXT_FRAMEDUMP_EVERY=N dumps
	// every Nth frame, bounded by W3DNEXT_FRAMEDUMP_FROM / W3DNEXT_FRAMEDUMP_TO (0 = open).
	// Additive to the frame list; off unless set.
	static unsigned int s_every = 0, s_from = 0, s_to = 0;
	if (!s_checked) {
		s_checked = true;
		s_prefix = W3DNext_GetEnv("D3D11_FRAMEDUMP");
		if (s_prefix != nullptr && s_prefix[0] == '\0') {
			s_prefix = nullptr;
		}
		const char * fl = W3DNext_GetEnv("FRAMEDUMP_FRAMES");
		if (fl != nullptr && fl[0] != '\0') {
			int n = 0;
			for (const char * c = fl; *c != '\0' && n < 8;) {
				unsigned int v = 0;
				while (*c >= '0' && *c <= '9') { v = v * 10u + (unsigned int)(*c - '0'); ++c; }
				if (v != 0u) s_frames[n++] = v;
				while (*c != '\0' && (*c < '0' || *c > '9')) ++c;
			}
			for (int i = n; i < 8; ++i) s_frames[i] = 0;
		}
		const char * ev = W3DNext_GetEnv("FRAMEDUMP_EVERY");
		if (ev != nullptr) s_every = static_cast<unsigned int>(std::atoi(ev));
		const char * fr = W3DNext_GetEnv("FRAMEDUMP_FROM");
		if (fr != nullptr) s_from = static_cast<unsigned int>(std::atoi(fr));
		const char * to = W3DNext_GetEnv("FRAMEDUMP_TO");
		if (to != nullptr) s_to = static_cast<unsigned int>(std::atoi(to));
	}
	if (s_prefix == nullptr || device == nullptr || context == nullptr || swapChain == nullptr) {
		return;
	}
	bool want = false;
	for (int i = 0; i < 8 && s_frames[i] != 0u; ++i) {
		if (frame == s_frames[i]) { want = true; break; }
	}
	if (!want && s_every != 0u && frame >= s_from && (s_to == 0u || frame <= s_to) &&
		(frame - s_from) % s_every == 0u) {
		want = true;
	}
	if (!want) {
		return;
	}
	ID3D11Texture2D * back = nullptr;
	if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back))) || back == nullptr) {
		return;
	}
	D3D11_TEXTURE2D_DESC desc;
	back->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;
	ID3D11Texture2D * staging = nullptr;
	if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)) && staging != nullptr) {
		context->CopyResource(staging, back);
		D3D11_MAPPED_SUBRESOURCE map;
		if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
			char path[512];
			std::snprintf(path, sizeof(path), "%s_f%u.ppm", s_prefix, frame);
			FILE * f = std::fopen(path, "wb");
			unsigned long long lum = 0;
			if (f != nullptr) {
				std::fprintf(f, "P6\n%u %u\n255\n", desc.Width, desc.Height);
				// BGRA (or RGBA) 8-bit backbuffer assumed; channel order per format.
				const bool bgra = (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
					desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
				for (unsigned int y = 0; y < desc.Height; ++y) {
					const unsigned char * row = static_cast<const unsigned char *>(map.pData) + static_cast<size_t>(y) * map.RowPitch;
					for (unsigned int x = 0; x < desc.Width; ++x) {
						const unsigned char c0 = row[x * 4 + 0], c1 = row[x * 4 + 1], c2 = row[x * 4 + 2];
						unsigned char rgb[3];
						if (bgra) { rgb[0] = c2; rgb[1] = c1; rgb[2] = c0; }
						else      { rgb[0] = c0; rgb[1] = c1; rgb[2] = c2; }
						std::fwrite(rgb, 1, 3, f);
						lum += (rgb[0] + rgb[1] + rgb[2]);
					}
				}
				std::fclose(f);
			}
			context->Unmap(staging, 0);
			const double mean = static_cast<double>(lum) /
				(static_cast<double>(desc.Width) * desc.Height * 3.0);
			const char * lp = W3DNext_GetEnv("D3D11_LOG");
			FILE * lf = std::fopen(lp != nullptr ? lp : "d3d11_backend.log", "a");
			if (lf != nullptr) {
				// ms since process start on every dump line. Two rungs give a
				// machine-readable fps over that window: (f_b - f_a) / (t_b - t_a).
				// Added 2026-07-26 because the only fps signal was the on-screen
				// overlay, which cannot be read without OCR'ing the dump - and an
				// fps claim was made and then falsified against exactly that.
				std::fprintf(lf, "[D3D11 framedump] f%u %ux%u meanlum=%.2f texcache=%s t_ms=%llu mips=%s -> %s\n",
					frame, desc.Width, desc.Height, mean,
					texCacheWasOn ? "on" : "off",
					static_cast<unsigned long long>(GetTickCount64()),
					D3D11Backend::Mip_Upload_Enabled() ? "on" : "off", path);
				std::fclose(lf);
			}
		}
		staging->Release();
	}
	back->Release();
}

// Cache state for the draws of the frame currently being built. In toggle mode
// it alternates on the flip-frame counter, which End_Scene advances AFTER the
// frame is dumped - so the value read here is the one the just-drawn frame used.
bool D3D11Backend::Tex_Cache_Enabled_This_Frame() const
{
	static const bool s_baseEnabled = [] {
		const char * e = W3DNext_GetEnv("D3D11_TEXCACHE");
		return !(e != nullptr && e[0] == '0');
	}();
	if (!s_baseEnabled) {
		return false;
	}
	return !m_texCacheToggleMode || (m_flipFrame % 2u) == 0u;
}

void D3D11Backend::End_Scene(bool flip_frame)
{
	if (flip_frame && m_swapChain != nullptr) {
		// GPU profile phase cut: tail2d..here is WW3D::End_Render's internals
		// (sorting-renderer flush etc.); endscene..present is the backbuffer
		// dump (rung frames only) plus the Present itself.
		Gpu_Profile_Marker("endscene");
		// Sample BEFORE advancing the counter: this is the state the frame about
		// to be dumped actually rendered with.
		const bool texCacheWasOn = Tex_Cache_Enabled_This_Frame();
		const unsigned int frame = ++m_flipFrame;
		Dump_Back_Buffer(m_device, m_context, m_swapChain, frame, texCacheWasOn);
		// Texture-cache effectiveness, sampled once per 600 frames into the
		// W3DNEXT_D3D11_LOG sink: per-frame deltas make a regression to the old
		// upload-on-every-bind behavior (uploads ~= binds) machine-visible.
		{
			static unsigned int s_lastHits = 0, s_lastUploads = 0, s_lastEvictions = 0;
			if ((frame % 600u) == 0u) {
				const unsigned int dh = m_texCacheHits - s_lastHits;
				const unsigned int du = m_texCacheUploads - s_lastUploads;
				// Evictions are the churn witness: a run that reports evictions has
				// actually freed and reallocated textures, which is precisely the
				// condition under which the old pointer key aliased.
				const unsigned int de = m_texCacheEvictions - s_lastEvictions;
				s_lastHits = m_texCacheHits;
				s_lastUploads = m_texCacheUploads;
				s_lastEvictions = m_texCacheEvictions;
				char line[192];
				std::snprintf(line, sizeof(line),
					"[D3D11 texcache] f%u hits/600f=%u uploads/600f=%u evictions/600f=%u entries=%u",
					frame, dh, du, de, static_cast<unsigned int>(m_textureCache.size()));
				D3D11_Log_Line(line);
			}
		}
		if (Gpu_Profile_Enabled() && (frame % RB_GPUPROF_EMIT_FRAMES) == 0u) {
			Upload_Prof_Emit(frame);
		}
		// F8 toggles color grading, F6 toggles the night-vision filter - both
		// edge-detected so holding a key flips exactly once.
		{
			static bool s_gradeKeyWasDown = false;
			const bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
			if (down && !s_gradeKeyWasDown
				&& m_gradePixelShader != nullptr && m_gradeVertexShader != nullptr
				&& m_gradeBuffer != nullptr && m_gradeSampler != nullptr) {
				m_gradeEnabled = !m_gradeEnabled;
			}
			s_gradeKeyWasDown = down;

			static bool s_nvKeyWasDown = false;
			const bool nvDown = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
			if (nvDown && !s_nvKeyWasDown && m_gradeEnabled) {
				m_gradeNightVision = !m_gradeNightVision;
			}
			s_nvKeyWasDown = nvDown;
		}

		// F10 dumps the finished frame (post-grade) as a 24-bit uncompressed TGA
		// so render issues can be inspected offline. Default path
		// E:\GAVAD_Test\dncshot.tga, override with D3D11_SHOT=<file>.
		{
			static bool s_shotKeyWasDown = false;
			const bool shotDown = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
			if (shotDown && !s_shotKeyWasDown && m_swapChain != nullptr) {
				unsigned int sw = 0, sh = 0;
				if (Read_Back_Buffer(nullptr, sw, sh) && sw > 0 && sh > 0) {
					std::vector<unsigned char> rgb(static_cast<size_t>(sw) * sh * 3u);
					if (Read_Back_Buffer(rgb.data(), sw, sh)) {
						const char * path = W3DNext_GetEnv("D3D11_SHOT");
						FILE * f = std::fopen(path != nullptr ? path : "E:\\GAVAD_Test\\dncshot.tga", "wb");
						if (f != nullptr) {
							unsigned char hdr[18];
							ZeroMemory(hdr, sizeof(hdr));
							hdr[2] = 2;                    // uncompressed true-color
							hdr[12] = static_cast<unsigned char>(sw & 0xFF);
							hdr[13] = static_cast<unsigned char>((sw >> 8) & 0xFF);
							hdr[14] = static_cast<unsigned char>(sh & 0xFF);
							hdr[15] = static_cast<unsigned char>((sh >> 8) & 0xFF);
							hdr[16] = 24;                  // bits per pixel
							hdr[17] = 0x20;                // top-down rows
							std::fwrite(hdr, 1, sizeof(hdr), f);
							for (unsigned int row = 0; row < sh; ++row) {
								const unsigned char * r = rgb.data() + static_cast<size_t>(row) * sw * 3u;
								for (unsigned int x = 0; x < sw; ++x) {
									unsigned char bgr[3] = { r[x*3+2], r[x*3+1], r[x*3+0] };
									std::fwrite(bgr, 1, 3, f);
								}
							}
							std::fclose(f);
						}
					}
				}
			}
			s_shotKeyWasDown = shotDown;
		}
		// NOTE: the color-grade/god-ray pass now runs from W3DDisplay::draw()
		// right after the 3D scene and BEFORE the 2D UI (the radial march must
		// not sample interface elements). Only the hotkeys live here now.
		Handle_Present_Result(m_swapChain->Present(0, 0));
		Gpu_Profile_Close_Frame();
		// Per-frame resource diagnostic: watch for unbounded growth (the 200->1
		// decay). texEntries rising == texture-cache leak; up(+N) large every frame
		// == upload churn; vbPool/ibPool rising == dynamic-buffer leak.
		// draws(+N) and ms = CPU time of the frame (time since previous present).
		{
			static unsigned int s_dHits = 0, s_dUp = 0, s_dEv = 0, s_dDraws = 0;
			static LARGE_INTEGER s_dFreq = { 0 }, s_dPrev = { 0 };
			if (s_dFreq.QuadPart == 0) {
				QueryPerformanceFrequency(&s_dFreq);
			}
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			const double dt = (s_dPrev.QuadPart != 0)
				? static_cast<double>(now.QuadPart - s_dPrev.QuadPart) / static_cast<double>(s_dFreq.QuadPart) * 1000.0
				: 0.0;
			s_dPrev = now;
			const unsigned int dHits = m_texCacheHits - s_dHits;
			const unsigned int dUp = m_texCacheUploads - s_dUp;
			const unsigned int dEv = m_texCacheEvictions - s_dEv;
			const unsigned int dDraws = m_frameDrawCalls;
			const double dbgApply = m_dbgApplyMs, dbgConst = m_dbgConstMs;
			const double dbgOther = m_dbgOtherMs, dbgDraw = m_dbgDrawMs;
			const double dbgSetTex = m_dbgSetTexMs;
			const double dbgUp = m_dbgUploadMs;
			const unsigned int dbgUploads = m_dbgUploads;
			s_dHits = m_texCacheHits; s_dUp = m_texCacheUploads; s_dEv = m_texCacheEvictions;
			m_frameDrawCalls = 0;
			m_dbgApplyMs = 0.0; m_dbgConstMs = 0.0; m_dbgOtherMs = 0.0; m_dbgDrawMs = 0.0;
			m_dbgSetTexMs = 0.0; m_dbgUploadMs = 0.0; m_dbgUploads = 0;
			char line[512];
			std::snprintf(line, sizeof(line),
				"[frame] f=%u texEntries=%u hits(+%u) up(+%u) evict(+%u) draws(+%u) ms=%.1f rms=%.1f apply=%.1f const=%.1f other=%.1f draw=%.1f settex=%.1f uploads(+%u) upMs=%.1f vbKB=%u ibKB=%u",
				frame,
				static_cast<unsigned int>(m_textureCache.size()),
				dHits, dUp, dEv, dDraws, dt,
				(s_dFreq.QuadPart != 0)
					? static_cast<double>(now.QuadPart - m_frameStartT.QuadPart) / static_cast<double>(s_dFreq.QuadPart) * 1000.0
					: 0.0,
				dbgApply, dbgConst, dbgOther, dbgDraw, dbgSetTex,
				dbgUploads, dbgUp,
				m_dynVBOffset / 1024u, m_dynIBOffset / 1024u);
			D3D11_Log_Line(line);
		}
		// Release this frame's dynamic buffers after Present. The draws are
		// submitted; releasing only drops our refcount and the driver keeps a GPU
		// reference until they finish - so this is safe and bounds memory/FPS even
		// if Begin_Scene's flush is missed.
		Flush_Dynamic_Pools();
	}
}

void D3D11Backend::Flip_To_Primary()
{
	if (m_swapChain != nullptr) {
		Handle_Present_Result(m_swapChain->Present(0, 0));
	}
}

void D3D11Backend::Handle_Present_Result(long hr)
{
	if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET) {
		return;
	}
	if (!m_deviceRemoved) {
		m_deviceRemoved = true;
		const long reason = (m_device != nullptr) ? static_cast<long>(m_device->GetDeviceRemovedReason()) : 0;
		char line[160];
		std::snprintf(line, sizeof(line),
			"[D3D11] FATAL: Present() failed 0x%08lX, GetDeviceRemovedReason=0x%08lX - device removed (TDR?), exiting",
			hr, reason);
		D3D11_Log_Line(line);
	}
	// No engine-side consumer of Is_Device_Lost() exists on the D3D11 path and
	// the DX8 reset machinery services only the D3D8 device, so recovery is not
	// possible here. A clean exit beats the alternative: a frozen swapchain with
	// the simulation and audio running headless-blind.
	// TerminateProcess, NOT ExitProcess: ExitProcess kills the other threads and
	// then runs DLL_PROCESS_DETACH, which deadlocks if a killed thread held the
	// CRT or vendor-driver heap lock - likely on exactly the TDR'ing machines
	// this path fires on. The log line above is already flushed (fopen/append/
	// fclose per line); nothing here needs orderly teardown.
	TerminateProcess(GetCurrentProcess(), 1);
}

void D3D11Backend::Clear(bool clear_color, bool clear_z_stencil, const Vector3 & color, float dest_alpha, float z, unsigned int stencil)
{
	if (m_context == nullptr) {
		return;
	}

	if (clear_color && m_currentRTV != nullptr) {
		const float rgba[4] = { color.X, color.Y, color.Z, dest_alpha };
		m_context->ClearRenderTargetView(m_currentRTV, rgba);
	}
	if (clear_z_stencil && m_currentDSV != nullptr) {
		m_context->ClearDepthStencilView(
			m_currentDSV,
			D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
			z,
			static_cast<UINT8>(stencil));
	}
}

void D3D11Backend::Set_Viewport(const RenderBackendViewport & viewport)
{
	if (m_context == nullptr) {
		return;
	}

	D3D11_VIEWPORT vp;
	vp.TopLeftX = static_cast<float>(viewport.x);
	vp.TopLeftY = static_cast<float>(viewport.y);
	vp.Width = static_cast<float>(viewport.width);
	vp.Height = static_cast<float>(viewport.height);
	vp.MinDepth = viewport.min_z;
	vp.MaxDepth = viewport.max_z;
	m_context->RSSetViewports(1, &vp);
	if (m_viewportWidth != static_cast<int>(viewport.width) || m_viewportHeight != static_cast<int>(viewport.height)) {
		m_viewportWidth = static_cast<int>(viewport.width);
		m_viewportHeight = static_cast<int>(viewport.height);
		m_transformDirty = true; // half-pixel offset depends on the viewport extent
	}
}

// ----------------------------------------------------------------------------
//
// Device queries (real, answered from the objects created above)
//
// ----------------------------------------------------------------------------

bool D3D11Backend::Is_Device_Lost() const
{
	// D3D11 has no DX8-style lost-device model, but the device CAN be removed
	// (TDR, driver update). Handle_Present_Result latches the flag; kept as a
	// truthful query even though the current failure policy exits first.
	return m_deviceRemoved;
}

bool D3D11Backend::Has_Stencil()
{
	// The depth buffer is always created as D24S8.
	return m_depthStencilView != nullptr;
}

WW3DFormat D3D11Backend::Get_Back_Buffer_Format()
{
	// DXGI_FORMAT_B8G8R8A8_UNORM in engine terms.
	return WW3D_FORMAT_A8R8G8B8;
}

// ----------------------------------------------------------------------------
//
// Everything below is a stub (later RENDERER_PORT steps: geometry, state,
// transforms, lighting, draw, render targets).
//
// ----------------------------------------------------------------------------

SurfaceClass * D3D11Backend::Get_Back_Buffer(unsigned int num)
{
	D3D11_STUB();
	return nullptr;
}

void D3D11Backend::Set_Gamma(float gamma, float bright, float contrast, bool calibrate, bool uselimit)
{
	D3D11_STUB();
}

// The four W3D-typed geometry-bind overloads dereference VertexBufferClass /
// IndexBufferClass / the dynamic-access classes, which need the full WW3D2 header
// graph the standalone smoke translation unit does not link. Their real bodies
// live in the ww3d2-only D3D11Backend_W3D.cpp (built only into the game, which
// defines W3DNEXT_D3D11_W3D_TU). For the smoke build (macro undefined) these records-
// only stubs remain so the vtable stays complete and the smoke test - which drives
// the GPU path through the typed Upload_* entry points - links unchanged.
#ifndef W3DNEXT_D3D11_W3D_TU
void D3D11Backend::Set_Vertex_Buffer(const VertexBufferClass * vb, unsigned int stream)
{
	D3D11_TRACE_PARTIAL();
	m_boundVertexBuffer = vb;
	m_boundVertexStream = stream;
}

void D3D11Backend::Set_Vertex_Buffer(const DynamicVBAccessClass & vba)
{
	D3D11_STUB();
}

void D3D11Backend::Set_Index_Buffer(const IndexBufferClass * ib, unsigned short index_base_offset)
{
	D3D11_TRACE_PARTIAL();
	m_boundIndexBuffer = ib;
	m_indexBaseOffset = index_base_offset;
}

void D3D11Backend::Set_Index_Buffer(const DynamicIBAccessClass & iba, unsigned short index_base_offset)
{
	D3D11_TRACE_PARTIAL();
	m_indexBaseOffset = index_base_offset;
}
// Real body in D3D11Backend_W3D.cpp (mirrors the offset into DX8Wrapper's
// render_state record); records-only stub here for the smoke link.
void D3D11Backend::Set_Index_Buffer_Index_Offset(unsigned int offset)
{
	D3D11_TRACE_PARTIAL();
	m_indexBaseOffset = offset;
}
#endif // W3DNEXT_D3D11_W3D_TU

// Real body in D3D11Backend_W3D.cpp (needs shader.h); stub here for the smoke link.
#ifndef W3DNEXT_D3D11_W3D_TU
void D3D11Backend::Set_Shader(const ShaderClass & shader)
{
	D3D11_STUB();
}

// Real body in D3D11Backend_W3D.cpp (needs texture.h for Get_Texture_Name);
// the smoke build binds no W3D textures, so the stub reports none.
const char * D3D11Backend::Peek_Stage_Tex_Name(unsigned int stage) const
{
	(void)stage;
	return "";
}

// Real body in D3D11Backend_W3D.cpp (needs dx8wrapper.h); the smoke build has
// no DX8Wrapper record to mirror transforms into, so this is a no-op.
void Mirror_Transform_To_Wrapper(TransformKind transform, const Matrix4x4 & m)
{
	(void)transform;
	(void)m;
}
#endif // W3DNEXT_D3D11_W3D_TU

void D3D11Backend::Get_Shader(ShaderClass & shader)
{
	D3D11_STUB(); // leaves the caller's shader untouched
}

// Real bodies in D3D11Backend_W3D.cpp (need vertmaterial.h / texture.h); records-
// only stubs here for the smoke link (macro undefined).
#ifndef W3DNEXT_D3D11_W3D_TU
void D3D11Backend::Set_Material(const VertexMaterialClass * material)
{
	(void)material;
	D3D11_STUB();
}

void D3D11Backend::Set_Texture(unsigned int stage, TextureBaseClass * texture)
{
	D3D11_TRACE_PARTIAL();
	if (stage < RB_MAX_TEXTURE_STAGES) {
		m_boundTextures[stage] = texture;
	}
}
#endif // W3DNEXT_D3D11_W3D_TU

// ----------------------------------------------------------------------------
//
// Blend / depth / rasterizer state-object cache (RENDERER_PORT.md step 9)
//
// The typed setters mutate the legacy render-state vector and mark it dirty.
// Apply_Render_State_Changes is the deferred-flush boundary: it asks the cache
// for the three immutable state objects keyed by the current vector (creating
// them on a miss, reusing the cached pointer on a hit) and binds them via
// OMSetBlendState / OMSetDepthStencilState / RSSetState. No per-draw CreateXState.
//
// ----------------------------------------------------------------------------

void D3D11Backend::Set_Blend_Enable(bool enable)
{
	if (m_renderState.blendEnable != enable) {
		m_renderState.blendEnable = enable;
		m_renderStateDirty = true;
	}
}

void D3D11Backend::Set_Blend_Func(RenderBackendBlendFactor src, RenderBackendBlendFactor dst)
{
	if (m_renderState.srcBlend != src || m_renderState.dstBlend != dst) {
		m_renderState.srcBlend = src;
		m_renderState.dstBlend = dst;
		m_renderStateDirty = true;
	}
}

// Raw D3DBLEND_* writes forwarded from DX8Wrapper::Set_DX8_Render_State (the
// soft-water-edge DESTALPHA blend and the water alpha pass set these through
// the legacy path, which has no ShaderClass equivalent). Single-sided so the
// render-state vector stays the single source of truth.
static RenderBackendBlendFactor D3DBlend_To_RB(unsigned int v)
{
	switch (v) {
	case D3DBLEND_ZERO:            return RB_BLEND_ZERO;
	case D3DBLEND_SRCCOLOR:        return RB_BLEND_SRCCOLOR;
	case D3DBLEND_INVSRCCOLOR:     return RB_BLEND_INVSRCCOLOR;
	case D3DBLEND_SRCALPHA:        return RB_BLEND_SRCALPHA;
	case D3DBLEND_INVSRCALPHA:     return RB_BLEND_INVSRCALPHA;
	case D3DBLEND_DESTALPHA:       return RB_BLEND_DESTALPHA;
	case D3DBLEND_INVDESTALPHA:    return RB_BLEND_INVDESTALPHA;
	case D3DBLEND_DESTCOLOR:       return RB_BLEND_DESTCOLOR;
	case D3DBLEND_INVDESTCOLOR:    return RB_BLEND_INVDESTCOLOR;
	case D3DBLEND_ONE:
	default:                       return RB_BLEND_ONE;
	}
}

void D3D11Backend::Set_Raw_Blend_Enable(unsigned int enable)
{
	Set_Blend_Enable(enable != 0);
}

void D3D11Backend::Set_Raw_Src_Blend(unsigned int d3dblend)
{
	const RenderBackendBlendFactor f = D3DBlend_To_RB(d3dblend);
	if (m_renderState.srcBlend != f) {
		m_renderState.srcBlend = f;
		m_renderStateDirty = true;
	}
}

void D3D11Backend::Set_Raw_Dst_Blend(unsigned int d3dblend)
{
	const RenderBackendBlendFactor f = D3DBlend_To_RB(d3dblend);
	if (m_renderState.dstBlend != f) {
		m_renderState.dstBlend = f;
		m_renderStateDirty = true;
	}
}

// Raw D3DRS_STENCIL* writes (52..59) forwarded from DX8Wrapper - the shadow
// volume passes configure stenciling through the legacy path. D3DCMPFUNC and
// D3DSTENCILOP are 1-based in DX8 and 0-based here, hence the -1 maps.
void D3D11Backend::Set_Raw_Stencil(unsigned int d3drs, unsigned int value)
{
	switch (d3drs) {
	case 52: // D3DRS_STENCILENABLE
		if (m_renderState.stencilEnable != (value != 0)) {
			m_renderState.stencilEnable = (value != 0);
			m_renderStateDirty = true;
		}
		break;
	case 53: // D3DRS_STENCILFAIL: D3DSTENCILOP_KEEP=1..DECR=8 -> RB 0..7
		if (m_renderState.stencilFail != (RenderBackendStencilOp)(value - 1)) {
			m_renderState.stencilFail = (RenderBackendStencilOp)(value - 1);
			m_renderStateDirty = true;
		}
		break;
	case 54: // D3DRS_STENCILZFAIL
		if (m_renderState.stencilZFail != (RenderBackendStencilOp)(value - 1)) {
			m_renderState.stencilZFail = (RenderBackendStencilOp)(value - 1);
			m_renderStateDirty = true;
		}
		break;
	case 55: // D3DRS_STENCILPASS
		if (m_renderState.stencilPass != (RenderBackendStencilOp)(value - 1)) {
			m_renderState.stencilPass = (RenderBackendStencilOp)(value - 1);
			m_renderStateDirty = true;
		}
		break;
	case 56: // D3DRS_STENCILFUNC: D3DCMPFUNC NEVER=1..ALWAYS=8 -> RB_CMP 0..7
		if (m_renderState.stencilFunc != (RenderBackendCmpFunc)(value - 1)) {
			m_renderState.stencilFunc = (RenderBackendCmpFunc)(value - 1);
			m_renderStateDirty = true;
		}
		break;
	case 57: // D3DRS_STENCILREF
		if (m_renderState.stencilRef != value) {
			m_renderState.stencilRef = value;
			m_renderStateDirty = true;
		}
		break;
	case 58: // D3DRS_STENCILMASK
		if (m_renderState.stencilMask != value) {
			m_renderState.stencilMask = value;
			m_renderStateDirty = true;
		}
		break;
	case 59: // D3DRS_STENCILWRITEMASK
		if (m_renderState.stencilWriteMask != value) {
			m_renderState.stencilWriteMask = value;
			m_renderStateDirty = true;
		}
		break;
	// Two-sided stencil (185..189) - the shadow-volume passes rely on these to
	// set DIFFERENT ops for front- vs back-facing shadow polygons (e.g. incr on
	// back-face depth-fail, decr on front-face depth-fail) so the volume counts
	// correctly. These fell into the default case (silently dropped) before,
	// which left D3D11States.cpp always mirroring the front-face op onto the
	// back face - breaking the volume count and making shadows render as if
	// stuck to the casting object instead of projected onto the ground.
	case 185: // D3DRS_TWOSIDEDSTENCILMODE
		if (m_renderState.twoSidedStencil != (value != 0)) {
			m_renderState.twoSidedStencil = (value != 0);
			m_renderStateDirty = true;
		}
		break;
	case 186: // D3DRS_CCW_STENCILFAIL
		if (m_renderState.ccwStencilFail != (RenderBackendStencilOp)(value - 1)) {
			m_renderState.ccwStencilFail = (RenderBackendStencilOp)(value - 1);
			m_renderStateDirty = true;
		}
		break;
	case 187: // D3DRS_CCW_STENCILZFAIL
		if (m_renderState.ccwStencilZFail != (RenderBackendStencilOp)(value - 1)) {
			m_renderState.ccwStencilZFail = (RenderBackendStencilOp)(value - 1);
			m_renderStateDirty = true;
		}
		break;
	case 188: // D3DRS_CCW_STENCILPASS
		if (m_renderState.ccwStencilPass != (RenderBackendStencilOp)(value - 1)) {
			m_renderState.ccwStencilPass = (RenderBackendStencilOp)(value - 1);
			m_renderStateDirty = true;
		}
		break;
	case 189: // D3DRS_CCW_STENCILFUNC
		if (m_renderState.ccwStencilFunc != (RenderBackendCmpFunc)(value - 1)) {
			m_renderState.ccwStencilFunc = (RenderBackendCmpFunc)(value - 1);
			m_renderStateDirty = true;
		}
		break;
	default:
		break;
	}
}

void D3D11Backend::Set_Blend_Op(RenderBackendBlendOp op)
{
	if (m_renderState.blendOp != op) {
		m_renderState.blendOp = op;
		m_renderStateDirty = true;
	}
}

void D3D11Backend::Set_Color_Write_Enable(unsigned int mask)
{
	const unsigned int m = mask & 0xF;
	if (m_renderState.colorWriteEnable != m) {
		m_renderState.colorWriteEnable = m;
		m_renderStateDirty = true;
	}
}

void D3D11Backend::Set_Depth_Test_Enable(bool enable)
{
	if (m_renderState.depthEnable != enable) {
		m_renderState.depthEnable = enable;
		m_renderStateDirty = true;
	}
}

void D3D11Backend::Set_Depth_Write_Enable(bool enable)
{
	if (m_renderState.depthWrite != enable) {
		m_renderState.depthWrite = enable;
		m_renderStateDirty = true;
	}
}

void D3D11Backend::Set_Depth_Func(RenderBackendCmpFunc func)
{
	if (m_renderState.depthFunc != func) {
		m_renderState.depthFunc = func;
		m_renderStateDirty = true;
	}
}

void D3D11Backend::Set_Cull_Mode(RenderBackendCullMode mode)
{
	if (m_renderState.cullMode != mode) {
		m_renderState.cullMode = mode;
		m_renderStateDirty = true;
	}
}

void D3D11Backend::Set_Fill_Mode(RenderBackendFillMode mode)
{
	if (m_renderState.fillMode != mode) {
		m_renderState.fillMode = mode;
		m_renderStateDirty = true;
	}
}

const void * D3D11Backend::Get_Bound_Blend_State() const { return m_activeBlendState; }
const void * D3D11Backend::Get_Bound_Depth_State() const { return m_activeDepthState; }
const void * D3D11Backend::Get_Bound_Rasterizer_State() const { return m_activeRasterizerState; }

void D3D11Backend::Apply_Render_State_Changes()
{
	if (m_context == nullptr || m_device == nullptr) {
		return;
	}
	if (!m_renderStateDirty) {
		return; // nothing changed since the last flush
	}

	// Build-or-reuse the three cached objects for the current vector. Repeated
	// identical vectors reuse one object each (see D3D11StateCache).
	ID3D11BlendState * blend = m_stateCache.Get_Blend_State(m_device, m_renderState);
	ID3D11DepthStencilState * depth = m_stateCache.Get_Depth_State(m_device, m_renderState);
	ID3D11RasterizerState * raster = m_stateCache.Get_Rasterizer_State(m_device, m_renderState);

	const float blend_factor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	if (blend != m_lastBlendState) {
		m_context->OMSetBlendState(blend, blend_factor, 0xffffffffu);
		m_lastBlendState = blend;
	}
	if (depth != m_lastDepthState || m_lastStencilRef != m_renderState.stencilRef) {
		m_context->OMSetDepthStencilState(depth, m_renderState.stencilRef);
		m_lastDepthState = depth;
		m_lastStencilRef = m_renderState.stencilRef;
	}
	if (raster != m_lastRasterState) {
		if (raster != nullptr) {
			m_context->RSSetState(raster);
		}
		m_lastRasterState = raster;
	}

	m_activeBlendState = blend;
	m_activeDepthState = depth;
	m_activeRasterizerState = raster;
	m_renderStateDirty = false;
}

void D3D11Backend::Apply_Default_State()
{
	// Reset the render-state vector to the DX8 opaque default (mirrors
	// DX8Wrapper::Apply_Default_State: blend off ONE/ZERO, depth test+write on
	// LESSEQUAL, cull CW, solid) and mark it dirty; the next
	// Apply_Render_State_Changes binds the cached objects.
	m_renderState = RenderStateVector();
	m_renderStateDirty = true;
}

void D3D11Backend::Invalidate_Cached_Render_States()
{
	// Force the next Apply_Render_State_Changes to re-bind, e.g. after external
	// code touched the immediate context's OM/RS stages. Clear the last-bound
	// cache too so the re-bind is unconditional (not skipped as a no-op).
	m_renderStateDirty = true;
	m_lastBlendState = nullptr;
	m_lastDepthState = nullptr;
	m_lastRasterState = nullptr;
	m_lastStencilRef = 0xFFFFFFFFu;   // force the OMSet (ref is its argument)
}

void D3D11Backend::Set_Transform(TransformKind transform, const Matrix4x4 & m)
{
	switch (transform) {
	// World/View also feed cbLighting (normal transform + fog depth), so mark it
	// dirty too; Projection does not.
	case RB_TRANSFORM_WORLD:      m_world = m; m_transformDirty = true; m_lightingDirty = true; break;
	case RB_TRANSFORM_VIEW:       m_view = m;  m_transformDirty = true; m_lightingDirty = true; break;
	case RB_TRANSFORM_PROJECTION: m_proj = m;  m_transformDirty = true; break;
	default:
		// Texture transforms (RB_TRANSFORM_TEXTUREn) belong to the combiner
		// step, not this geometry/transform slice; ignore for now.
		break;
	}
	// Mirror WORLD/VIEW into DX8Wrapper's engine-side render_state record (see
	// Mirror_Transform_To_Wrapper in D3D11Backend_W3D.cpp): the sorting renderer
	// captures world/view via DX8Wrapper::Get_Render_State at Insert time; an
	// unmirrored record left every sorted-translucent node (particles, rocket
	// sprites, rotor blur discs) with stale matrices - garbage sort keys and,
	// at flush, a garbage re-apply.
	Mirror_Transform_To_Wrapper(transform, m);
}

void D3D11Backend::Set_Transform(TransformKind transform, const Matrix3D & m)
{
	// Matrix3D is the 3x4 affine subset; widen to a full 4x4 (last row 0,0,0,1).
	Set_Transform(transform, Matrix4x4(m));
}

void D3D11Backend::Get_Transform(TransformKind transform, Matrix4x4 & m)
{
	switch (transform) {
	case RB_TRANSFORM_WORLD:      m = m_world; break;
	case RB_TRANSFORM_VIEW:       m = m_view;  break;
	case RB_TRANSFORM_PROJECTION: m = m_proj;  break;
	default: break; // leaves the caller's matrix untouched
	}
}

void D3D11Backend::Set_World_Identity()
{
	m_world.Make_Identity();
	m_transformDirty = true;
	m_lightingDirty = true;
	Mirror_Transform_To_Wrapper(RB_TRANSFORM_WORLD, m_world);
}

void D3D11Backend::Set_View_Identity()
{
	m_view.Make_Identity();
	m_transformDirty = true;
	m_lightingDirty = true;
	Mirror_Transform_To_Wrapper(RB_TRANSFORM_VIEW, m_view);
}

bool D3D11Backend::Is_World_Identity()
{
	return Matrix_Is_Identity(m_world);
}

bool D3D11Backend::Is_View_Identity()
{
	return Matrix_Is_Identity(m_view);
}

void D3D11Backend::Set_Projection_Transform_With_Z_Bias(const Matrix4x4 & matrix, float znear, float zfar)
{
	// The z-bias remap (near/far) is a combiner/state-step concern; the
	// skeleton stores the projection as-is.
	m_proj = matrix;
	m_transformDirty = true;
}

void D3D11Backend::Set_Light(unsigned int index, const LightClass & light)
{
	// Records-only bind point. Extracting the directional/point parameters from a
	// LightClass needs the full WW3D2 header graph (light.h), which the standalone
	// smoke translation unit does not compile against - so the real GPU-side
	// lighting is driven through Set_Light_Directional() (mirroring the
	// Upload_Vertices / Set_Material split). A DX8-facing caller reads
	// light.Get_Spot_Direction() / light.Get_Diffuse() and calls the typed setter;
	// this overload folds into that path when the backend is wired into DX8Wrapper.
	(void)index;
	(void)light;
	D3D11_STUB();
}

void D3D11Backend::Disable_Light(unsigned int index)
{
	// Turn a light slot off: zero its diffuse so it contributes nothing to the FF
	// sum (the equation loops numLights; a zero-diffuse light is a no-op term).
	if (index >= RB_MAX_LIGHTS) {
		return;
	}
	m_lighting.lightDiffuse[index][0] = 0.0f;
	m_lighting.lightDiffuse[index][1] = 0.0f;
	m_lighting.lightDiffuse[index][2] = 0.0f;
	m_lighting.lightDiffuse[index][3] = 0.0f;
	m_lightingDirty = true;
}

void D3D11Backend::Set_Ambient(const Vector3 & color)
{
	// Real: scene ambient (D3DRS_AMBIENT) feeds the FF ambient term in cbLighting.
	m_ambient = color;
	m_lighting.globalAmbient[0] = color.X;
	m_lighting.globalAmbient[1] = color.Y;
	m_lighting.globalAmbient[2] = color.Z;
	m_lighting.globalAmbient[3] = 1.0f;
	m_lightingDirty = true;
}

const Vector3 & D3D11Backend::Get_Ambient() const
{
	return m_ambient;
}

void D3D11Backend::Set_Fog(bool enable, const Vector3 & color, float start, float end)
{
	// Real: linear fog (the game's D3DFOGTABLEMODE default). Routes to the typed
	// path; density is unused for LINEAR.
	Set_Fog_Params(enable, color, RB_FOG_LINEAR, start, end, 1.0f);
}

bool D3D11Backend::Get_Fog_Enable() const
{
	return m_fogEnable;
}

#ifndef W3DNEXT_D3D11_W3D_TU
// Real body in D3D11Backend_W3D.cpp (mirrors into DX8Wrapper::Set_Light_Environment
// so the sorted-flush light state is captured); records-only stub for the smoke link.
void D3D11Backend::Set_Light_Environment(LightEnvironmentClass * light_env)
{
	// Records the pointer (bind-state, as the DX8 path stores it). Unpacking its
	// up-to-4 directional lights + ambient into cbLighting needs the full WW3D2
	// header graph; a DX8-facing caller drives Set_Ambient / Set_Light_Directional
	// from it. Folds in when the backend is wired into DX8Wrapper.
	D3D11_TRACE_PARTIAL();
	m_lightEnvironment = light_env;
}
#endif // W3DNEXT_D3D11_W3D_TU

LightEnvironmentClass * D3D11Backend::Get_Light_Environment() const
{
	return m_lightEnvironment;
}

// Optional per-draw sequence log, enabled by setting env W3DNEXT_D3D11_DRAWLOG to an
// output file path. One line per executed draw: running index, geometry counts,
// and the CURRENT render-state vector + whether it was still unflushed when the
// draw arrived. Zero cost when the env var is absent (checked once).
static FILE * Draw_Log_File()
{
	static FILE * s_file = nullptr;
	static bool s_checked = false;
	if (!s_checked) {
		s_checked = true;
		const char * path = W3DNext_GetEnv("D3D11_DRAWLOG");
		if (path != nullptr && path[0] != '\0') {
			s_file = std::fopen(path, "a");
		}
	}
	return s_file;
}

static void Log_Draw(const char * what, unsigned int polys, unsigned int verts,
	unsigned int start_index, const D3D11Backend & be, bool was_dirty, bool textured)
{
	FILE * f = Draw_Log_File();
	if (f == nullptr) {
		return;
	}
	const RenderStateVector & rs = be.Peek_Render_State();

	// Optional overlay-only filter (D3D11_DRAWLOG_OVERLAY=1): only log
	// transparent / fullscreen-overlay draws (additive, alpha blend, or
	// depth-always-no-write). These are the candidates for a fullscreen
	// "white flash" post-process quad and for the spread menu icons.
	static int s_overlayOnly = -1;
	if (s_overlayOnly < 0) {
		const char * ov = W3DNext_GetEnv("D3D11_DRAWLOG_OVERLAY");
		s_overlayOnly = (ov != nullptr && ov[0] == '1') ? 1 : 0;
	}
	if (s_overlayOnly == 1) {
		const bool additive = rs.blendEnable && rs.srcBlend == RB_BLEND_SRCALPHA && rs.dstBlend == RB_BLEND_ONE;
		const bool alphaBlend = rs.blendEnable && rs.srcBlend == RB_BLEND_SRCALPHA && rs.dstBlend == RB_BLEND_INVSRCALPHA;
		const bool overlay = rs.depthFunc == RB_CMP_ALWAYS && !rs.depthWrite;
		if (!(additive || alphaBlend || overlay)) {
			return;
		}
	}

	// Timestamp (ms since first logged draw) so we can correlate to the
	// "~3s after load" white-flash window the user reported.
	static ULONGLONG s_t0 = 0;
	if (s_t0 == 0) { s_t0 = GetTickCount64(); }
	const ULONGLONG tMs = GetTickCount64() - s_t0;

	static unsigned int s_drawIndex = 0;
	std::fprintf(f,
		"[t=%llu ms][draw %u] %s polys=%u verts=%u start=%u | blend=%d src=%d dst=%d | depthTest=%d write=%d func=%d | tex=%d dirtyAtDraw=%d"
		" | shader=0x%08x stages=%u c0=%u/%u,%u a0=%u/%u,%u texfmt=%u texname=%s\n",
		tMs, s_drawIndex++, what, polys, verts, start_index,
		(int)rs.blendEnable, (int)rs.srcBlend, (int)rs.dstBlend,
		(int)rs.depthEnable, (int)rs.depthWrite, (int)rs.depthFunc,
		(int)textured, (int)was_dirty,
		be.Peek_Last_Shader_Bits(), be.Peek_Combiner_Num_Stages(),
		be.Peek_Combiner_Color0(0), be.Peek_Combiner_Color0(1), be.Peek_Combiner_Color0(2),
		be.Peek_Combiner_Alpha0(0), be.Peek_Combiner_Alpha0(1), be.Peek_Combiner_Alpha0(2),
		be.Peek_Stage_Tex_Format(0), be.Peek_Stage_Tex_Name(0));
	std::fflush(f);
}

void D3D11Backend::Mark_Draw_Complete()
{
	// Buffers are released in bulk by Flush_Dynamic_Pools() at the next
	// Begin_Scene (when the previous frame's GPU work is done). Nothing to do
	// per-draw: reusing in-flight DEFAULT buffers via UpdateSubresource crashes
	// the Intel D3D11 driver (AV), so we never reuse within a frame.
}

bool D3D11Backend::Ensure_Dyn_VB_Cap(unsigned int need)
{
	if (m_dynVB != nullptr && need <= m_dynVBCap) {
		return true;
	}
	Safe_Release(m_dynVB);
	unsigned int cap = m_dynVBCap ? m_dynVBCap : (4u * 1024u * 1024u);
	while (cap < need) {
		cap = (cap > 0x40000000u) ? 0x80000000u : (cap * 2u);
	}
	D3D11_BUFFER_DESC bd;
	ZeroMemory(&bd, sizeof(bd));
	bd.ByteWidth = cap;
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.CPUAccessFlags = 0;
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	if (FAILED(m_device->CreateBuffer(&bd, nullptr, &m_dynVB))) {
		m_dynVBCap = 0;
		m_dynVBOffset = 0;
		return false;
	}
	m_dynVBCap = cap;
	m_dynVBOffset = 0;
	return true;
}

bool D3D11Backend::Ensure_Dyn_IB_Cap(unsigned int need)
{
	if (m_dynIB != nullptr && need <= m_dynIBCap) {
		return true;
	}
	Safe_Release(m_dynIB);
	unsigned int cap = m_dynIBCap ? m_dynIBCap : (1u * 1024u * 1024u);
	while (cap < need) {
		cap = (cap > 0x10000000u) ? 0x20000000u : (cap * 2u);
	}
	D3D11_BUFFER_DESC bd;
	ZeroMemory(&bd, sizeof(bd));
	bd.ByteWidth = cap;
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.CPUAccessFlags = 0;
	bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
	if (FAILED(m_device->CreateBuffer(&bd, nullptr, &m_dynIB))) {
		m_dynIBCap = 0;
		m_dynIBOffset = 0;
		return false;
	}
	m_dynIBCap = cap;
	m_dynIBOffset = 0;
	return true;
}

void D3D11Backend::Flush_Dynamic_Pools()
{
	// Release the per-frame staging buffers (now the previous frame's GPU work
	// is done, so no slice is in flight). A fresh buffer is allocated lazily by
	// the first upload of the new frame.
	Safe_Release(m_dynVB);
	m_dynVBCap = 0;
	m_dynVBOffset = 0;
	Safe_Release(m_dynIB);
	m_dynIBCap = 0;
	m_dynIBOffset = 0;
}

void D3D11Backend::Draw_Triangles(
	unsigned int start_index,
	unsigned int polygon_count,
	unsigned int min_vertex_index,
	unsigned int vertex_count)
{
	if (!m_pipelineReady || m_context == nullptr || m_indexBuffer == nullptr || m_vertexBuffer == nullptr) {
		D3D11_TRACE_NOOP("no-op: no engine VB/IB uploaded (Draw_Triangles skipped)");
		return;
	}
	// W3DNext: catch the unit-normal state leak that makes shadow/decal quads
	// render magenta/pink. When the unit-normal pipeline is bound, the FIRST draw
	// through here is the real unit mesh (capture its FVF); any later draw whose
	// FVF differs (a shadow volume or decal quad) inherited the bound pipeline
	// without re-driving the unit-normal flag, so force the FF combiner back on.
	if (m_unitNormalIsActive) {
		if (m_unitNormalFVF == 0xFFFFFFFFu) {
			m_unitNormalFVF = m_vertexFVF;   // capture the genuine unit mesh FVF
		} else if (m_vertexFVF != m_unitNormalFVF) {
			Set_Unit_Normal_Pixel_Shader(false);
		}
	}
	Log_Draw("tris", polygon_count, vertex_count, start_index,
		*this, m_renderStateDirty, m_stageSRV[0] != nullptr);
	// DX8 semantics: deferred render state is applied AT DRAW TIME
	// (DX8Wrapper::Draw calls Apply_Render_State_Changes before every
	// DrawIndexedPrimitive). Callers like Render2DClass::Render rely on that -
	// they Set_Shader then Draw_Triangles with no explicit flush. Without this,
	// the 2D menu path ran on the never-bound D3D11 default depth state
	// (LESS + depth-write) and the first full-screen quad z-rejected every
	// later same-depth menu quad.
	// Timing is opt-in (D3D11_PROFILE=1, see Profiling_Enabled()). This is the
	// single busiest call in the renderer - hundreds to thousands of these per
	// frame in a big battle - so by default we skip all 5 QueryPerformanceCounter
	// calls entirely rather than pay their cost just to fill in debug stats
	// nobody is reading.
	const bool profiling = Profiling_Enabled();
	LARGE_INTEGER t0, t1, t2, t3, t4;
	if (profiling) { QueryPerformanceCounter(&t0); }
	Apply_Render_State_Changes();
	if (profiling) { QueryPerformanceCounter(&t1); }
	Update_Constant_Buffer();
	if (profiling) { QueryPerformanceCounter(&t2); }
	Update_Combiner_Buffer();
	Update_Lighting_Buffer();
	Update_Fog_Buffer();
	Update_Skinning_Buffer();
	Update_TexGen_Buffer();
	if (profiling) { QueryPerformanceCounter(&t3); }
	m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	// A triangle list has 3 indices per polygon. m_indexBaseOffset is the DX8
	// SetIndices base-vertex (D3D8 adds it to every index); it is 0 for the smoke
	// test and the dynamic 2D path, non-zero for static meshes that share a VB.
	m_context->DrawIndexed(polygon_count * 3, start_index, static_cast<INT>(m_indexBaseOffset));
	++m_frameDrawCalls;
	if (profiling) {
		QueryPerformanceCounter(&t4);
		const double f = static_cast<double>(m_qpcFreq.QuadPart);
		m_dbgApplyMs += static_cast<double>(t1.QuadPart - t0.QuadPart) / f * 1000.0;
		m_dbgConstMs += static_cast<double>(t2.QuadPart - t1.QuadPart) / f * 1000.0;
		m_dbgOtherMs += static_cast<double>(t3.QuadPart - t2.QuadPart) / f * 1000.0;
		m_dbgDrawMs += static_cast<double>(t4.QuadPart - t3.QuadPart) / f * 1000.0;
	}
	Mark_Draw_Complete();
}

void D3D11Backend::Draw_Triangles(
	unsigned int buffer_type,
	unsigned int start_index,
	unsigned int polygon_count,
	unsigned int min_vertex_index,
	unsigned int vertex_count)
{
	// buffer_type selects the sorting/backend buffer in the DX8 path; the D3D11
	// skeleton draws from the currently bound buffers regardless.
	(void)buffer_type;
	Draw_Triangles(start_index, polygon_count, min_vertex_index, vertex_count);
}

void D3D11Backend::Draw_Strip(
	unsigned int start_index,
	unsigned int primitive_count,
	unsigned int min_vertex_index,
	unsigned int vertex_count)
{
	if (!m_pipelineReady || m_context == nullptr || m_indexBuffer == nullptr || m_vertexBuffer == nullptr) {
		D3D11_TRACE_NOOP("no-op: no engine VB/IB uploaded (Draw_Strip skipped)");
		return;
	}
	Log_Draw("strip", primitive_count, vertex_count, start_index,
		*this, m_renderStateDirty, m_stageSRV[0] != nullptr);
	const bool profiling = Profiling_Enabled();
	LARGE_INTEGER t0, t1, t2, t3, t4;
	if (profiling) { QueryPerformanceCounter(&t0); }
	Apply_Render_State_Changes(); // draw-time state flush (DX8 semantics; see Draw_Triangles)
	if (profiling) { QueryPerformanceCounter(&t1); }
	Update_Constant_Buffer();
	if (profiling) { QueryPerformanceCounter(&t2); }
	Update_Combiner_Buffer();
	Update_Lighting_Buffer();
	Update_Fog_Buffer();
	Update_Skinning_Buffer();
	Update_TexGen_Buffer();
	if (profiling) { QueryPerformanceCounter(&t3); }
	m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	// Same base-vertex handling as Draw_Triangles: D3D8 adds the SetIndices
	// base to every index. Non-zero for 2nd+ wave-track batches (the only
	// live strip caller accumulates batchStart).
	// Count contract (IRenderBackend.h): primitive_count TRIANGLES; a strip of
	// N triangles consumes N + 2 indices (DrawIndexed wants the index count).
	m_context->DrawIndexed(primitive_count + 2, start_index, static_cast<INT>(m_indexBaseOffset));
	++m_frameDrawCalls;
	if (profiling) {
		QueryPerformanceCounter(&t4);
		const double f = static_cast<double>(m_qpcFreq.QuadPart);
		m_dbgApplyMs += static_cast<double>(t1.QuadPart - t0.QuadPart) / f * 1000.0;
		m_dbgConstMs += static_cast<double>(t2.QuadPart - t1.QuadPart) / f * 1000.0;
		m_dbgOtherMs += static_cast<double>(t3.QuadPart - t2.QuadPart) / f * 1000.0;
		m_dbgDrawMs += static_cast<double>(t4.QuadPart - t3.QuadPart) / f * 1000.0;
	}
	Mark_Draw_Complete();
}

void D3D11Backend::Set_Vertex_Shader(unsigned long vertex_shader)
{
	D3D11_STUB();
}

void D3D11Backend::Set_Pixel_Shader(unsigned long pixel_shader)
{
	D3D11_STUB();
}

void D3D11Backend::Set_Vertex_Shader_Constant(int reg, const void * data, int count)
{
	D3D11_STUB();
}

void D3D11Backend::Set_Pixel_Shader_Constant(int reg, const void * data, int count)
{
	D3D11_STUB();
}

// ----------------------------------------------------------------------------
//
// Screen-filter path: backbuffer snapshot + legacy XYZRHW quad draw
//
// The DX8 screen filters redirect rendering into a texture and re-draw it with
// raw pre-transformed DrawPrimitiveUP quads. Here the equivalent is: snapshot
// the backbuffer AFTER the scene rendered (CopyResource - same content the DX8
// redirect would have accumulated), then draw the quad through the normal
// FF-emulation pipeline with a screen-space ortho projection. The transforms /
// combiner state are snapshotted and restored around the draw so the frame's
// remaining rendering is unaffected.
//
// ----------------------------------------------------------------------------

void D3D11Backend::Capture_Backbuffer()
{
	if (m_device == nullptr || m_context == nullptr || m_swapChain == nullptr) {
		return;
	}
	ID3D11Texture2D * back_buffer = nullptr;
	HRESULT hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back_buffer));
	if (FAILED(hr) || back_buffer == nullptr) {
		return;
	}
	D3D11_TEXTURE2D_DESC desc;
	back_buffer->GetDesc(&desc);

	if (m_captureTexture != nullptr) {
		D3D11_TEXTURE2D_DESC have;
		m_captureTexture->GetDesc(&have);
		if (have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format) {
			Safe_Release(m_captureSRV);
			Safe_Release(m_captureTexture);
		}
	}
	if (m_captureTexture == nullptr) {
		D3D11_TEXTURE2D_DESC cd = desc;
		cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		cd.Usage = D3D11_USAGE_DEFAULT;
		cd.CPUAccessFlags = 0;
		cd.MiscFlags = 0;
		cd.MipLevels = 1;
		cd.ArraySize = 1;
		cd.SampleDesc.Count = 1;
		cd.SampleDesc.Quality = 0;
		hr = m_device->CreateTexture2D(&cd, nullptr, &m_captureTexture);
		if (FAILED(hr)) {
			m_captureTexture = nullptr;
			back_buffer->Release();
			return;
		}
		hr = m_device->CreateShaderResourceView(m_captureTexture, nullptr, &m_captureSRV);
		if (FAILED(hr)) {
			m_captureSRV = nullptr;
			Safe_Release(m_captureTexture);
			back_buffer->Release();
			return;
		}
		char line[128];
		std::snprintf(line, sizeof(line), "[D3D11 filter] backbuffer capture %ux%u created", desc.Width, desc.Height);
		D3D11_Log_Line(line);
	}
	if (m_captureSampler == nullptr) {
		// Linear + clamp: the sampling state endRenderToTexture forces on DX8.
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxAnisotropy = 1;
		sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(m_device->CreateSamplerState(&sd, &m_captureSampler))) {
			m_captureSampler = nullptr;
		}
	}

	m_context->CopyResource(m_captureTexture, back_buffer);
	back_buffer->Release();
}

bool D3D11Backend::Read_Back_Buffer(unsigned char * rgb_dst, unsigned int & width, unsigned int & height)
{
	if (m_device == nullptr || m_context == nullptr || m_swapChain == nullptr) {
		return false;
	}
	ID3D11Texture2D * back = nullptr;
	if (FAILED(m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back))) || back == nullptr) {
		return false;
	}
	D3D11_TEXTURE2D_DESC desc;
	back->GetDesc(&desc);
	width = desc.Width;
	height = desc.Height;
	if (rgb_dst == nullptr) { // query mode: dimensions only
		back->Release();
		return true;
	}
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;
	ID3D11Texture2D * staging = nullptr;
	bool ok = false;
	if (SUCCEEDED(m_device->CreateTexture2D(&desc, nullptr, &staging)) && staging != nullptr) {
		m_context->CopyResource(staging, back);
		D3D11_MAPPED_SUBRESOURCE map;
		if (SUCCEEDED(m_context->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
			const bool bgra = (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
				desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
			for (unsigned int y = 0; y < desc.Height; ++y) {
				const unsigned char * row = static_cast<const unsigned char *>(map.pData) + static_cast<size_t>(y) * map.RowPitch;
				unsigned char * out = rgb_dst + static_cast<size_t>(y) * desc.Width * 3u;
				for (unsigned int x = 0; x < desc.Width; ++x) {
					const unsigned char c0 = row[x * 4 + 0], c1 = row[x * 4 + 1], c2 = row[x * 4 + 2];
					if (bgra) { out[x * 3 + 0] = c2; out[x * 3 + 1] = c1; out[x * 3 + 2] = c0; }
					else      { out[x * 3 + 0] = c0; out[x * 3 + 1] = c1; out[x * 3 + 2] = c2; }
				}
			}
			m_context->Unmap(staging, 0);
			ok = true;
		}
		staging->Release();
	}
	back->Release();
	return ok;
}

void D3D11Backend::Set_Sun_Screen(float x, float y, float intensity)
{
	// Diagnostic: log the first few updates INCLUDING zeros so a dead
	// projection is distinguishable from weak rays.
	static int s_logCount = 0;
	if (s_logCount < 40) {
		if (s_logCount < 4 || (s_logCount % 10) == 0) {
			char line[128];
			std::snprintf(line, sizeof(line),
				"[D3D11] god rays #%d: sun uv=(%.3f, %.3f) intensity=%.3f",
				s_logCount, x, y, intensity);
			D3D11_Log_Line(line);
		}
		++s_logCount;
	}
	m_sunScreen[0] = x;
	m_sunScreen[1] = y;
	m_sunScreen[2] = intensity;
}

void D3D11Backend::Apply_Color_Grade()
{
	// W3DNext color-grading post-process. Runs once per frame at End_Frame,
	// just before Present: copies the finished back buffer into an off-screen
	// texture, then redraws it full-screen through the grade shader pair with
	// brightness / contrast / saturation / tint constants applied. One copy +
	// one fullscreen draw; the FF pipeline state is restored afterwards.
	if (m_device == nullptr || m_context == nullptr || m_swapChain == nullptr
		|| !m_pipelineReady || m_backBufferRTV == nullptr
		|| m_gradePixelShader == nullptr || m_gradeVertexShader == nullptr
		|| m_gradeBuffer == nullptr || m_gradeSampler == nullptr) {
		static bool s_gradeFailLogged = false;
		if (!s_gradeFailLogged) {
			char line[192];
			std::snprintf(line, sizeof(line),
				"[D3D11] grade pass SKIP: pipelineReady=%d rtv=%d ps=%d vs=%d buf=%d samp=%d",
				(int)m_pipelineReady, (int)(m_backBufferRTV != nullptr),
				(int)(m_gradePixelShader != nullptr), (int)(m_gradeVertexShader != nullptr),
				(int)(m_gradeBuffer != nullptr), (int)(m_gradeSampler != nullptr));
			D3D11_Log_Line(line);
			s_gradeFailLogged = true;
		}
		return;
	}
	{
		static bool s_gradeRunLogged = false;
		if (!s_gradeRunLogged) {
			D3D11_Log_Line("[D3D11] grade pass RUNNING");
			s_gradeRunLogged = true;
		}
	}
	ID3D11Texture2D * back = nullptr;
	if (FAILED(m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back))) || back == nullptr) {
		return;
	}
	D3D11_TEXTURE2D_DESC desc;
	back->GetDesc(&desc);
	if (m_gradeTexture == nullptr || m_gradeWidth != desc.Width || m_gradeHeight != desc.Height) {
		if (m_gradeSRV != nullptr) { m_gradeSRV->Release(); m_gradeSRV = nullptr; }
		if (m_gradeTexture != nullptr) { m_gradeTexture->Release(); m_gradeTexture = nullptr; }
		D3D11_TEXTURE2D_DESC td = desc;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		td.CPUAccessFlags = 0;
		td.MiscFlags = 0;
		if (SUCCEEDED(m_device->CreateTexture2D(&td, nullptr, &m_gradeTexture))) {
			m_device->CreateShaderResourceView(m_gradeTexture, nullptr, &m_gradeSRV);
		}
		if (m_gradeSRV == nullptr) {
			if (m_gradeTexture != nullptr) { m_gradeTexture->Release(); m_gradeTexture = nullptr; }
			back->Release();
			return;
		}
		m_gradeWidth = desc.Width;
		m_gradeHeight = desc.Height;
	}
	m_context->CopyResource(m_gradeTexture, back);

	// Cinematic defaults (neutral = all ones). Tune here.
	struct GradeConstants { float Params[4]; float Tint[4]; float Mode[4]; float Sun[4]; float Bloom[4]; };
	GradeConstants gc;
	gc.Params[0] = 1.04f;  // brightness
	gc.Params[1] = 1.12f;  // contrast
	gc.Params[2] = 1.14f;  // saturation
	gc.Params[3] = static_cast<float>(GetTickCount() % 3600000) * 0.001f;  // time: dust drift
	gc.Tint[0] = 1.02f;
	gc.Tint[1] = 1.00f;
	gc.Tint[2] = 0.97f;
	gc.Tint[3] = 1.0f;
	gc.Mode[0] = m_gradeNightVision ? 1.0f : 0.0f;
	gc.Mode[1] = 1.0f;     // god rays enable
	gc.Mode[2] = 1.0f;     // filmic tone-map + bloom enable
	gc.Mode[3] = 0.0f;
	gc.Sun[0] = m_sunScreen[0];
	gc.Sun[1] = m_sunScreen[1];
	gc.Sun[2] = m_sunScreen[2];
	gc.Sun[3] = 0.0f;
	gc.Bloom[0] = 1.06f;   // exposure
	gc.Bloom[1] = 0.40f;   // bloom strength
	gc.Bloom[2] = 0.55f;   // bloom threshold
	gc.Bloom[3] = 1.0f;    // bloom radius (texels)
	m_context->UpdateSubresource(m_gradeBuffer, 0, nullptr, &gc, 0, 0);

	m_backBufferRTV->AddRef();
	ID3D11RenderTargetView * rtv = m_backBufferRTV;
	// No depth view: the fullscreen quad must never depth-test/write. Only 2D
	// UI draws follow this pass, none of which need the depth buffer.
	m_context->OMSetRenderTargets(1, &rtv, nullptr);
	m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	m_context->IASetInputLayout(nullptr);
	m_context->VSSetShader(m_gradeVertexShader, nullptr, 0);
	m_context->PSSetShader(m_gradePixelShader, nullptr, 0);
	m_context->PSSetConstantBuffers(0, 1, &m_gradeBuffer);
	m_context->PSSetShaderResources(0, 1, &m_gradeSRV);
	m_context->PSSetSamplers(0, 1, &m_gradeSampler);
	m_context->Draw(4, 0);
	rtv->Release();

	// Restore the FF pipeline and unbind the SRV so next frame's CopyResource
	// into the same texture never races a bound input.
	ID3D11ShaderResourceView * null_srv[1] = { nullptr };
	m_context->PSSetShaderResources(0, 1, null_srv);
	m_context->VSSetShader(m_vertexShader, nullptr, 0);
	m_context->PSSetShader(m_pixelShader, nullptr, 0);
	m_context->PSSetConstantBuffers(0, 1, &m_combinerBuffer);
	m_context->PSSetConstantBuffers(1, 1, &m_fogBuffer);
	m_context->PSSetConstantBuffers(2, 1, &m_texgenBuffer);
	back->Release();
}

void D3D11Backend::Draw_Screen_Filter_Quad(const RenderBackendFilterQuad & quad)
{
	// W3DNext D3D11: the screen-filter (post-process / FOW-reveal / flash) path
	// draws a full-screen WHITE overlay on some state changes (e.g. toggling
	// Graphics Detail Low<->High) - confirmed as the white-flash/halo source
	// (diag modes 2/4 disabling it removed the artifact). The core shroud
	// darkening is done by the terrain shader, not this quad, so skipping it
	// only drops cosmetic post-process. Until reworked, skip it by default;
	// set W3DNEXT_D3D11_FILTERS=1 to re-enable for testing.
	if (g_w3dnextDiagMode == 2 || g_w3dnextDiagMode == 4) {
		return;
	}
	static int s_filtersEnabled = -1;
	static bool s_logged = false;
	if (s_filtersEnabled < 0) {
		const char * e = W3DNext_GetEnv("W3DNEXT_D3D11_FILTERS");
		s_filtersEnabled = (e != nullptr && e[0] == '1') ? 1 : 0;
	}
	if (s_filtersEnabled == 0) {
		if (!s_logged) {
			D3D11_Log_Line("[D3D11] screen-filter quads disabled (set W3DNEXT_D3D11_FILTERS=1 to enable)");
			s_logged = true;
		}
		return;
	}
	const unsigned int kMaxVerts = 8;
	if (!m_pipelineReady || m_context == nullptr || quad.verts == nullptr) {
		return;
	}
	if (quad.vertex_count < 3 || quad.vertex_count > kMaxVerts || quad.uv_sets < 1 || quad.uv_sets > 2) {
		return;
	}
	// W3DNext: force the fixed-function pipeline for this full-screen filter quad.
	// Otherwise, if a unit/normal-mapped draw left m_unitNormalIsActive true, this
	// post-process quad would be drawn with the unit-normal shader and produce
	// rainbow artifacts (observed on FOW/radar reveal over normal-mapped buildings;
	// diag mode 2 / screen-filter-off removed it, confirming the source).
	Set_Unit_Normal_Pixel_Shader(false);
	if (quad.use_captured_scene && m_captureSRV == nullptr) {
		D3D11_TRACE_NOOP("no-op: no captured backbuffer for filter quad");
		return;
	}

	// 1) Convert the legacy pre-transformed vertices (float4 screen-pixel pos +
	// DWORD diffuse + uv sets) into the XYZ|DIFFUSE|TEXn layout the FF pipeline
	// consumes. The rhw component is dropped (always 1 on these paths).
	struct ConvertedVertex
	{
		float x, y, z;
		unsigned int diffuse;
		float uv[4];
	};
	ConvertedVertex out[kMaxVerts];
	const unsigned int out_stride = 16 + quad.uv_sets * 8; // pos(12)+diffuse(4)+uvs
	unsigned char * dst = reinterpret_cast<unsigned char *>(&out[0]);
	const unsigned char * src = static_cast<const unsigned char *>(quad.verts);
	for (unsigned int i = 0; i < quad.vertex_count; ++i) {
		const float * p = reinterpret_cast<const float *>(src + i * quad.stride_bytes);
		float * o = reinterpret_cast<float *>(dst + i * out_stride);
		o[0] = p[0]; // x (screen px)
		o[1] = p[1]; // y (screen px)
		o[2] = p[2]; // z
		reinterpret_cast<unsigned int *>(o)[3] = reinterpret_cast<const unsigned int *>(p)[4]; // diffuse
		for (unsigned int t = 0; t < quad.uv_sets * 2; ++t) {
			o[4 + t] = p[5 + t];
		}
	}
	const unsigned int fvf =
		0x002u /*XYZ*/ | 0x040u /*DIFFUSE*/ | (quad.uv_sets == 2 ? 0x200u /*TEX2*/ : 0x100u /*TEX1*/);

	// 2) Snapshot the state this draw overrides.
	const Matrix4x4 saved_world = m_world;
	const Matrix4x4 saved_view = m_view;
	const Matrix4x4 saved_proj = m_proj;
	const CombinerConstants saved_combiner = m_combiner;
	const unsigned int saved_fog_enable = m_fog.enable;

	// Screen-space ortho (pixel x right, y down -> clip). The D3D8 half-pixel
	// convention is applied by the WVP fold in Update_Constant_Buffer, exactly
	// as for the engine's own 2D path.
	const float vw = static_cast<float>(m_viewportWidth > 0 ? m_viewportWidth : m_width);
	const float vh = static_cast<float>(m_viewportHeight > 0 ? m_viewportHeight : m_height);
	if (vw <= 0.0f || vh <= 0.0f) {
		return;
	}
	Matrix4x4 ortho(true);
	ortho[0][0] = 2.0f / vw;
	ortho[0][3] = -1.0f;
	ortho[1][1] = -2.0f / vh;
	ortho[1][3] = 1.0f;
	m_world = Matrix4x4(true);
	m_view = Matrix4x4(true);
	m_proj = ortho;
	m_transformDirty = true;

	// 3) Combiner override: stage 0 = scene texture * diffuse (the filters draw
	// with white or per-quad-alpha diffuse); optional stage 1 = mask modulate
	// (crossfade circle). Alpha follows the DX8 override when requested
	// (ALPHAOP SELECTARG1(CURRENT) == the vertex diffuse alpha at stage 0).
	m_combiner.numStages = quad.stage1_mask_modulate ? 2u : 1u;
	m_combiner.stageColor[0][0] = RB_TEXOP_MODULATE;
	m_combiner.stageColor[0][1] = RB_TEXARG_TEXTURE;
	m_combiner.stageColor[0][2] = RB_TEXARG_DIFFUSE;
	m_combiner.stageColor[0][3] = 0;
	m_combiner.stageAlpha[0][0] = quad.alpha_from_diffuse ? RB_TEXOP_SELECTARG2 : RB_TEXOP_MODULATE;
	m_combiner.stageAlpha[0][1] = RB_TEXARG_TEXTURE;
	m_combiner.stageAlpha[0][2] = RB_TEXARG_DIFFUSE;
	m_combiner.stageAlpha[0][3] = 0;
	if (quad.stage1_mask_modulate) {
		m_combiner.stageColor[1][0] = RB_TEXOP_MODULATE;
		m_combiner.stageColor[1][1] = RB_TEXARG_TEXTURE;
		m_combiner.stageColor[1][2] = RB_TEXARG_CURRENT;
		m_combiner.stageColor[1][3] = 1;
		m_combiner.stageAlpha[1][0] = RB_TEXOP_MODULATE;
		m_combiner.stageAlpha[1][1] = RB_TEXARG_TEXTURE;
		m_combiner.stageAlpha[1][2] = RB_TEXARG_CURRENT;
		m_combiner.stageAlpha[1][3] = 0;
	}
	m_combiner.monoEnable = quad.monochrome_enable ? 1u : 0u;
	for (int c = 0; c < 4; ++c) {
		m_combiner.monoLum[c] = quad.mono_lum[c];
		m_combiner.monoTint[c] = quad.mono_tint[c];
		m_combiner.monoFade[c] = quad.mono_fade[c];
	}
	m_combinerDirty = true;

	// The quad is a screen-space overlay: no scene fog, depth always-pass, no
	// depth write (DX8 sets ZFUNC ALWAYS / ZWRITEENABLE FALSE around these
	// draws). Blend per the raw render states the DX8 path sets.
	m_fog.enable = 0;
	m_fogDirty = true;
	Set_Depth_Func(RB_CMP_ALWAYS);
	Set_Depth_Write_Enable(false);
	switch (quad.blend) {
	case RenderBackendFilterQuad::BLEND_ALPHA:
		Set_Blend_Enable(true);
		Set_Blend_Func(RB_BLEND_SRCALPHA, RB_BLEND_INVSRCALPHA);
		break;
	case RenderBackendFilterQuad::BLEND_ADDITIVE:
		Set_Blend_Enable(true);
		Set_Blend_Func(RB_BLEND_SRCALPHA, RB_BLEND_ONE);
		break;
	default:
		Set_Blend_Enable(false);
		break;
	}

	// 4) Geometry + draw through the normal FF path.
	unsigned short indices[kMaxVerts];
	for (unsigned int i = 0; i < quad.vertex_count; ++i) {
		indices[i] = static_cast<unsigned short>(i);
	}
	if (!Upload_Vertices(&out[0], quad.vertex_count * out_stride, fvf) ||
		!Upload_Indices16(indices, quad.vertex_count)) {
		m_world = saved_world;
		m_view = saved_view;
		m_proj = saved_proj;
		m_transformDirty = true;
		m_combiner = saved_combiner;
		m_combinerDirty = true;
		m_fog.enable = saved_fog_enable;
		m_fogDirty = true;
		return;
	}
	// NOTE: out_stride < sizeof(ConvertedVertex) for uv_sets==1 is fine - the
	// vertices were written tightly packed at out_stride above.

	Apply_Render_State_Changes();
	Update_Constant_Buffer();
	Update_Combiner_Buffer();
	Update_Lighting_Buffer();
	Update_Fog_Buffer();
	Update_Skinning_Buffer();
	Update_TexGen_Buffer();

	if (quad.use_captured_scene) {
		m_context->PSSetShaderResources(0, 1, &m_captureSRV);
		if (m_captureSampler != nullptr) {
			m_context->PSSetSamplers(0, 1, &m_captureSampler);
		}
	}

	m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	m_context->DrawIndexed(quad.vertex_count, 0, 0);
	Mark_Draw_Complete();

	// 5) Restore. The blend/depth vector is left for the next Set_Shader (every
	// engine draw path re-establishes it); stage-0 SRV/sampler rebind so the
	// next textured draw is unaffected.
	if (quad.use_captured_scene) {
		m_context->PSSetShaderResources(0, 1, &m_stageSRV[0]);
		m_context->PSSetSamplers(0, 1, &m_stageSampler[0]);
	}
	m_world = saved_world;
	m_view = saved_view;
	m_proj = saved_proj;
	m_transformDirty = true;
	m_combiner = saved_combiner;
	m_combinerDirty = true;
	m_fog.enable = saved_fog_enable;
	m_fogDirty = true;
}

// W3DNext D3D11 render-to-texture: water reflection (W3DWater) and projected
// shadows (W3DProjectedShadow) build their targets via Create_Render_Target and
// sample them directly. The D3D11 backend owns the real D3D11 textures keyed by
// the returned TextureClass*; Set_Texture (D3D11Backend_W3D.cpp) detects these and
// binds the pre-made SRV so the scene samples the reflection/shadow, not null.
static DXGI_FORMAT WW3DFormat_To_DXGI(WW3DFormat fmt)
{
	switch (fmt) {
	case WW3D_FORMAT_A8R8G8B8:
	case WW3D_FORMAT_X8R8G8B8:
	case WW3D_FORMAT_R8G8B8:
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	case WW3D_FORMAT_A4R4G4B4:
	case WW3D_FORMAT_X4R4G4B4:
		return DXGI_FORMAT_B4G4R4A4_UNORM;
	case WW3D_FORMAT_A1R5G5B5:
	case WW3D_FORMAT_X1R5G5B5:
		return DXGI_FORMAT_B5G5R5A1_UNORM;
	case WW3D_FORMAT_R5G6B5:
		return DXGI_FORMAT_B5G6R5_UNORM;
	case WW3D_FORMAT_A8:
		return DXGI_FORMAT_A8_UNORM;
	case WW3D_FORMAT_L8:
		return DXGI_FORMAT_R8_UNORM;
	case WW3D_FORMAT_A8L8:
		return DXGI_FORMAT_R8G8_UNORM;
	default:
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	}
}

TextureClass * D3D11Backend::Create_Render_Target(int width, int height, WW3DFormat format)
{
	if (m_device == nullptr || m_context == nullptr) return nullptr;
	if (width <= 0) width = 1;
	if (height <= 0) height = 1;

	DXGI_FORMAT fmt = WW3DFormat_To_DXGI(format);

	RTTTarget t;
	t.width = (unsigned int)width;
	t.height = (unsigned int)height;
	t.tex = nullptr;
	t.rtv = nullptr;
	t.srv = nullptr;
	t.sampler = nullptr;
	t.depthTex = nullptr;
	t.dsv = nullptr;

	D3D11_TEXTURE2D_DESC cd = {};
	cd.Width = t.width;
	cd.Height = t.height;
	cd.MipLevels = 1;
	cd.ArraySize = 1;
	cd.Format = fmt;
	cd.SampleDesc.Count = 1;
	cd.SampleDesc.Quality = 0;
	cd.Usage = D3D11_USAGE_DEFAULT;
	cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	cd.CPUAccessFlags = 0;
	cd.MiscFlags = 0;
	if (FAILED(m_device->CreateTexture2D(&cd, nullptr, &t.tex))) return nullptr;
	if (FAILED(m_device->CreateRenderTargetView(t.tex, nullptr, &t.rtv))) { Safe_Release(t.tex); return nullptr; }
	if (FAILED(m_device->CreateShaderResourceView(t.tex, nullptr, &t.srv))) { Safe_Release(t.rtv); Safe_Release(t.tex); return nullptr; }

	// Own depth-stencil, sized to the render target.
	D3D11_TEXTURE2D_DESC dd = cd;
	dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	if (FAILED(m_device->CreateTexture2D(&dd, nullptr, &t.depthTex))) { Safe_Release(t.srv); Safe_Release(t.rtv); Safe_Release(t.tex); return nullptr; }
	if (FAILED(m_device->CreateDepthStencilView(t.depthTex, nullptr, &t.dsv))) {
		Safe_Release(t.depthTex); Safe_Release(t.srv); Safe_Release(t.rtv); Safe_Release(t.tex); return nullptr;
	}

	// Linear + clamp sampler (matches the scene-capture sampling state).
	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxAnisotropy = 1;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	if (FAILED(m_device->CreateSamplerState(&sd, &t.sampler))) {
		Safe_Release(t.dsv); Safe_Release(t.depthTex); Safe_Release(t.srv); Safe_Release(t.rtv); Safe_Release(t.tex); return nullptr;
	}

	// Wrap in a TextureClass so the engine can hold it as a normal texture handle.
	// The DX8 surface inside is unused (we bind the D3D11 SRV directly); we key on
	// the TextureClass* in m_rtt so Set_Texture routes sampling to the real RT.
	TextureClass * tc = NEW_REF(TextureClass, (width, height, format, MIP_LEVELS_1, TextureClass::POOL_DEFAULT, true));
	if (tc == nullptr) {
		Safe_Release(t.sampler); Safe_Release(t.dsv); Safe_Release(t.depthTex);
		Safe_Release(t.srv); Safe_Release(t.rtv); Safe_Release(t.tex);
		return nullptr;
	}

	m_rtt[tc] = t;
	D3D11_Log_Line("[D3D11 RTT] created render target");
	return tc;
}

void D3D11Backend::Set_Render_Target_With_Z(TextureClass * texture, ZTextureClass * ztexture)
{
	(void)ztexture;
	if (m_context == nullptr) return;
	if (texture == nullptr) {            // restore the main backbuffer
		Bind_Back_Buffer_Targets();
		m_savedBackBufferRTV = nullptr;
		return;
	}
	auto it = m_rtt.find(texture);
	if (it == m_rtt.end()) {             // not one of our RTs: fall back to backbuffer
		Bind_Back_Buffer_Targets();
		return;
	}
	const RTTTarget & t = it->second;
	m_context->OMSetRenderTargets(1, &t.rtv, t.dsv);
	m_currentRTV = t.rtv;
	m_currentDSV = t.dsv;
	D3D11_VIEWPORT vp;
	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.Width = (float)t.width;
	vp.Height = (float)t.height;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	m_context->RSSetViewports(1, &vp);
	m_savedBackBufferRTV = m_backBufferRTV; // mark "in RTT"
}

bool D3D11Backend::Is_Render_To_Texture()
{
	return m_savedBackBufferRTV != nullptr;
}

void D3D11Backend::Set_Shadow_Map(int idx, ZTextureClass * ztex)
{
	D3D11_STUB();
}

ZTextureClass * D3D11Backend::Get_Shadow_Map(int idx)
{
	D3D11_STUB();
	return nullptr;
}
