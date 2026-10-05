#pragma once
#include <unknwn.h>

namespace streamline_native {

// Streamline's native-interface QI contract. Unlike the public slGetNativeInterface
// export's proxy-dependent ownership, a successful COM QueryInterface owns one ref.
inline constexpr GUID native_interface_guid{
    0xadec44e2, 0x61f0, 0x45c3, {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}};

// The returned pointer is BORROWED. The caller must keep input (and its ownership
// of the native base) alive throughout use; it must not release the result.
// This helper retains no COM object, calls no SL export, and changes no public API.
inline bool get_borrowed(IUnknown* input, IUnknown** output,
    HRESULT* query_result = nullptr) noexcept {
    if (query_result) *query_result = E_POINTER;
    if (!output) return false;
    *output = nullptr;
    if (!input) return false;
    IUnknown* native{};
    const HRESULT result = input->QueryInterface(native_interface_guid,
        reinterpret_cast<void**>(&native));
    if (query_result) *query_result = result; // Exact existing QI, not a second query.
    if (SUCCEEDED(result) && native) {
        *output = native;
        native->Release(); // Balance ONLY the successful QI reference, exactly once.
        return true;
    }
    if (result == E_NOINTERFACE) {
        *output = input; // Already native / unsupported proxy: borrowed unchanged.
        return true;
    }
    // Failed QI owns nothing; a malformed success/null result is not usable.
    return false;
}

} // namespace streamline_native
