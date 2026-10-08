#pragma once
#include <d3d12.h>
#include <wrl/client.h>

namespace DlssNr
{
// NGX can receive Streamline's proxy while native ExecuteCommandLists receives
// the underlying list. Record and detach against that same real object.
inline Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> NrNativeCommandList(ID3D12CommandList* list)
{
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> identity;
    if (!list) return identity;
    constexpr GUID nativeObject {0xadec44e2, 0x61f0, 0x45c3, {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}};
    Microsoft::WRL::ComPtr<IUnknown> native;
    const auto result = list->QueryInterface(nativeObject, reinterpret_cast<void**>(native.GetAddressOf()));
    if (SUCCEEDED(result))
    {
        if (native) native.As(&identity);
    }
    else if (result == E_NOINTERFACE)
        list->QueryInterface(IID_PPV_ARGS(&identity));
    return identity;
}
}
