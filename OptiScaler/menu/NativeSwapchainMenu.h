#pragma once
#include "NativeSwapchainOverlay.h"
namespace NativeSwapchainMenu
{
void Draw(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*, IUnknown*, HWND, bool);
void Reset(bool, HWND);
}
