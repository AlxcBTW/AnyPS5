#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
alignas(256) constexpr std::array<std::uint32_t, 9> Code{0xd7460000u, 0x04010c08u, 0xf4201a82u, 0xfa000000u, 0xbf8cc07fu, 0x7e02026au, 0xe0102000u, 0x80000100u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 6> ZeroCode{0xd7460000u, 0x04010c04u, 0x7e020280u, 0xe0102000u, 0x80000100u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 6> OneCode{0xd7460000u, 0x04010c04u, 0x7e020281u, 0xe0102000u, 0x80000100u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 6> SixtyFourCode{0xd7460000u, 0x04010c04u, 0x7e0202c0u, 0xe0102000u, 0x80000100u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 6> MinusOneCode{0xd7460000u, 0x04010c04u, 0x7e0202c1u, 0xe0102000u, 0x80000100u, 0xbf810000u};
alignas(256) std::array<std::uint32_t, 4> Value{};
alignas(256) std::array<std::uint32_t, 1028> Output{};

std::array<std::uint32_t, 8> UserData(const std::uint32_t* output, std::uint32_t records, std::uint32_t valueBytes, std::uint32_t outputFormat) {
    const auto address = reinterpret_cast<std::uintptr_t>(output);
    const auto value = reinterpret_cast<std::uintptr_t>(Value.data());
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u), records, outputFormat, static_cast<std::uint32_t>(value), static_cast<std::uint32_t>((value >> 32u) & 0xffffu), valueBytes, 0x00014004u};
}

std::vector<std::uint32_t> Commands(const std::uint32_t* output, std::uint32_t records, std::uint32_t groups, std::uint32_t wave, std::uint32_t valueBytes = sizeof(Value), std::uint32_t outputFormat = 0x00014004u) {
    std::vector<std::uint32_t> words;
    const auto registers = [&](std::uint32_t first, std::span<const std::uint32_t> values) {
        words.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
        words.push_back(first);
        words.insert(words.end(), values.begin(), values.end());
    };
    const std::array<std::uint32_t, 3> shape{64, 1, 1};
    registers(0x207, shape);
    const auto address = reinterpret_cast<std::uintptr_t>(Code.data());
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u)};
    registers(0x20c, program);
    const std::array<std::uint32_t, 1> resource{(8u << 1u) | (1u << 7u)};
    registers(0x213, resource);
    registers(0x240, UserData(output, records, valueBytes, outputFormat));
    words.insert(words.end(), {0xc0031500u, groups, 1, 1, 0x41u | (wave == 32 ? 0x8000u : 0u)});
    return words;
}

std::vector<std::uint32_t> ConstantCommands(std::span<const std::uint32_t> code, const std::uint32_t* output, std::uint32_t records, std::uint32_t groups, std::uint32_t wave) {
    std::vector<std::uint32_t> words;
    const auto registers = [&](std::uint32_t first, std::span<const std::uint32_t> values) {
        words.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
        words.push_back(first);
        words.insert(words.end(), values.begin(), values.end());
    };
    const std::array<std::uint32_t, 3> shape{64, 1, 1};
    registers(0x207, shape);
    const auto address = reinterpret_cast<std::uintptr_t>(code.data());
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u)};
    registers(0x20c, program);
    const std::array<std::uint32_t, 1> resource{(4u << 1u) | (1u << 7u)};
    registers(0x213, resource);
    const auto target = reinterpret_cast<std::uintptr_t>(output);
    const std::array<std::uint32_t, 4> descriptor{static_cast<std::uint32_t>(target), static_cast<std::uint32_t>((target >> 32u) & 0xffffu) | (4u << 16u), records, 0x00014004u};
    registers(0x240, descriptor);
    words.insert(words.end(), {0xc0031500u, groups, 1, 1, 0x41u | (wave == 32 ? 0x8000u : 0u)});
    return words;
}

void Label(std::vector<std::uint32_t>& words, std::uint32_t* target, std::uint32_t value) {
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    words.insert(words.end(), {0xc0064900u, 0x514u, (1u << 29u) | (2u << 24u), static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), value, 0, 0});
}

void Submit(const std::vector<std::uint32_t>& words) {
    Packet packet{const_cast<std::uint32_t*>(words.data()), static_cast<std::uint32_t>(words.size()), 0, {}};
    AgcDriver::Submit(&packet, 0);
    AgcDriverWaitIdle_nid_postfix();
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Output.data()), sizeof(Output));
}

void Expect(std::size_t first, std::size_t count, std::uint32_t value, const std::string& what) {
    for (std::size_t i = 0; i < Output.size(); ++i) {
        const auto expected = i >= first && i < first + count ? value : 0xcafebabeu;
        Require(Output[i] == expected, what + ": word " + std::to_string(i) + " is " + std::to_string(Output[i]) + ", expected " + std::to_string(expected));
    }
}

void ExecuteTests() {
    struct Case { std::uint32_t value, first, records, groups, valueBytes, stored; const char* what; };
    constexpr std::array<Case, 7> cases{{
        {0xffffffffu, 0, 256, 4, 16, 0xffffffffu, "a 16-byte aligned fill of every launched thread"},
        {0u, 0, 256, 2, 16, 0u, "a fill shorter than its records"},
        {0x40404040u, 0, 1000, 4, 16, 0x40404040u, "a fill longer than its launched threads"},
        {0x12345678u, 0, 255, 4, 16, 0x12345678u, "a fill whose size is not a multiple of 16 bytes"},
        {0x80808080u, 1, 256, 4, 16, 0x80808080u, "a fill whose start is not 16-byte aligned"},
        {0x20202020u, 0, 0, 4, 16, 0x20202020u, "an empty fill"},
        {0xc0c0c0c0u, 0, 256, 4, 0, 0u, "a fill whose value descriptor has no records (the load returns 0)"},
    }};
    for (const auto wave : {32u, 64u}) {
        Output.fill(0xcafebabeu);
        Value[0] = 0x12345678u;
        Submit(Commands(Output.data(), 256, 4, wave, sizeof(Value), 0x00027facu));
        Expect(0, 256, 0u, std::string("a store_format_x fill through a 10_11_11 descriptor stores no 32-bit value (wave ") + std::to_string(wave) + ")");
    }
    for (const auto wave : {32u, 64u}) {
        for (const auto& item : cases) {
            Output.fill(0xcafebabeu);
            Value[0] = item.value;
            Submit(Commands(Output.data() + item.first, item.records, item.groups, wave, item.valueBytes));
            Expect(item.first, std::min(item.records, item.groups * 64u), item.stored, std::string(item.what) + " (wave " + std::to_string(wave) + ")");
        }
    }
    struct ConstantCase { std::span<const std::uint32_t> code; std::uint32_t value, first, records, groups; const char* what; };
    const std::array<ConstantCase, 6> constants{{
        {ZeroCode, 0u, 0, 256, 4, "a zero fill of every launched thread"},
        {MinusOneCode, 0xffffffffu, 0, 256, 2, "a fill of -1 shorter than its records"},
        {OneCode, 1u, 0, 1000, 4, "a fill of 1 longer than its launched threads"},
        {SixtyFourCode, 64u, 0, 255, 4, "a fill of 64 whose size is not a multiple of 16 bytes"},
        {ZeroCode, 0u, 1, 256, 4, "a zero fill whose start is not 16-byte aligned"},
        {OneCode, 1u, 0, 0, 4, "an empty constant fill"},
    }};
    for (const auto wave : {32u, 64u}) {
        for (const auto& item : constants) {
            Output.fill(0xcafebabeu);
            Submit(ConstantCommands(item.code, Output.data() + item.first, item.records, item.groups, wave));
            Expect(item.first, std::min(item.records, item.groups * 64u), item.value, std::string(item.what) + " (wave " + std::to_string(wave) + ")");
        }
    }

    Output.fill(0xcafebabeu);
    Value[0] = 0x11111111u;
    auto words = Commands(Output.data(), 256, 4, 64);
    Label(words, &Value[0], 0xc0c0c0c0u);
    const auto again = Commands(Output.data(), 128, 2, 64);
    words.insert(words.end(), again.begin(), again.end());
    Submit(words);
    for (std::size_t i = 0; i < 256; ++i) {
        Require(Output[i] == (i < 128 ? 0xc0c0c0c0u : 0x11111111u), "a fill did not read the value a preceding GPU label stored, at word " + std::to_string(i));
    }
}

}

int main() {
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        ExecuteTests();
        AgcDriverShutdown_nid_postfix();
        std::puts("dword value fill tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
