#pragma once
#include <windows.h>
#include <atomic>

namespace xrfg {
// The host publishes its actual NGX core after normal discovery. Native FG
// never re-enters the OptiScaler SR/FG exports or triggers another ingress.
inline std::atomic<HMODULE> native_ngx_owner{};
inline void register_native_ngx_owner(HMODULE module) noexcept {
    if(module)native_ngx_owner.store(module,std::memory_order_release);
}
inline FARPROC native_ngx_entry(const char* name) noexcept {
    auto module=native_ngx_owner.load(std::memory_order_acquire);
    if(!module)module=GetModuleHandleW(L"_nvngx.dll");
    if(!module)module=GetModuleHandleW(L"nvngx.dll");
    if(!module)return nullptr;
    HMODULE self{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&native_ngx_entry),&self);
    return module!=self?GetProcAddress(module,name):nullptr;
}
}
