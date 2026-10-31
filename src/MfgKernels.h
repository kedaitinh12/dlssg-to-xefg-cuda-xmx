#pragma once

#include <Windows.h>

#include <cstdint>

// MFG-Unlocker's ported XMX multi frame model, driven the same way it drives it.
//
// libxess_fg.dll only ships the DP4a model (18). With more than one generated frame XeFG would use its MFG model (16),
// which that build doesn't have, so it falls back to 18 and every generated frame gets its own optical flow field.
// MFG-Unlocker ports model 16 into the loaded library: the model catalog entry, its graph, and the ported XMX
// programs that compute one optical flow field for the whole batch.
namespace MfgKernels
{
	// Ids in XeFG's model catalog
	constexpr int32_t Dp4aModel = 18;
	constexpr int32_t MfgModel = 16;

	// Installs the port once, before the first context is created, and keeps it for the process lifetime. False when
	// the program set isn't next to this dll or libxess_fg.dll isn't a build MFG-Unlocker's port knows, in which case
	// XeFG keeps its DP4a model
	bool Install(HMODULE module);
} // namespace MfgKernels
