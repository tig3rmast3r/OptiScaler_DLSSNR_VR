# V33034 source and import provenance

Prepared locally on 2026-10-22 above the published `witcher3vr-stereo` base
`0b4abc0808ac49679e73674d6d0ecc59303e0242`. The 57 source commits reconstruct the useful
surviving changes; related fixture/lifetime corrections remain with their change.
Rejected experiments and their original implementation ancestry are excluded.

## Source identity

The final source commit is `613b08d117ba15c5bd3ae5ce804d622fa67690ca`. Its complete tree is
`39f775f22564df0924eea8ab20bc2f610ce12e3f`, identical to the accepted V33034 source subtree.
Original tested source: `91a07361a09a4f7d8d92c6fa9d564a5ae05a9c0e`.
Canonical accepted source: `2018dbe81e6eab87e8bbeb5bc12fcc06e418dd2f`.
These source identifiers describe provenance; their repository history is not
merged into this publication history.

The sequence is the retained UEVR/Witcher convergence and Luke/Crimson endpoint,
the selected djules adaptations through V32037, KCD compatibility, runtime
unification/live controls and djules V438, the accepted DX12 menu endpoint,
Luke V30098, Village/Returnal ownership, and the accepted CSP/DX11 endpoint.
The stop is V33034: no V33035+ startup/delivery changes or later Elliott imports.

## Attribution

[The detailed commit map](V33034_IMPORT_MAP.csv) records every projected commit,
original integration/snapshot, tree, original author/date and upstream source.
The integration author's identity/date and existing coauthors are retained.
Adapted [djules75 OFXR work](https://github.com/djules75/OFXR-Bridge) additionally
records verified network SHAs and `Upstream-Repository`, `Upstream-Commit` and
`Co-authored-by` trailers. All 32 distinct upstream source commits were fetched
from that public repository; their author/date metadata is recorded in the map.
The verified djules identity is
`djules75 <1819336+djules75@users.noreply.github.com>`; historical credit aliases
and additional original coauthors are preserved as well.

V32001 maps the actual import to `4451000c0ef996b799e53d401279f343d66a1d13`;
its snapshot includes the later fixture completion fixes. The V32030 readiness
mechanism is represented only with V32031's native-only ownership correction.
V33023 and V33034 use their accepted final menu/consumer/binding endpoints,
without replaying discarded tracing or context-clearing implementations.
V33018 identifies the seven adapted commits from
[djules 0.2.13.1](https://github.com/djules75/OFXR-Bridge/releases/tag/0.2.13.1).
Pre-SR work retains the [sadbee166 PR #6 credit](https://github.com/Dagherbou/OptiScaler_DLSSNR/pull/6)
and SHA `914963ffb641943fba8f19f3c8654f0620fc68ac` from the published base.
All inherited OptiScaler, OFXR and dependency licence/attribution files remain.

## Build and verification

Use Visual Studio 2022 C++ x64 Build Tools, its CMake component and the Windows
SDK shader compiler. Initialize the pinned submodules with
`git submodule update --init --recursive`. The solution invokes
`build_ofxr.ps1` for the embedded OFXR libraries. Generate the ignored build
metadata inputs before building from a clean checkout:

```powershell
('#define VER_BUILD_DATE "{0}"' -f [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss')) | Set-Content OptiScaler/resource_build_date.h -Encoding ASCII
('#define VER_BUILD_COMMIT "{0}"' -f (git rev-parse --short HEAD)) | Set-Content OptiScaler/resource_build_commit.h -Encoding ASCII
msbuild OptiScaler.sln /p:Configuration=Release /p:Platform=x64 /m:8
```

The fresh optimized Release identifies itself as `10.0.0.33034`, built from
`613b08d1`. DLL size: 27,403,264 bytes;
SHA256 `f227c4a6e721d3dc01488766d450058b285606ca08d1780f91e775a5f8acc07f`. The production DLL stayed byte-identical
through both route suites and all additional fixtures.

| Verification | Result |
| --- | --- |
| new embedded CTest | 111/111 passed; 71 s |
| djules embedded CTest | 126/126 passed; 75 s |
| Production D11 guides/bindings | 877 checks passed; D11/D12 validation enabled, zero GPU errors |
| Additional fixtures | Menu GPU lifetime495, physical menu input13, native swapchain overlay104, DRED48, NR native identity31, KCD camera90 and matched NR forwarder9 passed; pre-SR and current-command-list GPU camera passed |
| Source/helper checks | Origin/stereo ownership contract and pose helper selftest passed |

The original owner-tested V33034 DLL is separate: 27,403,264 bytes, SHA256
`2B7738E3F9E71AF9904B14DB51A3FDEACD1D07EC418DA87C59312D3EA11940C7`.
The fresh build differs in build metadata and is an automated reproduction,
not a new game/headset acceptance. No runtime installation or online publication
is part of this preparation. NVIDIA's proprietary NR runtime is not included.
