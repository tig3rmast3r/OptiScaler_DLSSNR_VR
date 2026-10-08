#pragma once
#include <menu/NativeSwapchainOverlay.h>
#include <menu/NativeSwapchainMenu.h>
#include <Config.h>
#include <State.h>
#include <Logger.h>
#include <SysUtils.h>

namespace NativeSwapchainCreation
{
inline bool Required()
{
    return static_cast<bool>(State::Instance().gameQuirks & GameQuirk::NativeSwapchainOverlay);
}
template<class Swapchain> HRESULT Complete(HRESULT result, Swapchain** output, IUnknown* device)
{
    if (FAILED(result) || !output || !*output) return result;
    DXGI_SWAP_CHAIN_DESC desc {};
    if (FAILED((*output)->GetDesc(&desc))) return result;
    // REFramework's bootstrap swapchain must also remain native, but only the
    // game's presentation window owns menu input and GPU resources.
    if (desc.OutputWindow != Util::GetProcessWindow() || Config::Instance()->UseUpscalerMenu()) return result;
    auto attached = NativeSwapchainOverlay::Attach(*output, device, desc.OutputWindow,
                                                  NativeSwapchainMenu::Draw, NativeSwapchainMenu::Reset);
    if (SUCCEEDED(attached))
        LOG_INFO("V33025 native swapchain menu attached without a COM proxy: {}", static_cast<void*>(*output));
    else
        LOG_ERROR("V33025 native swapchain menu attachment failed: {:X}", static_cast<unsigned>(attached));
    return result;
}
}
