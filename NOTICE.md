# Third-party notices

## MFG-Unlocker

`src/MfgPort/` and the model port in `src/MfgKernels.cpp` are ported from
[MFG-Unlocker](https://github.com/dashdogy/MFG-Unlocker), specifically `source/native/xess/runtime_layout.h`,
`runtime_layout.generated.h`, `runtime_payload.*` and `port/model_port.h`, `port/shader_port.h`.

`src/CudaRoute/` is the tensor core route, ported from `source/native/xess/cuda/cuda_route.{h,cpp}` and
`xefg_cuda_params.h`, with the NVAPI owner checks and the device unwrap taken from `source/native/adapter_discovery.h`,
`overlay_native.h` and `overlay_slots.h`. `CudaRoute.cpp` differs from MFG-Unlocker's file by its includes, one renamed
call and the two functions at its end (`Active`, `AdoptRecordingList`). `src/MfgPort/xefg_mfg_programs/cuda/xefg_cuda_layers.fatbin`
is MFG-Unlocker's build of `source/native/xess/cuda/xefg_cuda_layers.cu` (the SHA-256 pinned in its
`generate_cuda_layers.py`), unmodified. `src/CudaKernels.cpp` is this project's glue for it.

MIT License, Copyright (c) 2026 Michael Robles. The full text is in `MFG-Unlocker/LICENSE` in the source tree this port
was taken from, and is reproduced here:

```
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## Intel XeSS Frame Generation program data

This project does not ship Intel's XeSS SDK binaries, model weights or tensors, and it does not ship the ported program
set either. It reads `xefg_mfg_programs\programs.tsv` and the DXIL programs it lists from next to the dll, after
verifying them against MFG-Unlocker's pinned manifest SHA-256.

That program data is derived from programs in Intel's XeSS SDK and is not covered by MFG-Unlocker's MIT license; it
remains subject to Intel's license terms for the XeSS SDK. MFG-Unlocker's own description of it is in
`source/native/xess/XEFG-PROGRAMS-NOTICE.txt`.

Intel and XeSS are trademarks of Intel Corporation or its subsidiaries.
