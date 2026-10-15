#include <string>
#include <optional>
#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAW_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAW_HPP

#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/Recipe.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <array>
#include <memory>
#include <vector>

namespace AgcDriver::Graphics {

struct HostImport;

// `recipe`, when given, receives the DrawRecipe a recorded, cacheable, reusable, non-indirect draw
// built for its draw-cache entry (design_cpu_final M8; null otherwise, and always under
// APS5_NO_DRAW_RECIPE=1).
void Draw(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots = {}, std::shared_ptr<const DrawRecipe>* recipe = nullptr);
std::optional<std::string> KnownValidationFailure(const Context& context, std::span<const CompiledShader> shaders, const State& state);
std::uint64_t DrawRenderPassKey(const Context& context, const State& state, std::span<const VkImageView> targetViews);

struct DrawInputCopy {
    std::shared_ptr<Buffer> buffer;
    bool reused = false;
    std::uint32_t derived = 0;
    std::uint64_t generation = 0;
    std::uint64_t registryGeneration = 0;
};
DrawInputCopy CopyDrawInput(const Context& context, Recorder* recorder, std::uint64_t address, std::size_t bytes, std::size_t alignment, Recorder::SnapshotUse use);
DrawInputCopy CopyZeroPaddedDrawInput(const Context& context, std::uint64_t address, std::size_t bytes, std::size_t validBytes);
void KeepDrawInput(Recorder* recorder, std::uint64_t address, const DrawInputCopy& copy, Recorder::SnapshotUse use, std::uint32_t derived);
const HostImport* InPlaceDrawInput(const Context& context, std::uint64_t address, std::size_t bytes, std::size_t alignment);
struct InPlaceIndices {
    std::optional<std::uint32_t> highest;
    bool reused = false;
};
InPlaceIndices InPlaceIndexRange(Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint32_t indexSize, bool skipRestart);

std::array<std::uint32_t, 4> MeshIndexBufferDescriptor(const Pm4::DrawParameters& draw);

struct MeshArguments {
    std::uint32_t groups;
    std::uint32_t instances;
    std::uint32_t layers;
    std::uint32_t indexCount;
    std::uint32_t firstIndex;
};
static_assert(sizeof(MeshArguments) == ShaderRecompiler::MeshArgumentBytes);
// The rules that turn one indirect record into MeshArguments, on the GPU (MeshArguments.comp, the
// eight words in this order) or on the CPU (ResolveMeshArguments). `indexed` is 0 for a non-indexed
// draw (DRAW_INDIRECT 0x24): its record's start vertex goes only to the SH register the packet
// names (the driver keeps a draw that names one on the CPU path), never into the generated vertex
// indices, so it is ignored and the vertex count is unbounded (no index buffer clamps it); the
// indices start at the draw's vertex constant (GE_INDX_OFFSET plus the folded offset), exactly as
// the CPU path's direct draw per record does.
struct MeshArgumentRules {
    std::uint32_t indexCount;
    std::uint32_t inputSize;
    std::uint32_t step;
    std::uint32_t primitivesPerGroup;
    std::uint32_t maxGroups;
    std::uint32_t maxInstances;
    std::uint32_t maxTotal;
    std::uint32_t indexed;
};
MeshArgumentRules MeshArgumentRulesFor(const Context& context, const ShaderRecompiler::MeshConfiguration& mesh, std::uint32_t indexCount, bool indexed = true);
MeshArguments ResolveMeshArguments(const Pm4::DrawArguments& record, const MeshArgumentRules& rules);
// Whether single-record non-indexed mesh indirect draws without SGPR locations take the GPU-side
// path (MeshArguments pass + vkCmdDrawMeshTasksIndirectEXT) like the indexed ones, so queue 0
// never waits for their GPU-written records (APS5_NO_GPU_MESH_INDIRECT=1: the CPU loop as before;
// read once per process).
bool GpuMeshIndirectDraws();

// Why DrawWithRecipe did not record from the recipe (the caller then runs Draw): the draw is not
// recordable (no recorder, APS5_SYNC_DRAWS, APS5_DUMP_TARGETS), a resident target is gone from the
// storage cache, the template is gone, the pipeline is gone from the pipeline store, or the
// template's proof failed (ShaderResources::ProveCurrent; the template is removed from the
// resource cache and kept by the batch).
enum class DrawRecipeMiss : std::uint8_t { None, NotRecordable, TargetGone, TemplateGone, ObjectsGone, Proof, Count };
const char* DrawRecipeMissName(DrawRecipeMiss miss);
struct DrawRecipeOutcome {
    bool recorded = false;
    DrawRecipeMiss miss = DrawRecipeMiss::None;
    ShaderResources::ProofReport proof;
    // APS5_PROFILE_DRAW: the proof's and the record's time.
    double proofUs = 0;
    double recordUs = 0;
};
// A draw-cache hit recorded from its recipe, under GuestMemory::GpuMutex after the packet's labels
// (design_cpu_final M8, step 8b): the validation with the recipe's stored outputs, the index and
// vertex work as Draw's, the resident-target proof on the stored images (Refresh while Cached()),
// the template's proof (ProveCurrent) with the alias checks repeated, the pipeline and framebuffer
// from the recipe, then the record as Draw's (pass continued or begun, Kept, MarkGpuWrites). A
// miss records nothing.
DrawRecipeOutcome DrawWithRecipe(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots, const DrawRecipe& recipe);
// Whether recorded draws build recipes and draw-cache hits use them (APS5_NO_DRAW_RECIPE unset).
bool DrawRecipes();

// How an indirect draw's records were read, for the [draws] line (APS5_PROFILE_DRAW): Gpu is
// vkCmdDrawIndirect from the host import; every other value names why the CPU read them instead,
// either decided by the driver before the draw (its patched-SGPR fold rules, the shader path, the
// draw index, the vertex ranges, the device features, APS5_NO_GPU_INDIRECT_DRAW) or under the device
// lock in Draw (the memory state of the records). `readMs` is the CPU read's time (its sync);
// `rewritten` counts a GPU-side draw whose records were copied and patched with a constant.
enum class IndirectDrawPath : std::uint8_t { Gpu, NotFolded, FetchUnknown, NonVertexPath, IndxOffset, DrawIndex, VertexRange, FeatureGap, PendingImage, PendingLabelOrCopy, NotImported, Disabled, Count };
void CountIndirectDraw(IndirectDrawPath path, double readMs, bool rewritten = false);
const char* IndirectDrawPathName(IndirectDrawPath path);

// Lab switch APS5_GPU_INDEXED_INDIRECT=1 (default off, read once per process): rect-list indirect
// draws (DRAW_INDIRECT 0x24, DRAW_INDEX_INDIRECT 0x25 and the multi / counted 0x2c, 0x38) take the
// GPU-side path (vkCmd[Draw|DrawIndexed]Indirect[Count] from the host import, queue 0 never reads
// their records) like vertex-path draws. A rect list is a patch-list draw of generated stages that
// read no draw count, so its records are consumed exactly as a vertex-path draw's, with the same
// per-dimension rules. =verify also reads the records on the CPU when the draw is recorded and
// again when its batch completed and logs a mismatch. Tessellation, geometry and located mesh
// draws keep the CPU path.
bool GpuIndexedIndirect();
bool GpuIndexedIndirectVerify();

// Where an indirect packet's records were consumed, for the [gpuind] line (every 10 s when
// APS5_GPU_INDEXED_INDIRECT or APS5_PROFILE_DRAW is set): GpuSide (recorded, not read), EarlySkip
// (CPU read before the stage captures), CpuLoop (CPU read, the loop of direct draws),
// RecordFallback (the memory state sent a recorded draw to the CPU read inside Draw) and Peek (the
// final-record read of a GPU-side non-indexed mesh draw). `path` is why the CPU read them.
enum class IndirectSite : std::uint8_t { GpuSide, EarlySkip, CpuLoop, RecordFallback, Peek };
void CountIndirectPacket(IndirectSite site, const State& state, const Pm4::DrawParameters::IndirectDraw& indirect, IndirectDrawPath path, double readMs);

// The FlattenedSrt words draws filled (CompiledShader::srtFills) since the previous call: by the
// path that served each (Gpu: a copy from the host import before the draw; PendingImage,
// PendingLabelOrCopy, NotImported: a CPU read through the flush hook, as the capture would have
// made), `notRecorded` of them read on the CPU because the draw was not a lean recorded one, and
// the draws that had any.
struct SrtFillCounts {
    std::array<std::uint64_t, static_cast<std::size_t>(IndirectDrawPath::Count)> words{};
    std::uint64_t notRecorded = 0;
    std::uint64_t draws = 0;
};
SrtFillCounts TakeSrtFillCounts();

// A FlattenedSrt word a lean recorded draw copies from the host import before its pass (Draw's SRT
// fills): `destination`+`offset` is the stage's FlattenedSrt buffer word, `import` covers `address`.
struct SrtFillCopy {
    VkBuffer destination;
    VkDeviceSize offset;
    const HostImport* import;
    std::uint64_t address;
};
// Records the copies outside any render pass: a barrier after everything recorded before (the
// words' writers), one 4-byte copy per fill, a barrier before every later shader read; with a
// recorder, each source word is noted as read in place by the open batch.
void RecordSrtFills(const Context& context, VkCommandBuffer commands, Recorder* recorder, std::span<const SrtFillCopy> fills);

// Draw packets that drew nothing, for the [draws] line: Nothing (an empty count, or no color writes
// and no pixel shader), Prechecked (State.hpp's DrawRejection refused it before the decode) and
// Thrown (the decode or the build threw) and EmptyIndirect (a CPU-side indirect draw whose records
// draw nothing, ended before its stage captures); `us` is the packet's time in the driver.
// FinalEmptyIndirect: a GPU-side non-indexed mesh draw whose record was already final in guest
// memory, read without a wait after the captures and found empty (nothing recorded); `us` is the
// read's time.
enum class DrawSkip : std::uint8_t { Nothing, Prechecked, Thrown, EmptyIndirect, FinalEmptyIndirect, Count };
void CountDrawSkip(DrawSkip kind, double us);

// Recorded draws whose written guest buffers were copied (their results reach guest memory by a CPU
// write-back when their batch completes), listed until that write-back ran. It is the draw
// counterpart of VulkanDevice's State::copiedWriters for dispatches, which VulkanDevice.cpp consults
// in two places: DispatchIndirect (an indirect dispatch whose arguments such a writer produced reads
// them on the CPU, behind the write-back, instead of from the host import on the GPU) and the fill
// HLE (a vkCmdFillBuffer over a range a listed write-back will land on must wait for it, or the
// write-back overwrites the fill). Draws are listed here only under APS5_RECORD_COPIED_DRAWS=1,
// which is safe only once BOTH of those checks also test this list, i.e. each extends its
// `std::any_of(state->copiedWriters ...)` with
// `|| std::any_of(Graphics::DrawCopiedWriters()->begin(), Graphics::DrawCopiedWriters()->end(), [&](const auto& writer) { return writer->WritesOverlap(<its range>); })`;
// cleaner still, State::copiedWriters can become this list (one registry for both producers), after
// which the switch can turn into APS5_NO_RECORD_COPIED_DRAWS. Until then such draws are recorded and
// waited for at once. Read and written under GuestMemory::GpuMutex, like the device's list.
std::shared_ptr<std::vector<std::shared_ptr<ShaderResources>>> DrawCopiedWriters();

void RunColorMetadataPass(const Context& context, const ColorMetadataPass& pass);

}

#endif
