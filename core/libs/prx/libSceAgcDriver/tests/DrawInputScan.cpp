// The draw input scans of the in-place draw inputs (Graphics::prepareDrawInputs): the vectorized
// highest-index scan, the any-index check of a mesh draw and the restart table's early out give
// exactly what the scalar HighestDrawIndex and MeshRestartTable give, for both index sizes, with and
// without primitive restart, over buffers of every length around the vector widths.
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include <cstdio>
#include <cstring>
#include <exception>
#include <random>
#include <stdexcept>
#include <vector>

using AgcDriver::Graphics::AnyDrawIndex;
using AgcDriver::Graphics::HighestDrawIndex;
using AgcDriver::Graphics::HighestDrawIndexFast;
using AgcDriver::Graphics::MeshRestartTable;
using AgcDriver::Graphics::MeshRestartTableFast;

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Index>
std::vector<std::byte> bytesOf(const std::vector<Index>& indices) {
    std::vector<std::byte> bytes(indices.size() * sizeof(Index));
    if (!bytes.empty()) std::memcpy(bytes.data(), indices.data(), bytes.size());
    return bytes;
}

template <typename Index>
void compare(const std::vector<Index>& indices, const char* what) {
    const auto bytes = bytesOf(indices);
    const std::span<const std::byte> span(bytes);
    for (const bool skipRestart : {false, true}) {
        const auto expected = HighestDrawIndex(span, sizeof(Index), skipRestart);
        const auto actual = HighestDrawIndexFast(span, sizeof(Index), skipRestart);
        if (expected != actual) {
            std::fprintf(stderr, "%s: %zu indices of %zu bytes, restart %d: expected %s%u, got %s%u\n", what, indices.size(), sizeof(Index), skipRestart ? 1 : 0, expected ? "" : "none ", expected.value_or(0u), actual ? "" : "none ", actual.value_or(0u));
            throw std::runtime_error("the vectorized highest index differs from the scalar scan");
        }
        check(AnyDrawIndex(span, sizeof(Index), skipRestart) == expected.has_value(), "the any-index check differs from the scalar scan");
    }
    check(MeshRestartTableFast(span, sizeof(Index)) == MeshRestartTable(span, sizeof(Index)), "the restart table's early out changed the table");
}

template <typename Index>
void testIndexSize(std::mt19937& random) {
    constexpr Index restart = std::numeric_limits<Index>::max();
    compare(std::vector<Index>{}, "empty");
    compare(std::vector<Index>{restart}, "one restart");
    compare(std::vector<Index>(37, restart), "only restarts");
    compare(std::vector<Index>{0}, "one zero");
    compare(std::vector<Index>{restart, 0, restart}, "zero between restarts");
    compare(std::vector<Index>{static_cast<Index>(restart - 1), restart}, "highest below restart");
    for (std::size_t length = 1; length <= 70; ++length) {
        for (int round = 0; round < 8; ++round) {
            std::vector<Index> indices(length);
            const auto ceiling = round % 2 == 0 ? static_cast<std::uint64_t>(restart) : 300u;
            std::uniform_int_distribution<std::uint64_t> value(0, ceiling);
            std::uniform_int_distribution<int> restarts(0, 5);
            for (auto& index : indices) index = restarts(random) == 0 ? restart : static_cast<Index>(value(random));
            compare(indices, "random");
            // The maximum at either end, where a vector loop's head or tail could miss it.
            indices.front() = static_cast<Index>(restart - 1);
            compare(indices, "maximum first");
            indices.front() = 0;
            indices.back() = static_cast<Index>(restart - 1);
            compare(indices, "maximum last");
        }
    }
    std::vector<Index> large(100000);
    std::uniform_int_distribution<std::uint64_t> value(0, 50000);
    for (auto& index : large) index = static_cast<Index>(value(random));
    large[77777] = restart;
    compare(large, "large");
}

}

int main() {
    try {
        std::mt19937 random(5529);
        testIndexSize<std::uint16_t>(random);
        testIndexSize<std::uint32_t>(random);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "draw input scan test failed: %s\n", error.what());
        return 1;
    }
    std::puts("draw input scan tests passed");
    return 0;
}
