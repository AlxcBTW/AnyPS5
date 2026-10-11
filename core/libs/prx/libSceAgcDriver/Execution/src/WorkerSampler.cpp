#include "prx/libSceAgcDriver/Execution/include/WorkerSampler.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <stop_token>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>
#endif

namespace AgcDriver {

#ifdef _WIN32
namespace {

std::jthread processSamplerThread;
std::jthread workerSamplerThread;

// Frames per walk. The process sampler keeps its 12; the worker's walks go down to the thread's root,
// so that its stacks merge into one flame graph.
constexpr int kProcessDepth = 12;
constexpr int kWorkerDepth = 64;

// DWARF call frame information of one module. The libraries are built with
// -fno-asynchronous-unwind-tables: most of their functions have no .pdata entry, only a record in
// .ehfram (.eh_frame renamed), so RtlLookupFunctionEntry alone ends a walk at the first such frame.
struct CfiModule {
    struct Cie {
        const std::uint8_t* instructions;
        const std::uint8_t* end;
        std::uint64_t codeAlign;
        std::int64_t dataAlign;
        unsigned returnColumn;
        bool augmentationData;
    };
    struct Fde {
        std::uint64_t begin;
        std::uint64_t end;
        const std::uint8_t* instructions;
        const std::uint8_t* instructionsEnd;
        std::uint32_t cie;
    };
    std::uint64_t base = 0;
    std::uint64_t end = 0;
    std::vector<Cie> cies;
    std::vector<Fde> fdes;
};

std::uint64_t ReadUleb(const std::uint8_t*& p, const std::uint8_t* end) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; p < end && shift < 64; shift += 7) {
        const std::uint8_t byte = *p++;
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) break;
    }
    return value;
}

std::int64_t ReadSleb(const std::uint8_t*& p, const std::uint8_t* end) {
    std::uint64_t value = 0;
    unsigned shift = 0;
    std::uint8_t byte = 0;
    do {
        if (p >= end || shift >= 64) return static_cast<std::int64_t>(value);
        byte = *p++;
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        shift += 7;
    } while (byte & 0x80);
    if (shift < 64 && (byte & 0x40)) value |= ~std::uint64_t{0} << shift;
    return static_cast<std::int64_t>(value);
}

// A DW_EH_PE pointer: absolute or pc-relative, 4 or 8 bytes. Anything else is not read.
bool ReadEncoded(const std::uint8_t*& p, const std::uint8_t* end, std::uint8_t encoding, std::uint64_t& value) {
    const std::uint8_t* field = p;
    switch (encoding & 0x0f) {
    case 0x00: case 0x04: case 0x0c:
        if (end - p < 8) return false;
        std::memcpy(&value, p, 8);
        p += 8;
        break;
    case 0x03: {
        if (end - p < 4) return false;
        std::uint32_t v;
        std::memcpy(&v, p, 4);
        value = v;
        p += 4;
        break;
    }
    case 0x0b: {
        if (end - p < 4) return false;
        std::int32_t v;
        std::memcpy(&v, p, 4);
        value = static_cast<std::uint64_t>(static_cast<std::int64_t>(v));
        p += 4;
        break;
    }
    default: return false;
    }
    if ((encoding & 0x70) == 0x10) value += reinterpret_cast<std::uint64_t>(field);
    else if ((encoding & 0x70) != 0) return false;
    return true;
}

// Indexes the .ehfram records of the module at base (outside the suspended window: this allocates).
CfiModule IndexCfi(std::uint64_t base) {
    CfiModule module;
    module.base = base;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    module.end = base + nt->OptionalHeader.SizeOfImage;
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    const std::uint8_t* data = nullptr;
    const std::uint8_t* dataEnd = nullptr;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(sections[i].Name, ".ehfram", 8) != 0) continue;
        data = reinterpret_cast<const std::uint8_t*>(base + sections[i].VirtualAddress);
        dataEnd = data + sections[i].Misc.VirtualSize;
    }
    std::unordered_map<const std::uint8_t*, std::uint32_t> cieIndex;
    std::vector<std::uint8_t> cieEncoding;
    for (const std::uint8_t* p = data; p != nullptr && dataEnd - p >= 8;) {
        std::uint32_t length;
        std::memcpy(&length, p, 4);
        // The linker leaves the zero terminators of the merged objects' sections in between.
        if (length == 0) {
            p += 4;
            continue;
        }
        if (length == 0xffffffff) break;
        const std::uint8_t* record = p + 4;
        const std::uint8_t* next = record + length;
        if (length < 4 || next > dataEnd) break;
        std::uint32_t id;
        std::memcpy(&id, record, 4);
        if (id == 0) {
            const std::uint8_t* c = record + 4;
            const std::uint8_t version = *c++;
            const char* augmentation = reinterpret_cast<const char*>(c);
            while (c < next && *c != 0) ++c;
            ++c;
            if (version == 4) c += 2;
            CfiModule::Cie cie{};
            cie.codeAlign = ReadUleb(c, next);
            cie.dataAlign = ReadSleb(c, next);
            cie.returnColumn = version == 1 ? *c++ : static_cast<unsigned>(ReadUleb(c, next));
            std::uint8_t encoding = 0;
            bool usable = (version == 1 || version == 3 || version == 4) && cie.returnColumn <= 32;
            cie.augmentationData = augmentation[0] == 'z';
            if (usable && cie.augmentationData) {
                const std::uint64_t size = ReadUleb(c, next);
                const std::uint8_t* augmentationEnd = c + size;
                for (const char* a = augmentation + 1; usable && *a != 0; ++a) {
                    if (*a == 'R') encoding = *c++;
                    else if (*a == 'L') ++c;
                    else if (*a == 'P') {
                        const std::uint8_t personality = *c++;
                        std::uint64_t ignored;
                        usable = ReadEncoded(c, augmentationEnd, personality & 0x0f, ignored);
                    } else if (*a != 'S') usable = false;
                }
                c = augmentationEnd;
            } else if (augmentation[0] != 0) {
                usable = false;
            }
            if (usable && c <= next) {
                cie.instructions = c;
                cie.end = next;
                cieIndex.emplace(p, static_cast<std::uint32_t>(module.cies.size()));
                module.cies.push_back(cie);
                cieEncoding.push_back(encoding);
            }
        } else {
            const auto found = cieIndex.find(record - id);
            const std::uint8_t* f = record + 4;
            std::uint64_t begin = 0;
            std::uint64_t range = 0;
            if (found != cieIndex.end() && ReadEncoded(f, next, cieEncoding[found->second], begin) &&
                ReadEncoded(f, next, cieEncoding[found->second] & 0x0f, range)) {
                // The FDE's augmentation data (the LSDA pointer) is skipped by its length.
                if (module.cies[found->second].augmentationData) {
                    const std::uint64_t size = ReadUleb(f, next);
                    f += size;
                }
                if (f <= next) module.fdes.push_back({begin, begin + range, f, next, found->second});
            }
        }
        p = next;
    }
    std::sort(module.fdes.begin(), module.fdes.end(), [](const auto& a, const auto& b) { return a.begin < b.begin; });
    return module;
}

struct CfiRule {
    std::uint8_t kind = 0; // 0 unchanged, 1 saved at CFA + value, 2 in register value, 3 is CFA + value, 4 unknown
    std::int64_t value = 0;
};

struct CfiState {
    std::array<CfiRule, 33> rules{};
    unsigned cfaRegister = 7;
    std::int64_t cfaOffset = 0;
    bool cfaExpression = false;
};

// Runs call frame instructions up to target; false on anything a GCC frame does not use.
bool RunCfi(const std::uint8_t* p, const std::uint8_t* end, const CfiModule::Cie& cie, std::uint64_t location, std::uint64_t target, CfiState& state, const CfiState& initial) {
    std::array<CfiState, 4> saved;
    unsigned depth = 0;
    auto set = [&](std::uint64_t reg, CfiRule rule) {
        if (reg < state.rules.size()) state.rules[reg] = rule;
    };
    while (p < end && location <= target) {
        const std::uint8_t opcode = *p++;
        switch (opcode & 0xc0) {
        case 0x40: location += (opcode & 0x3f) * cie.codeAlign; continue;
        case 0x80: set(opcode & 0x3f, {1, static_cast<std::int64_t>(ReadUleb(p, end)) * cie.dataAlign}); continue;
        case 0xc0: set(opcode & 0x3f, initial.rules[opcode & 0x3f]); continue;
        default: break;
        }
        switch (opcode) {
        case 0x00: break;
        case 0x02: location += *p++ * cie.codeAlign; break;
        case 0x03: { std::uint16_t delta; std::memcpy(&delta, p, 2); p += 2; location += delta * cie.codeAlign; break; }
        case 0x04: { std::uint32_t delta; std::memcpy(&delta, p, 4); p += 4; location += delta * cie.codeAlign; break; }
        case 0x05: { const auto reg = ReadUleb(p, end); set(reg, {1, static_cast<std::int64_t>(ReadUleb(p, end)) * cie.dataAlign}); break; }
        case 0x11: { const auto reg = ReadUleb(p, end); set(reg, {1, ReadSleb(p, end) * cie.dataAlign}); break; }
        case 0x14: { const auto reg = ReadUleb(p, end); set(reg, {3, static_cast<std::int64_t>(ReadUleb(p, end)) * cie.dataAlign}); break; }
        case 0x15: { const auto reg = ReadUleb(p, end); set(reg, {3, ReadSleb(p, end) * cie.dataAlign}); break; }
        case 0x06: { const auto reg = ReadUleb(p, end); if (reg < state.rules.size()) state.rules[reg] = initial.rules[reg]; break; }
        case 0x07: set(ReadUleb(p, end), {4, 0}); break;
        case 0x08: set(ReadUleb(p, end), {0, 0}); break;
        case 0x09: { const auto reg = ReadUleb(p, end); set(reg, {2, static_cast<std::int64_t>(ReadUleb(p, end))}); break; }
        case 0x0a: if (depth == saved.size()) return false; saved[depth++] = state; break;
        case 0x0b: if (depth == 0) return false; state = saved[--depth]; break;
        case 0x0c: state.cfaRegister = static_cast<unsigned>(ReadUleb(p, end)); state.cfaOffset = static_cast<std::int64_t>(ReadUleb(p, end)); state.cfaExpression = false; break;
        case 0x12: state.cfaRegister = static_cast<unsigned>(ReadUleb(p, end)); state.cfaOffset = ReadSleb(p, end) * cie.dataAlign; state.cfaExpression = false; break;
        case 0x0d: state.cfaRegister = static_cast<unsigned>(ReadUleb(p, end)); break;
        case 0x0e: state.cfaOffset = static_cast<std::int64_t>(ReadUleb(p, end)); break;
        case 0x13: state.cfaOffset = ReadSleb(p, end) * cie.dataAlign; break;
        case 0x0f: { state.cfaExpression = true; const auto size = ReadUleb(p, end); p += size; break; }
        case 0x10: case 0x16: { const auto reg = ReadUleb(p, end); const auto size = ReadUleb(p, end); p += size; set(reg, {4, 0}); break; }
        case 0x2e: ReadUleb(p, end); break;
        default: return false;
        }
    }
    return true;
}

DWORD64* Register(CONTEXT& context, unsigned dwarf) {
    switch (dwarf) {
    case 0: return &context.Rax;
    case 1: return &context.Rdx;
    case 2: return &context.Rcx;
    case 3: return &context.Rbx;
    case 4: return &context.Rsi;
    case 5: return &context.Rdi;
    case 6: return &context.Rbp;
    case 7: return &context.Rsp;
    case 8: return &context.R8;
    case 9: return &context.R9;
    case 10: return &context.R10;
    case 11: return &context.R11;
    case 12: return &context.R12;
    case 13: return &context.R13;
    case 14: return &context.R14;
    case 15: return &context.R15;
    default: return nullptr;
    }
}

// Debug aid: APS5_SAMPLE_WORKER=<file> samples the calling thread every millisecond (in practice every
// timer tick, ~16 ms) and, every 20 s, writes "module+offset self inclusive" lines to <file> for offline
// symbolization with nm. The worker sampler also writes its whole stacks to <file stem>.collapsed.txt
// (one "frame;...;leaf count" line per distinct stack, root first, frames "module+0xRVA", the loaded
// modules in "# module" lines) and keeps each 20 s state as <file stem>.collapsed.t<seconds>.txt;
// lab/flame.py symbolizes them.
struct Sampler {
    HANDLE target = nullptr;
    std::string path;
    int depth = kProcessDepth;
    bool keepStacks = false;
    // The target's stack; set, the walk also steps through frames that only have DWARF unwind data.
    std::uint64_t stackLow = 0;
    std::uint64_t stackHigh = 0;
    std::vector<CfiModule> cfiModules;
    std::uint64_t pendingModule = 0;
    std::mutex mutex;
    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> counts;
    // Whole walks, leaf first; a trailing 0 marks a walk that did not reach the thread's root.
    std::map<std::vector<std::uint64_t>, std::uint64_t> stacks;
    std::uint64_t samples = 0;
    std::uint64_t truncated = 0;
    std::int64_t suspendedTicks = 0;
    std::int64_t maxSuspendedTicks = 0;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    ~Sampler() noexcept(false) {
        if (target != nullptr && !CloseHandle(target)) throw std::system_error(GetLastError(), std::system_category(), "Closing sampler thread handle");
    }

    bool readStack(std::uint64_t address, std::uint64_t& value) const {
        if (address < stackLow || address > stackHigh - 8) return false;
        std::memcpy(&value, reinterpret_cast<const void*>(address), 8);
        return true;
    }

    enum class Cfi { Stepped, NoRecord, Failed };

    // The DWARF record covering pc, nullptr without one; *module is null when pc lies in no indexed module.
    const CfiModule::Fde* findFde(std::uint64_t pc, const CfiModule** module) const {
        const auto at = std::upper_bound(cfiModules.begin(), cfiModules.end(), pc, [](std::uint64_t value, const CfiModule& m) { return value < m.base; });
        *module = at != cfiModules.begin() && pc < std::prev(at)->end ? &*std::prev(at) : nullptr;
        if (*module == nullptr) return nullptr;
        const auto& fdes = (*module)->fdes;
        const auto fde = std::upper_bound(fdes.begin(), fdes.end(), pc, [](std::uint64_t value, const CfiModule::Fde& f) { return value < f.begin; });
        return fde != fdes.begin() && pc < std::prev(fde)->end ? &*std::prev(fde) : nullptr;
    }

    bool hasUnwindData(std::uint64_t pc) const {
        DWORD64 imageBase = 0;
        const CfiModule* module = nullptr;
        return RtlLookupFunctionEntry(pc, &imageBase, nullptr) != nullptr || findFde(pc - 1, &module) != nullptr;
    }

    // One frame through its DWARF record: the CFA from its rule, then the return address and the
    // registers the function saved, all read from inside the target's stack. Runs while the target is
    // suspended, so it neither allocates nor takes locks; a module seen for the first time is indexed
    // after the target resumes, and this walk ends here.
    Cfi stepCfi(CONTEXT& context, bool leaf) {
        const std::uint64_t pc = context.Rip - (leaf ? 0 : 1);
        const CfiModule* module = nullptr;
        const CfiModule::Fde* fde = findFde(pc, &module);
        if (module == nullptr) {
            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQuery(reinterpret_cast<LPCVOID>(pc), &memory, sizeof(memory)) != 0 && memory.Type == MEM_IMAGE) pendingModule = reinterpret_cast<std::uint64_t>(memory.AllocationBase);
            return Cfi::Failed;
        }
        if (fde == nullptr) return Cfi::NoRecord;
        const auto& record = *fde;
        const auto& cie = module->cies[record.cie];
        CfiState initial;
        if (!RunCfi(cie.instructions, cie.end, cie, 0, ~std::uint64_t{0}, initial, initial)) return Cfi::Failed;
        // MinGW x86-64 keeps the return address in column 32, which the functions' own records also use
        // for xmm15's save slot: the CIE's rule moves to column 16 before they run.
        unsigned returnColumn = cie.returnColumn;
        if (returnColumn == 32) {
            initial.rules[16] = initial.rules[32];
            initial.rules[32] = {};
            returnColumn = 16;
        }
        CfiState state = initial;
        if (!RunCfi(record.instructions, record.instructionsEnd, cie, record.begin, pc, state, initial)) return Cfi::Failed;
        const DWORD64* cfaBase = Register(context, state.cfaRegister);
        if (state.cfaExpression || cfaBase == nullptr) return Cfi::Failed;
        const std::uint64_t cfa = *cfaBase + state.cfaOffset;
        const auto& returnRule = state.rules[returnColumn];
        std::uint64_t returnAddress = 0;
        if (cfa <= context.Rsp || returnRule.kind != 1 || !readStack(cfa + returnRule.value, returnAddress)) return Cfi::Failed;
        CONTEXT next = context;
        for (unsigned reg = 0; reg < 16; ++reg) {
            const auto& rule = state.rules[reg];
            DWORD64* destination = Register(next, reg);
            std::uint64_t value = 0;
            if (rule.kind == 1) {
                if (!readStack(cfa + rule.value, value)) return Cfi::Failed;
                *destination = value;
            } else if (rule.kind == 2) {
                const DWORD64* source = Register(context, static_cast<unsigned>(rule.value));
                if (source == nullptr) return Cfi::Failed;
                *destination = *source;
            } else if (rule.kind == 3) {
                *destination = cfa + rule.value;
            }
        }
        next.Rsp = cfa;
        next.Rip = returnAddress;
        context = next;
        return Cfi::Stepped;
    }

    void sample() {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        // A fixed array: no heap use while the target is suspended, it may hold the heap lock.
        std::array<std::uint64_t, kWorkerDepth> frames;
        int count = 0;
        LARGE_INTEGER before{};
        LARGE_INTEGER after{};
        QueryPerformanceCounter(&before);
        if (SuspendThread(target) == static_cast<DWORD>(-1)) return;
        const bool captured = GetThreadContext(target, &context) != 0;
        if (captured) {
            // Unwinding reads the target's stack while it is suspended: .pdata frames through
            // RtlVirtualUnwind and, with the stack bounds known, the others through their DWARF record. A
            // leaf function without either keeps its return address at Rsp; deeper frames without unwind
            // data (JIT/guest code) end the walk.
            const bool dwarf = stackHigh != 0 && context.Rsp >= stackLow && context.Rsp < stackHigh;
            while (count < depth && context.Rip != 0) {
                frames[count++] = context.Rip;
                DWORD64 imageBase = 0;
                if (auto* entry = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr)) {
                    PVOID handler = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, entry, &context, &handler, &establisher, nullptr);
                    continue;
                }
                if (!dwarf) break;
                const Cfi cfi = stepCfi(context, count == 1);
                if (cfi == Cfi::Stepped) continue;
                std::uint64_t returnAddress = 0;
                if (cfi == Cfi::Failed || count != 1 || !readStack(context.Rsp, returnAddress) || !hasUnwindData(returnAddress)) break;
                context.Rip = returnAddress;
                context.Rsp += 8;
            }
        }
        ResumeThread(target);
        QueryPerformanceCounter(&after);
        if (pendingModule != 0) {
            const auto at = std::upper_bound(cfiModules.begin(), cfiModules.end(), pendingModule, [](std::uint64_t value, const CfiModule& m) { return value < m.base; });
            cfiModules.insert(at, IndexCfi(pendingModule));
            pendingModule = 0;
        }
        std::lock_guard lock(mutex);
        ++samples;
        const std::int64_t suspended = after.QuadPart - before.QuadPart;
        suspendedTicks += suspended;
        maxSuspendedTicks = std::max(maxSuspendedTicks, suspended);
        for (int i = 0; i < count; ++i) {
            auto& entry = counts[frames[i]];
            if (i == 0) ++entry.first;
            ++entry.second;
        }
        if (keepStacks && count != 0) {
            std::vector<std::uint64_t> stack(frames.begin(), frames.begin() + count);
            if (context.Rip != 0) {
                stack.push_back(0);
                ++truncated;
            }
            ++stacks[std::move(stack)];
        }
    }

    void write() {
        std::lock_guard lock(mutex);
        std::FILE* file = std::fopen(path.c_str(), "w");
        if (file == nullptr) return;
        std::fprintf(file, "# %llu samples\n", static_cast<unsigned long long>(samples));
        for (const auto& [address, count] : counts) {
            HMODULE module = nullptr;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module)) GetModuleFileNameA(module, name, sizeof(name));
            const char* base = std::strrchr(name, '\\');
            std::fprintf(file, "%s 0x%llx %llu %llu\n", base ? base + 1 : name, static_cast<unsigned long long>(address - reinterpret_cast<std::uint64_t>(module)), static_cast<unsigned long long>(count.first), static_cast<unsigned long long>(count.second));
        }
        std::fclose(file);
        if (keepStacks) writeStacks();
    }

    // <file stem>.collapsed.txt, replaced atomically, and a copy named after the seconds since the start.
    void writeStacks() {
        const std::size_t dot = path.rfind('.');
        const std::size_t slash = path.find_last_of("\\/");
        const std::string stem = dot != std::string::npos && (slash == std::string::npos || dot > slash) ? path.substr(0, dot) : path;
        const std::string collapsed = stem + ".collapsed.txt";
        const std::string temporary = collapsed + ".tmp";
        std::FILE* file = std::fopen(temporary.c_str(), "w");
        if (file == nullptr) return;
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        const double tickUs = 1e6 / static_cast<double>(frequency.QuadPart);
        std::fprintf(file, "# samples=%llu seconds=%lld depth=%d truncated=%llu suspended_us_mean=%.1f suspended_us_max=%.1f\n", static_cast<unsigned long long>(samples), static_cast<long long>(seconds), depth,
            static_cast<unsigned long long>(truncated), samples != 0 ? static_cast<double>(suspendedTicks) * tickUs / static_cast<double>(samples) : 0.0, static_cast<double>(maxSuspendedTicks) * tickUs);
        // Every sampled address is in counts: name each one once, and list the modules they lie in.
        std::unordered_map<std::uint64_t, std::string> names;
        std::map<std::uint64_t, std::string> modules;
        names.reserve(counts.size());
        for (const auto& [address, count] : counts) {
            HMODULE module = nullptr;
            char name[MAX_PATH] = "?";
            char frame[MAX_PATH + 32];
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module) && GetModuleFileNameA(module, name, sizeof(name)) != 0) {
                const char* base = std::strrchr(name, '\\');
                std::snprintf(frame, sizeof(frame), "%s+0x%llx", base ? base + 1 : name, static_cast<unsigned long long>(address - reinterpret_cast<std::uint64_t>(module)));
                modules.emplace(reinterpret_cast<std::uint64_t>(module), name);
            } else {
                std::snprintf(frame, sizeof(frame), "?+0x%llx", static_cast<unsigned long long>(address));
            }
            names.emplace(address, frame);
        }
        // "# module base size timestamp path": the image as loaded, to check the file symbolized against it.
        for (const auto& [base, name] : modules) {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            std::fprintf(file, "# module 0x%llx 0x%lx 0x%lx %s\n", static_cast<unsigned long long>(base), static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage), static_cast<unsigned long>(nt->FileHeader.TimeDateStamp), name.c_str());
        }
        for (const auto& [stack, count] : stacks) {
            for (auto frame = stack.rbegin(); frame != stack.rend(); ++frame) {
                std::fputs(*frame == 0 ? "[truncated]" : names[*frame].c_str(), file);
                std::fputc(std::next(frame) == stack.rend() ? ' ' : ';', file);
            }
            std::fprintf(file, "%llu\n", static_cast<unsigned long long>(count));
        }
        if (std::fclose(file) != 0 || !MoveFileExA(temporary.c_str(), collapsed.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
        CopyFileA(collapsed.c_str(), (stem + ".collapsed.t" + std::to_string(seconds) + ".txt").c_str(), FALSE);
    }
};

// Debug aid: APS5_SAMPLE_THREADS=<file> samples every thread in the process every 2 ms and, every
// 20 s, writes each busy thread's hottest frames ("module+offset self inclusive") to <file>.
struct ProcessSampler {
    std::string path;
    DWORD self = 0;
    std::map<DWORD, Sampler> threads;
    std::uint64_t rounds = 0;

    std::chrono::steady_clock::time_point enumerated{};

    // The system-wide thread snapshot is slow (tens of ms); the thread list is refreshed every 5 s and
    // the rounds in between only sample the known threads.
    void enumerate() {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        const DWORD process = GetCurrentProcessId();
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
            auto& thread = threads[entry.th32ThreadID];
            if (thread.target == nullptr) thread.target = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        }
        CloseHandle(snapshot);
        enumerated = std::chrono::steady_clock::now();
    }

    void sample() {
        if (threads.empty() || std::chrono::steady_clock::now() - enumerated > std::chrono::seconds(5)) enumerate();
        for (auto& [id, thread] : threads) {
            if (thread.target != nullptr) thread.sample();
        }
        ++rounds;
    }

    void write() {
        std::FILE* file = std::fopen(path.c_str(), "w");
        if (file == nullptr) return;
        std::fprintf(file, "# %llu rounds\n", static_cast<unsigned long long>(rounds));
        for (auto& [id, thread] : threads) {
            std::lock_guard lock(thread.mutex);
            std::vector<std::pair<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>>> hot(thread.counts.begin(), thread.counts.end());
            std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
            std::uint64_t leaves = 0;
            for (const auto& [address, count] : hot) leaves += count.first;
            if (leaves == 0) continue;
            std::fprintf(file, "thread %lu samples %llu\n", static_cast<unsigned long>(id), static_cast<unsigned long long>(thread.samples));
            for (std::size_t i = 0; i < hot.size() && i < 40; ++i) {
                const auto address = hot[i].first;
                HMODULE module = nullptr;
                char name[MAX_PATH] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module)) GetModuleFileNameA(module, name, sizeof(name));
                const char* base = std::strrchr(name, '\\');
                std::fprintf(file, "  %s 0x%llx %llu %llu\n", base ? base + 1 : name, static_cast<unsigned long long>(address - reinterpret_cast<std::uint64_t>(module)), static_cast<unsigned long long>(hot[i].second.first), static_cast<unsigned long long>(hot[i].second.second));
            }
        }
        std::fclose(file);
    }
};

void StartProcessSampler() {
    const char* path = std::getenv("APS5_SAMPLE_THREADS");
    if (path == nullptr) return;
    processSamplerThread = std::jthread([path = std::string(path)](std::stop_token token) {
        auto sampler = std::make_unique<ProcessSampler>();
        sampler->path = path;
        sampler->self = GetCurrentThreadId();
        auto flushed = std::chrono::steady_clock::now();
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            sampler->sample();
            if (std::chrono::steady_clock::now() - flushed > std::chrono::seconds(20)) {
                sampler->write();
                flushed = std::chrono::steady_clock::now();
            }
        }
        sampler->write();
    });
}

}

void StartWorkerSampler() {
    StartProcessSampler();
    const char* path = std::getenv("APS5_SAMPLE_WORKER");
    if (path == nullptr) return;
    auto sampler = std::make_unique<Sampler>();
    sampler->path = path;
    sampler->depth = kWorkerDepth;
    sampler->keepStacks = true;
    // This thread's stack bounds: every read the DWARF steps make stays inside them.
    const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    MEMORY_BASIC_INFORMATION stack{};
    if (VirtualQuery(tib->StackLimit, &stack, sizeof(stack)) != 0) {
        sampler->stackLow = reinterpret_cast<std::uint64_t>(stack.AllocationBase);
        sampler->stackHigh = reinterpret_cast<std::uint64_t>(tib->StackBase);
    }
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sampler->target, THREAD_ALL_ACCESS, FALSE, 0)) throw std::system_error(GetLastError(), std::system_category(), "Duplicating sampler thread handle");
    workerSamplerThread = std::jthread([sampler = std::move(sampler)](std::stop_token token) {
        auto flushed = std::chrono::steady_clock::now();
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            sampler->sample();
            if (std::chrono::steady_clock::now() - flushed > std::chrono::seconds(20)) {
                sampler->write();
                flushed = std::chrono::steady_clock::now();
            }
        }
        sampler->write();
    });
}

void StopWorkerSampler() {
    processSamplerThread.request_stop();
    workerSamplerThread.request_stop();
    if (processSamplerThread.joinable()) processSamplerThread.join();
    if (workerSamplerThread.joinable()) workerSamplerThread.join();
}
#else
void StartWorkerSampler() {}
void StopWorkerSampler() {}
#endif

}
