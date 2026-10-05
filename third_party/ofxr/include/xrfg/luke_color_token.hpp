#pragma once

#include <cstdint>
#include <limits>

namespace xrfg {

// Serialized by the owning process registry mutex. There is deliberately no
// reset, copy, or assignment operation: stream epochs cannot recycle a token.
class LukeColorTokenAllocator final {
public:
    constexpr explicit LukeColorTokenAllocator(std::uint64_t first = 1) noexcept
        : next_(first) {}
    LukeColorTokenAllocator(const LukeColorTokenAllocator&) = delete;
    LukeColorTokenAllocator& operator=(const LukeColorTokenAllocator&) = delete;

    [[nodiscard]] constexpr std::uint32_t allocate() noexcept {
        if (!next_ || next_ > std::numeric_limits<std::uint32_t>::max()) return 0;
        return static_cast<std::uint32_t>(next_++);
    }

private:
    std::uint64_t next_;
};

} // namespace xrfg
