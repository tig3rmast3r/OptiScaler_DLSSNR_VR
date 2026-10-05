#pragma once
#include "Streamline_NativeInterface.h"

namespace d3d_native {
// ReShade IID_UnwrappedObject, source/com_utils.hpp (v6.8.0).
// Both supported contracts return the same interface kind with one owned QI ref.
inline constexpr GUID reshade_unwrapped_guid{
    0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};

// Borrowed result: input must remain alive and own its underlying objects.
// Reject malformed queries, cycles and excessively deep wrapper chains.
inline bool get_borrowed(IUnknown* input, IUnknown** output,
    HRESULT* query_result = nullptr) noexcept {
    if (query_result) *query_result = E_POINTER;
    if (!output) return false;
    *output = nullptr;
    if (!input) return false;
    IUnknown* visited[8]{};
    auto* current = input;
    bool unwrapped = false;
    for (unsigned depth = 0; depth < 8; ++depth) {
        visited[depth] = current;
        IUnknown* next{};
        auto result = current->QueryInterface(streamline_native::native_interface_guid,
            reinterpret_cast<void**>(&next));
        if (result == E_NOINTERFACE) {
            next = nullptr;
            result = current->QueryInterface(reshade_unwrapped_guid,
                reinterpret_cast<void**>(&next));
        }
        if (query_result) *query_result = result;
        if (result == E_NOINTERFACE) {
            *output = current;
            if (query_result && unwrapped) *query_result = S_OK;
            return true;
        }
        if (FAILED(result) || !next) return false;
        next->Release(); // Balance this QI only; parent keeps borrowed child alive.
        if (next == current) {
            *output = current;
            return true;
        }
        for (unsigned i = 0; i <= depth; ++i) {
            if (visited[i] == next) {
                if (query_result) *query_result = E_UNEXPECTED;
                return false;
            }
        }
        current = next;
        unwrapped = true;
    }
    if (query_result) *query_result = E_UNEXPECTED;
    return false;
}
} // namespace d3d_native
