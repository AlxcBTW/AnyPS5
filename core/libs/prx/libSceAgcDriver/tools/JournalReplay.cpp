#include "prx/libSceAgcDriver/Journal/include/Journal.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "Decoder/Png.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using AgcDriver::Journal::Frame;

void require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

#ifdef _WIN32
void requireFixedBase() {
    const auto* image = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    const auto relocatable = headers->OptionalHeader.DllCharacteristics & (IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE | IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA);
    require(relocatable == 0 && reinterpret_cast<std::uintptr_t>(image) == headers->OptionalHeader.ImageBase, "agc_journal_replay is not linked at a fixed base without high-entropy ASLR, so its own allocations can land on recorded guest addresses");
}
#endif

void applyEnvironment(const std::filesystem::path& journal) {
    for (const auto& variable : AgcDriver::Journal::ReplayEnvironment(journal)) {
#ifdef _WIN32
        const bool applied = _putenv_s(variable.name.c_str(), variable.value ? variable.value->c_str() : "") == 0;
#else
        const bool applied = (variable.value ? setenv(variable.name.c_str(), variable.value->c_str(), 1) : unsetenv(variable.name.c_str())) == 0;
#endif
        require(applied, "cannot set " + variable.name);
    }
}

void writePng(const std::filesystem::path& path, const Frame& frame) {
    std::vector<std::uint8_t> rgba(frame.bgra.size());
    for (std::size_t at = 0; at + 3 < rgba.size(); at += 4) {
        rgba[at] = static_cast<std::uint8_t>(frame.bgra[at + 2]);
        rgba[at + 1] = static_cast<std::uint8_t>(frame.bgra[at + 1]);
        rgba[at + 2] = static_cast<std::uint8_t>(frame.bgra[at]);
        rgba[at + 3] = static_cast<std::uint8_t>(frame.bgra[at + 3]);
    }
    const auto png = Decoder::Png::Encode(rgba, frame.width, frame.height, 4);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    file.close();
    require(file.good(), "cannot write " + path.string());
}

std::optional<std::string> divergence(const Frame& frame, const Frame& reference) {
    if (frame.width != reference.width || frame.height != reference.height) return "is " + std::to_string(frame.width) + "x" + std::to_string(frame.height) + ", the reference " + std::to_string(reference.width) + "x" + std::to_string(reference.height);
    for (std::size_t at = 0; at < frame.bgra.size(); at += 4) {
        if (std::equal(frame.bgra.begin() + static_cast<std::ptrdiff_t>(at), frame.bgra.begin() + static_cast<std::ptrdiff_t>(at + 4), reference.bgra.begin() + static_cast<std::ptrdiff_t>(at))) continue;
        const auto pixel = at / 4;
        const auto word = [](const std::vector<std::byte>& bytes, std::size_t offset) {
            char text[12];
            std::snprintf(text, sizeof(text), "%02x%02x%02x%02x", static_cast<unsigned>(bytes[offset + 2]), static_cast<unsigned>(bytes[offset + 1]), static_cast<unsigned>(bytes[offset]), static_cast<unsigned>(bytes[offset + 3]));
            return std::string(text);
        };
        return "differs first at pixel (" + std::to_string(pixel % frame.width) + ", " + std::to_string(pixel / frame.width) + "): RGBA " + word(frame.bgra, at) + ", the reference " + word(reference.bgra, at);
    }
    return std::nullopt;
}

}

int main(int argc, char** argv) {
    try {
#ifdef _WIN32
        requireFixedBase();
#endif
        std::optional<std::filesystem::path> frames;
        if (argc == 4 && std::string(argv[2]) == "--frames") frames = argv[3];
        if (argc != 2 && !frames) {
            std::fprintf(stderr, "usage: agc_journal_replay <journal directory> [--frames <directory>]\n");
            return 1;
        }
        applyEnvironment(argv[1]);
        const auto report = AgcDriver::Journal::Replay(argv[1]);
        AgcDriverShutdown_nid_postfix();
        if (frames) {
            std::filesystem::create_directories(*frames);
            for (std::size_t index = 0; index < report.frames.size(); ++index) {
                if (report.frames[index].width != 0) writePng(*frames / ("flip-" + std::to_string(index) + ".png"), report.frames[index]);
            }
        }
        bool diverged = report.frames.size() != report.references.size();
        if (diverged) std::printf("the replay presented %zu flips, the recording %zu\n", report.frames.size(), report.references.size());
        for (std::size_t index = 0; index < report.frames.size() && index < report.references.size(); ++index) {
            const auto difference = divergence(report.frames[index], report.references[index]);
            if (difference) std::printf("flip %zu %s\n", index, difference->c_str());
            diverged = diverged || difference.has_value();
        }
        std::printf("replayed %zu calls and %zu flips in %.1f ms; %s\n", report.calls, report.frames.size(), report.milliseconds, diverged ? "the replay diverged from the reference frames" : "every flip matches its reference frame");
        return diverged ? 2 : 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception& shutdown) {
            std::fprintf(stderr, "shutdown: %s\n", shutdown.what());
        }
        return 1;
    }
}
