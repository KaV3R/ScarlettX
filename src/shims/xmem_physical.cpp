// XMem physical page emulation (AWE-style API of the console xmem.dll).
//
// Signatures recovered from a retail title's call sites:
//   BOOL  XMemAllocatePhysicalPages(ULONG flags, ULONG_PTR* count, ULONG_PTR* pages)
//   BOOL  XMemFreePhysicalPages(ULONG_PTR count, ULONG_PTR* pages)
//   void* XMemMapPhysicalPages(void* va, ULONG_PTR count, ULONG_PTR* pages)   (pages==NULL unmaps)
//   BOOL  XMemMapPhysicalPagesScatter(void** vas, ULONG_PTR count, ULONG_PTR* pages)
//   BOOL  XMemGetWorkingSetStatistics(ULONG flags, void* stats)
//   BOOL  XMemIsToolsMemAvailable(void)
//   BOOL  XMemTransferMemory(ULONG direction, SIZE_T size)
//
// Pages are 64 KB. Each allocation call is backed by one pagefile section;
// a "page frame number" indexes a table of (section, offset). Address ranges
// reserved by XMemVirtualAlloc are host placeholders, split per 64 KB page so
// a view of any physical page (or plain committed memory) can replace any
// page of the range.

#include "xmem_physical.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

namespace {

constexpr uintptr_t kPage = 0x10000;

struct ExtParam {  // MEM_EXTENDED_PARAMETER
    uint64_t type : 8;
    uint64_t reserved : 56;
    union { uint64_t ull; void* ptr; size_t size; HANDLE handle; ULONG ul; };
};
struct AddrReq {  // MEM_ADDRESS_REQUIREMENTS
    void* lowest;
    void* highest;
    size_t alignment;
};

using VirtualAlloc2Fn = void*(WINAPI*)(HANDLE, void*, SIZE_T, ULONG, ULONG, ExtParam*, ULONG);
using MapViewOfFile3Fn = void*(WINAPI*)(HANDLE, HANDLE, void*, ULONG64, SIZE_T, ULONG, ULONG, ExtParam*, ULONG);
using UnmapViewOfFile2Fn = BOOL(WINAPI*)(HANDLE, void*, ULONG);

struct Api {
    VirtualAlloc2Fn va2 = nullptr;
    MapViewOfFile3Fn map3 = nullptr;
    UnmapViewOfFile2Fn unmap2 = nullptr;
    bool ok = false;
    Api() {
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        if (!kb) kb = LoadLibraryW(L"kernelbase.dll");
        if (!kb) return;
        va2 = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kb, "VirtualAlloc2"));
        map3 = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kb, "MapViewOfFile3"));
        unmap2 = reinterpret_cast<UnmapViewOfFile2Fn>(GetProcAddress(kb, "UnmapViewOfFile2"));
        ok = va2 && map3 && unmap2;
    }
};
Api& api() { static Api a; return a; }

struct Section {
    HANDLE handle = nullptr;
    size_t live_pages = 0;
    // Slots (64 KB) of the section handed to pages so far. A page gets its
    // slot when it is first mapped, so pages the title maps side by side end
    // up side by side in the section and one GPU heap range can cover them.
    std::vector<bool> slot_used;
    size_t next_free = 0;
    ~Section() { if (handle) CloseHandle(handle); }
    int64_t claim(size_t wanted) {
        if (wanted < slot_used.size() && !slot_used[wanted]) {
            slot_used[wanted] = true;
            return static_cast<int64_t>(wanted);
        }
        while (next_free < slot_used.size() && slot_used[next_free]) ++next_free;
        if (next_free == slot_used.size()) return -1;
        slot_used[next_free] = true;
        return static_cast<int64_t>(next_free++);
    }
};

struct PhysPage {
    std::shared_ptr<Section> section;  // null when free
    int64_t offset = -1;               // byte offset in the section, -1 until first mapped
};

enum class Occ : uint8_t { View, Commit };
struct Occupant { Occ kind; uint64_t pfn; };

struct Region { uintptr_t end; DWORD protect; };

struct State {
    std::mutex mu;
    std::map<uintptr_t, Region> regions;                // placeholder reservations
    std::map<uintptr_t, uintptr_t> pieces;              // free placeholder pieces: start -> end
    std::unordered_map<uintptr_t, Occupant> occupied;   // 64 KB page -> what replaced it
    std::vector<PhysPage> pfns;                         // index = pfn - kPfnBase
    std::vector<uint64_t> free_pfns;
    std::map<uintptr_t, uintptr_t> raw_placeholders;    // MEM_RESERVE_PLACEHOLDER ranges: start -> end
};
State& st() { static State s; return s; }

constexpr uint64_t kPfnBase = 0x100000;

bool trace() {
    static const bool on = [] {
        char buf[8];
        return GetEnvironmentVariableA("SCARLETTX_XMEM_TRACE", buf, sizeof buf) > 0;
    }();
    return on;
}

const Region* find_region(State& s, uintptr_t a) {
    auto it = s.regions.upper_bound(a);
    if (it == s.regions.begin()) return nullptr;
    --it;
    return a < it->second.end ? &it->second : nullptr;
}

// Turns page p of a free placeholder piece into its own 64 KB placeholder.
bool isolate_page(State& s, uintptr_t p) {
    auto it = s.pieces.upper_bound(p);
    if (it == s.pieces.begin()) return false;
    --it;
    const uintptr_t start = it->first, end = it->second;
    if (p >= end) return false;
    if (start == p && end == p + kPage) {
        s.pieces.erase(it);
        return true;
    }
    if (!VirtualFree(reinterpret_cast<void*>(p), kPage, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
        std::fprintf(stderr, "[ScarlettX][xmem] split %p failed (%lu)\n", reinterpret_cast<void*>(p), GetLastError());
        return false;
    }
    s.pieces.erase(it);
    if (start < p) s.pieces[start] = p;
    if (p + kPage < end) s.pieces[p + kPage] = end;
    return true;
}

// Returns an occupied page to placeholder state.
bool vacate_page(State& s, uintptr_t p) {
    auto it = s.occupied.find(p);
    if (it == s.occupied.end()) return true;
    BOOL ok;
    if (it->second.kind == Occ::View) {
        ok = api().unmap2(GetCurrentProcess(), reinterpret_cast<void*>(p), MEM_PRESERVE_PLACEHOLDER);
    } else {
        ok = VirtualFree(reinterpret_cast<void*>(p), kPage, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    }
    if (!ok) {
        std::fprintf(stderr, "[ScarlettX][xmem] vacate %p failed (%lu)\n", reinterpret_cast<void*>(p), GetLastError());
        return false;
    }
    s.occupied.erase(it);
    s.pieces[p] = p + kPage;
    return true;
}

DWORD view_protect(DWORD region_protect) {
    switch (region_protect & 0xFF) {
        case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_READ: case PAGE_EXECUTE:
            return PAGE_EXECUTE_READWRITE;
        case PAGE_READONLY: return PAGE_READONLY;
        default: return PAGE_READWRITE;
    }
}

bool map_page(State& s, uintptr_t p, uint64_t pfn) {
    const uint64_t idx = pfn - kPfnBase;
    if (pfn < kPfnBase || idx >= s.pfns.size() || !s.pfns[idx].section) {
        std::fprintf(stderr, "[ScarlettX][xmem] map: bad pfn 0x%llx\n", static_cast<unsigned long long>(pfn));
        return false;
    }
    const Region* r = find_region(s, p);
    if (!r) {
        std::fprintf(stderr, "[ScarlettX][xmem] map: %p is not in an XMem reservation\n", reinterpret_cast<void*>(p));
        return false;
    }
    PhysPage& pp = s.pfns[idx];
    if (pp.offset < 0) {
        // First mapping: prefer the slot matching the page's position in the
        // title's placeholder reservation, so a range it lays out contiguously
        // is contiguous in the section as well.
        size_t wanted = static_cast<size_t>(-1);
        auto ph = s.raw_placeholders.upper_bound(p);
        if (ph != s.raw_placeholders.begin()) {
            --ph;
            if (p < ph->second) wanted = (p - ph->first) / kPage;
        }
        const int64_t slot = pp.section->claim(wanted);
        if (slot < 0) {
            std::fprintf(stderr, "[ScarlettX][xmem] map: section has no free slot\n");
            return false;
        }
        pp.offset = slot * static_cast<int64_t>(kPage);
    }
    if (!vacate_page(s, p) || !isolate_page(s, p)) return false;
    void* v = api().map3(pp.section->handle, GetCurrentProcess(), reinterpret_cast<void*>(p),
                         static_cast<ULONG64>(pp.offset), kPage,
                         MEM_REPLACE_PLACEHOLDER, view_protect(r->protect), nullptr, 0);
    if (v != reinterpret_cast<void*>(p)) {
        std::fprintf(stderr, "[ScarlettX][xmem] MapViewOfFile3 %p failed (%lu)\n", reinterpret_cast<void*>(p), GetLastError());
        s.pieces[p] = p + kPage;
        return false;
    }
    s.occupied[p] = {Occ::View, pfn};
    return true;
}

}  // namespace

// --- hooks used by XMemVirtualAlloc -----------------------------------------

void* ScxXMemReservePlaceholder(size_t size, size_t align, DWORD protect) {
    if (!api().ok) return nullptr;
    size = (size + kPage - 1) & ~(kPage - 1);
    AddrReq req{nullptr, nullptr, align > kPage ? align : 0};
    ExtParam ep{};
    ep.type = 1;  // MemExtendedParameterAddressRequirements
    ep.ptr = &req;
    void* p = api().va2(GetCurrentProcess(), nullptr, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
                        req.alignment ? &ep : nullptr, req.alignment ? 1 : 0);
    if (!p) {
        std::fprintf(stderr, "[ScarlettX][xmem] placeholder reserve 0x%zx failed (%lu)\n", size, GetLastError());
        return nullptr;
    }
    State& s = st();
    std::lock_guard<std::mutex> lock(s.mu);
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    s.regions[a] = {a + size, protect};
    s.pieces[a] = a + size;
    if (trace()) std::fprintf(stderr, "[ScarlettX][xmem] reserve %p +0x%zx\n", p, size);
    return p;
}

void* ScxXMemPlaceholderAlloc(void* address, size_t size, DWORD type, DWORD protect, size_t page) {
    if (!api().ok) return nullptr;
    size = (size + 0xFFF) & ~static_cast<size_t>(0xFFF);
    // MEM_RESERVE | MEM_REPLACE_PLACEHOLDER with 64 KB pages: the title turns
    // one of its own placeholders into a reservation that physical pages are
    // mapped into. Host views can only replace placeholders, so it stays one
    // and is tracked like an XMem reservation.
    if (address && (type & MEM_REPLACE_PLACEHOLDER) && !(type & MEM_COMMIT) && page >= kPage) {
        State& s = st();
        std::lock_guard<std::mutex> lock(s.mu);
        const uintptr_t a = reinterpret_cast<uintptr_t>(address);
        size = (size + kPage - 1) & ~(kPage - 1);
        s.regions[a] = {a + size, protect};
        s.pieces[a] = a + size;
        if (trace()) std::fprintf(stderr, "[ScarlettX][xmem] adopt placeholder %p +0x%zx\n", address, size);
        return address;
    }
    // MEM_RESERVE_PLACEHOLDER, or a replace that commits: host VirtualAlloc2.
    const DWORD prot = (type & MEM_RESERVE_PLACEHOLDER) ? PAGE_NOACCESS : protect;
    void* p = api().va2(GetCurrentProcess(), address, size, type, prot, nullptr, 0);
    if (!p) {
        std::fprintf(stderr, "[ScarlettX][xmem] VirtualAlloc2(%p, 0x%zx, 0x%lx, 0x%lx) failed (%lu)\n", address, size,
                     type, prot, GetLastError());
    } else {
        if (type & MEM_RESERVE_PLACEHOLDER) {
            State& s = st();
            std::lock_guard<std::mutex> lock(s.mu);
            const uintptr_t a = reinterpret_cast<uintptr_t>(p);
            s.raw_placeholders[a] = a + size;
        }
        if (trace()) {
            std::fprintf(stderr, "[ScarlettX][xmem] VirtualAlloc2(%p, 0x%zx, 0x%lx) -> %p\n", address, size, type, p);
        }
    }
    return p;
}

int ScxXMemCommitInPlaceholder(void* address, size_t size, DWORD protect, void** out) {
    State& s = st();
    std::lock_guard<std::mutex> lock(s.mu);
    const uintptr_t a = reinterpret_cast<uintptr_t>(address);
    if (!find_region(s, a)) return 0;  // not ours
    const uintptr_t first = a & ~(kPage - 1);
    const uintptr_t last = (a + (size ? size : 1) + kPage - 1) & ~(kPage - 1);
    for (uintptr_t p = first; p < last; p += kPage) {
        auto occ = s.occupied.find(p);
        if (occ != s.occupied.end()) {
            DWORD old;
            VirtualProtect(reinterpret_cast<void*>(p), kPage, protect, &old);
            continue;  // already backed (view or commit)
        }
        if (!isolate_page(s, p)) { *out = nullptr; return 1; }
        void* v = api().va2(GetCurrentProcess(), reinterpret_cast<void*>(p), kPage,
                            MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, protect, nullptr, 0);
        if (v != reinterpret_cast<void*>(p)) {
            std::fprintf(stderr, "[ScarlettX][xmem] commit %p failed (%lu)\n", reinterpret_cast<void*>(p), GetLastError());
            s.pieces[p] = p + kPage;
            *out = nullptr;
            return 1;
        }
        s.occupied[p] = {Occ::Commit, 0};
    }
    *out = address;
    return 1;
}

// --- exports -----------------------------------------------------------------

extern "C" {

__declspec(dllexport) BOOL XMemAllocatePhysicalPages(ULONG flags, ULONG_PTR* count, ULONG_PTR* pages) {
    if (!count || !pages || *count == 0) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    const ULONG_PTR n = *count;
    const uint64_t bytes = static_cast<uint64_t>(n) * kPage;
    auto sec = std::make_shared<Section>();
    sec->handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_COMMIT,
                                     static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), nullptr);
    if (!sec->handle) {
        std::fprintf(stderr, "[ScarlettX][xmem] AllocatePhysicalPages(%llu) failed (%lu)\n",
                     static_cast<unsigned long long>(n), GetLastError());
        *count = 0;
        return FALSE;
    }
    sec->live_pages = n;
    sec->slot_used.assign(n, false);
    State& s = st();
    std::lock_guard<std::mutex> lock(s.mu);
    for (ULONG_PTR i = 0; i < n; ++i) {
        uint64_t idx;
        if (!s.free_pfns.empty()) { idx = s.free_pfns.back(); s.free_pfns.pop_back(); }
        else { idx = s.pfns.size(); s.pfns.emplace_back(); }
        s.pfns[idx].section = sec;
        s.pfns[idx].offset = -1;  // slot assigned on first map
        pages[i] = static_cast<ULONG_PTR>(kPfnBase + idx);
    }
    if (trace()) std::fprintf(stderr, "[ScarlettX][xmem] AllocatePhysicalPages flags=0x%lx n=%llu (%llu MB)\n",
                              flags, static_cast<unsigned long long>(n), static_cast<unsigned long long>(bytes >> 20));
    return TRUE;
}

__declspec(dllexport) BOOL XMemFreePhysicalPages(ULONG_PTR count, ULONG_PTR* pages) {
    if (!pages) return FALSE;
    State& s = st();
    std::lock_guard<std::mutex> lock(s.mu);
    for (ULONG_PTR i = 0; i < count; ++i) {
        const uint64_t idx = pages[i] - kPfnBase;
        if (pages[i] < kPfnBase || idx >= s.pfns.size() || !s.pfns[idx].section) continue;
        s.pfns[idx].section.reset();  // views already mapped keep the section alive
        s.free_pfns.push_back(idx);
    }
    return TRUE;
}

__declspec(dllexport) void* XMemMapPhysicalPages(void* va, ULONG_PTR count, ULONG_PTR* pages) {
    if (!va || !api().ok) return nullptr;
    State& s = st();
    std::lock_guard<std::mutex> lock(s.mu);
    const uintptr_t base = reinterpret_cast<uintptr_t>(va) & ~(kPage - 1);
    for (ULONG_PTR i = 0; i < count; ++i) {
        const uintptr_t p = base + i * kPage;
        if (!pages || pages[i] == 0) {
            if (!vacate_page(s, p)) return nullptr;
        } else if (!map_page(s, p, pages[i])) {
            return nullptr;
        }
    }
    if (trace()) std::fprintf(stderr, "[ScarlettX][xmem] MapPhysicalPages %p n=%llu %s\n", va,
                              static_cast<unsigned long long>(count), pages ? "map" : "unmap");
    return va;
}

__declspec(dllexport) BOOL XMemMapPhysicalPagesScatter(void** vas, ULONG_PTR count, ULONG_PTR* pages) {
    if (!vas || !api().ok) return FALSE;
    State& s = st();
    std::lock_guard<std::mutex> lock(s.mu);
    for (ULONG_PTR i = 0; i < count; ++i) {
        const uintptr_t p = reinterpret_cast<uintptr_t>(vas[i]) & ~(kPage - 1);
        if (!pages || pages[i] == 0) {
            if (!vacate_page(s, p)) return FALSE;
        } else if (!map_page(s, p, pages[i])) {
            return FALSE;
        }
    }
    return TRUE;
}

// For d3d12_xs: describes what backs [va, va+size) as runs of
// (section handle, section offset, length), merged where consecutive.
// Returns the number of runs written (up to max_runs), or 0 when some page of
// the range is not a physical-page view. Not part of the console API.
struct ScxXMemBackingRun {
    HANDLE section;
    uint64_t offset;
    uint64_t va;
    uint64_t length;
};
__declspec(dllexport) size_t ScxXMemQueryBacking(uint64_t va, uint64_t size, ScxXMemBackingRun* runs,
                                                 size_t max_runs) {
    if (!size || !runs || !max_runs) return 0;
    State& s = st();
    std::lock_guard<std::mutex> lock(s.mu);
    size_t n = 0;
    const uintptr_t first = static_cast<uintptr_t>(va) & ~(kPage - 1);
    const uintptr_t last = static_cast<uintptr_t>(va + size + kPage - 1) & ~(kPage - 1);
    for (uintptr_t p = first; p < last; p += kPage) {
        auto it = s.occupied.find(p);
        if (it == s.occupied.end() || it->second.kind != Occ::View) return 0;
        const uint64_t idx = it->second.pfn - kPfnBase;
        if (idx >= s.pfns.size()) return 0;
        // A freed page keeps its section alive through the mapped view; the
        // table entry may be reused, so freed pages are not resolvable here.
        const PhysPage& pp = s.pfns[idx];
        if (!pp.section || pp.offset < 0) return 0;
        const uint64_t off = static_cast<uint64_t>(pp.offset);
        if (n && runs[n - 1].section == pp.section->handle && runs[n - 1].offset + runs[n - 1].length == off &&
            runs[n - 1].va + runs[n - 1].length == p) {
            runs[n - 1].length += kPage;
            continue;
        }
        if (n == max_runs) return 0;
        runs[n++] = {pp.section->handle, off, static_cast<uint64_t>(p), kPage};
    }
    return n;
}

// For d3d12_xs: bounds of the XMem reservation containing va. 0 if none.
__declspec(dllexport) int ScxXMemRegionOf(uint64_t va, uint64_t* base, uint64_t* size) {
    State& s = st();
    std::lock_guard<std::mutex> lock(s.mu);
    auto it = s.regions.upper_bound(static_cast<uintptr_t>(va));
    if (it == s.regions.begin()) return 0;
    --it;
    if (va >= it->second.end) return 0;
    if (base) *base = it->first;
    if (size) *size = it->second.end - it->first;
    return 1;
}

// Like ScxXMemQueryBacking, but pages that are not physical-page views end
// the current run instead of failing the whole query.
__declspec(dllexport) size_t ScxXMemQueryBackingSparse(uint64_t va, uint64_t size, ScxXMemBackingRun* runs,
                                                       size_t max_runs) {
    if (!size || !runs || !max_runs) return 0;
    State& s = st();
    std::lock_guard<std::mutex> lock(s.mu);
    size_t n = 0;
    bool open = false;
    const uintptr_t first = static_cast<uintptr_t>(va) & ~(kPage - 1);
    const uintptr_t last = static_cast<uintptr_t>(va + size + kPage - 1) & ~(kPage - 1);
    for (uintptr_t p = first; p < last; p += kPage) {
        auto it = s.occupied.find(p);
        const PhysPage* pp = nullptr;
        if (it != s.occupied.end() && it->second.kind == Occ::View) {
            const uint64_t idx = it->second.pfn - kPfnBase;
            if (idx < s.pfns.size() && s.pfns[idx].section && s.pfns[idx].offset >= 0) pp = &s.pfns[idx];
        }
        if (!pp) { open = false; continue; }
        const uint64_t off = static_cast<uint64_t>(pp->offset);
        if (open && runs[n - 1].section == pp->section->handle && runs[n - 1].offset + runs[n - 1].length == off) {
            runs[n - 1].length += kPage;
            continue;
        }
        if (n == max_runs) break;
        runs[n++] = {pp->section->handle, off, static_cast<uint64_t>(p), kPage};
        open = true;
    }
    return n;
}

__declspec(dllexport) BOOL XMemGetWorkingSetStatistics(ULONG /*flags*/, void* stats) {
    // Layout unknown; the title zeroes the 0x78-byte block itself and only
    // compares fields against each other, so all-zero is self-consistent.
    return stats ? TRUE : FALSE;
}

__declspec(dllexport) BOOL XMemIsToolsMemAvailable(void) { return TRUE; }

__declspec(dllexport) BOOL XMemTransferMemory(ULONG /*direction*/, SIZE_T /*size*/) { return TRUE; }

}  // extern "C"

#endif  // _WIN32
