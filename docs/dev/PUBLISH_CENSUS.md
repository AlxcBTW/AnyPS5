# Publish census (lab, `APS5_PUBLISH_CENSUS=1`)

Counters only; the default path is unchanged and every hook is a no-op when the switch is unset
(it also needs the unit shadows on). Paths below are under `core/libs/prx/libSceAgcDriver/`.

## The publish path

The "shadow-publish" command class (`Recorder.hpp` `CommandClass::ShadowPublish`) is
`recordCopies` in `Graphics/src/UnitShadow.cpp` (one `vkCmdCopyBuffer` slab -> import per slab, two
barriers). It is reached through `publishUnits` / `publishRange` from:

- `PublishShadow` (`UnitShadow.cpp`), called with a scope (Whole: a reader; PartialUnits: a writer, only
  partly covered units) and a reason (`PublishReason`),
- `evictOne` (slab LRU, reason Evict), `RetireShadow` (import retired, Retire),
  `PublishAllShadows` (Teardown; `Execution/src/VulkanDevice.cpp:845,2108`).

A unit becomes publishable when `MarkShadowed` (`UnitShadow.cpp`, from the write-back retile in
`Graphics/src/Texture.cpp` ~4701) stamps it; `published[unit]` marks it published. The in-place
buffer flush is `StorageTexture::FlushPending` (`Graphics/src/Texture.cpp:3256`), which stores
pending images into the shadow and then calls `PublishShadow` with the caller's scope (None = no
publish).

Flushes of buffer regions (build, reuse, revalidate), the places APS5_YOLO_GPU removes:

- build, region bound in place: `Graphics/src/GuestBufferMemory.cpp:4205-4212` (YOLO test, flush at 4212)
- build, written range outside a staged region: `GuestBufferMemory.cpp:4021` (YOLO test at 4019)
- build, copy path: `GuestBufferMemory.cpp:4235,4239` (copied region), 4246 (CPU copy of a written range)
- reuse, epoch revalidate: `Graphics/src/ShaderResources.cpp:3033-3038`
- reuse, plain revalidate: `ShaderResources.cpp:3056-3066` (regions at 3060, written ranges at 3066)
- staged-from-shadow edges: `GuestBufferMemory.cpp:3750-3760` (`publishEdges`), `ShaderResources.cpp` `PublishShadowSourcedEdges`
- BDA / address-based builds after copy-backs: `GuestBufferMemory.cpp:3473` (`copyBackBdaPublish`)

## Observer sites (what needs the published bytes in the import)

Reads by the host (CPU), through `GuestMemory::FlushGpuWrites` -> `FlushForAccess`
(`Graphics/src/Recorder.cpp:1005`, publish at 1050-1055; `GuestMemory::Read` family at
`Execution/src/GuestMemory.cpp:2064-2211`). Scanout is the same hook with `ReadSite::Scanout`
(`Execution/src/DisplayBuffer.cpp:251,269`). Other `FlushGpuWrites` callers: `VulkanDevice.cpp:3014`,
`Draw.cpp:900,916,933,1096` (index/vertex data), `GuestBufferMemory.cpp:1597`, `Texture.cpp:2121,2147`,
`DepthSurface.cpp:104`, `MultisampleTarget.cpp:318`, `ShaderMemory.cpp:192`, `ShaderRegistry.cpp:78`,
`VariantValidation.cpp:56`.

Explicit observers, by `PublishReason`:

- Indirect arguments: `Execution/src/VulkanDevice.cpp:4422` (dispatch), `Graphics/src/Draw.cpp:2108` (draw)
- Label / packet stores (writers, PartialUnits): `Execution/src/Driver/Packets/PacketMemory.cpp:195,388`,
  `VulkanDevice.cpp:2227` (label store), `2287` (sample dump), `2284` (occlusion counter dump)
- PM4 copies: `PacketMemory.cpp:227`, `VulkanDevice.cpp:2668` (source), `2669` (destination),
  `Graphics/src/Texture.cpp:4288` (destination image), `Execution/src/Driver/Memory/BufferCopy.cpp:175` (copy count)
- Fills: `Execution/src/Driver/Memory/BufferFill.cpp:334,350`, `Graphics/src/Texture.cpp:4168` (fill-clear)
- Key / metadata reads (DCC, CMASK): `Graphics/src/DccMetadata.cpp:252,354,501`, `Draw.cpp:207,258`
- Dispatch-cache / capture validation: `VariantValidation.cpp:100`, `Driver/Memory/CaptureValidation.cpp:166`
- Storage uploads / refresh (read the slab themselves when fresh; publish only the mixed rest):
  `Texture.cpp:2307`, `4755`, `1776` (refresh, scope None), `ShadowSources` readers
  (`Texture.cpp:2600,2710`, `GuestBufferMemory.cpp:3707,4415,4607`), `UnitShadow.cpp` tail publish
- Buffer regions bound in place or copied (the GPU reads the import): `GuestBufferMemory.cpp:4195,4212,4235,4639,4664`,
  `ShaderResources.cpp:674` (sampled texture), `3038,3060,3066`
- Eviction and retire: `UnitShadow.cpp` `evictOne`, `RetireShadow` (called from `GuestBufferMemory.cpp:249`)
- Decisions on "is it shadowed" without a publish: `AnyShadowedOverlaps` in `WriteEvidence.cpp:148`,
  `VariantValidation.cpp:98,107`, `VulkanDevice.cpp:2202,2570,2715`, `ShaderResources.cpp:4545,4594`,
  `Texture.cpp:4567`, `DccMetadata.cpp:757`, `GuestBufferMemory.cpp:3145`.

The census hooks every observer through three chokepoints: `FlushPending` and `PublishShadowsOnly`
(`Texture.cpp`, by the reason string), `PublishShadow` itself, and a `PublishCensusObserve` line before
each `AnyShadowedOverlaps`-gated publish (listed above).

## Output (every 10 s, by the first census call after the interval)

```
[publish-census] (10 s) P publishes, U units / X MiB published: needed ... , wasted ... (rewritten by the GPU unread, superseded by another writer unread, whole-unit writer), bound in place only ..., lapsed on evict/retire ...; still open ...; first need after N presents
[publish-census] needed by observer (units/MiB): hook .. region .. indirect .. label ..; hook by read site (MiB): ..
[publish-census] by YOLO class (MiB): other {...} yolo-skippable {...} yolo-kept {...}
[publish-census] APS5_YOLO_GPU off (skips are potential), regions flushed in place that YOLO skips vs keeps: build-region skip n/MiB keep n/MiB; ...
```

A published unit is closed exactly once: needed (a definite observer read it first), rewritten (the
next `MarkShadowed` on the unit came before any read), superseded (a tracker stamp newer than the
unit's generation came before any read; detected at the next observer or in the open scan), writer
(a PartialUnits writer covered it whole), bound-only (only a GPU bind in place met it, which may be
write-only) or lapsed (evicted or retired). "YOLO class": skippable = published by a flush that
APS5_YOLO_GPU skips (a written region bound in place), kept = by one it keeps.

## Not seen by the census (risks for publish-on-demand)

- Native guest loads. The flush hook only runs for `GuestMemory::Read`-style accesses made by the
  runtime; guest code reading GPU-written memory directly (plain loads) never reaches it. Publish-on-
  demand must prove no such reader exists for a range, or keep publishing ranges the title can load
  from natively.
- CPU stores to a published unit are stamped only when a collect walks the page; before that the unit
  looks unchanged (counted as needed instead of superseded).
- GPU in-place binds are counted as bound-only; whether the shader read the bytes is unknown.
