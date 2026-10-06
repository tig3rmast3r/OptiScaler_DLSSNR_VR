#pragma once
#include <cstdint>
#include <cstring>

namespace luke_source {
constexpr std::uint32_t unknown = ~std::uint32_t{};
struct Context { std::uint32_t frame{unknown}, viewport{unknown}, feature{unknown}; };
inline thread_local Context active{};
// Diagnostic nesting only. Does NOT select or modify the production camera.
struct Scope {
    Context previous{active};
    Scope(std::uint32_t frame, std::uint32_t viewport, std::uint32_t feature) noexcept {
        active = {frame, viewport, feature};
    }
    ~Scope() { active = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
inline std::uint64_t key(std::uint32_t frame, std::uint32_t viewport) noexcept {
    return (std::uint64_t(viewport) << 32) | frame;
}
inline std::uint64_t pair(float x, float y) noexcept {
    std::uint32_t a{}, b{}; std::memcpy(&a,&x,4); std::memcpy(&b,&y,4);
    return (std::uint64_t(a) << 32) | b;
}
inline std::uint64_t hash(const float* values, unsigned count,
    std::uint64_t h=14695981039346656037ULL) noexcept {
    for(unsigned i=0;i<count;++i) {
        std::uint32_t bits{}; std::memcpy(&bits,values+i,4);
        h=(h^bits)*1099511628211ULL;
    }
    return h;
}
}
