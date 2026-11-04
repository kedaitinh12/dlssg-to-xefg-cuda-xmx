#pragma once

#include <Windows.h>
#include <d3d12.h>

// MFG-Unlocker's tensor core route for the ported MFG model (CudaRoute/CudaRoute.h). With the ported model published,
// its int8 conv layers run as CUDA kernels on the tensor cores of RTX 20/30/40/50 GPUs, launched through NVAPI in the
// same command list XeFG records into. Everything else, other GPUs and any layer the route doesn't recognize keep
// running the DXIL programs, so nothing here is required for the frame generation itself to work.
//
// Needs xefg_cuda_layers.fatbin, MFG-Unlocker's pinned build of its kernels, next to this dll in
// xefg_mfg_programs\cuda. The opt variant there (xefg_cuda_layers.opt.fatbin, built by build.yml) runs instead when it is
// present and usable; XeFGTensorCoresKernels = stock keeps the pinned build. dlssg_to_xefg_intel_is_better.ini next to the dll turns it off:
//   [XeFG]
//   XeFGTensorCores = off
// and XeFGTensorCoresOwnTables = on lets it hook game command lists that keep a table of their own (see Adopt)
namespace CudaKernels
{
	// Before XeFG creates its first context on device (it creates its pipeline states there, which is where the route
	// recognizes the layers). Idempotent. False when the route is off or unavailable, the reason is in the log
	bool Install(ID3D12Device* device, HMODULE provider);

	// On the open command list XeFG is about to record into, so the layer Dispatches it records there can be launches.
	// Only lists on a class table the runtime shares between its lists, unless XeFGTensorCoresOwnTables is on
	void Adopt(ID3D12GraphicsCommandList* cmdList);

	// After a successful Execute, logs the first time kernels have run
	void Executed();

	// Every XeFG context pairs these. When the last one is destroyed the route lets go of its pipeline states
	void ContextCreated();
	void ContextDestroyed();
} // namespace CudaKernels
