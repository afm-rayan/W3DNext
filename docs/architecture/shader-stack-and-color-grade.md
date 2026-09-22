# Shader Stack, Color Grade & Night-Vision Filter

This document describes the W3DNext graphics shader stack above the D3D11 backend:
the fixed-function-emulation shaders, the per-frame color-grading post-process,
and the night-vision "green filter" grade pass (F6).

> Scope: this is documentation for the post-processing layer. The backend design
> itself (why the renderer is a fixed-function emulation layer, how each D3D8
> surface maps to D3D11) is in [`../../RENDERER_PORT.md`](../../RENDERER_PORT.md);
> the DX8-vs-D3D11 A/B history is in
> [`d3d11-parity-log.md`](d3d11-parity-log.md).

---

## Where the shaders live

All HLSL for the backend is embedded as C string literals in one header:

- `Core/Libraries/Source/WWVegas/WW3D2/Shaders/D3D11FFShaders.h`

It contains, among others:

| Shader | Role |
|---|---|
| Fixed-function vertex shader | Transform & lighting, FVF layouts, indexed vertex blending (GPU skinning), texgen |
| Fixed-function pixel shader | Multitexture-combiner cascade (the `D3DTSS_*` state tree), fog, alpha-ref |
| Terrain shaders | Terrain texturing, height/normal reads, shroud darkening |
| Water shaders | Water rendering above the FF path |
| `kColorGradeVertexShaderHLSL` | Fullscreen-quad VS for the color-grade pass |
| `kColorGradePixelShaderHLSL` | Fullscreen-quad PS: bloom+filmic, brightness/contrast/saturation/tint, night vision, god rays |

The game above `DX8Wrapper` is unchanged: it still calls the same API; the
backend translates it into these shaders, constant buffers and state objects.

The D3D11 backend ships **off by default**. Enable it with:

```
generalszh.exe -d3d11        # alias: -dx11;  -dx8 forces the original DX8 backend
```

---

## The color-grade post-process

Runs once per frame in `D3D11Backend::Apply_Color_Grade()` at `End_Frame`,
immediately before `Present` (`Core/Libraries/Source/WWVegas/WW3D2/Backend/D3D11Backend.cpp`):

1. `CopyResource` the finished back buffer into an off-screen texture
   (`m_gradeTexture`, re-created on resize).
2. Draw a 4-vertex `TRIANGLESTRIP` fullscreen quad through the grade VS/PS
   pair — no input layout, no depth view, one sampler at `t0`.
3. Restore the FF pipeline state (shaders, constant buffers) and unbind the SRV
   so the next frame's `CopyResource` never races a bound input.

Cost: one copy + one fullscreen draw per frame.

### Constants (current cinematic defaults)

Written to a single constant buffer per frame:

| Field | Value | Meaning |
|---|---|---|
| `GradeParams.x` | `1.04` | brightness |
| `GradeParams.y` | `1.12` | contrast |
| `GradeParams.z` | `1.14` | saturation |
| `GradeParams.w` | `tick % 3600000 * 0.001` | time in seconds (dust drift / grain animation) |
| `GradeTint.rgb` | `(1.02, 1.00, 0.97)` | warm/cool tint |
| `GradeMode.x` | NV on/off | night-vision branch |
| `GradeMode.y` | `1.0` | god rays (dust-mote radial march toward the sun) |
| `GradeMode.z` | `1.0` | filmic tone-map + bloom |
| `Bloom` | exposure `1.06`, strength `0.40`, threshold `0.55`, radius `1.0` | bloom controls |

### Pixel-shader order of operations

```
filmic tone-map + bloom   (if GradeMode.z > 0.5)
brightness                c *= GradeParams.x
contrast                  c = (c - 0.5) * GradeParams.y + 0.5
saturation                c = lerp(luma, c, GradeParams.z)
tint                      c *= GradeTint.rgb
night vision              (if GradeMode.x > 0.5)   -> see below
god rays                  (if GradeMode.y > 0.5 && sun on screen)
```

Each branch is a no-op when its mode flag is `0`; with grade disabled the pass
is skipped entirely and the frame is untouched.

---

## Night vision — the green filter (F6)

Press **F6** in-game (grade must be enabled, see F8 below) to toggle the
night-vision filter. It is implemented as a branch in
`kColorGradePixelShaderHLSL` that converts the already-graded frame to a
single luminance channel and re-paints it as phosphor green, mimicking real
NV optics. With `GradeMode.x = 0` the output is bit-identical to no-NV.

The exact math, in order:

```hlsl
float nvL = dot(c, float3(0.299, 0.587, 0.114));        // Rec.601 luma
nvL = (nvL * 0.9 + 0.10) * 1.35;                        // night-lift: raise black
                                                        // point, re-expose midtones
nvL = (nvL - 0.5) * 1.18 + 0.5;                         // contrast push so
                                                        // silhouettes read clearly
float nvT = GradeParams.w;                              // time (seconds)
float nvA = frac(sin(dot(input.uv * 423.1 + nvT * 7.0,
              float2(12.9898, 78.233))) * 43758.5453);  // sensor grain field A
float nvB = frac(sin(dot(input.uv * 391.7 - nvT * 5.0,
              float2(263.5, 126.9))) * 24634.6345);     // sensor grain field B
c  = float3(nvL * 0.23, nvL * 0.72, nvL * 0.26);        // phosphor green
c += float3(0.015, 0.045, 0.017) * (nvA + nvB);         // green-biased grain
float2 nvV = input.uv - 0.5;
c *= 1.0 - saturate(dot(nvV, nvV) * 1.6) * 0.35;        // NV vignette
c  = saturate(c);
```

Summary:

- **Single-luminance extraction** — color information is discarded, so
  camouflage and tint differences vanish, like real image-intensifier tubes.
- **Night-lift + re-exposure** — `(L * 0.9 + 0.10) * 1.35` lifts the black
  point and pushes generic midtones up while bright sources (muzzle flashes,
  fires) stay blown.
- **Contrast push** — a 1.18 gain around mid-grey for silhouette readability.
- **Phosphor green** — the classic P43-ish green, biased to the green channel
  (`0.23 / 0.72 / 0.26`).
- **Timed sensor grain** — two independent hash fields, scrolled in opposite
  directions by the frame time (`GradeParams.w`), so the noise sparkles rather
  than sitting still.
- **Vignette** — quadratic screen-edge falloff (`* 1.6`, `* 0.35`) that mimics
  the circular optic of NV goggles.

---

## Controls & configuration

### Runtime hotkeys (D3D11 backend only)

| Key | Action |
|---|---|
| `F8` | Toggle the whole color-grade pass on/off (edge-detected) |
| `F6` | Toggle night vision (only when the grade pass is enabled, edge-detected) |
| `F10` | Dump the finished (post-grade) frame as a 24-bit uncompressed TGA |
| `F11` | Diagnostic baseline (all subsystems on) |
| `1` | Diag: unit normal-mapping OFF |
| `2` | Diag: screen filters OFF |
| `3` | Diag: water OFF |
| `4` | Diag: all diag suspects OFF |

### Environment variables

| Variable | Default | Effect |
|---|---|---|
| `W3DNEXT_GRADE` | on | `0` disables the color-grade pass entirely |
| `D3D11_SHOT` | `E:\GAVAD_Test\dncshot.tga` | F10 screenshot output path |
| `W3DNEXT_DAYCYCLE` | on | `0` disables the accelerated day/night cycle |
| `W3DNEXT_DAYCYCLE_MINUTES` | `4` | Length of one full day/night loop (1–1440 min) |

The day/night cycle drives `GlobalData::m_timeOfDay` and the terrain light
array so the night vision filter has actual darkness to work with; it lives in
`Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DShaderManager.cpp`.

---

## History

- `2e66626` — *feat(ww3d2): implement night-vision grade pass on GradeMode.x (F6)*:
  the F6 key had been setting `gc.Mode[0]` since the first port, but the grade
  shader never read `GradeMode.x`, so night vision was a no-op flag. This commit
  added the missing branch (luma extraction, night-lift, contrast, phosphor
  green, timed grain, vignette), neutral when `GradeMode.x = 0`.
- `f44129b` — *chore: snap current D3D11 work for public GitHub release*:
  color grade, god rays, bloom and the current tuning constants landed here.
