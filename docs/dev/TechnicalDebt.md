# Project technical debt

Entries about one library or component are in [debt](debt/), one file per library or component, under the same sections as this page. Entries that belong to none stay here.

### Build

- The bundled [SPIRV-Tools build](../../core/shader/recompiler/PatchSpirvTools.cmake) replaces the redundancy-elimination source with a build-directory copy that releases processed traversal maps. Remove this patch when the pinned revision includes [SPIRV-Tools #6962](https://github.com/KhronosGroup/SPIRV-Tools/pull/6962). System-dependency builds need the fix in their installed SPIRV-Tools package.
- Building on Windows requires a specific version of mingw - MinGW-w64 GCC 15.2.0 (`winlibs-gcc15`, `x86_64-ucrt-posix-seh`)
- Even compiled prx libraries on Windows require nearby (static linking of these dependencies causes conflicts):
  - libgcc_s_seh-1.dll
  - libstdc++-6.dll
  - libwinpthread-1.dll

### Direct memory

- Windows direct-memory backing reserves a paging-file section and commits each mapped guest page before setting its protection. Committed physical contents remain in the section across unmap/remap and are released when the backing and its remaining views are destroyed; commitment is not recovered merely by unmapping a page. Linux retains its existing backing. Peak gameplay memory and Windows versions other than the tested host remain unverified.

### Silent stubs

Throughout the project, every function at every stage either **does exactly what it's supposed to or throws an exception**. Everywhere... except:

### Unknown function info

- AudioIn and NpSessionSignaling exports added without an implementation have assumed signatures
- `sceNetCtlGetIfStat` signature and 56-byte layout come from PPSA09030's call site and SharpProspero's `SceNetCtlIfStat`; the device kind is always 0, the byte counters are the host's totals over its up non-loopback hardware interfaces (not "since cleared"), and the not-connected result is assumed to match `sceNetCtlGetState`
- Font, Http2, Net, Ssl, SystemService and libkernel exports imported by PPSA12544 and added without an implementation have unknown signatures
- The Share, NpTrophy2, NpEntitlementAccess and WebBrowserDialog exports imported by PPSA12544's Unity plugins and added without an implementation have unknown signatures
- Acm, GameLiveStreaming, Net, NpEntitlementAccess, NpManager, NpWebApi2, VoiceQoS, WebBrowserDialog and libkernel exports imported by PPSA14632 and added without an implementation are declared without parameters

### Functional

- [Cancellation coverage](ARCHITECTURE.md#thread-cancellation) checks deferred request handling and recovery, not full asynchronous support or console ABI compatibility. Cancellation during file/socket I/O, sleep interruption, cancellation versus exit/detach, condition signal conservation under cancellation, and safe rejection of non-null invalid or reclaimed thread handles remain unverified. No console title or homebrew establishes the cleanup NIDs or buffer layout. Accepted asynchronous mode currently has no diagnostic for its deferred behavior.
- `--to-intel` guest module trampolines are covered only by a synthetic relinker test; no game title has been verified with them on Linux or Windows.
- [Vertex clip and cull distances](../../core/shader/recompiler/Translation/src/ShaderInputInfoBuilder.cpp) (libSceAgcDriver) - `PA_CL_VS_OUT_CNTL` enable bit `s` of `CLIP_DIST_ENA`/`CULL_DIST_ENA` is taken to select component `s` of the `pos1`/`pos2` exports, with the Vulkan array index counted by the enabled bits below it, and `VS_OUT_CCDIST0_VEC_ENA` as bit 22; this follows the recompiler's existing position-export mapping, not hardware or documentation
- [Layered color targets](../../core/libs/prx/libSceAgcDriver/Graphics/src/State.cpp) (libSceAgcDriver) - only 3D color targets with 64 KiB render-target or 4 KiB standard tiling render several slices: a `CB_COLOR_VIEW` slice range on an array target, a layered draw with a depth target, and color targets of different slice counts throw. The vertex layer export (`PA_CL_VS_OUT_CNTL` USE_VTX_RENDER_TARGET_INDX) reaches the shader only for a layered draw and is otherwise ignored, as before; a layer beyond the view's last slice is undefined in Vulkan, while the console's handling (clamping to `SLICE_MAX`) is not modeled, and a view whose last slice lies past the surface is clamped to its last slice
