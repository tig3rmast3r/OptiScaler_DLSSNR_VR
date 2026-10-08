#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>

// Observe the actual DXGI object. Do not substitute a COM proxy: Steam caches
// native Present entry points and REFramework reads native swapchain internals.
namespace NativeSwapchainOverlay
{
using Draw = void (*)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*,
                      IUnknown*, HWND, bool);
using Reset = void (*)(bool, HWND);
HRESULT Attach(IDXGISwapChain* swapchain, IUnknown* device, HWND window, Draw draw, Reset reset);
}
