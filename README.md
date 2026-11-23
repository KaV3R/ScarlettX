# ScarlettX — large-title bring-up fork

This is a fork of [**Freminet/ScarlettX**](https://github.com/Freminet/ScarlettX), an experimental, clean-room
high-level emulator (HLE) / compatibility layer for **Xbox Series X|S (Scarlett, GDK)** x86-64 titles on Windows.
All credit for the emulator itself goes to the upstream author. The original project README is kept unchanged
in [`UPSTREAM_README.md`](UPSTREAM_README.md).

This fork records an attempt to bring up a **large, recent AAA retail title** (Rockstar Games' *Grand Theft Auto VI*,
Xbox Series build 1.0.2.1, GDK device SDK version `0x20004`) on ScarlettX, and the emulator features that had to be
added along the way. The title does **not** run: it now gets through most of its memory and graphics
initialisation, and stops at a fundamental barrier — its shaders contain native console GPU code instead of
DXIL bitcode (see [Where it stops](#where-it-stops)).

Everything here is generic emulator code. Nothing from any game (binaries, data, shaders, disassembly dumps) is
included in this repository.

> **Legal notice:** as upstream, this fork contains no proprietary Microsoft/Xbox binaries, keys, firmware or
> game assets, and it cannot decrypt anything. It only runs titles that are already unencrypted and extracted.
> Use it only with software you legally own.

---

## Contents

- [Status at a glance](#status-at-a-glance)
- [What was added](#what-was-added)
  - [Xbox physical page memory (`xmem`)](#1-xbox-physical-page-memory-xmem)
  - [Unified memory: GPU access to title memory](#2-unified-memory-gpu-access-to-title-memory)
  - [Descriptor heaps placed at title addresses](#3-descriptor-heaps-placed-at-title-addresses)
  - [New / fixed `d3d12_xs` device and command-list slots](#4-new--fixed-d3d12_xs-device-and-command-list-slots)
  - [PC D3D12 validity fixes](#5-pc-d3d12-validity-fixes)
  - [Diagnostics](#6-diagnostics)
- [Bring-up log: how far the title gets](#bring-up-log-how-far-the-title-gets)
- [Where it stops](#where-it-stops)
- [Assumptions and known shortcuts](#assumptions-and-known-shortcuts)
- [Building](#building)
- [Running and environment variables](#running-and-environment-variables)
- [Changed files](#changed-files)

---

## Status at a glance

| Area | Before this fork | Now |
| :--- | :--- | :--- |
| `xmem.dll` physical page API | stubs (title asserts on first call) | implemented on host placeholders + pagefile sections |
| `MEM_RESERVE_PLACEHOLDER` / `MEM_REPLACE_PLACEHOLDER` via `XMemVirtualAlloc` | passed to `VirtualAlloc` → fails | routed to `VirtualAlloc2`, tracked |
| `PAGE_GRAPHICS_*` protections | passed through → `VirtualAlloc` fails | translated to host protections |
| GPU buffers over title memory | separate host copy only | same physical pages as the CPU (placed on the backing section) |
| Title GPU virtual addresses in CBVs / root views / IA views | passed through (invalid on PC) | translated; GPU views of title memory created on demand |
| Descriptor heaps at a title-chosen address | host address returned → title asserts | title address returned, all handles translated |
| Command-list vtable | 96 slots (title calls slots 131/132 → jumps through unrelated memory) | 256 slots, unknown slots logged |
| Device vtable | 128 slots (title calls slot 133 → null call) | 256 slots, slots 61/74/86/127/133 implemented |
| Device removal (`DXGI_ERROR_INVALID_CALL`) during init | yes | fixed (CBV size, >2 GB resources, oversized UAV) |
| Title shaders | — | **not loadable**: DXIL containers with native GPU code (`XDAT`) instead of bitcode |

The upstream test suite (26 tests) still passes with all changes.

---

## What was added

### 1. Xbox physical page memory (`xmem`)

New file: [`src/shims/xmem_physical.cpp`](src/shims/xmem_physical.cpp) (exported from `xgameruntime.dll`, to which
`xmem.dll` is redirected).

The console `xmem.dll` exposes an AWE-style API: a title allocates 64 KB *physical pages*, gets page frame numbers
back, and maps them into address ranges it reserved itself — often several of them, and often the same pages
twice. The signatures were recovered from a retail title's call sites (they are not public):

```c
BOOL  XMemAllocatePhysicalPages(ULONG flags, ULONG_PTR* count, ULONG_PTR* pageArray);
BOOL  XMemFreePhysicalPages(ULONG_PTR count, ULONG_PTR* pageArray);
void* XMemMapPhysicalPages(void* va, ULONG_PTR count, ULONG_PTR* pageArray);      // pageArray == NULL unmaps
BOOL  XMemMapPhysicalPagesScatter(void** vas, ULONG_PTR count, ULONG_PTR* pageArray);
BOOL  XMemGetWorkingSetStatistics(ULONG flags, void* stats);
BOOL  XMemIsToolsMemAvailable(void);
BOOL  XMemTransferMemory(ULONG direction, SIZE_T size);
```

Implementation:

- Every `XMemAllocatePhysicalPages` call is backed by one pagefile-backed section (`CreateFileMapping`,
  `SEC_COMMIT`). A PFN indexes a table of *(section, offset)*.
- 64 KB / 2 MB reservations made through `XMemVirtualAlloc` become **host placeholders** (`VirtualAlloc2` +
  `MEM_RESERVE_PLACEHOLDER`). A page is mapped by splitting a 64 KB placeholder out of the range and replacing it
  with a view of the right section offset (`MapViewOfFile3` + `MEM_REPLACE_PLACEHOLDER`). Unmapping puts the
  placeholder back (`UnmapViewOfFile2` + `MEM_PRESERVE_PLACEHOLDER`). Commits into such a range are handled the
  same way (`VirtualAlloc2` + `MEM_REPLACE_PLACEHOLDER`).
- Titles also build these layouts *themselves*: `XMemVirtualAlloc(NULL, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER)`,
  split it with `VirtualFree(MEM_PRESERVE_PLACEHOLDER)`, then turn parts into reservations with
  `XMemVirtualAlloc(part, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER)`. The first now goes through `VirtualAlloc2`; the
  second keeps the part a host placeholder (so pages can still be mapped into it) and adopts it as an XMem
  reservation.
- **Contiguity-preserving slot assignment.** A page only gets its offset in the section when it is first mapped.
  If it is mapped inside a title placeholder reservation, it gets the slot matching its position in that
  reservation. A title that maps one allocation's pages over a reservation of the same size in several pieces, in
  any order, therefore ends up with one contiguous run in the section — which is what makes a single GPU resource
  over it possible (next section).
- `PAGE_GRAPHICS_*` protections (`0x800`–`0x40000`, sometimes OR'ed with a plain `PAGE_*`) are translated to host
  protections in `XMemVirtualAlloc`.

Two helper exports (not console API) let `d3d12_xs.dll` ask what backs a range:
`ScxXMemQueryBacking` / `ScxXMemQueryBackingSparse` (runs of *section, offset, va, length*) and `ScxXMemRegionOf`.

Set `SCARLETTX_XMEM_TRACE=1` to log reservations, allocations and mappings.

### 2. Unified memory: GPU access to title memory

On the console, CPU and GPU share one address space: a buffer's GPU virtual address *is* its CPU address, and
titles write constant data, vertices, etc. through plain pointers and hand the same pointers to the GPU.

- **Device slot 133 (`+0x428`) — `CreatePlacedResourceX(address, desc, initialState, clearValue, riid, ppv)`**:
  a resource created at a title address. For buffers over physical-page memory the backing section is opened as a
  D3D12 heap (`OpenExistingHeapFromFileMapping`) and the buffer is placed at the matching section offset, so the GPU
  reads exactly what the CPU wrote. Other cases fall back to `OpenExistingHeapFromAddress` (only when the host
  allocation is large enough) and finally to a separate committed resource.
- **GPU virtual address translation.** Title addresses are translated to host GPU virtual addresses in
  `CreateConstantBufferView`, `Set{Graphics,Compute}Root{ConstantBuffer,ShaderResource,UnorderedAccess}View`,
  `IASetVertexBuffers` and `IASetIndexBuffer`.
- **On-demand GPU views.** An address that is not inside a known buffer is resolved lazily: plain title
  allocations (`MEM_PRIVATE`) are wrapped with `OpenExistingHeapFromAddress`; physical-page views (`MEM_MAPPED`)
  get one placed buffer per contiguous section run of their XMem reservation.
- Ranges over 1 GB are covered by **1 GB buffers every 512 MB** (D3D12 resources are limited to 2 GB; views on
  larger ones remove the device), so any view of up to 512 MB lies inside one buffer.
- Device slot 74 (`+0x250`) — `CreateHeapFromPhysicalPagesX(address, numPages64K, ppHeap)` returns a heap proxy
  of the right size (like `CreateHeap` in this layer, it does not alias the title memory).

Tiled (reserved) resources were tried for ranges made of several section runs, but `UpdateTileMappings` does not
accept cross-adapter (file-mapping) heaps, so that path was removed.

### 3. Descriptor heaps placed at title addresses

The console `D3D12_DESCRIPTOR_HEAP_DESC` carries two extra fields: a placement address (`+0x10`) and a size in
bytes (`+0x18`). The title then requires `GetCPUDescriptorHandleForHeapStart()` to return *that* address.

- Such heaps are registered with their title range; `GetCPU/GPUDescriptorHandleForHeapStart` return the title
  address.
- Every incoming descriptor handle is translated to the host heap: `Create{CBV,SRV,UAV,RTV,DSV,Sampler}`,
  `CopyDescriptors`, `CopyDescriptorsSimple`, `OMSetRenderTargets`, `Clear{RenderTarget,DepthStencil}View`,
  `Set{Graphics,Compute}RootDescriptorTable`.
- Shader-visible CBV/SRV/UAV heaps larger than the PC limit (the title asks for 1,245,184 descriptors) are clamped
  to 1,000,000 (samplers to 2,048).

### 4. New / fixed `d3d12_xs` device and command-list slots

| Interface | Slot (offset) | Meaning (as identified) | Status |
| :--- | :--- | :--- | :--- |
| Device | 61 (`+0x1e8`) | create sampler from an 0x54-byte console sampler desc | implemented; layout partly inferred, fields used only when valid |
| Device | 74 (`+0x250`) | heap over physical-page memory | implemented (proxy only) |
| Device | 86 (`+0x2b0`) | `ID3D12Device2::CreatePipelineState` (stream) — identified by `IID_ID3D12PipelineState` | implemented: stream walked, root signatures replaced, passed to host |
| Device | 127 (`+0x3f8`) | `ID3D12Device10::CreateCommittedResource3` (initial *layout*) | implemented: layout mapped to a legacy state |
| Device | 133 (`+0x428`) | `CreatePlacedResourceX` (resource at a title address) | implemented (see above) |
| Device | 70 (`+0x230`) | unknown, writes a descriptor (return value ignored by the title) | logged only |
| Command list | 131 / 132 (`+0x418` / `+0x420`) | unknown console methods (register-like writes, mask `0xffffffffff00`) | logged only |
| Command signature | argument type `105` (12-byte stride) | console-only indirect argument | **assumed** to be a mesh dispatch |

The device and command-list vtables were enlarged to 256 slots: retail titles call methods past the old table
ends, which made those calls jump through whatever memory followed the table. Unknown slots are now logged with
their arguments and the caller's address (`ret=` is now the raw return address, which matches a disassembly of
the title directly since titles are mapped at their preferred base).

### 5. PC D3D12 validity fixes

Each of these removed the device (`DXGI_ERROR_INVALID_CALL`) or crashed the driver:

- **CBV sizes** must be multiples of 256 bytes on PC; consoles accept e.g. 128. Sizes are rounded up.
- A UAV over a buffer that had been clamped to the host allocation size ran past its end — buffers are no longer
  shrunk; they are made to cover the real range (or fall back to a committed resource).
- Resources over 2 GB (see the 1 GB / 512 MB chunking above).
- A command-list call through the too-short vtable forwarded garbage (`root parameter 0x700000`, null table)
  to the driver; `SetGraphicsRootDescriptorTable` now also ignores parameter indices ≥ 64 and null tables.

### 6. Diagnostics

New file: [`src/runtime/guest_breakpoints.cpp`](src/runtime/guest_breakpoints.cpp), armed right before the guest
entry point, plus extra reporting in `d3d12_xs`:

| Variable | Effect |
| :--- | :--- |
| `SCARLETTX_CRASH_LOG=1` | logs access violations and the title's own `int3` checks with registers and an unwound call stack (module names, title frames marked `title`) |
| `SCARLETTX_BREAKPOINTS=0x…,0x…` | software breakpoints at any address (title or host DLL); each hit logs registers and the call stack, then single-steps and re-arms |
| `SCARLETTX_BREAKPOINT_HITS=N` | log lines per breakpoint (default 4) |
| `SCARLETTX_SKIP_INT3=0x…,0x…` | steps over the listed `int3` instructions of the title (consistency checks known to be harmless under emulation) |
| `SCARLETTX_DRED=1` | enables D3D12 DRED (auto-breadcrumbs, page faults) and prints them when device removal is detected |
| `SCARLETTX_D3D12_DEBUG=1` | debug layer (upstream); stored messages are now printed at failure points |
| `SCARLETTX_D3D12_BREAK=1` | with the debug layer: break on the first error/corruption message |
| `SCARLETTX_DUMP_SHADERS=<dir>` | writes the first shaders passed to `CreatePipelineState` to `<dir>` and logs their container parts |
| `SCARLETTX_XMEM_TRACE=1` | XMem reservation / physical page trace |

Device removal is also checked after every device call that can trigger it, and the first call after which the
device is found removed is named in the log (`DEVICE REMOVED (…) first seen after …`).

---

## Bring-up log: how far the title gets

In order, with the fix that got past each point:

1. `XMemVirtualAlloc` with `PAGE_GRAPHICS_NOACCESS | PAGE_NOACCESS` failed → protection translation.
2. `XMemAllocatePhysicalPages` was a stub → physical page API.
3. DirectStorage factory and all 12 data archives open (already worked upstream once memory was fixed).
4. D3D12 device created on the host GPU; command queues, fences, allocators, command lists created.
5. First command list calls slots 131/132 past the vtable end → vtable enlarged.
6. Graphics init failed on `MEM_RESERVE_PLACEHOLDER` → placeholder support in `XMemVirtualAlloc`.
7. Null call through device slot 133 → `CreatePlacedResourceX`.
8. Shader-visible heap of 1,245,184 descriptors rejected → clamp.
9. Descriptor heap must sit at the title's address → placed heaps + handle translation.
10. Device removed by a UAV past the end of a too-small buffer → buffers over the physical-page sections.
11. Device slot 127 → `CreateCommittedResource3`; two 3840×2160 display buffers, SRVs and RTVs created.
12. A memory-budget consistency check (`int3`) fails because back buffers get committed memory → skipped
    with `SCARLETTX_SKIP_INT3`.
13. Command signature with console argument type 105 → mapped to mesh dispatch.
14. Device slot 74 (heap over a ~6 GB physical page range) → heap proxy.
15. Device removed after `CreateCBV` → title CPU addresses translated to GPU addresses, CBV size rounded to 256.
16. Device slots 61 (samplers) and 86 (`CreatePipelineState`) implemented.
17. **`CreatePipelineState` is rejected by the host** → see below.

Frame pacing is set up by the title at this point (`SetFrameIntervalX(33333 µs)`, i.e. 30 FPS); no frame is
submitted or presented yet.

## Where it stops

The title's pipeline state streams parse cleanly (all subobjects understood, root signatures replaced), but the
host runtime rejects them. Dumping the shaders (`SCARLETTX_DUMP_SHADERS`) shows DXBC containers with the usual
parts:

```
magic='DXBC'  parts: SFI0 ISG1 OSG1 PSV0 HASH DXIL PRIV
DXIL part:    program version 0x00010066 (vs_6_6), DXIL 1.6, bitcode size 4 bytes, followed by an 'XDAT' block
```

The `DXIL` part has a valid header but **no LLVM bitcode** — only a 4-byte stub followed by a console-specific
`XDAT` block, presumably the pre-compiled native GPU (RDNA2) code. The official validator (`dxv`) reports
`Invalid bitcode signature`. There is nothing a PC driver could compile.

Running such titles would need a **console GPU ISA → DXIL recompiler**, comparable to the shader translators of
mature console emulators — far beyond a bring-up fix. After that would still come console texture layouts,
`XboxBvhBuilder` ray tracing, the audio hardware APIs (`XApu`, `XDsp` / `chad.dll`), Bink video and the rest of
rendering.

## Assumptions and known shortcuts

Things that are guesses or deliberate shortcuts, so they are easy to revisit:

- **Indirect argument type 105** is mapped to `D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH` because of its 12-byte
  stride. Not verified.
- **Sampler desc layout** (device slot 61) is inferred from the code that fills it; only fields within valid PC
  ranges are used, the rest is defaulted (linear, wrap).
- **`CreateCommittedResource3`** layouts are converted to legacy resource states, since the rest of the layer uses
  legacy barriers.
- **Heap from physical pages** (slot 74) and resources placed in it do not alias the title's memory.
- **Shader-visible heaps are clamped** to 1,000,000 descriptors; title handles past that point would be invalid.
- `XMemGetWorkingSetStatistics` returns success with an all-zero block (layout unknown);
  `XMemIsToolsMemAvailable` and `XMemTransferMemory` return `TRUE`.
- Freed physical pages keep their section slot (no slot reuse yet).
- `SCARLETTX_SKIP_INT3` steps over checks of the title. Use only for checks understood to be emulation artefacts.
- With the D3D12 debug layer on, the title takes a different (slower) path and exits on its own earlier, so most
  of the work above was done with DRED and the removal checks instead.

## Building

Requirements: Windows 10/11 x64, CMake ≥ 3.20, Ninja, a C++20 compiler. Tested with portable
[WinLibs](https://winlibs.com/) GCC 16.2 (MinGW-w64 UCRT), CMake 4.5 and Ninja 1.13:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build
ctest --test-dir build --output-on-failure
```

MSVC 2022 should work as upstream documents, but was not tested with these changes.

## Running and environment variables

As upstream, point `boot` at an extracted, unencrypted title folder (the one holding `MicrosoftGame.config`):

```powershell
$env:SCARLETTX_TRACE     = "1"
$env:SCARLETTX_CRASH_LOG = "1"
$env:SCARLETTX_DRED      = "1"
.\build\scarlettx.exe boot "D:\Titles\SomeTitle" --in-process --gpu --trace
```

See [Diagnostics](#6-diagnostics) for the variables added in this fork, and `UPSTREAM_README.md` for the rest
(`--warp`, `--fcap`, input mapping, save location).

## Changed files

| File | Change |
| :--- | :--- |
| `src/shims/xmem_physical.cpp`, `.h` | **new** — physical page API, placeholder management, backing queries |
| `src/shims/xgameruntime_shim.cpp` | `XMemVirtualAlloc`: graphics protections, placeholder reserve/replace, commits into placeholder ranges |
| `src/shims/d3d12_xs_shim.cpp` | placed descriptor heaps, handle and GPU VA translation, GPU views of title memory, slots 61/74/86/127/133, 256-slot vtables, CBV/heap/size fixes, DRED, info-queue dumps, removal checks, shader dumping |
| `src/runtime/guest_breakpoints.cpp`, `.h` | **new** — crash log, breakpoints, `int3` skipping, stack unwinding |
| `src/runtime/emulator.cpp` | arms the diagnostics before the guest entry point |
| `CMakeLists.txt` | new sources |
| `UPSTREAM_README.md` | the original README, unchanged (renamed) |

---

*The changes in this fork were made with the help of [Claude Code](https://claude.com/claude-code).*
