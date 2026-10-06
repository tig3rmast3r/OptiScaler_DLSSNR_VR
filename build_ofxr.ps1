param([ValidateSet("standard","djules")][string]$ComparisonRoute = "standard")
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'Visual Studio 2022 C++ toolchain is required' }
$cmake = Join-Path $vsInstall 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) { throw 'Visual Studio CMake component is required' }
$sdkBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin'
$fxc = Get-ChildItem -LiteralPath $sdkBin -Directory | Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'x64/fxc.exe' } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $fxc) { throw 'Windows SDK x64 shader compiler is required' }
$sdk = Join-Path $PSScriptRoot 'external/FidelityFX-SDK/sdk'
$ffxBin = Join-Path $sdk 'bin/ffx_sdk'
$ffxBackend = Join-Path $ffxBin 'ofxr_ffx_backend_dx12_x64.lib'
$ffxOpticalFlow = Join-Path $ffxBin 'ofxr_ffx_opticalflow_x64.lib'
$openXrSdk = Join-Path $PSScriptRoot 'third_party/ofxr/external/OpenXR-SDK'
$nvidiaOpticalFlowSdk = Join-Path $PSScriptRoot 'third_party/ofxr/external/NVIDIA-Optical-Flow-SDK-5.0.7/Optical_Flow_SDK_5.0.7'
if (-not (Test-Path -LiteralPath $ffxBackend) -or
    -not (Test-Path -LiteralPath $ffxOpticalFlow)) {
    & $cmake --fresh -S $sdk -B "$PSScriptRoot/build-ofxr-ffx" -G 'Visual Studio 17 2022' -A x64 -DFFX_API_BACKEND=DX12_X64 -DFFX_ALL=OFF -DFFX_OF=ON -DFFX_BUILD_AS_DLL=OFF "-DCMAKE_PROJECT_INCLUDE=$PSScriptRoot/third_party/ofxr/ffx_private_build.cmake"
    if ($LASTEXITCODE) { throw 'FidelityFX configuration failed' }
    & $cmake --build "$PSScriptRoot/build-ofxr-ffx" --config Release --parallel 8
    if ($LASTEXITCODE) { throw 'FidelityFX build failed' }
}
& $cmake --fresh -S "$PSScriptRoot/third_party/ofxr" -B "$PSScriptRoot/build-ofxr" -G 'Visual Studio 17 2022' -A x64 -DXRFG_BUILD_STANDALONE=OFF -DXRFG_BUILD_TESTS=OFF "-DXRFG_COMPARISON_ROUTE=$ComparisonRoute" "-DXRFG_FIDELITYFX_SDK_ROOT=$sdk" "-DXRFG_OPENXR_SDK_ROOT=$openXrSdk" "-DXRFG_NVIDIA_OPTICAL_FLOW_SDK_ROOT=$nvidiaOpticalFlowSdk" "-DXRFG_FXC_EXECUTABLE=$fxc" "-DXRFG_TEST_LAYER_DLL=$PSScriptRoot/x64/Release/OptiScaler.dll"
if ($LASTEXITCODE) { throw 'OFXR configuration failed' }
& $cmake --build "$PSScriptRoot/build-ofxr" --config Release --parallel 8
if ($LASTEXITCODE) { throw 'OFXR build failed' }
