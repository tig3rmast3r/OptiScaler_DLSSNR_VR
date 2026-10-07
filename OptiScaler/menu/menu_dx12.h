#pragma once
#include "menu_dx_base.h"
#include "InUpscalerDx12Renderer.h"
#include <mutex>

class Menu_Dx12 : public MenuDxBase
{
    std::unique_ptr<InUpscalerDx12Renderer> renderer_;
    std::mutex renderMutex_;
public:
    bool Render(ID3D12GraphicsCommandList* list, ID3D12Resource* output);
    Menu_Dx12(HWND handle, ID3D12Device* device);
    ~Menu_Dx12();
};
