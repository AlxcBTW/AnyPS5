#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// Loads a dword through the pointer in a two-dword table and stores it through
// the buffer descriptor behind the second user-data pointer.
alignas(256) std::array<std::uint32_t, 12> ReaderCode{0xf4040100u, 0xfa000000u, 0xf4080201u, 0xfa000000u, 0xbf8cc07fu, 0xf4000182u, 0xfa000000u, 0xbf8cc07fu, 0x7e000206u, 0xe0700000u, 0x80020000u, 0xbf810000u};

alignas(256) std::array<std::uint32_t, 4> Source{};
alignas(256) std::array<std::uint32_t, 2> Table{};
alignas(256) std::array<std::uint32_t, 4> FirstOutput{};
alignas(256) std::array<std::uint32_t, 4> MovedOutput{};
alignas(256) std::array<std::uint32_t, 4> OutputDescriptor{};

std::uint32_t Low(const void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::uint32_t High(const void* pointer) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer)) >> 32u);
}

std::array<std::uint32_t, 4> Descriptor(const void* pointer, std::uint32_t bytes) {
    return {Low(pointer), High(pointer) & 0xffffu, bytes, 0x31016facu};
}

void SetShaderRegister(std::vector<std::uint32_t>& words, std::uint32_t offset, std::uint32_t value) {
    words.insert(words.end(), {0xc0017600u, offset, value});
}

void Dispatch(std::vector<std::uint32_t>& words, const void* code, std::initializer_list<std::uint32_t> userData) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(code));
    SetShaderRegister(words, 0x20c, static_cast<std::uint32_t>(address >> 8u));
    SetShaderRegister(words, 0x20d, static_cast<std::uint32_t>(address >> 40u) & 0xffu);
    for (const std::uint32_t offset : {0x207u, 0x208u, 0x209u}) SetShaderRegister(words, offset, 1);
    SetShaderRegister(words, 0x212, 0);
    SetShaderRegister(words, 0x213, static_cast<std::uint32_t>(userData.size()) << 1u);
    std::uint32_t offset = 0x240;
    for (const auto value : userData) SetShaderRegister(words, offset++, value);
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x8041u});
}

struct Counts {
    std::uint64_t lookups, inserts, relocatedLookups, relocatedHits, tagged, verifyOk, verifyMismatch;
};

Counts Snapshot() {
    const auto entries = AgcDriver::DriverDetail::Driver::Get().DispatchCacheCounters();
    const auto reloc = AgcDriver::DriverDetail::Driver::Get().DispatchRelocationStats();
    return {entries.lookups, entries.inserts, reloc.lookups, reloc.hits, reloc.insertsTagged, reloc.verifyOk, reloc.verifyMismatch};
}

void RunOnce(std::uint32_t value, std::array<std::uint32_t, 4>& output) {
    Source[0] = value;
    Table = {Low(Source.data()), High(Source.data())};
    OutputDescriptor = Descriptor(output.data(), sizeof(output));
    std::vector<std::uint32_t> words;
    Dispatch(words, ReaderCode.data(), {Low(Table.data()), High(Table.data()), Low(OutputDescriptor.data()), High(OutputDescriptor.data())});
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitAcb(0x20, &packet) == 0, "compute submit failed");
    AgcDriverWaitIdle_nid_postfix();
}

}

int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        device.reset();
        const char* mode = std::getenv("APS5_DISPATCH_RELOCATION");
        const bool enabled = mode != nullptr && mode[0] != '\0' && std::strcmp(mode, "0") != 0;
        const bool verify = enabled && std::strcmp(mode, "verify") == 0;

        const auto base = Snapshot();
        RunOnce(0x11111111u, FirstOutput);
        Require(FirstOutput[0] == 0x11111111u, "the first dispatch did not store through the first buffer");
        const auto first = Snapshot();
        Require(first.inserts - base.inserts == 1, "the first dispatch made " + std::to_string(first.inserts - base.inserts) + " cache inserts, expected 1");
        if (enabled) Require(first.tagged - base.tagged >= 1, "the inserted dispatch variant carries no tagged buffer base words");

        RunOnce(0x22222222u, FirstOutput);
        const auto again = Snapshot();
        Require(FirstOutput[0] == 0x22222222u && again.inserts == first.inserts, "the same dispatch again did not hit the cache");

        RunOnce(0x33333333u, MovedOutput);
        const auto moved = Snapshot();
        Require(MovedOutput[0] == 0x33333333u, "a dispatch whose buffer moved did not store through the moved buffer");
        Require(FirstOutput[0] == 0x22222222u, "a dispatch whose buffer moved stored through the stale buffer");
        if (enabled) {
            Require(moved.relocatedHits - base.relocatedHits == 1, "a dispatch whose buffer moved did not reuse the cached build (" + std::to_string(moved.relocatedHits - base.relocatedHits) + " relocated hits)");
            Require(moved.inserts == first.inserts, "a dispatch whose buffer moved inserted a fresh variant instead of patching the cached one");
        } else {
            Require(moved.relocatedHits == base.relocatedHits, "a relocated hit happened without APS5_DISPATCH_RELOCATION");
            Require(moved.inserts - first.inserts == 1, "without relocation the moved dispatch should capture afresh");
        }

        RunOnce(0x44444444u, MovedOutput);
        const auto settled = Snapshot();
        Require(MovedOutput[0] == 0x44444444u && settled.inserts == moved.inserts, "the moved dispatch again did not hit the cache");

        RunOnce(0x55555555u, FirstOutput);
        const auto back = Snapshot();
        Require(FirstOutput[0] == 0x55555555u, "a dispatch back at the first buffer did not store through it");
        if (enabled) {
            Require(back.relocatedHits - base.relocatedHits == 2, "a dispatch back at the first buffer did not reuse the cached build (" + std::to_string(back.relocatedHits - base.relocatedHits) + " relocated hits)");
            Require(back.inserts == first.inserts, "a dispatch back at the first buffer inserted a fresh variant");
            if (verify) {
                Require(back.verifyMismatch == base.verifyMismatch, "a relocated hit disagreed with its full capture under =verify");
                Require(back.verifyOk - base.verifyOk == 2, "=verify did not check both relocated hits (" + std::to_string(back.verifyOk - base.verifyOk) + " checked)");
            }
        }
        std::cout << "dispatch moved-buffer " << (enabled ? (verify ? "relocation verified" : "relocation") : "no-relocation") << " test passed\n";
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception& shutdownError) {
            std::cerr << shutdownError.what() << '\n';
        }
        return 1;
    }
}
