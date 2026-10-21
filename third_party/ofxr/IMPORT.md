# Embedded OFXR provenance

Initially imported from [OFXR-Bridge](https://github.com/tig3rmast3r/OFXR-Bridge),
internal XRFG-V072 (2026-09-09).
The renderer, layer frame loop, ownership, synchronization, overlay and flight
recorder are copied unchanged. CMake builds a static layer target, uses an
external combined-DLL path for runtime harnesses and disables the standalone.
The OFXR DX12 backend, optical-flow API, context/interface types, shader-blob
dispatchers and shared object-management functions get private symbols via a
forced-include header. The host's prebuilt FSR libraries have incompatible
interfaces despite the pinned source submodule. Sharing ffxSafeReleasePipeline
reproduced an access violation through the wrong callback offset in MapResourceDX12.
Never remove the symbol isolation or resolve duplicate symbols with FORCE:MULTIPLE.
The initial integration was V23043. Subsequent surviving imports through the
V33034 endpoint are identified in [the source provenance](../../docs/V33034_PROVENANCE.md)
and [the detailed commit map](../../docs/V33034_IMPORT_MAP.csv), including
the adapted work by [djules75](https://github.com/djules75/OFXR-Bridge).

OFXR: LGPL-3.0-or-later; preserve LICENSE and licenses/. Combined OptiScaler
remains GPL-3.0. OpenXR headers: Khronos commit
5267613edf3d937e3d77556a106a65c2f82b25c6, Apache-2.0/MIT; LICENSES included.
NVIDIA Optical Flow interface headers: SDK 5.0.7, header-specific MIT notice.
Driver nvofapi64.dll is loaded dynamically and is not distributed.
FidelityFX: reuse external/FidelityFX-SDK, v1.1.4,
c6efa6bf7f2027b3ec94f28578bb5965eabb9e55, MIT.

Use the VS2022 x64 Release build described in the source provenance.
build_ofxr.ps1 builds the DX12 optical-flow backend from the pinned SDK
incrementally, compiles the OFXR HLSL entrypoints, and builds the static libraries.
The OptiScaler MSBuild project links them directly, including the OpenXR
negotiation export. No separate OFXR DLL is built or loaded.
