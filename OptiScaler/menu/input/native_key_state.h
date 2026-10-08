#pragma once

#include <Windows.h>

namespace OptiInput
{
// The menu needs physical state, not the game-facing USER32 detour chain.
// Other overlays and USER32's per-thread state can suppress that chain even
// after an input producer has recorded a genuine button/key press.
using NativeAsyncKeyState = SHORT(WINAPI*)(int);

inline NativeAsyncKeyState NativeAsyncKeyStateReader()
{
    static const auto reader = []() -> NativeAsyncKeyState
    {
        const auto module = GetModuleHandleW(L"win32u.dll");
        return module == nullptr ? nullptr :
            reinterpret_cast<NativeAsyncKeyState>(GetProcAddress(module, "NtUserGetAsyncKeyState"));
    }();
    return reader;
}

inline SHORT ReadPhysicalAsyncKeyState(int vk)
{
    if (vk < 0 || vk >= 256)
        return 0;
    const auto reader = NativeAsyncKeyStateReader();
    return reader == nullptr ? 0 : reader(vk);
}
} // namespace OptiInput
