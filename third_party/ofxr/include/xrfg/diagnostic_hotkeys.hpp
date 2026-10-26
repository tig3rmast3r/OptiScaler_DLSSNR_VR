#pragma once
#include <Windows.h>
#include <atomic>
namespace xrfg {
inline constexpr int kImageCaptureKey = VK_F8;
inline constexpr int kLoggingKey = VK_F9;
struct DiagnosticKeyEdge {
    std::atomic_bool down{};
    bool pressed(bool value) noexcept {
        const bool previous = down.exchange(value, std::memory_order_relaxed);
        return value && !previous;
    }
    bool poll(int key) noexcept {
        return pressed((GetAsyncKeyState(key) & 0x8000) != 0);
    }
};
}
