#include "xrfg/luke_color_token.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace {
unsigned checks{};
void require(bool value, const char* message) {
    ++checks;
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
}

int main() {
    static_assert(!std::is_copy_constructible_v<xrfg::LukeColorTokenAllocator>);
    static_assert(!std::is_copy_assignable_v<xrfg::LukeColorTokenAllocator>);
    xrfg::LukeColorTokenAllocator allocator;
    std::unordered_set<std::uint32_t> seen;
    std::array<std::uint32_t,6> history{};
    std::uint32_t previous{};
    for (unsigned frame = 0; frame < 4096; ++frame) {
        const auto token = allocator.allocate();
        require(token != 0 && token > previous, "token zero, reordered or recycled");
        require(seen.insert(token).second, "token collision across history reuse");
        history[frame % history.size()] = token;
        previous = token;
        if (frame % 17 == 0) history = {}; // epoch/disable/retirement clear ONLY history
    }
    history = {};
    const auto restarted = allocator.allocate();
    require(restarted > previous, "stream restart reset process token sequence");
    require(!seen.contains(restarted), "restart reused an evicted token");
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    xrfg::LukeColorTokenAllocator exhausted(static_cast<std::uint64_t>(maximum) - 1);
    require(exhausted.allocate() == maximum - 1, "last-but-one token missing");
    require(exhausted.allocate() == maximum, "UINT32_MAX token missing");
    for (unsigned retry = 0; retry < 16; ++retry)
        require(exhausted.allocate() == 0, "exhausted token wrapped or restarted");
    xrfg::LukeColorTokenAllocator invalid(0);
    require(invalid.allocate() == 0 && invalid.allocate() == 0, "invalid initial zero became valid");
    xrfg::LukeColorTokenAllocator outside(static_cast<std::uint64_t>(maximum) + 1);
    require(outside.allocate() == 0, "out-of-range token truncated to32bits");
    std::cout << "Luke color token allocator: " << checks << " checks PASS\n";
}
