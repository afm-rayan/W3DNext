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

// Render backend global owner. Holds the single g_renderBackend pointer and
// constructs/destroys the concrete backend instance.

#include "RenderBackend.h"
#include "DX8Backend.h"
#include "D3D11Backend.h"

#include <cstdio>
#include <cstdlib>
#include <windows.h>

IRenderBackend * g_renderBackend = nullptr;

// Backend selection, driven by the Generals-side -gfxBackend flag before the
// device-dependent inits run (see Set_Use_D3D11_Backend). Default false keeps the
// DX8 reference backend so the default game path is byte-identical.
static bool s_useD3D11Backend = false;

void Set_Use_D3D11_Backend(bool use)
{
	s_useD3D11Backend = use;
}

const char * W3DNext_GetEnv(const char * suffix)
{
	if (suffix == nullptr) {
		return nullptr;
	}

	char name[128];

	// Preferred, project-named form.
	std::snprintf(name, sizeof(name), "W3DNEXT_%s", suffix);
	const char * value = std::getenv(name);
	if (value != nullptr) {
		return value;
	}

	// Legacy zpower-tree form, kept so existing harness scripts keep working.
	std::snprintf(name, sizeof(name), "ZP_%s", suffix);
	return std::getenv(name);
}

namespace
{
// Recon-only diagnostic sink, self-contained (Core must not depend on the
// Generals debug log). Appends one line to the file named by env W3DNEXT_D3D11_LOG
// (else "d3d11_backend.log" in the process CWD) and mirrors to OutputDebugString,
// so the selected-backend line is capturable both in-game and from a smoke run.
void RB_Log_Line(const char * line)
{
	const char * path = W3DNext_GetEnv("D3D11_LOG");
	FILE * f = std::fopen(path != nullptr ? path : "d3d11_backend.log", "a");
	if (f != nullptr) {
		std::fputs(line, f);
		std::fputc('\n', f);
		std::fclose(f);
	}
	OutputDebugStringA(line);
	OutputDebugStringA("\n");
}
}

bool Is_D3D11_Backend_Active()
{
	return s_useD3D11Backend && g_renderBackend != nullptr;
}

void Init_Render_Backend()
{
	if (g_renderBackend != nullptr) {
		return;
	}

	if (s_useD3D11Backend) {
		g_renderBackend = new D3D11Backend();
		RB_Log_Line("[RenderBackend] constructed D3D11Backend (-gfxBackend d3d11)");
	} else {
		g_renderBackend = new DX8Backend();
		RB_Log_Line("[RenderBackend] constructed DX8Backend (default path)");
	}
}

void Shutdown_Render_Backend()
{
	// Null-guarded so error-recovery paths that shut down without a matching
	// init don't dereference a null backend.
	if (g_renderBackend == nullptr) {
		return;
	}

	g_renderBackend->Shutdown();
	delete g_renderBackend;
	g_renderBackend = nullptr;
}

// --- Diagnostic mode (F11 hotkey, polled in D3D11Backend::Begin_Scene) -------
volatile int g_w3dnextDiagMode = 0;

const char * W3DNext_Diag_Mode_Name(int mode)
{
	switch (mode) {
	case 0: return "0=NORMAL (all on)";
	case 1: return "1=WATER OFF";
	case 2: return "2=SCREEN FILTERS OFF";
	case 3: return "3=UNIT NORMALMAP OFF";
	case 4: return "4=ALL SUSPECTS OFF (water+filters+normals)";
	default: return "?";
	}
}

// Snapshot the debug logs for the given mode into debug_snapshot_modeN.log so
// every keypress leaves a diffable record (the perf log, the unit-normal probe
// log, and the terrain normal atlas log). Sources may be momentarily held open
// by the live backend; unreadable ones are noted as "(locked)" rather than
// aborting the snapshot.
static void Snapshot_Debug(int mode, const char * modeName)
{
	const char * sources[] = { "d3d11_backend.log", "unitnorm.log", "normaltrace.log" };
	char snapName[MAX_PATH];
	std::snprintf(snapName, sizeof(snapName), "debug_snapshot_mode%d.log", mode);
	FILE * out = std::fopen(snapName, "w");
	if (out == nullptr) {
		return;
	}
	std::fprintf(out, "=== debug_snapshot mode %d : %s ===\n", mode, modeName);
	for (int i = 0; i < 3; ++i) {
		std::fprintf(out, "\n----- %s -----\n", sources[i]);
		FILE * in = std::fopen(sources[i], "rb");
		if (in) {
			char buf[256];
			size_t n;
			while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
				std::fwrite(buf, 1, n, out);
			}
			std::fclose(in);
		} else {
			std::fprintf(out, "(locked/unavailable)\n");
		}
	}
	std::fclose(out);
}

void Set_W3DNext_Diag_Mode(int mode)
{
	if (mode < 0) mode = 0;
	if (mode > 4) mode = 4;
	g_w3dnextDiagMode = mode;

	const char * name = W3DNext_Diag_Mode_Name(mode);
	FILE * f = std::fopen("diagmode.txt", "w");
	if (f) {
		std::fprintf(f, "diagMode=%d  %s\n", mode, name);
		std::fclose(f);
	}
	char diagLog[256];
	std::snprintf(diagLog, sizeof(diagLog), "[RenderBackend] diag mode -> %s", name);
	RB_Log_Line(diagLog);

	// Diagnostic note: mode 1 disables water (W3DWater.cpp early-out), mode 2 the
	// screen-filter quad, mode 3 unit normal-mapping, mode 4 all three. Each press
	// is a self-contained on/off test - if the artifact survives mode 4 it lives in
	// a base pass (terrain/FOW/decal/sky) outside the diag gates.
	if (mode == 1) {
		RB_Log_Line("[RenderBackend] WATER OFF (mode 1): water render gated at W3DWater::Render - artifact still present means it is NOT water.");
	}

	Snapshot_Debug(mode, name);
}
