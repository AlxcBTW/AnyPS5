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
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
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

struct Options {
    std::filesystem::path journal;
    std::optional<std::filesystem::path> frames;
    std::optional<std::filesystem::path> gpuTimes;
    std::optional<std::filesystem::path> dumpTargets;
    std::size_t repeat = 1;
};

Options parse(int argc, char** argv) {
    require(argc >= 2, "usage: agc_journal_replay <journal directory> [--frames <directory>] [--repeat <count>] [--gpu-times <json file>] [--dump-targets <directory>]");
    Options options;
    options.journal = std::filesystem::absolute(argv[1]);
    for (int index = 2; index < argc; ++index) {
        const std::string option = argv[index];
        require(index + 1 < argc, option + " needs a value");
        const char* value = argv[++index];
        if (option == "--frames") options.frames = std::filesystem::absolute(value);
        else if (option == "--gpu-times") options.gpuTimes = std::filesystem::absolute(value);
        else if (option == "--dump-targets") options.dumpTargets = std::filesystem::absolute(value);
        else if (option == "--repeat") options.repeat = static_cast<std::size_t>(std::stoul(value));
        else throw std::runtime_error("unknown option " + option);
    }
    require(options.repeat >= 1, "--repeat needs at least 1");
    return options;
}

void setVariable(const std::string& name, const char* value) {
#ifdef _WIN32
    const bool applied = _putenv_s(name.c_str(), value != nullptr ? value : "") == 0;
#else
    const bool applied = (value != nullptr ? setenv(name.c_str(), value, 1) : unsetenv(name.c_str())) == 0;
#endif
    require(applied, "cannot set " + name);
}

std::string hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

std::string shapeText(const AgcDriver::Journal::GpuRange& range) {
    if (range.kind == "draw") return std::to_string(range.shape[0]) + " vertices x" + std::to_string(range.shape[1]);
    if (range.shape == std::array<std::uint32_t, 3>{}) return range.kind == "dispatch-indirect" ? "indirect" : "";
    return std::to_string(range.shape[0]) + "x" + std::to_string(range.shape[1]) + "x" + std::to_string(range.shape[2]);
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    return values.size() % 2 == 1 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
}

double coveredMicroseconds(const std::vector<AgcDriver::Journal::GpuRange>& ranges) {
    std::vector<std::pair<double, double>> intervals;
    for (const auto& range : ranges) intervals.emplace_back(range.beginNs, range.endNs);
    std::sort(intervals.begin(), intervals.end());
    double covered = 0;
    double reached = 0;
    for (const auto& [begin, end] : intervals) {
        const auto from = std::max(begin, reached);
        if (end > from) covered += end - from;
        reached = std::max(reached, end);
    }
    return covered / 1000.0;
}

struct Group {
    std::string kind;
    std::uint32_t queue = 0;
    std::uint64_t program = 0;
    std::uint64_t vertexProgram = 0;
    std::vector<double> microseconds;
    std::vector<double> calls;
    std::map<std::string, std::uint64_t> shapes;
};

std::string jsonText(const std::string& text) {
    std::string escaped;
    for (const char character : text) {
        if (character == '"' || character == '\\') escaped += '\\';
        escaped += character;
    }
    return "\"" + escaped + "\"";
}

void reportGpuTimes(const AgcDriver::Journal::Report& report, const std::filesystem::path& journal, const std::filesystem::path& output) {
    const auto iterations = report.iterations.size();
    std::map<std::tuple<std::string, std::uint32_t, std::uint64_t, std::uint64_t>, Group> groups;
    std::vector<double> frames;
    std::vector<double> dropped;
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        const auto& ranges = report.iterations[iteration].gpuRanges;
        frames.push_back(coveredMicroseconds(ranges));
        dropped.push_back(static_cast<double>(report.iterations[iteration].droppedRanges));
        for (const auto& range : ranges) {
            auto& group = groups[{range.kind, range.queue, range.program, range.vertexProgram}];
            if (group.microseconds.empty()) group = Group{range.kind, range.queue, range.program, range.vertexProgram, std::vector<double>(iterations, 0.0), std::vector<double>(iterations, 0.0), {}};
            group.microseconds[iteration] += (range.endNs - range.beginNs) / 1000.0;
            group.calls[iteration] += 1;
            ++group.shapes[shapeText(range)];
        }
    }
    const auto frame = median(frames);
    std::vector<const Group*> sorted;
    for (const auto& [key, group] : groups) sorted.push_back(&group);
    std::sort(sorted.begin(), sorted.end(), [](const Group* a, const Group* b) { return median(a->microseconds) > median(b->microseconds); });
    std::ofstream json(output, std::ios::trunc);
    json << "{\n  \"journal\": " << jsonText(journal.string()) << ",\n  \"iterations\": " << iterations << ",\n  \"frameMicroseconds\": " << frame << ",\n  \"frameMicrosecondsPerIteration\": [";
    for (std::size_t index = 0; index < frames.size(); ++index) json << (index == 0 ? "" : ", ") << frames[index];
    json << "],\n  \"droppedRanges\": " << median(dropped) << ",\n  \"groups\": [\n";
    for (std::size_t index = 0; index < sorted.size(); ++index) {
        const auto& group = *sorted[index];
        const auto total = median(group.microseconds);
        const auto calls = median(group.calls);
        json << "    {\"kind\": " << jsonText(group.kind) << ", \"queue\": " << (group.queue == 0xffffffffu ? -1 : static_cast<long long>(group.queue)) << ", \"program\": " << jsonText(hex(group.program)) << ", \"vertexProgram\": " << jsonText(hex(group.vertexProgram)) << ", \"calls\": " << calls << ", \"totalMicroseconds\": " << total << ", \"microsecondsPerCall\": " << (calls != 0 ? total / calls : 0.0) << ", \"share\": " << (frame != 0 ? total / frame : 0.0) << ", \"shapes\": {";
        bool first = true;
        for (const auto& [shape, count] : group.shapes) {
            json << (first ? "" : ", ") << jsonText(shape) << ": " << static_cast<double>(count) / static_cast<double>(iterations);
            first = false;
        }
        json << "}}" << (index + 1 < sorted.size() ? "," : "") << "\n";
    }
    json << "  ]\n}\n";
    json.close();
    require(json.good(), "cannot write " + output.string());
    std::printf("GPU time per replayed frame: median %.1f us over %zu iterations (", frame, iterations);
    for (std::size_t index = 0; index < frames.size(); ++index) std::printf("%s%.1f", index == 0 ? "" : ", ", frames[index]);
    std::printf(" us), %.0f timed ranges dropped at the recorder's cap\n", median(dropped));
    std::printf("%-17s %-6s %-14s %-14s %7s %11s %9s %7s  %s\n", "kind", "queue", "program", "vertex", "calls", "total us", "us/call", "share", "grid / draw");
    for (std::size_t index = 0; index < sorted.size() && index < 30; ++index) {
        const auto& group = *sorted[index];
        const auto total = median(group.microseconds);
        const auto calls = median(group.calls);
        const auto shape = std::max_element(group.shapes.begin(), group.shapes.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        const auto queue = group.queue == 0xffffffffu ? std::string("-") : hex(group.queue);
        const auto program = group.program != 0 ? hex(group.program) : std::string("-");
        const auto vertex = group.vertexProgram != 0 ? hex(group.vertexProgram) : std::string("-");
        std::printf("%-17s %-6s %-14s %-14s %7.0f %11.1f %9.1f %6.1f%%  %s%s\n", group.kind.c_str(), queue.c_str(), program.c_str(), vertex.c_str(), calls, total, calls != 0 ? total / calls : 0.0, frame != 0 ? 100.0 * total / frame : 0.0, shape != group.shapes.end() ? shape->first.c_str() : "", group.shapes.size() > 1 ? " (varies)" : "");
    }
}

}

int main(int argc, char** argv) {
    try {
#ifdef _WIN32
        requireFixedBase();
#endif
        const auto options = parse(argc, argv);
        applyEnvironment(options.journal);
        if (options.gpuTimes) setVariable("APS5_PROFILE_GPU", "1");
        if (options.dumpTargets) {
            setVariable("APS5_DUMP_TARGETS", "1000000");
            std::filesystem::create_directories(*options.dumpTargets);
            std::filesystem::current_path(*options.dumpTargets);
        }
        const auto report = AgcDriver::Journal::Replay(options.journal, {options.repeat, options.gpuTimes.has_value()});
        AgcDriverShutdown_nid_postfix();
        const auto& first = report.iterations.front();
        if (options.frames) {
            std::filesystem::create_directories(*options.frames);
            for (std::size_t index = 0; index < first.frames.size(); ++index) {
                if (first.frames[index].width != 0) writePng(*options.frames / ("flip-" + std::to_string(index) + ".png"), first.frames[index]);
            }
        }
        bool diverged = false;
        for (std::size_t iteration = 0; iteration < report.iterations.size(); ++iteration) {
            const auto& frames = report.iterations[iteration].frames;
            bool differs = frames.size() != report.references.size();
            if (differs) std::printf("iteration %zu presented %zu flips, the recording %zu\n", iteration, frames.size(), report.references.size());
            for (std::size_t index = 0; index < frames.size() && index < report.references.size(); ++index) {
                const auto difference = divergence(frames[index], report.references[index]);
                if (difference) std::printf("iteration %zu: flip %zu %s\n", iteration, index, difference->c_str());
                differs = differs || difference.has_value();
            }
            std::printf("iteration %zu: replayed %zu calls and %zu flips in %.1f ms; %s\n", iteration, report.calls, frames.size(), report.iterations[iteration].milliseconds, differs ? "the replay diverged from the reference frames" : "every flip matches its reference frame");
            diverged = diverged || differs;
        }
        if (options.gpuTimes) reportGpuTimes(report, options.journal, *options.gpuTimes);
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
