# Embedded OFXR provenance

Imported from D:/repos/openxr-framegen-bridge, internal XRFG-V072 (2026-09-09).
The renderer, layer frame loop, ownership, synchronization, overlay and flight
recorder are copied unchanged. CMake builds a static layer target, uses an
external combined-DLL path for runtime harnesses and disables the standalone.
The OFXR DX12 backend, optical-flow API, context/interface types, shader-blob
dispatchers and shared object-management functions get private symbols via a
forced-include header. The host's prebuilt FSR libraries have incompatible
interfaces despite the pinned source submodule. Sharing ffxSafeReleasePipeline
reproduced an access violation through the wrong callback offset in MapResourceDX12.
Never remove the symbol isolation or resolve duplicate symbols with FORCE:MULTIPLE.
Source identities are recorded by V23043's laboratory checkpoint manifest.

OFXR: LGPL-3.0-or-later; preserve LICENSE and licenses/. Combined OptiScaler
remains GPL-3.0. OpenXR headers: Khronos commit
5267613edf3d937e3d77556a106a65c2f82b25c6, Apache-2.0/MIT; LICENSES included.
NVIDIA Optical Flow interface headers: SDK 5.0.7, header-specific MIT notice.
Driver nvofapi64.dll is loaded dynamically and is not distributed.
FidelityFX: reuse external/FidelityFX-SDK, v1.1.4,
c6efa6bf7f2027b3ec94f28578bb5965eabb9e55, MIT.

Run build_v23043.ps1 with VS2022 BuildTools installed. build_ofxr.ps1 builds the
DX12 optical-flow backend from the pinned SDK incrementally, compiles all eight
OFXR HLSL entrypoints, and builds the static libraries and test harnesses.
The OptiScaler MSBuild project links them directly, including the OpenXR
negotiation export. No separate OFXR DLL is built or loaded.
