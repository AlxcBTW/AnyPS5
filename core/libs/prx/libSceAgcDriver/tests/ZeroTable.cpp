#include "prx/libSceAgcDriver/Execution/include/ZeroTable.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

#ifdef _WIN32
DWORD state(const void* address) {
    MEMORY_BASIC_INFORMATION memory{};
    require(VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory), "cannot query the table");
    return memory.State;
}
#endif

}

int main() {
    try {
        using Table = AgcDriver::ZeroTable<std::uint32_t>;
        constexpr std::size_t entries = Table::ChunkEntries * 64 + 3;
        Table table;
        require(!table.Allocate(0), "an empty table was allocated");
        require(table.Allocate(entries) && table.Size() == entries, "the table was not reserved");
        require(table.Bytes() == Table::ChunkBytes * 65, "the reservation does not cover whole chunks");
        require(table.Get(0) == 0 && table.Get(entries - 1) == 0 && table.Max(0, entries) == 0, "an untouched table is not zero");
#ifdef _WIN32
        require(!table.Committed(0) && state(table.Data()) == MEM_RESERVE, "reading the table committed it");
#endif
        const auto middle = Table::ChunkEntries * 20 + 5;
        table.At(middle) = 7;
        table.At(entries - 1) = 9;
        require(table.Committed(middle) && table.Get(middle) == 7 && table.Get(middle + 1) == 0, "a stored entry was lost");
        require(table.Max(0, middle) == 0 && table.Max(0, middle + 1) == 7 && table.Max(middle + 1, entries) == 9, "the maximum skips or invents entries");
        require(Table::ChunkEnd(middle) == Table::ChunkEntries * 21, "the chunk end is wrong");
#ifdef _WIN32
        const auto* bytes = static_cast<const unsigned char*>(table.Data());
        require(state(bytes + Table::ChunkBytes * 20) == MEM_COMMIT, "the stored chunk is not committed");
        require(!table.Committed(Table::ChunkEntries * 19) && state(bytes + Table::ChunkBytes * 19) == MEM_RESERVE && state(bytes + Table::ChunkBytes * 21) == MEM_RESERVE, "a neighbouring chunk was committed");
#endif
        require(table.Allocate(Table::ChunkEntries) && table.Get(middle % Table::ChunkEntries) == 0, "a new reservation kept old entries");
        std::cout << "zero table tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
