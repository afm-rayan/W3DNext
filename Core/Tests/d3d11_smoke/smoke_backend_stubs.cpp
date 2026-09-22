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

// Standalone smoke harness scaffolding. The real owner of these symbols is
// RenderBackend.cpp, but that translation unit also constructs DX8Backend and
// therefore drags the whole DX8 surface (dx8wrapper + engine) into the link.
// The oracle instantiates D3D11Backend directly, so provide the two dev-knob
// entry points it calls - with the same semantics as RenderBackend.cpp - here
// instead, keeping the test executable independent of the ww3d2 library.

#include "Backend/RenderBackend.h"

#include <cstdio>
#include <cstdlib>

volatile int g_w3dnextDiagMode = 0;

const char * W3DNext_GetEnv(const char * suffix)
{
	if (suffix == nullptr) {
		return nullptr;
	}

	static char name[128];
	std::snprintf(name, sizeof(name), "W3DNEXT_%s", suffix);
	return std::getenv(name);
}

void Set_W3DNext_Diag_Mode(int mode)
{
	if (mode < 0) mode = 0;
	if (mode > 4) mode = 4;
	g_w3dnextDiagMode = mode;
}
