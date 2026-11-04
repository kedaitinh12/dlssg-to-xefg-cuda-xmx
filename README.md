# dlssg-to-xefg-xmx

Replaces DLSS Frame Generation with Intel XeSS Frame Generation (XeFG), up to 6x. Inspired by Nukem9's
[dlssg-to-fsr3](https://github.com/Nukem9/dlssg-to-fsr3).

`libxess_fg.dll` only exports XeFG's proxy swap chain. Internally that swap chain drives an unexported
`xefgD3D12CreateContext / Init / Execute` API, which this dll locates and calls directly with the DLSSG inputs. That's
what makes it work as an `nvngx_dlssg` replacement, and it lifts XeFG's 4x cap and its non-Intel 2x clamp without
patching any code.

DX12 only, XeFG has no Vulkan API.

## Usage

`dlssg_to_xefg_intel_is_better.dll` exports the same `NVSDK_NGX_D3D12_*` functions as `nvngx_dlssg.dll`, for a loader
that calls those directly. Put `libxess_fg.dll` (XeFG 1.3.1 or 1.3.4) next to it, unless the game loads a compatible one
itself.

## Installation

1. Download dlssg-to-xefg-#.zip from the [latest release](https://github.com/realdody/dlssg-to-xefg/releases/latest).
2. Make sure you're using OptiScaler v10 https://github.com/optiscaler/OptiScaler-nightly/releases/latest.
3. Extract the XeMFG XMX folder from the zip into OptiScaler\custom. The folder structure should be: gamefolderwithoptiscaler'sdll\OptiScaler\custom\XeMFG XMX
4. Launch the game and open OptiScaler with insert. 
5. The recommended FG input is DLSSG via NVNGX, as it lets you use XeMFG XMX as an output directly.
   If DLSSG via NVNGX is not available or doesn't work, you can use any other input by setting the FG Output to DLSSG, and the FG replacement to XeMFG XMX. Then press Save Settings in the bottom right to save

7. Make sure "Current DLSSG state" says "ON" with the fake frame count you selected. Then check \OptiScaler\custom\XeMFG XMX\dlssg\dlssg_to_xefg_intel_is_better.log
   It should say "Ported XMX MFG model installed, one optical flow field per generated batch" and "Creating XeFG context: 2560x1440, format: 28, flags: 1, ui mode: 4, model: 16"

Model 16 means it's running the XMX path.

## Tensor cores (NVIDIA RTX 20, 30, 40 and 50, experimental)

With the ported model published, its int8 conv layers can run as CUDA kernels on the tensor cores instead of as D3D12
compute, launched through NVAPI in the command list XeFG records into (no CUDA context, no second queue). It is
MFG-Unlocker's route, see [NOTICE.md](NOTICE.md). It needs `xefg_mfg_programs\cuda\xefg_cuda_layers.fatbin` next to the
dll (the release zip has it) and an NVIDIA GPU of compute capability 7.5 (RTX), 8.6, 8.9 or 12.0. Every other GPU, every
layer the route doesn't recognize and any failure keep running the DXIL programs.

XeFG records into the game's own command list, so the route hooks that list's table. By default it only does that when
the list sits on a class table the D3D12 runtime shares between its lists (the system runtime, which most games use).
Games on an Agility SDK fast path keep a table inside each list object; MFG-Unlocker never writes those of a game's
lists, because modules that validate them refuse the hooks. `XeFGTensorCoresOwnTables = on` allows it anyway
(`cuda_list_refused ... game_list_own_table` in the log is the default declining them).

Settings go in `dlssg_to_xefg_intel_is_better.ini` next to the dll, or in the environment variables named below:

```ini
[XeFG]
; off turns the route off (DLSSG_TO_XEFG_TENSOR_CORES=off)
XeFGTensorCores = off
; on also hooks game command lists that keep a table of their own (DLSSG_TO_XEFG_TENSOR_CORES_OWN_TABLES=on)
XeFGTensorCoresOwnTables = on
; stock = MFG-Unlocker's pinned build, auto (the default) = the opt variant when its file is there, or the name of another
; variant the workflow built (DLSSG_TO_XEFG_TENSOR_CORES_KERNELS)
XeFGTensorCoresKernels = stock
```

`.github/workflows/build.yml` compiles variants of MFG-Unlocker's kernels from `src/CudaRoute/kernels/xefg_cuda_layers.cu`
with nvcc and packs them next to the pinned build: `base` (the original code, same compiler), `swizzle`, `ldmatrix`, `opt`
(both) and `opt-min4` (both, and a register cap on one kernel). When nothing is set, the `opt` variant runs if
`xefg_cuda_layers.opt.fatbin` is next to the dll (the workflow's package has it) and carries all 16 kernels. A missing or
unusable file means the pinned build runs instead, which is all the release zip has. Their output is meant to be
byte-identical to the pinned build's, but nobody has run `tests/xess_cuda` (MFG-Unlocker's byte-for-byte comparison
against Intel's programs) on them. `XeFGTensorCoresKernels = stock` in the ini (or `DLSSG_TO_XEFG_TENSOR_CORES_KERNELS=stock`)
goes back to the pinned build, another name picks that variant. To try a fatbin of your own, point
`DLSSG_TO_XEFG_TENSOR_CORES_FATBIN` at it. Neither is held to the pinned SHA-256 (the log says so), so a set has to keep
the kernels' names, 128 thread blocks and parameters.

`dlssg_to_xefg_intel_is_better.log` shows what happened. `MfgPort cuda_route {"state":"active"...}` means the kernels
were created, `MfgPort cuda_layer` lines name the layers recognized, and `Tensor cores are running` appears once kernels
have launched. `Tensor cores use the ...` names the kernel set that was chosen. `cuda_route ... "state":"unavailable"` or
`cuda_list_refused` lines say why the layers stayed on D3D12.

## Which libxess_fg.dll is used

1. Every `libxess_fg.dll` already loaded in the process, the first compatible one wins
2. The `libxess_fg.dll` next to this dll
3. A `libxess_fg.dll` up to three folders above this dll, which covers OptiScaler's own copy in its root
   (`\OptiScaler\libxess_fg.dll`)

Compatible means a build with known function addresses (XeFG 1.3.1 from XeSS SDK 3.0.2, XeFG 1.3.4), or any XeFG
1.3.x where the functions can be found by scanning and the settings match one of the known layouts. The struct layouts
were only verified for 1.3.1 and 1.3.4.

`dlssg_to_xefg_intel_is_better.log` next to the dll says which one was picked.

## Build

Visual Studio 2026 (v145 toolset), open `dlssg-to-xefg.sln` and build Release x64. The output lands in `bin\Release`.

## Credits

The port in `src/MfgPort/`, `src/MfgKernels.cpp`, `src/CudaRoute/` and the kernels in
`src/MfgPort/xefg_mfg_programs/cuda` is from [MFG-Unlocker](https://github.com/dashdogy/MFG-Unlocker), MIT licensed,
Copyright (c) 2026 Michael Robles. See [NOTICE.md](NOTICE.md).

## License

GPLv3, see [LICENSE](LICENSE).
