#ifndef CORE_LIBS_HOTLEVER_HPP
#define CORE_LIBS_HOTLEVER_HPP

// Local test aid (never push): hot levers, so one run measures several settings in turn.
//
// With APS5_LEVER_FILE set, a running process re-reads that file of NAME=VALUE lines (blank lines and
// lines starting with '#' are skipped; a later line wins) and every lever read through this header
// takes the file's value from then on. A lever the file does not name has the value its environment
// variable gave at start, so an empty or missing file is the plain environment run. NAME is the
// variable with or without its APS5_EXP_ / APS5_LOCAL_ / APS5_ prefix (LEAN_DRAW = APS5_EXP_LEAN_DRAW).
// Values are integers, 0 is off. Names no lever reads (SEGMENT=..., notes) are only logged.
// Without APS5_LEVER_FILE nothing is read and each lever is its environment value for the whole run.
//
//     static const Aps5Hot::Lever lean("APS5_EXP_LEAN_DRAW", std::getenv("APS5_EXP_LEAN_DRAW") != nullptr);
//     if (lean.On()) ...                       // one relaxed atomic load
//     if (lean.Generation() != seen) flush();  // bumps on every change of the lever's value
//
// Header-only: each module that includes it keeps its own registry and polls the file itself
// (Aps5Hot::Poll, at most every 250 ms, re-read only when the file's time or size changed). The
// graphics driver polls on every present and logs each change as
// "[lever] t=<s since module load> present=<n> NAME=<value>". Writers replace the file whole (write a
// temporary file, then rename it over it); a read that does not end in a newline counts as torn and
// is retried at the next poll.
//
// A lever is only as hot as its readers: a value read once into a static stays as it was. Which
// levers are safe to flip mid-run, and what each flip flushes, is listed in anyps5-work/sots/hot-levers.md.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace Aps5Hot {

struct Slot {
    std::string name;
    std::string shortName;
    std::int64_t envValue = 0;
    std::atomic<std::int64_t> value{0};
    std::atomic<std::uint64_t> generation{0};
};

inline std::string ShortName(const std::string& name) {
    for (const char* prefix : {"APS5_EXP_", "APS5_LOCAL_", "APS5_"}) {
        const auto length = std::strlen(prefix);
        if (name.size() > length && name.compare(0, length, prefix) == 0) return name.substr(length);
    }
    return name;
}

class Registry {
public:
    static Registry& Get() {
        // Never destroyed: levers are read until the process ends.
        static Registry* registry = new Registry;
        return *registry;
    }

    bool Active() const { return active; }
    const std::string& Path() const { return path; }

    double Seconds() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - loaded).count(); }

    Slot* Register(const char* name, std::int64_t envValue) {
        std::lock_guard lock(mutex);
        for (auto* slot : slots) {
            if (slot->name == name) return slot;
        }
        auto* slot = new Slot;
        slot->name = name;
        slot->shortName = ShortName(slot->name);
        slot->envValue = envValue;
        std::int64_t value = envValue;
        if (const auto* text = fileValue(*slot)) value = std::strtoll(text->c_str(), nullptr, 0);
        slot->value.store(value, std::memory_order_relaxed);
        slots.push_back(slot);
        return slot;
    }

    // Re-reads the file when due. True when its contents changed (the levers then hold the new values).
    // `present` goes into the log line (< 0: not known here); `log` prints one line per changed name.
    bool Poll(std::int64_t present, bool log) {
        if (!active) return false;
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        auto due = nextPoll.load(std::memory_order_relaxed);
        if (now < due) return false;
        if (!nextPoll.compare_exchange_strong(due, now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(250)).count())) return false;
        std::lock_guard lock(mutex);
        std::error_code error;
        std::filesystem::file_time_type time{};
        std::uintmax_t size = 0;
        const bool exists = std::filesystem::exists(path, error) && !error;
        if (exists) {
            time = std::filesystem::last_write_time(path, error);
            if (!error) size = std::filesystem::file_size(path, error);
            if (error) return false;
        }
        if (polled && exists == lastExists && time == lastTime && size == lastSize) return false;
        std::map<std::string, std::string> values;
        if (exists) {
            std::ifstream file(path, std::ios::binary);
            if (!file) return false;
            std::stringstream text;
            text << file.rdbuf();
            const auto all = text.str();
            if (!all.empty() && all.back() != '\n') return false;
            std::istringstream lines(all);
            std::string line;
            while (std::getline(lines, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
                const auto first = line.find_first_not_of(" \t");
                if (first == std::string::npos || line[first] == '#') continue;
                const auto equals = line.find('=', first);
                if (equals == std::string::npos) continue;
                auto name = line.substr(first, equals - first);
                while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
                auto value = line.substr(equals + 1);
                const auto start = value.find_first_not_of(" \t");
                value = start == std::string::npos ? std::string{} : value.substr(start);
                if (!name.empty()) values[name] = value;
            }
        }
        polled = true;
        lastExists = exists;
        lastTime = time;
        lastSize = size;
        if (values == file) return false;
        const auto seconds = Seconds();
        std::vector<std::string> lines;
        const auto described = [&](const std::string& name, const std::string& value) {
            char head[96];
            if (present >= 0) std::snprintf(head, sizeof(head), "[lever] t=%.1f present=%lld ", seconds, static_cast<long long>(present));
            else std::snprintf(head, sizeof(head), "[lever] t=%.1f present=- ", seconds);
            return std::string(head) + name + "=" + value;
        };
        for (const auto& [name, value] : values) {
            const auto old = file.find(name);
            if (old == file.end() || old->second != value) lines.push_back(described(name, value));
        }
        for (const auto& [name, value] : file) {
            if (values.count(name) != 0) continue;
            const auto* slot = slotNamed(name);
            lines.push_back(described(name, slot != nullptr ? std::to_string(slot->envValue) + " (env)" : std::string("(removed)")));
        }
        file = std::move(values);
        for (auto* slot : slots) {
            std::int64_t value = slot->envValue;
            if (const auto* text = fileValue(*slot)) value = std::strtoll(text->c_str(), nullptr, 0);
            if (slot->value.load(std::memory_order_relaxed) == value) continue;
            slot->value.store(value, std::memory_order_relaxed);
            slot->generation.fetch_add(1, std::memory_order_release);
        }
        generation.fetch_add(1, std::memory_order_release);
        if (log) {
            for (const auto& line : lines) std::fprintf(stderr, "%s\n", line.c_str());
            std::fflush(stderr);
        }
        return true;
    }

    // The file's raw value for `name` (as written, short or full name), empty when absent.
    std::string FileValue(const std::string& name) {
        std::lock_guard lock(mutex);
        const auto found = file.find(name);
        if (found != file.end()) return found->second;
        const auto alternative = file.find(ShortName(name));
        return alternative != file.end() ? alternative->second : std::string{};
    }

    // Every registered lever as NAME=value (value) for the start line.
    std::string Describe() {
        std::lock_guard lock(mutex);
        std::string text;
        for (const auto* slot : slots) text += (text.empty() ? "" : " ") + slot->shortName + "=" + std::to_string(slot->value.load(std::memory_order_relaxed));
        return text;
    }

    std::uint64_t Generation() const { return generation.load(std::memory_order_acquire); }

private:
    Registry() : loaded(std::chrono::steady_clock::now()) {
        const char* text = std::getenv("APS5_LEVER_FILE");
        if (text != nullptr && *text != '\0') {
            path = text;
            active = true;
        }
    }

    const std::string* fileValue(const Slot& slot) const {
        auto found = file.find(slot.name);
        if (found == file.end()) found = file.find(slot.shortName);
        if (found == file.end() || found->second.empty()) return nullptr;
        return &found->second;
    }

    const Slot* slotNamed(const std::string& name) const {
        for (const auto* slot : slots) {
            if (slot->name == name || slot->shortName == name) return slot;
        }
        return nullptr;
    }

    std::mutex mutex;
    std::vector<Slot*> slots;
    std::map<std::string, std::string> file;
    std::string path;
    bool active = false;
    bool polled = false;
    bool lastExists = false;
    std::filesystem::file_time_type lastTime{};
    std::uintmax_t lastSize = 0;
    std::atomic<std::chrono::steady_clock::rep> nextPoll{0};
    std::atomic<std::uint64_t> generation{0};
    std::chrono::steady_clock::time_point loaded;
};

// Whether APS5_LEVER_FILE is set (constant for the run).
inline bool Active() {
    static const bool active = Registry::Get().Active();
    return active;
}

inline bool Poll(std::int64_t present = -1, bool log = false) { return Active() && Registry::Get().Poll(present, log); }

// One lever: define it once (a static) and read it where the old static read the environment.
class Lever {
public:
    Lever(const char* name, std::int64_t envValue) : slot(Registry::Get().Register(name, envValue)) {}
    std::int64_t Value() const { return slot->value.load(std::memory_order_relaxed); }
    bool On() const { return Value() != 0; }
    std::uint64_t Generation() const { return slot->generation.load(std::memory_order_acquire); }
    const std::string& Name() const { return slot->shortName; }

private:
    Slot* slot;
};

// The plain accessor: the live value of `name`, `envDefault` (the caller's environment value) until the
// lever file names it. A lookup under a mutex per call: hot paths keep a static Lever instead.
inline std::int64_t HotLever(const char* name, std::int64_t envDefault) {
    if (!Active()) return envDefault;
    return Registry::Get().Register(name, envDefault)->value.load(std::memory_order_relaxed);
}

}  // namespace Aps5Hot

#endif  // CORE_LIBS_HOTLEVER_HPP
