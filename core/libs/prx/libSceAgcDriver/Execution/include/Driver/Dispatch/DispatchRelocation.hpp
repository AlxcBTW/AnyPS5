#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DISPATCH_DISPATCHRELOCATION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DISPATCH_DISPATCHRELOCATION_HPP

#include "Recompiler.hpp"
#include "UserDataUse.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ShaderRecompiler {
struct DescriptorValue;
}

// Address-free dispatch-cache entries (APS5_NO_DISPATCH_CONTENT_KEY=1 turns them off).
//
// A dispatch-cache variant stores the SRT words a capture walked, at the addresses it walked them.
// UE5 allocates both the tables (a user-data pointer into a per-frame ring) and many of the buffers
// they name (V# base addresses) from rings, so the same content comes back every frame at new
// addresses and the cache never hits. A relocation profile, built at insert from the capture,
// records which of the stored words only carry an address:
//   - pointer user words: user-data words the walk used only as the base of raw reads (the
//     SrtUserDataUse asBase bits without asValue); the runs read through them are kept relative;
//   - V# base fields: word 0 bits 2..31 (the low 32 address bits; word 1 with the high bits and
//     the stride stays exact) of a buffer descriptor that the walk copied verbatim from one place
//     (a run or the user data) into the snapshot, that no other descriptor shares, that no walked
//     run lies inside, and that the compiled result copies only where it can be patched.
// The content key leaves exactly those bits out and hashes the words behind the pointers instead.
// A hit validates every other word by value at the rebased addresses, then patches the live
// addresses into a copy of the compiled result (the guest-buffer elements that copy a V#, the
// flattened-SRT, shader-data and push-constant words that copy a base, the shader-data slots that
// forward a user word).
namespace AgcDriver::DriverDetail {

inline constexpr std::uint32_t BufferBaseLowMask = 0xfffffffcu;
inline constexpr std::uint32_t PointerWordMask = 0xffffffffu;
// Relocated dispatch-cache hits since start (tests).
extern std::atomic<std::uint64_t> relocatedDispatchHits;
// Bound on the words a content key hashes behind the pointer words.
inline constexpr std::size_t ContentKeyMaxWords = 256;

struct AddressField {
    std::uint32_t position = 0;
    std::uint32_t mask = 0;

    bool operator==(const AddressField& other) const = default;
};

// One word of the compiled result that copies a relocatable V# base (word 0 of a guest-buffer
// element, or a flattened-SRT, shader-data or push-constant word): binding index (PushConstants for
// a push dword), the word in it, and the source of the base (a position among the variant's words,
// or a user-data word).
struct ElementPatch {
    static constexpr std::uint32_t PushConstants = 0xffffffffu;
    std::uint32_t binding = 0;
    std::uint32_t word = 0;
    std::uint32_t source = 0;
    bool user = false;

    bool operator==(const ElementPatch& other) const = default;
};

// Why FindBufferFields keeps a buffer V# exact: the program reads raw addresses (BDA), a null V#,
// no verbatim source, more than one source, a source inside a pointer word, a walked run inside
// the buffer, a built descriptor inside it, (unused), the base in an image, sampler or other element.
enum class FieldRefusal : std::uint8_t { Bda, NotPresent, NoSource, Sources, Pointer, Walked, Derived, Flattened, Copied };
inline constexpr std::size_t FieldRefusalCount = 9;

struct RelocationProfile {
    // User words whose runs are relative to them (a run's base is in DispatchVariant::runBases).
    std::uint64_t pointerWords = 0;
    // The user data the variant was captured with.
    std::vector<std::uint32_t> userData;
    // Masked bits among the variant's words and among the user words, sorted by position.
    std::vector<AddressField> wordFields;
    std::vector<AddressField> userFields;
    // The positions of wordFields (sorted), for the masked compare.
    std::vector<std::uint32_t> maskPositions;
    std::vector<ElementPatch> patches;
    // The compiled result's shader-data slots that forward user words (RecompileResult::shaderDataUserWords).
    std::vector<std::uint32_t> forwarded;
    std::uint32_t relocatableBuffers = 0;
    // Buffer V#s kept exact, by FieldRefusal.
    std::array<std::uint32_t, FieldRefusalCount> refused{};

    [[nodiscard]] bool Empty() const { return pointerWords == 0 && wordFields.empty() && userFields.empty(); }
};

// What a content key reads behind the pointer words, learned once per dispatch identity.
struct DispatchShape {
    // Per user word, the bits the key leaves out (all of a pointer word).
    std::vector<std::uint32_t> userMasks;
    struct Read {
        std::uint32_t word = 0;
        std::int64_t offset = 0;
        std::vector<std::uint32_t> masks;
    };
    std::vector<Read> reads;
};

inline std::uint64_t AlignedPointer(std::uint32_t word) {
    return word & ~std::uint32_t{3};
}

// The runs of a capture's SRT regions, split where the origin changes: a run is relative to user
// word i (runBases i) when the walk read each of its words through user word i, absolute (-1)
// otherwise. `candidates` are the user words that may be relative (asBase without asValue); a
// word drops out when one of its reads is unaligned, outside the regions, or aliases a read of
// another origin. Returns the user words some run stayed relative to. `words` is filled with the
// regions' words in order (the positions the masks name).
std::uint64_t SplitRelativeRuns(std::span<const ShaderRecompiler::MemoryRegion> regions, const ShaderRecompiler::SrtUserDataUse& use, std::uint64_t candidates, std::vector<std::pair<std::uint64_t, std::uint64_t>>& runs, std::vector<std::int8_t>& runBases, std::vector<std::uint32_t>& words);

// The V# base fields and the element patches of a profile (see the file comment): `runs`/`words`
// are the variant's, `buffers` the capture's buffer descriptors,
// `compiled` the result materialized from it. Fills profile.wordFields, userFields,
// maskPositions, patches and relocatableBuffers.
void FindBufferFields(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::uint32_t> words, std::span<const std::uint32_t> userData, std::span<const ShaderRecompiler::DescriptorValue> buffers, const ShaderRecompiler::RecompileResult& compiled, RelocationProfile& profile);

// The dispatch shape a profile implies (empty userMasks when there is nothing to leave out).
DispatchShape ShapeOf(const RelocationProfile& profile, std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::int8_t> runBases);

// The content key of a dispatch: `identity` (everything but the user data), the user words with
// their address bits left out, and the words behind the pointer words (address bits left out)
// read through `read` (address, out) -> readable.
template <typename Reader>
std::uint64_t DispatchContentKey(std::uint64_t identity, const DispatchShape& shape, std::span<const std::uint32_t> userData, Reader&& read) {
    std::uint64_t key = 0x84222325cbf29ce4ull ^ identity;
    const auto mix = [&](std::uint64_t value) {
        key ^= value;
        key *= 0x100000001b3ull;
    };
    mix(userData.size());
    for (std::size_t i = 0; i < userData.size(); ++i) {
        const auto mask = i < shape.userMasks.size() ? shape.userMasks[i] : 0u;
        mix(userData[i] & ~mask);
        if (mask == PointerWordMask) mix(userData[i] & 3u);
    }
    std::uint32_t buffer[64];
    std::size_t budget = ContentKeyMaxWords;
    for (const auto& reading : shape.reads) {
        if (budget == 0) break;
        const auto count = std::min({reading.masks.size(), std::size(buffer), budget});
        budget -= count;
        if (reading.word >= userData.size() || userData[reading.word] == 0 || !read(AlignedPointer(userData[reading.word]) + static_cast<std::uint64_t>(reading.offset), std::span<std::uint32_t>(buffer, count))) {
            mix(0xdeadull);
            continue;
        }
        for (std::size_t i = 0; i < count; ++i) mix(buffer[i] & ~reading.masks[i]);
    }
    return key;
}

// Whether the live user words fit a profile: pointer words keep their low two bits and stay
// non-null, masked fields agree outside the mask and stay non-null, every other word is equal.
bool UserWordsFit(const RelocationProfile& profile, std::span<const std::uint32_t> live);

// Whether the live values of masked words (position, value) fit: equal outside the mask, the V#
// base non-null. `stored` are the variant's words.
bool FieldsFit(const RelocationProfile& profile, std::span<const std::uint32_t> stored, std::span<const std::pair<std::uint32_t, std::uint32_t>> live);

// Patches the live addresses into a copy of the compiled result: `liveWords` are the variant's
// words with the live values at the masked positions, `liveUser` the live user words.
void ApplyRelocation(ShaderRecompiler::RecompileResult& result, const RelocationProfile& profile, std::span<const std::uint32_t> liveWords, std::span<const std::uint32_t> liveUser);

// Whether two materializations bind the same: bindings (role, count, descriptor words), push
// constants, variant and specialization. `why` names the first difference.
bool SameMaterialization(const ShaderRecompiler::RecompileResult& a, const ShaderRecompiler::RecompileResult& b, std::string& why);

}

#endif
