#define SCARLETTX_BUILDING_D3D12_XS 1
#include "scarlettx/gpu/d3d12_xs_abi.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <map>
#include <mutex>
#include "scarlettx/gpu/xg_layout.h"
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "scarlettx/gpu/d3d12_backend.h"
#include "scarlettx/gpu/rt_bvh.h"
#include "scarlettx/gpu/shader_ir.h"

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define SCARLETTX_RETURN_ADDRESS() _ReturnAddress()
#else
#define SCARLETTX_RETURN_ADDRESS() __builtin_return_address(0)
#endif

namespace {

#if defined(_WIN32)
HMODULE GetSystemD3D12Module() {
    static HMODULE mod = []() -> HMODULE {
        wchar_t sys_dir[MAX_PATH] = {};
        if (GetSystemDirectoryW(sys_dir, MAX_PATH) == 0) return nullptr;
        std::wstring path = std::wstring(sys_dir) + L"\\d3d12.dll";
        return LoadLibraryW(path.c_str());
    }();
    return mod;
}

HRESULT CallSystemD3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** ppv) {
    HMODULE m = GetSystemD3D12Module();
    if (!m) return E_FAIL;
    using Fn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(m, "D3D12CreateDevice"));
    return fn ? fn(adapter, fl, riid, ppv) : E_FAIL;
}

HRESULT CallSystemD3D12GetDebugInterface(REFIID riid, void** ppv) {
    HMODULE m = GetSystemD3D12Module();
    if (!m) return E_FAIL;
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(m, "D3D12GetDebugInterface"));
    return fn ? fn(riid, ppv) : E_FAIL;
}

HRESULT CallSystemD3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC* desc,
    D3D_ROOT_SIGNATURE_VERSION ver,
    ID3DBlob** blob,
    ID3DBlob** err) {
    HMODULE m = GetSystemD3D12Module();
    if (!m) return E_FAIL;
    using Fn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(m, "D3D12SerializeRootSignature"));
    return fn ? fn(desc, ver, blob, err) : E_FAIL;
}

HRESULT CallSystemD3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
    ID3DBlob** blob,
    ID3DBlob** err) {
    HMODULE m = GetSystemD3D12Module();
    if (!m) return E_FAIL;
    using Fn = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(m, "D3D12SerializeVersionedRootSignature"));
    return fn ? fn(desc, blob, err) : E_FAIL;
}

HRESULT CallSystemD3D12CreateRootSignatureDeserializer(
    LPCVOID src,
    SIZE_T size,
    REFIID riid,
    void** ppv) {
    HMODULE m = GetSystemD3D12Module();
    if (!m) return E_FAIL;
    using Fn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(m, "D3D12CreateRootSignatureDeserializer"));
    return fn ? fn(src, size, riid, ppv) : E_FAIL;
}
#endif

uint32_t IntervalToHz(uint32_t interval) {
    switch (interval) {
        case D3D12XBOX_FRAME_INTERVAL_120_HZ: return 120;
        case D3D12XBOX_FRAME_INTERVAL_60_HZ:  return 60;
        case D3D12XBOX_FRAME_INTERVAL_30_HZ:  return 30;
        case D3D12XBOX_FRAME_INTERVAL_24_HZ:  return 24;
        default: return 60;
    }
}

uint64_t AlignUp(uint64_t v, uint64_t align) {
    return (v + align - 1) & ~(align - 1);
}

struct AccelerationStructureEntry {
    uint32_t type = D3D12XBOX_AS_TYPE_BOTTOM_LEVEL;
    uint32_t flags = 0;
    std::vector<scarlettx::RtTriangle> triangles;
    scarlettx::BvhAccelerationStructure bvh;
};

struct RootSignatureEntry {
    D3D12XBOX_ROOT_SIGNATURE_DESC desc{};
    int32_t constants_param_idx = -1;
    int32_t uav0_param_idx = -1;
#if defined(_WIN32)
    ID3D12RootSignature* native = nullptr;
    ~RootSignatureEntry() {
        if (native) native->Release();
    }
#endif
};

struct DescriptorHeapEntry {
    D3D12XBOX_DESCRIPTOR_HEAP_DESC desc{};
#if defined(_WIN32)
    ID3D12DescriptorHeap* native = nullptr;
    ~DescriptorHeapEntry() {
        if (native) native->Release();
    }
#endif
};

struct PipelineStateEntry {
    bool is_compute = false;
    bool is_raytracing = false;
    uint64_t root_sig_handle = 0;
    uint32_t rtv_format = 28;  // DXGI_FORMAT_R8G8B8A8_UNORM
    uint64_t pso_key = 0;
    uint32_t max_payload_bytes = 0;
    uint32_t max_attribute_bytes = 0;
    uint32_t max_recursion_depth = 0;
#if defined(_WIN32)
    ID3D12PipelineState* native = nullptr;
    ~PipelineStateEntry() {
        if (native) native->Release();
    }
#endif
};

struct D3D12XsDeviceState {
    std::unique_ptr<scarlettx::D3D12Backend> backend;
    uint64_t next_frame_token = 1;
    uint64_t last_issued_token = 0;
    uint64_t last_presented_token = 0;
    uint64_t presented_frame_count = 0;
    uint32_t frame_interval = D3D12XBOX_FRAME_INTERVAL_60_HZ;
    uint32_t frame_interval_flags = 0;
    bool suspended = false;
    std::chrono::steady_clock::time_point last_present_time = std::chrono::steady_clock::now();
    uint64_t last_frame_duration_us = 16667;
    uint64_t next_as_handle = 0xA5000001ull;
    std::unordered_map<uint64_t, std::unique_ptr<AccelerationStructureEntry>> accel_structs;

    uint64_t next_root_sig_handle = 0x21000001ull;
    uint64_t next_heap_handle = 0x22000001ull;
    uint64_t next_pso_handle = 0x23000001ull;
    std::unordered_map<uint64_t, std::unique_ptr<RootSignatureEntry>> root_signatures;
    std::unordered_map<uint64_t, std::unique_ptr<DescriptorHeapEntry>> descriptor_heaps;
    std::unordered_map<uint64_t, std::unique_ptr<PipelineStateEntry>> psos;
    uint64_t pso_cache_hits = 0;
    uint64_t pso_cache_misses = 0;
#if defined(_WIN32)
    std::unordered_map<uint64_t, ID3D12PipelineState*> pso_cache;
    ~D3D12XsDeviceState() {
        for (auto& kv : pso_cache) {
            if (kv.second) kv.second->Release();
        }
        pso_cache.clear();
        psos.clear();
        descriptor_heaps.clear();
        root_signatures.clear();
    }
#endif
};

std::mutex g_mutex;
uint64_t g_next_handle = 0x12000001ull;
std::unordered_map<uint64_t, std::unique_ptr<D3D12XsDeviceState>> g_devices;
std::atomic<uint32_t> g_process_debug_flags{0};

bool TraceEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("SCARLETTX_TRACE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

#if defined(_WIN32)
// Prints and clears the debug layer's stored messages (SCARLETTX_D3D12_DEBUG=1).
void DumpD3D12InfoQueue(ID3D12Device* dev, const char* where) {
    if (!dev) return;
    ID3D12InfoQueue* q = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D12InfoQueue), reinterpret_cast<void**>(&q))) || !q) return;
    const UINT64 n = q->GetNumStoredMessages();
    for (UINT64 i = 0; i < n && i < 64; ++i) {
        SIZE_T len = 0;
        if (FAILED(q->GetMessage(i, nullptr, &len)) || !len) continue;
        std::vector<char> buf(len);
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (SUCCEEDED(q->GetMessage(i, m, &len)) && m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
            std::cerr << "[ScarlettX][d3d12-debug][" << where << "] sev=" << static_cast<int>(m->Severity)
                      << " id=" << static_cast<int>(m->ID) << ": "
                      << std::string(m->pDescription, m->DescriptionByteLength ? m->DescriptionByteLength - 1 : 0)
                      << "\n";
        }
    }
    q->ClearStoredMessages();
    q->Release();
}

// Logs (once) the first call after which the device is found removed.
void CheckDeviceRemoved(ID3D12Device* dev, const char* where) {
    static std::atomic<bool> reported{false};
    if (!dev || reported.load(std::memory_order_relaxed)) return;
    const HRESULT r = dev->GetDeviceRemovedReason();
    if (SUCCEEDED(r) || reported.exchange(true)) return;
    std::cerr << "[ScarlettX][d3d12_xs] DEVICE REMOVED (0x" << std::hex << static_cast<uint32_t>(r) << std::dec
              << ") first seen after " << where << "\n";
}

// Prints DRED breadcrumbs (last GPU operations per command list) and the
// page-fault address, when SCARLETTX_DRED=1 enabled them.
void ReportDred(ID3D12Device* dev) {
    ID3D12DeviceRemovedExtendedData* dred = nullptr;
    if (!dev || FAILED(dev->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData),
                                           reinterpret_cast<void**>(&dred))) || !dred) {
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc))) {
        int lists = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE* n = bc.pHeadAutoBreadcrumbNode; n && lists < 16; n = n->pNext, ++lists) {
            const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            if (done >= n->BreadcrumbCount) continue;  // list completed
            std::cerr << "[ScarlettX][dred] list " << n->pCommandList << " (" << n->BreadcrumbCount
                      << " ops) stopped after op " << done << ":";
            for (UINT i = (done > 3 ? done - 3 : 0); i < n->BreadcrumbCount && i < done + 3; ++i) {
                std::cerr << (i == done ? " [" : " ") << static_cast<int>(n->pCommandHistory[i])
                          << (i == done ? "]" : "");
            }
            std::cerr << "\n";
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf))) {
        std::cerr << "[ScarlettX][dred] page fault at GPU VA 0x" << std::hex << pf.PageFaultVA << std::dec;
        int k = 0;
        for (auto* a = pf.pHeadExistingAllocationNode; a && k < 4; a = a->pNext, ++k) {
            std::cerr << " existing-alloc-type=" << static_cast<int>(a->AllocationType);
        }
        for (auto* a = pf.pHeadRecentFreedAllocationNode; a && k < 8; a = a->pNext, ++k) {
            std::cerr << " freed-alloc-type=" << static_cast<int>(a->AllocationType);
        }
        std::cerr << "\n";
    }
    dred->Release();
}
#endif

bool RealTitleMode() {
    const char* v = std::getenv("SCARLETTX_REAL_TITLE");
    return v && v[0] != '\0' && v[0] != '0';
}

bool PreferWarpFromEnv(bool fallback) {
    const char* v = std::getenv("SCARLETTX_PREFER_WARP");
    if (!v || v[0] == '\0') {
        return fallback;
    }
    return v[0] != '0';
}

// ---------------------------------------------------------------------------
// Xbox ID3D12Device / IDXGIDevice / IDXGIAdapter / ID3D12CommandQueue COM
// vtable proxies for real Scarlett binaries.
// Never hands back raw host PC COM pointers to guest code; translates only
// verified guest vtable slots and logs any unimplemented slot calls.
// ---------------------------------------------------------------------------
using ComVtableFn = intptr_t (SCARLETTX_GDK_CALLBACK*)(void*, uintptr_t, uintptr_t, uintptr_t);

struct ProxyGuid {
    uint32_t d1;
    uint16_t d2;
    uint16_t d3;
    uint8_t d4[8];
};

bool ProxyGuidEquals(uintptr_t riid, const ProxyGuid& g) {
    return riid != 0 && std::memcmp(reinterpret_cast<const void*>(riid), &g, sizeof(ProxyGuid)) == 0;
}

// Standard DXGI / D3D12 IIDs verified from target disassembly
constexpr ProxyGuid kIidIDXGIDevice = {
    0x54EC77FAu, 0x1377u, 0x44E6u, {0x8C, 0x32, 0x88, 0xFD, 0x5F, 0x44, 0xC8, 0x4C}};
constexpr ProxyGuid kIidIDXGIAdapter = {
    0x2411E7E1u, 0x12ACu, 0x4CCFu, {0xBD, 0x14, 0x97, 0x98, 0xE8, 0x53, 0x4D, 0xC0}};
constexpr ProxyGuid kIidIDXGIAdapter1 = {
    0x29038F61u, 0x3839u, 0x4626u, {0x91, 0xFD, 0x08, 0x68, 0x79, 0x01, 0x1A, 0x05}};
constexpr ProxyGuid kIidIDXGIDevice1 = {
    0x77DB970Fu, 0x6276u, 0x48BAu, {0xBA, 0x28, 0x07, 0x01, 0x43, 0xB4, 0x39, 0x2C}};

// A title gets its output from IDXGIAdapter::EnumOutputs only to hand it to
// SetFrameIntervalX, so the output is an identity with a reference count.
struct XboxDXGIOutputProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
};

intptr_t SCARLETTX_GDK_CALLBACK OutputProxyQueryInterface(void* self, uintptr_t, uintptr_t ppv, uintptr_t) {
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    static_cast<XboxDXGIOutputProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK OutputProxyAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxDXGIOutputProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}

intptr_t SCARLETTX_GDK_CALLBACK OutputProxyRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxDXGIOutputProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete obj;
    return static_cast<intptr_t>(rem);
}

template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK OutputProxyTracedSlot(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxIDXGIOutput::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}

template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeOutputProxyVtable(std::index_sequence<Is...>) {
    return {
        &OutputProxyQueryInterface,
        &OutputProxyAddRef,
        &OutputProxyRelease,
        &OutputProxyTracedSlot<static_cast<int>(Is + 3)>...,
    };
}

const auto kXboxDXGIOutputProxyVtbl = MakeOutputProxyVtable(std::make_index_sequence<21>{});

struct XboxDXGIAdapterProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
#if defined(_WIN32)
    IDXGIAdapter1* pc_adapter = nullptr;
#endif
};

intptr_t SCARLETTX_GDK_CALLBACK AdapterProxyQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxDXGIAdapterProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    if (TraceEnabled() && riid) {
        const auto* g = reinterpret_cast<const uint32_t*>(riid);
        std::cerr << "[ScarlettX][d3d12_xs] XboxIDXGIAdapter::QueryInterface("
                  << std::hex << g[0] << "-" << g[1] << "-" << g[2] << "-" << g[3] << std::dec << ")\n";
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK AdapterProxyAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxDXGIAdapterProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}

intptr_t SCARLETTX_GDK_CALLBACK AdapterProxyRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxDXGIAdapterProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        if (obj->pc_adapter) obj->pc_adapter->Release();
#endif
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}

intptr_t SCARLETTX_GDK_CALLBACK AdapterProxyGetDesc(void* self, uintptr_t pDesc, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxDXGIAdapterProxy*>(self);
    if (!pDesc) return static_cast<intptr_t>(E_INVALIDARG);
    if (obj->pc_adapter) {
        return static_cast<intptr_t>(obj->pc_adapter->GetDesc(reinterpret_cast<DXGI_ADAPTER_DESC*>(pDesc)));
    }
#endif
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}

intptr_t SCARLETTX_GDK_CALLBACK AdapterProxyGetDesc1(void* self, uintptr_t pDesc, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxDXGIAdapterProxy*>(self);
    if (!pDesc) return static_cast<intptr_t>(E_INVALIDARG);
    if (obj->pc_adapter) {
        return static_cast<intptr_t>(obj->pc_adapter->GetDesc1(reinterpret_cast<DXGI_ADAPTER_DESC1*>(pDesc)));
    }
#endif
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}

// IDXGIAdapter::EnumOutputs (+0x38), observed at 0x1404f5290 in a title: one output.
intptr_t SCARLETTX_GDK_CALLBACK AdapterProxyEnumOutputs(void* /*self*/, uintptr_t index, uintptr_t ppOutput, uintptr_t) {
    auto** out = reinterpret_cast<void**>(ppOutput);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    *out = nullptr;
    if (static_cast<uint32_t>(index) != 0) return static_cast<intptr_t>(static_cast<int32_t>(0x887A0002u));
    auto* output = new XboxDXGIOutputProxy();
    output->lpVtbl = kXboxDXGIOutputProxyVtbl.data();
    *out = output;
    return 0;
}

template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK AdapterProxyTracedSlot(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxIDXGIAdapter::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}

template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeAdapterProxyVtable(std::index_sequence<Is...>) {
    return {
        &AdapterProxyQueryInterface,
        &AdapterProxyAddRef,
        &AdapterProxyRelease,
        &AdapterProxyTracedSlot<static_cast<int>(Is + 3)>...,
    };
}

std::array<ComVtableFn, 24> BuildAdapterProxyVtable() {
    auto vtbl = MakeAdapterProxyVtable(std::make_index_sequence<21>{});
    vtbl[7] = &AdapterProxyEnumOutputs;
    vtbl[8] = &AdapterProxyGetDesc;
    vtbl[10] = &AdapterProxyGetDesc1;
    return vtbl;
}

const auto kXboxDXGIAdapterProxyVtbl = BuildAdapterProxyVtable();

struct XboxD3D12DescriptorHeapProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
#if defined(_WIN32)
    ID3D12DescriptorHeap* pc_heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC orig_desc{};
    // Xbox heaps can be placed at a title-chosen address (desc +0x10/+0x18).
    // Handles then live in the title's range and are translated to the host
    // heap wherever they come back in.
    uintptr_t guest_base = 0;
    uintptr_t guest_size = 0;
#endif
};

#if defined(_WIN32)
struct PlacedDescriptorRange {
    uintptr_t guest_base, guest_end;
    SIZE_T host_cpu;
    UINT64 host_gpu;  // 0 when the heap is not shader visible
};
std::mutex g_placed_heaps_mutex;
std::vector<PlacedDescriptorRange> g_placed_heaps;
std::atomic<bool> g_have_placed_heaps{false};

void RegisterPlacedDescriptorHeap(uintptr_t base, uintptr_t size, SIZE_T cpu, UINT64 gpu) {
    std::lock_guard<std::mutex> lock(g_placed_heaps_mutex);
    g_placed_heaps.push_back({base, base + size, cpu, gpu});
    g_have_placed_heaps.store(true, std::memory_order_release);
}

// Title CPU descriptor handle -> host handle (unchanged when not placed).
SIZE_T HostCpuHandle(uintptr_t h) {
    if (!g_have_placed_heaps.load(std::memory_order_acquire)) return static_cast<SIZE_T>(h);
    std::lock_guard<std::mutex> lock(g_placed_heaps_mutex);
    for (const auto& r : g_placed_heaps) {
        if (h >= r.guest_base && h < r.guest_end) return r.host_cpu + (h - r.guest_base);
    }
    return static_cast<SIZE_T>(h);
}

// On the console a buffer's GPU virtual address is its CPU address, so titles
// put their own pointers into CBVs, root views and IA views. Buffers created
// at a title address (CreatePlacedResourceX) are recorded here with the host
// resource's GPU address so those pointers can be translated.
struct GuestGpuRange {
    uint64_t guest_base, guest_end;
    D3D12_GPU_VIRTUAL_ADDRESS host_base;
};
std::mutex g_gpu_ranges_mutex;
std::map<uint64_t, GuestGpuRange> g_gpu_ranges;  // keyed by guest_base
std::atomic<bool> g_have_gpu_ranges{false};

void RegisterGuestGpuRange(uint64_t base, uint64_t size, D3D12_GPU_VIRTUAL_ADDRESS host) {
    std::lock_guard<std::mutex> lock(g_gpu_ranges_mutex);
    g_gpu_ranges[base] = {base, base + size, host};
    g_have_gpu_ranges.store(true, std::memory_order_release);
}

// Title GPU virtual address -> host GPU virtual address (unchanged when it is
// not inside a recorded range, e.g. an address the host handed out itself).
bool LookupGuestGpuVa(uint64_t va, D3D12_GPU_VIRTUAL_ADDRESS* out) {
    std::lock_guard<std::mutex> lock(g_gpu_ranges_mutex);
    auto it = g_gpu_ranges.upper_bound(va);
    if (it == g_gpu_ranges.begin()) return false;
    --it;
    if (va >= it->second.guest_end) return false;
    *out = it->second.host_base + (va - it->second.guest_base);
    return true;
}

// Defined with the physical-page GPU helpers further down: creates host
// buffers over the title memory containing va and records them.
bool ResolveGuestGpuVa(uint64_t va);

D3D12_GPU_VIRTUAL_ADDRESS HostGpuVa(uint64_t va) {
    if (!va) return va;
    D3D12_GPU_VIRTUAL_ADDRESS host = 0;
    if (g_have_gpu_ranges.load(std::memory_order_acquire) && LookupGuestGpuVa(va, &host)) return host;
    if (ResolveGuestGpuVa(va) && LookupGuestGpuVa(va, &host)) return host;
    static std::atomic<int> misses{0};
    if (TraceEnabled() && misses.fetch_add(1) < 16) {
        std::cerr << "[ScarlettX][d3d12_xs] GPU VA 0x" << std::hex << va << std::dec
                  << " is not backed by any GPU-visible title memory\n";
    }
    return va;
}

// Title GPU descriptor handle -> host handle. Placed heaps hand the title
// its own address as the GPU start too.
UINT64 HostGpuHandle(uint64_t h) {
    if (!g_have_placed_heaps.load(std::memory_order_acquire)) return h;
    std::lock_guard<std::mutex> lock(g_placed_heaps_mutex);
    for (const auto& r : g_placed_heaps) {
        if (r.host_gpu && h >= r.guest_base && h < r.guest_end) return r.host_gpu + (h - r.guest_base);
    }
    return h;
}
#endif

intptr_t SCARLETTX_GDK_CALLBACK DescHeapProxyQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxD3D12DescriptorHeapProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK DescHeapProxyAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxD3D12DescriptorHeapProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}

intptr_t SCARLETTX_GDK_CALLBACK DescHeapProxyRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxD3D12DescriptorHeapProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        if (obj->pc_heap) obj->pc_heap->Release();
#endif
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}

intptr_t SCARLETTX_GDK_CALLBACK DescHeapProxySetName(void* self, uintptr_t name_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DescriptorHeapProxy*>(self);
    if (obj->pc_heap && name_ptr) {
        return static_cast<intptr_t>(obj->pc_heap->SetName(reinterpret_cast<LPCWSTR>(name_ptr)));
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK DescHeapProxyGetCPUStart(void* self, uintptr_t, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DescriptorHeapProxy*>(self);
    if (obj && obj->pc_heap) {
        if (obj->guest_base) return static_cast<intptr_t>(obj->guest_base);
        const D3D12_CPU_DESCRIPTOR_HANDLE h = obj->pc_heap->GetCPUDescriptorHandleForHeapStart();
        return static_cast<intptr_t>(h.ptr);
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK DescHeapProxyGetGPUStart(void* self, uintptr_t a1, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DescriptorHeapProxy*>(self);
    if (obj && obj->pc_heap) {
        if (obj->guest_base) return static_cast<intptr_t>(obj->guest_base);
        const D3D12_GPU_DESCRIPTOR_HANDLE h = obj->pc_heap->GetGPUDescriptorHandleForHeapStart();
        if (TraceEnabled()) {
            std::cerr << "[ScarlettX][d3d12_xs] DescHeap::GetGPUStart(self=" << self
                      << ", a1=0x" << std::hex << a1 << ", h=0x" << h.ptr << std::dec << ")\n";
        }
        return static_cast<intptr_t>(h.ptr);
    }
#endif
    return 0;
}

template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK DescHeapProxyTracedSlot(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12DescriptorHeap::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}

template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeDescHeapProxyVtable(std::index_sequence<Is...>) {
    return {
        &DescHeapProxyQueryInterface,
        &DescHeapProxyAddRef,
        &DescHeapProxyRelease,
        &DescHeapProxyTracedSlot<static_cast<int>(Is + 3)>...,
    };
}

std::array<ComVtableFn, 16> BuildDescHeapProxyVtable() {
    auto vtbl = MakeDescHeapProxyVtable(std::make_index_sequence<13>{});
    vtbl[6] = &DescHeapProxySetName;      // +0x30
    vtbl[9] = &DescHeapProxyGetCPUStart;  // +0x48 (verified in ConceptDestruction.exe: returns SIZE_T in RAX)
    vtbl[10] = &DescHeapProxyGetGPUStart; // +0x50
    return vtbl;
}

const auto kXboxD3D12DescriptorHeapProxyVtbl = BuildDescHeapProxyVtable();

struct XboxD3D12FenceProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
#if defined(_WIN32)
    ID3D12Fence* pc_fence = nullptr;
#endif
};

intptr_t SCARLETTX_GDK_CALLBACK FenceProxyQueryInterface(void* self, uintptr_t /*riid*/, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxD3D12FenceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK FenceProxyAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxD3D12FenceProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}
intptr_t SCARLETTX_GDK_CALLBACK FenceProxyRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxD3D12FenceProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        if (obj->pc_fence) obj->pc_fence->Release();
#endif
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}
intptr_t SCARLETTX_GDK_CALLBACK FenceProxySetName(void* self, uintptr_t name_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12FenceProxy*>(self);
    if (obj->pc_fence && name_ptr) return static_cast<intptr_t>(obj->pc_fence->SetName(reinterpret_cast<LPCWSTR>(name_ptr)));
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK FenceProxyGetCompletedValue(void* self, uintptr_t, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12FenceProxy*>(self);
    if (obj->pc_fence) return static_cast<intptr_t>(obj->pc_fence->GetCompletedValue());
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK FenceProxySetEventOnCompletion(void* self, uintptr_t val, uintptr_t hEvent, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12FenceProxy*>(self);
    if (obj->pc_fence) return static_cast<intptr_t>(obj->pc_fence->SetEventOnCompletion(val, reinterpret_cast<HANDLE>(hEvent)));
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK FenceProxySignal(void* self, uintptr_t val, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12FenceProxy*>(self);
    if (obj->pc_fence) return static_cast<intptr_t>(obj->pc_fence->Signal(val));
#endif
    return 0;
}
template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK FenceProxyTracedSlot(void*, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Fence::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}
template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeFenceProxyVtable(std::index_sequence<Is...>) {
    return {&FenceProxyQueryInterface, &FenceProxyAddRef, &FenceProxyRelease, &FenceProxyTracedSlot<static_cast<int>(Is + 3)>...};
}
std::array<ComVtableFn, 16> BuildFenceProxyVtable() {
    auto vtbl = MakeFenceProxyVtable(std::make_index_sequence<13>{});
    vtbl[6] = &FenceProxySetName;
    vtbl[8] = &FenceProxyGetCompletedValue;
    vtbl[9] = &FenceProxySetEventOnCompletion;
    vtbl[10] = &FenceProxySignal;
    return vtbl;
}
const auto kXboxFenceProxyVtbl = BuildFenceProxyVtable();

struct XboxD3D12CommandAllocatorProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
#if defined(_WIN32)
    ID3D12CommandAllocator* pc_alloc = nullptr;
#endif
};
intptr_t SCARLETTX_GDK_CALLBACK AllocProxyQueryInterface(void* self, uintptr_t, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxD3D12CommandAllocatorProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK AllocProxyAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxD3D12CommandAllocatorProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}
intptr_t SCARLETTX_GDK_CALLBACK AllocProxyRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxD3D12CommandAllocatorProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        if (obj->pc_alloc) obj->pc_alloc->Release();
#endif
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}
intptr_t SCARLETTX_GDK_CALLBACK AllocProxySetName(void* self, uintptr_t name_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandAllocatorProxy*>(self);
    if (obj->pc_alloc && name_ptr) return static_cast<intptr_t>(obj->pc_alloc->SetName(reinterpret_cast<LPCWSTR>(name_ptr)));
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK AllocProxyReset(void* self, uintptr_t, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandAllocatorProxy*>(self);
    if (obj->pc_alloc) return static_cast<intptr_t>(obj->pc_alloc->Reset());
#endif
    return 0;
}
template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK AllocProxyTracedSlot(void*, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12CommandAllocator::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}
template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeAllocProxyVtable(std::index_sequence<Is...>) {
    return {&AllocProxyQueryInterface, &AllocProxyAddRef, &AllocProxyRelease, &AllocProxyTracedSlot<static_cast<int>(Is + 3)>...};
}
std::array<ComVtableFn, 16> BuildAllocProxyVtable() {
    auto vtbl = MakeAllocProxyVtable(std::make_index_sequence<13>{});
    vtbl[6] = &AllocProxySetName;
    vtbl[8] = &AllocProxyReset;
    return vtbl;
}
const auto kXboxCommandAllocatorProxyVtbl = BuildAllocProxyVtable();

// Generic simple proxy for RootSignature / PipelineState / QueryHeap / CommandSignature
struct XboxD3D12SimpleObjectProxy {
    const ComVtableFn* lpVtbl;
    // Xbox d3d12_xs inline fast-paths (e.g. SetPipelineState) read header bytes at
    // +0x10..+0x2F on PSO objects before checking the command-list fast-path buffer.
    uint8_t xbox_inline_header[0x80] = {};
    std::atomic<uint32_t> ref_count{1};
#if defined(_WIN32)
    ID3D12Object* pc_obj = nullptr;
#endif
    const char* kind = "Object";
};
intptr_t SCARLETTX_GDK_CALLBACK SimpleObjQueryInterface(void* self, uintptr_t, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxD3D12SimpleObjectProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK SimpleObjAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxD3D12SimpleObjectProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}
intptr_t SCARLETTX_GDK_CALLBACK SimpleObjRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxD3D12SimpleObjectProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        if (obj->pc_obj) obj->pc_obj->Release();
#endif
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}
intptr_t SCARLETTX_GDK_CALLBACK SimpleObjSetName(void* self, uintptr_t name_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12SimpleObjectProxy*>(self);
    if (obj->pc_obj && name_ptr) return static_cast<intptr_t>(obj->pc_obj->SetName(reinterpret_cast<LPCWSTR>(name_ptr)));
#endif
    return 0;
}
template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK SimpleObjTracedSlot(void* self, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        auto* obj = static_cast<XboxD3D12SimpleObjectProxy*>(self);
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12" << obj->kind << "::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}
template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeSimpleObjVtable(std::index_sequence<Is...>) {
    return {&SimpleObjQueryInterface, &SimpleObjAddRef, &SimpleObjRelease, &SimpleObjTracedSlot<static_cast<int>(Is + 3)>...};
}
std::array<ComVtableFn, 16> BuildSimpleObjVtable() {
    auto vtbl = MakeSimpleObjVtable(std::make_index_sequence<13>{});
    vtbl[6] = &SimpleObjSetName;
    return vtbl;
}
const auto kXboxSimpleObjProxyVtbl = BuildSimpleObjVtable();

// Guest-facing heap. The host has no heap behind it: the proxy records what
// the title asked for, and a resource placed in it gets its own host memory.
// Host heaps are bound to one resource category on tier-1 hardware and have
// their own placement rules, neither of which the console layout follows.
// Two resources placed at overlapping offsets therefore do not alias.
struct XboxD3D12HeapProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
#if defined(_WIN32)
    D3D12_HEAP_DESC desc{};
#endif
};
const ProxyGuid kIidID3D12Heap = {0x6b3b2502u, 0x6e51u, 0x45b3u, {0x90, 0xee, 0x98, 0x84, 0x26, 0x5e, 0x8d, 0xf3}};
const ProxyGuid kIidIUnknownForHeap = {0x00000000u, 0x0000u, 0x0000u, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

intptr_t SCARLETTX_GDK_CALLBACK HeapQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(static_cast<int32_t>(0x80004003u));
    *out = nullptr;
    if (!riid) return static_cast<intptr_t>(static_cast<int32_t>(0x80004003u));
    if (!ProxyGuidEquals(riid, kIidID3D12Heap) && !ProxyGuidEquals(riid, kIidIUnknownForHeap)) {
        return static_cast<intptr_t>(static_cast<int32_t>(0x80004002u));
    }
    static_cast<XboxD3D12HeapProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK HeapAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxD3D12HeapProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}
intptr_t SCARLETTX_GDK_CALLBACK HeapRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxD3D12HeapProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) delete obj;
    return static_cast<intptr_t>(rem);
}
template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK HeapTracedSlot(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Heap::vtable[" << Slot << "](a1=0x" << std::hex << a1
                  << ", a2=0x" << a2 << ", a3=0x" << a3 << std::dec << ") -> E_NOTIMPL\n";
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}
template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeHeapVtable(std::index_sequence<Is...>) {
    return {&HeapQueryInterface, &HeapAddRef, &HeapRelease, &HeapTracedSlot<static_cast<int>(Is + 3)>...};
}
const auto kXboxHeapProxyVtbl = MakeHeapVtable(std::make_index_sequence<13>{});

struct GuestTextureMemory;
struct XboxD3D12ResourceProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
#if defined(_WIN32)
    ID3D12Resource* pc_res = nullptr;
    std::atomic<uint32_t> tracked_state{0};
    DXGI_FORMAT guest_format = DXGI_FORMAT_UNKNOWN;
    bool promoted_typeless = false;
    GuestTextureMemory* guest_memory = nullptr;
#endif
    void* device = nullptr;  // the XboxD3D12DeviceProxy that created it, referenced
};
#if defined(_WIN32)
// Textures the title fills through the address returned by
// GetGPUVirtualAddress. On the console that address is the texture's own
// memory, and titles copy cooked texel data (console layout) straight into it.
// Here it is a write-watched guest buffer that is converted to rows and
// uploaded into the host texture whenever the title has written to it.
struct GuestTextureMemory {
    void* base = nullptr;
    size_t size = 0;
    XG_RESOURCE_LAYOUT layout{};
};
std::mutex g_guest_textures_mutex;
std::vector<XboxD3D12ResourceProxy*> g_guest_textures;
std::atomic<uint64_t> g_upload_textures{0};
std::atomic<uint64_t> g_upload_bytes{0};

// True when the guest layout describes the same rows as the host texture's
// subresources. Rules out planar formats and element sizes the layout code
// does not know, whose conversion would not fit the host footprints.
bool GuestLayoutMatchesHost(ID3D12Resource* res, const D3D12_RESOURCE_DESC& d, const XG_RESOURCE_LAYOUT& layout) {
    ID3D12Device* device = nullptr;
    if (FAILED(res->GetDevice(IID_PPV_ARGS(&device))) || !device) return false;
    D3D12_FEATURE_DATA_FORMAT_INFO info = {};
    info.Format = d.Format;
    bool ok = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info))) &&
              info.PlaneCount == 1;
    const UINT subresources = static_cast<UINT>(d.MipLevels) * d.DepthOrArraySize;
    std::vector<UINT> rows(subresources);
    std::vector<UINT64> row_bytes(subresources);
    if (ok) {
        device->GetCopyableFootprints(&d, 0, subresources, 0, nullptr, rows.data(), row_bytes.data(), nullptr);
        const XG_PLANE_LAYOUT& plane = layout.Plane[0];
        for (UINT s = 0; ok && s < subresources; ++s) {
            const XG_MIPLEVEL_LAYOUT& ml = plane.MipLayout[s % d.MipLevels];
            ok = rows[s] == ml.HeightElements &&
                 row_bytes[s] == static_cast<UINT64>(ml.WidthElements) * plane.BytesPerElement;
        }
    }
    device->Release();
    return ok;
}

void* EnsureGuestTextureMemory(XboxD3D12ResourceProxy* obj) {
    std::lock_guard<std::mutex> lock(g_guest_textures_mutex);
    if (obj->guest_memory) return obj->guest_memory->base;
    const D3D12_RESOURCE_DESC d = obj->pc_res->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count > 1 ||
        (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) {
        return nullptr;
    }
    XG_TEXTURE2D_DESC xd{};
    xd.Width = static_cast<uint32_t>(d.Width);
    xd.Height = d.Height;
    xd.MipLevels = d.MipLevels;
    xd.ArraySize = d.DepthOrArraySize;
    xd.Format = static_cast<uint32_t>(obj->guest_format != DXGI_FORMAT_UNKNOWN ? obj->guest_format : d.Format);
    xd.SampleDesc.Count = 1;
    xd.BindFlags = 0x8;
    xd.SwizzleMode = XG_SWIZZLE_MODE_4KB_S;
    auto mem = std::make_unique<GuestTextureMemory>();
    scarlettx::gpu::xg::ComputeTexture2DLayout(xd, &mem->layout);
    if (!scarlettx::gpu::xg::IsAddressable(mem->layout) || mem->layout.SizeBytes == 0 ||
        !GuestLayoutMatchesHost(obj->pc_res, d, mem->layout)) {
        return nullptr;
    }
    mem->size = static_cast<size_t>(mem->layout.SizeBytes);
    mem->base = VirtualAlloc(nullptr, mem->size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (!mem->base) return nullptr;
    if (TraceEnabled()) {
        std::ostringstream os;
        os << "[ScarlettX][d3d12_xs] texture " << d.Width << "x" << d.Height << " mips=" << d.MipLevels
           << " fmt=" << xd.Format << ": guest memory " << mem->base << " size=0x" << std::hex << mem->size
           << "\n";
        std::cerr << os.str();
    }
    obj->guest_memory = mem.release();
    g_guest_textures.push_back(obj);
    return obj->guest_memory->base;
}

void ReleaseGuestTextureMemory(XboxD3D12ResourceProxy* obj) {
    std::lock_guard<std::mutex> lock(g_guest_textures_mutex);
    if (!obj->guest_memory) return;
    g_guest_textures.erase(std::remove(g_guest_textures.begin(), g_guest_textures.end(), obj),
                           g_guest_textures.end());
    VirtualFree(obj->guest_memory->base, 0, MEM_RELEASE);
    delete obj->guest_memory;
    obj->guest_memory = nullptr;
}

// Converts the given subresources of a texture's guest memory to rows and
// copies them into the host texture. Blocks until the copy has run so the
// temporaries can be released.
void UploadGuestTexture(ID3D12CommandQueue* queue, XboxD3D12ResourceProxy* tex, const std::vector<UINT>& dirty) {
    ID3D12Device* device = nullptr;
    if (FAILED(tex->pc_res->GetDevice(IID_PPV_ARGS(&device))) || !device) return;
    const D3D12_RESOURCE_DESC d = tex->pc_res->GetDesc();
    const UINT subresources = static_cast<UINT>(d.MipLevels) * d.DepthOrArraySize;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fps(subresources);
    UINT64 total = 0;
    device->GetCopyableFootprints(&d, 0, subresources, 0, fps.data(), nullptr, nullptr, &total);

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* upload = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    void* mapped = nullptr;
    const bool ready =
        total != 0 &&
        SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                  nullptr, IID_PPV_ARGS(&upload))) &&
        SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list))) &&
        SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) &&
        SUCCEEDED(upload->Map(0, nullptr, &mapped));
    if (ready) {
        const GuestTextureMemory& mem = *tex->guest_memory;
        for (const UINT s : dirty) {
            scarlettx::gpu::xg::CopyFromSubresource(mem.layout, mem.base, s % d.MipLevels, s / d.MipLevels,
                                                    static_cast<uint8_t*>(mapped) + fps[s].Offset,
                                                    fps[s].Footprint.RowPitch);
        }
        upload->Unmap(0, nullptr);
        // The state recorded by the title's latest barrier; these textures are
        // created ready for sampling and are not transitioned by the title.
        const auto state = static_cast<D3D12_RESOURCE_STATES>(tex->tracked_state.load(std::memory_order_relaxed));
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex->pc_res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        if (state != D3D12_RESOURCE_STATE_COPY_DEST) list->ResourceBarrier(1, &b);
        for (const UINT s : dirty) {
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = tex->pc_res;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = s;
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = fps[s];
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        if (state != D3D12_RESOURCE_STATE_COPY_DEST) list->ResourceBarrier(1, &b);
        if (SUCCEEDED(list->Close())) {
            ID3D12CommandList* lists[] = {list};
            queue->ExecuteCommandLists(1, lists);
            // The temporaries below must outlive the copy: block until it has
            // run (a null event makes SetEventOnCompletion wait; a removed
            // device completes its fences).
            if (SUCCEEDED(queue->Signal(fence, 1))) fence->SetEventOnCompletion(1, nullptr);
        }
    }
    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (upload) upload->Release();
    device->Release();
}

struct GuestTextureUpload {
    XboxD3D12ResourceProxy* tex = nullptr;
    std::vector<UINT> dirty;
    D3D12_RESOURCE_DESC desc{};
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fps;
    uint64_t offset = 0;
    uint64_t size = 0;
};

// Uploads all dirty textures in one command list and one fence wait. The
// per-texture version used to allocate, submit and block once per texture,
// which turned a handful of streamed textures into a long CPU stall.
void UploadGuestTextureBatch(ID3D12CommandQueue* queue,
                             std::vector<GuestTextureUpload>& uploads) {
    if (uploads.empty()) return;
    ID3D12Device* device = nullptr;
    if (FAILED(uploads.front().tex->pc_res->GetDevice(IID_PPV_ARGS(&device))) || !device) return;

    uint64_t total = 0;
    for (GuestTextureUpload& u : uploads) {
        u.desc = u.tex->pc_res->GetDesc();
        const UINT subresources = static_cast<UINT>(u.desc.MipLevels) * u.desc.DepthOrArraySize;
        u.fps.resize(subresources);
        u.offset = AlignUp(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        device->GetCopyableFootprints(&u.desc, 0, subresources, 0, u.fps.data(), nullptr, nullptr,
                                      &u.size);
        total = u.offset + u.size;
    }
    if (total == 0) {
        device->Release();
        return;
    }
    g_upload_bytes.fetch_add(total, std::memory_order_relaxed);
    g_upload_textures.fetch_add(uploads.size(), std::memory_order_relaxed);

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* upload = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    void* mapped = nullptr;
    const bool ready =
        SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&upload))) &&
        SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&alloc))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                            IID_PPV_ARGS(&list))) &&
        SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) &&
        SUCCEEDED(upload->Map(0, nullptr, &mapped));

    if (ready) {
        for (const GuestTextureUpload& u : uploads) {
            const GuestTextureMemory& mem = *u.tex->guest_memory;
            for (const UINT s : u.dirty) {
                scarlettx::gpu::xg::CopyFromSubresource(
                    mem.layout, mem.base, s % u.desc.MipLevels, s / u.desc.MipLevels,
                    static_cast<uint8_t*>(mapped) + u.offset + u.fps[s].Offset,
                    u.fps[s].Footprint.RowPitch);
            }
        }
        upload->Unmap(0, nullptr);

        for (const GuestTextureUpload& u : uploads) {
            const auto state =
                static_cast<D3D12_RESOURCE_STATES>(u.tex->tracked_state.load(std::memory_order_relaxed));
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = u.tex->pc_res;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = state;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            if (state != D3D12_RESOURCE_STATE_COPY_DEST) list->ResourceBarrier(1, &b);
            for (const UINT s : u.dirty) {
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = u.fps[s];
                fp.Offset += u.offset;
                D3D12_TEXTURE_COPY_LOCATION dst = {};
                dst.pResource = u.tex->pc_res;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = s;
                D3D12_TEXTURE_COPY_LOCATION src = {};
                src.pResource = upload;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = fp;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            if (state != D3D12_RESOURCE_STATE_COPY_DEST) list->ResourceBarrier(1, &b);
        }

        if (SUCCEEDED(list->Close())) {
            ID3D12CommandList* lists[] = {list};
            queue->ExecuteCommandLists(1, lists);
            if (SUCCEEDED(queue->Signal(fence, 1))) fence->SetEventOnCompletion(1, nullptr);
        }
    }

    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (upload) upload->Release();
    device->Release();
}

// Uploads every guest-memory texture the title has written to since the last call.
void FlushGuestTextureWrites(ID3D12CommandQueue* queue) {
    std::lock_guard<std::mutex> lock(g_guest_textures_mutex);
    if (g_guest_textures.empty() || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    std::vector<GuestTextureUpload> uploads;
    std::vector<void*> pages;
    std::vector<UINT> dirty;
    for (XboxD3D12ResourceProxy* tex : g_guest_textures) {
        GuestTextureMemory& mem = *tex->guest_memory;
        pages.resize(mem.size / 4096 + 1);
        ULONG_PTR count = pages.size();
        DWORD granularity = 0;
        // Reading resets the watch: a write that lands during the copy is picked up next time.
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, mem.base, mem.size, pages.data(), &count, &granularity) != 0 ||
            count == 0) {
            continue;
        }
        pages.resize(count);
        // Only the subresources the title wrote to: the others may hold data
        // that arrived through command-list copies.
        const D3D12_RESOURCE_DESC d = tex->pc_res->GetDesc();
        dirty.clear();
        for (UINT s = 0; s < static_cast<UINT>(d.MipLevels) * d.DepthOrArraySize; ++s) {
            uint64_t offset = 0, size = 0;
            if (!scarlettx::gpu::xg::SubresourceRange(mem.layout, s % d.MipLevels, s / d.MipLevels, &offset, &size)) {
                continue;
            }
            const auto* first = static_cast<const uint8_t*>(mem.base) + offset;
            const bool written = std::any_of(pages.begin(), pages.end(), [&](void* page) {
                const auto* p = static_cast<const uint8_t*>(page);
                return p + granularity > first && p < first + size;
            });
            if (written) dirty.push_back(s);
        }
        if (dirty.empty()) continue;
        GuestTextureUpload u;
        u.tex = tex;
        u.dirty = dirty;
        uploads.push_back(std::move(u));
    }
    UploadGuestTextureBatch(queue, uploads);
    if (TraceEnabled() && !uploads.empty()) {
        std::cerr << "[ScarlettX][d3d12_xs] uploaded " << uploads.size()
                  << " guest texture(s) in one batch\n";
    }
}
#endif
intptr_t SCARLETTX_GDK_CALLBACK ResProxyQueryInterface(void* self, uintptr_t, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxD3D12ResourceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK ResProxyAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxD3D12ResourceProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}
intptr_t SCARLETTX_GDK_CALLBACK ResProxyRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxD3D12ResourceProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        ReleaseGuestTextureMemory(obj);
        if (obj->pc_res) obj->pc_res->Release();
#endif
        if (obj->device) (*static_cast<const ComVtableFn* const*>(obj->device))[2](obj->device, 0, 0, 0);
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}
// ID3D12DeviceChild::GetDevice (+0x38), observed at 0x14052510b in a title.
intptr_t SCARLETTX_GDK_CALLBACK ResProxyGetDevice(void* self, uintptr_t /*riid*/, uintptr_t ppDevice, uintptr_t) {
    auto* obj = static_cast<XboxD3D12ResourceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppDevice);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    *out = nullptr;
    if (!obj->device) return static_cast<intptr_t>(static_cast<int32_t>(0x80004005u));
    (*static_cast<const ComVtableFn* const*>(obj->device))[1](obj->device, 0, 0, 0);
    *out = obj->device;
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK ResProxySetName(void* self, uintptr_t name_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12ResourceProxy*>(self);
    if (obj->pc_res && name_ptr) return static_cast<intptr_t>(obj->pc_res->SetName(reinterpret_cast<LPCWSTR>(name_ptr)));
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK ResProxyMap(void* self, uintptr_t subres, uintptr_t pReadRange, uintptr_t ppData) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12ResourceProxy*>(self);
    if (obj->pc_res) {
        return static_cast<intptr_t>(obj->pc_res->Map(
            static_cast<UINT>(subres),
            reinterpret_cast<const D3D12_RANGE*>(pReadRange),
            reinterpret_cast<void**>(ppData)));
    }
#endif
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}
intptr_t SCARLETTX_GDK_CALLBACK ResProxyUnmap(void* self, uintptr_t subres, uintptr_t pWrittenRange, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12ResourceProxy*>(self);
    if (obj->pc_res) {
        obj->pc_res->Unmap(static_cast<UINT>(subres), reinterpret_cast<const D3D12_RANGE*>(pWrittenRange));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK ResProxyGetDesc(void* self, uintptr_t sret_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12ResourceProxy*>(self);
    if (obj->pc_res && sret_ptr) {
        *reinterpret_cast<D3D12_RESOURCE_DESC*>(sret_ptr) = obj->pc_res->GetDesc();
    }
#endif
    return static_cast<intptr_t>(sret_ptr);
}
intptr_t SCARLETTX_GDK_CALLBACK ResProxyGetGPUVirtualAddress(void* self, uintptr_t, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12ResourceProxy*>(self);
    if (obj->pc_res) {
        const auto va = obj->pc_res->GetGPUVirtualAddress();
        if (va != 0) return static_cast<intptr_t>(va);
        // Host textures have no address; hand out guest memory the title can fill.
        return reinterpret_cast<intptr_t>(EnsureGuestTextureMemory(obj));
    }
#endif
    return 0;
}
template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK ResProxyTracedSlot(void*, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Resource::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}
template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeResProxyVtable(std::index_sequence<Is...>) {
    return {&ResProxyQueryInterface, &ResProxyAddRef, &ResProxyRelease, &ResProxyTracedSlot<static_cast<int>(Is + 3)>...};
}
std::array<ComVtableFn, 24> BuildResProxyVtable() {
    auto vtbl = MakeResProxyVtable(std::make_index_sequence<21>{});
    vtbl[6] = &ResProxySetName;
    vtbl[7] = &ResProxyGetDevice;
    vtbl[8] = &ResProxyMap;
    vtbl[9] = &ResProxyUnmap;
    vtbl[10] = &ResProxyGetDesc;
    vtbl[11] = &ResProxyGetGPUVirtualAddress;
    return vtbl;
}
const auto kXboxResourceProxyVtbl = BuildResProxyVtable();

struct XboxD3D12GraphicsCommandListProxy {
    const ComVtableFn* lpVtbl;
    // Xbox d3d12_xs inline command-list fast-paths read/write cached state inside the
    // command-list object (e.g. +0x18/+0x30 cursor/end, +0x540/+0x550 cached PSO).
    // Keeping +0x08..+0x1007 zeroed ensures cursor >= end (0 >= 0) so inline fast-paths
    // safely fall through to the virtual vtable slot (e.g. vtable[25] SetPipelineState)
    // without corrupting proxy fields or faulting.
    uint8_t xbox_inline_cmdbuf[0x1000] = {};
    std::atomic<uint32_t> ref_count{1};
#if defined(_WIN32)
    ID3D12GraphicsCommandList* pc_list = nullptr;
    ID3D12Device* pc_device = nullptr;
    ID3D12Resource* scratch_buf = nullptr;
    uint64_t scratch_size = 0;
    std::vector<ID3D12Resource*> retired_scratch;
#endif
};
intptr_t SCARLETTX_GDK_CALLBACK CmdListQueryInterface(void* self, uintptr_t, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxD3D12GraphicsCommandListProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        if (obj->scratch_buf) obj->scratch_buf->Release();
        for (auto* r : obj->retired_scratch) {
            if (r) r->Release();
        }
        if (obj->pc_list) obj->pc_list->Release();
#endif
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetName(void* self, uintptr_t name_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list && name_ptr) return static_cast<intptr_t>(obj->pc_list->SetName(reinterpret_cast<LPCWSTR>(name_ptr)));
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListGetType(void* self, uintptr_t, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) return static_cast<intptr_t>(obj->pc_list->GetType());
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListClose(void* self, uintptr_t, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        const HRESULT hr = obj->pc_list->Close();
        if (TraceEnabled()) {
            std::cerr << "[ScarlettX][d3d12_xs] CmdList::Close(self=" << self << ") -> 0x"
                      << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
        }
        return static_cast<intptr_t>(hr);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListReset(void* self, uintptr_t pAlloc, uintptr_t pPso, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (!obj->pc_list || !pAlloc) return static_cast<intptr_t>(E_INVALIDARG);
    auto* alloc_proxy = reinterpret_cast<XboxD3D12CommandAllocatorProxy*>(pAlloc);
    ID3D12PipelineState* pc_pso = nullptr;
    if (pPso) {
        auto* pso_proxy = reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pPso);
        pc_pso = reinterpret_cast<ID3D12PipelineState*>(pso_proxy->pc_obj);
    }
    const HRESULT hr = obj->pc_list->Reset(alloc_proxy->pc_alloc, pc_pso);
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::Reset(self=" << self << ", alloc=" << alloc_proxy
                  << ", pso=" << reinterpret_cast<void*>(pPso) << ") -> 0x"
                  << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    return static_cast<intptr_t>(hr);
#else
    return 0;
#endif
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetDescriptorHeaps(void* self, uintptr_t num, uintptr_t ppHeaps, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (!obj->pc_list || !ppHeaps || num == 0) return 0;
    auto** guest_heaps = reinterpret_cast<XboxD3D12DescriptorHeapProxy**>(ppHeaps);
    std::vector<ID3D12DescriptorHeap*> pc_heaps;
    pc_heaps.reserve(num);
    for (uintptr_t i = 0; i < num; ++i) {
        if (guest_heaps[i] && guest_heaps[i]->pc_heap) pc_heaps.push_back(guest_heaps[i]->pc_heap);
    }
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::SetDescriptorHeaps(self=" << self
                  << ", num=" << num << ", pc_count=" << pc_heaps.size() << ")\n";
    }
    if (!pc_heaps.empty()) {
        obj->pc_list->SetDescriptorHeaps(static_cast<UINT>(pc_heaps.size()), pc_heaps.data());
    }
#endif
    return 0;
}
D3D12_RESOURCE_STATES SanitizePcResourceState(D3D12_RESOURCE_STATES state, D3D12_RESOURCE_DIMENSION dim) {
    uint32_t s = static_cast<uint32_t>(state);
    // A title built against SDK version 0x20004 keeps its display target in state 0x100000
    // between frames (created in it, back to it before PresentX). The host has no such state;
    // its equivalent is PRESENT, which is 0.
    s &= ~0x100000u;
    if (dim != D3D12_RESOURCE_DIMENSION_BUFFER) {
        // Buffer-only states are invalid on textures in PC D3D12 (Xbox passes 0x8c3 on textures).
        s &= ~(static_cast<uint32_t>(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) |
               static_cast<uint32_t>(D3D12_RESOURCE_STATE_INDEX_BUFFER) |
               static_cast<uint32_t>(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT) |
               static_cast<uint32_t>(D3D12_RESOURCE_STATE_STREAM_OUT));
    }
    return static_cast<D3D12_RESOURCE_STATES>(s);
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListResourceBarrier(void* self, uintptr_t num, uintptr_t pBarriers, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (!obj->pc_list || !pBarriers || num == 0) return 0;
    const auto* in = reinterpret_cast<const D3D12_RESOURCE_BARRIER*>(pBarriers);
    std::vector<D3D12_RESOURCE_BARRIER> translated;
    translated.reserve(static_cast<size_t>(num));
    for (size_t i = 0; i < static_cast<size_t>(num); ++i) {
        D3D12_RESOURCE_BARRIER b = in[i];
        if (TraceEnabled()) {
            std::cerr << "[ScarlettX][d3d12_xs] CmdList::ResourceBarrier(self=" << self
                      << ", i=" << i << "/" << num << ", type=" << b.Type << ", flags=0x"
                      << std::hex << b.Flags;
            if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                std::cerr << ", res=" << b.Transition.pResource << ", sub=" << b.Transition.Subresource
                          << ", before=0x" << b.Transition.StateBefore << ", after=0x" << b.Transition.StateAfter;
            }
            std::cerr << std::dec << ")\n";
        }
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.pResource) {
            auto* res_proxy = reinterpret_cast<XboxD3D12ResourceProxy*>(b.Transition.pResource);
            auto* pc_res = res_proxy->pc_res;
            if (!pc_res) continue;
            b.Transition.pResource = pc_res;
            const auto dim = pc_res->GetDesc().Dimension;
            b.Transition.StateBefore = SanitizePcResourceState(b.Transition.StateBefore, dim);
            b.Transition.StateAfter = SanitizePcResourceState(b.Transition.StateAfter, dim);
            res_proxy->tracked_state.store(static_cast<uint32_t>(b.Transition.StateAfter), std::memory_order_relaxed);
            if (b.Transition.StateBefore == b.Transition.StateAfter) continue;
            translated.push_back(b);
        } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
            if (b.UAV.pResource) {
                b.UAV.pResource = reinterpret_cast<XboxD3D12ResourceProxy*>(b.UAV.pResource)->pc_res;
                if (!b.UAV.pResource) continue;
            }
            translated.push_back(b);
        } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            if (b.Aliasing.pResourceBefore) b.Aliasing.pResourceBefore = reinterpret_cast<XboxD3D12ResourceProxy*>(b.Aliasing.pResourceBefore)->pc_res;
            if (b.Aliasing.pResourceAfter) b.Aliasing.pResourceAfter = reinterpret_cast<XboxD3D12ResourceProxy*>(b.Aliasing.pResourceAfter)->pc_res;
            translated.push_back(b);
        }
    }
    if (!translated.empty()) {
        obj->pc_list->ResourceBarrier(static_cast<UINT>(translated.size()), translated.data());
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListCopyBufferRegion(
    void* self, uintptr_t pDstBuf, uintptr_t dstOffset, uintptr_t pSrcBuf, uintptr_t srcOffset, uintptr_t numBytes) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (!obj || !obj->pc_list || !pDstBuf || !pSrcBuf) return 0;
    auto* dst = reinterpret_cast<XboxD3D12ResourceProxy*>(pDstBuf)->pc_res;
    auto* src = reinterpret_cast<XboxD3D12ResourceProxy*>(pSrcBuf)->pc_res;
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::CopyBufferRegion(self=" << self
                  << ", dst=" << reinterpret_cast<void*>(pDstBuf) << "+" << dstOffset
                  << ", src=" << reinterpret_cast<void*>(pSrcBuf) << "+" << srcOffset
                  << ", bytes=" << numBytes << ")\n";
    }
    if (dst && src) {
        obj->pc_list->CopyBufferRegion(dst, dstOffset, src, srcOffset, numBytes);
    }
#endif
    return 0;
}
std::atomic<uint64_t> g_copy_texture_count{0};

intptr_t SCARLETTX_GDK_CALLBACK CmdListCopyTextureRegion(
    void* self, uintptr_t pDst, uintptr_t dstX, uintptr_t dstY, uintptr_t dstZ, uintptr_t pSrc, uintptr_t pSrcBox) {
    g_copy_texture_count.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (!obj || !obj->pc_list || !pDst || !pSrc) return 0;
    D3D12_TEXTURE_COPY_LOCATION dst = *reinterpret_cast<const D3D12_TEXTURE_COPY_LOCATION*>(pDst);
    D3D12_TEXTURE_COPY_LOCATION src = *reinterpret_cast<const D3D12_TEXTURE_COPY_LOCATION*>(pSrc);
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::CopyTextureRegion(self=" << self
                  << ", dstRes=" << dst.pResource << "(type=" << dst.Type << ",sub=" << dst.SubresourceIndex
                  << "), xyz=(" << static_cast<UINT>(dstX) << "," << static_cast<UINT>(dstY) << "," << static_cast<UINT>(dstZ)
                  << "), srcRes=" << src.pResource << "(type=" << src.Type;
        if (src.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
            const auto& fp = src.PlacedFootprint;
            std::cerr << ",off=" << fp.Offset << ",fmt=" << fp.Footprint.Format
                      << ",w=" << fp.Footprint.Width << ",h=" << fp.Footprint.Height
                      << ",d=" << fp.Footprint.Depth << ",pitch=" << fp.Footprint.RowPitch;
        } else {
            std::cerr << ",sub=" << src.SubresourceIndex;
        }
        std::cerr << "), box=" << reinterpret_cast<void*>(pSrcBox) << ")\n";
    }
    if (dst.pResource) dst.pResource = reinterpret_cast<XboxD3D12ResourceProxy*>(dst.pResource)->pc_res;
    if (src.pResource) src.pResource = reinterpret_cast<XboxD3D12ResourceProxy*>(src.pResource)->pc_res;
    if (dst.pResource && src.pResource) {
        if (src.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT &&
            (src.PlacedFootprint.Offset & 511ULL) != 0 && obj->pc_device) {
            const auto& fp = src.PlacedFootprint;
            const uint32_t fmt = static_cast<uint32_t>(fp.Footprint.Format);
            const bool is_bc = (fmt >= 70u && fmt <= 84u) || (fmt >= 94u && fmt <= 99u);
            const uint64_t rows = is_bc ? ((static_cast<uint64_t>(fp.Footprint.Height) + 3ULL) / 4ULL)
                                        : static_cast<uint64_t>(fp.Footprint.Height);
            const uint64_t depth = std::max<uint64_t>(1ULL, fp.Footprint.Depth);
            uint64_t copy_bytes = rows * depth * static_cast<uint64_t>(fp.Footprint.RowPitch);
            const D3D12_RESOURCE_DESC src_desc = src.pResource->GetDesc();
            if (fp.Offset < src_desc.Width) {
                copy_bytes = std::min<uint64_t>(copy_bytes, src_desc.Width - fp.Offset);
            }
            if (copy_bytes > obj->scratch_size) {
                const uint64_t alloc_size = std::max<uint64_t>(65536ULL, (copy_bytes + 65535ULL) & ~65535ULL);
                D3D12_HEAP_PROPERTIES hp{};
                hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC bd{};
                bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bd.Width = alloc_size;
                bd.Height = 1;
                bd.DepthOrArraySize = 1;
                bd.MipLevels = 1;
                bd.SampleDesc.Count = 1;
                bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                ID3D12Resource* new_buf = nullptr;
                if (SUCCEEDED(obj->pc_device->CreateCommittedResource(
                        &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                        nullptr, IID_PPV_ARGS(&new_buf))) && new_buf) {
                    if (obj->scratch_buf) obj->retired_scratch.push_back(obj->scratch_buf);
                    obj->scratch_buf = new_buf;
                    obj->scratch_size = alloc_size;
                }
            }
            if (obj->scratch_buf && copy_bytes > 0 && copy_bytes <= obj->scratch_size) {
                obj->pc_list->CopyBufferRegion(obj->scratch_buf, 0, src.pResource, fp.Offset, copy_bytes);
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = obj->scratch_buf;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                obj->pc_list->ResourceBarrier(1, &b);
                D3D12_TEXTURE_COPY_LOCATION aligned_src = src;
                aligned_src.pResource = obj->scratch_buf;
                aligned_src.PlacedFootprint.Offset = 0;
                obj->pc_list->CopyTextureRegion(
                    &dst, static_cast<UINT>(dstX), static_cast<UINT>(dstY), static_cast<UINT>(dstZ),
                    &aligned_src, reinterpret_cast<const D3D12_BOX*>(pSrcBox));
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                obj->pc_list->ResourceBarrier(1, &b);
                return 0;
            }
        }
        obj->pc_list->CopyTextureRegion(
            &dst, static_cast<UINT>(dstX), static_cast<UINT>(dstY), static_cast<UINT>(dstZ),
            &src, reinterpret_cast<const D3D12_BOX*>(pSrcBox));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListClearState(void* self, uintptr_t pPso, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        ID3D12PipelineState* pc_pso = nullptr;
        if (pPso) {
            auto* pso = reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pPso);
            pc_pso = reinterpret_cast<ID3D12PipelineState*>(pso->pc_obj);
        }
        obj->pc_list->ClearState(pc_pso);
    }
#endif
    return 0;
}
std::atomic<uint64_t> g_draw_instanced_count{0};
std::atomic<uint64_t> g_draw_indexed_count{0};
std::atomic<uint64_t> g_dispatch_count{0};
std::atomic<uint64_t> g_clear_rtv_count{0};
std::atomic<uint64_t> g_om_set_rt_count{0};
std::atomic<uint64_t> g_set_pso_count{0};
std::atomic<uint64_t> g_set_grs_count{0};
std::atomic<uint64_t> g_set_gdt_count{0};
std::atomic<uint64_t> g_set_vb_count{0};
std::atomic<uint64_t> g_set_ib_count{0};
std::atomic<uint64_t> g_set_vp_count{0};
std::atomic<uint64_t> g_submit_count{0};
std::atomic<uint64_t> g_flush_count{0};
std::atomic<uint64_t> g_flush_us{0};
std::atomic<uint64_t> g_frame_epoch{0};
std::atomic<uint64_t> g_flushed_epoch{~0ull};

bool StatsEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("SCARLETTX_STATS");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

void MaybePrintStats(uint64_t frame) {
    if (!StatsEnabled() || frame == 0) return;
    static LARGE_INTEGER last_time{};
    static uint64_t last_frame = 0;
    static uint64_t last_draw = 0, last_dispatch = 0, last_pso = 0, last_grs = 0,
                    last_gdt = 0, last_copy = 0, last_submit = 0, last_flush = 0,
                    last_flush_us = 0, last_bytes = 0;
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (last_time.QuadPart != 0) {
        const double seconds = static_cast<double>(now.QuadPart - last_time.QuadPart) /
                               static_cast<double>(freq.QuadPart);
        if (seconds < 1.0) return;
        const uint64_t frames = frame - last_frame;
        const double f = frames ? static_cast<double>(frames) : 1.0;
        const uint64_t draw = g_draw_instanced_count.load() + g_draw_indexed_count.load();
        const uint64_t dispatch = g_dispatch_count.load();
        const uint64_t pso = g_set_pso_count.load();
        const uint64_t grs = g_set_grs_count.load();
        const uint64_t gdt = g_set_gdt_count.load();
        const uint64_t copy = g_copy_texture_count.load();
        const uint64_t submit = g_submit_count.load();
        const uint64_t flush = g_flush_count.load();
        const uint64_t flush_us = g_flush_us.load();
        const uint64_t bytes = g_upload_bytes.load();
        std::fprintf(stderr,
                     "[STATS] fps=%.1f draws/f=%.1f disp/f=%.1f pso/f=%.1f grs/f=%.1f "
                     "gdt/f=%.1f copytex/f=%.1f submit/f=%.1f flush/f=%.2f "
                     "flush_ms/f=%.2f uploadMB/f=%.2f\n",
                     frames / seconds, (draw - last_draw) / f, (dispatch - last_dispatch) / f,
                     (pso - last_pso) / f, (grs - last_grs) / f, (gdt - last_gdt) / f,
                     (copy - last_copy) / f, (submit - last_submit) / f,
                     (flush - last_flush) / f, (flush_us - last_flush_us) / 1000.0 / f,
                     (bytes - last_bytes) / 1048576.0 / f);
        std::fflush(stderr);
        last_draw = draw; last_dispatch = dispatch; last_pso = pso; last_grs = grs;
        last_gdt = gdt; last_copy = copy; last_submit = submit; last_flush = flush;
        last_flush_us = flush_us; last_bytes = bytes;
        last_frame = frame;
        last_time = now;
        return;
    }
    last_time = now;
    last_frame = frame;
    last_draw = g_draw_instanced_count.load() + g_draw_indexed_count.load();
    last_dispatch = g_dispatch_count.load();
    last_pso = g_set_pso_count.load();
    last_grs = g_set_grs_count.load();
    last_gdt = g_set_gdt_count.load();
    last_copy = g_copy_texture_count.load();
    last_submit = g_submit_count.load();
    last_flush = g_flush_count.load();
    last_flush_us = g_flush_us.load();
    last_bytes = g_upload_bytes.load();
}

intptr_t SCARLETTX_GDK_CALLBACK CmdListDrawInstanced(
    void* self, uintptr_t vCount, uintptr_t iCount, uintptr_t startV, uintptr_t startI) {
    g_draw_instanced_count.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        obj->pc_list->DrawInstanced(static_cast<UINT>(vCount), static_cast<UINT>(iCount),
                                    static_cast<UINT>(startV), static_cast<UINT>(startI));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListDrawIndexedInstanced(
    void* self, uintptr_t idxCount, uintptr_t instCount, uintptr_t startIdx,
    uintptr_t baseVertex, uintptr_t startInst) {
    const uint64_t n = g_draw_indexed_count.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (TraceEnabled() && n <= 5) {
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::DrawIndexedInstanced(#" << n
                  << ", self=" << self << ", idx=" << idxCount << ", inst=" << instCount
                  << ", startIdx=" << startIdx << ", baseV=" << static_cast<int64_t>(baseVertex)
                  << ", startI=" << startInst
                  << ", pso_cnt=" << g_set_pso_count.load(std::memory_order_relaxed)
                  << ", grs_cnt=" << g_set_grs_count.load(std::memory_order_relaxed)
                  << ", gdt_cnt=" << g_set_gdt_count.load(std::memory_order_relaxed)
                  << ", vb_cnt=" << g_set_vb_count.load(std::memory_order_relaxed)
                  << ", ib_cnt=" << g_set_ib_count.load(std::memory_order_relaxed)
                  << ", vp_cnt=" << g_set_vp_count.load(std::memory_order_relaxed) << ")\n";
        // Check if Xbox inline cmdbuf has non-zero qwords written by guest fast-paths
        const auto* qw = reinterpret_cast<const uint64_t*>(obj->xbox_inline_cmdbuf);
        for (size_t i = 0; i < 0x1000 / 8; ++i) {
            if (qw[i] != 0) {
                std::cerr << "  [inline_cmdbuf +0x" << std::hex << (8 + i * 8)
                          << "] = 0x" << qw[i] << std::dec << "\n";
            }
        }
    }
    if (obj->pc_list) {
        obj->pc_list->DrawIndexedInstanced(static_cast<UINT>(idxCount), static_cast<UINT>(instCount),
                                           static_cast<UINT>(startIdx), static_cast<INT>(baseVertex),
                                           static_cast<UINT>(startInst));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListDispatch(void* self, uintptr_t x, uintptr_t y, uintptr_t z) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        obj->pc_list->Dispatch(static_cast<UINT>(x), static_cast<UINT>(y), static_cast<UINT>(z));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListCopyResource(void* self, uintptr_t pDst, uintptr_t pSrc, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list && pDst && pSrc) {
        auto* dst = reinterpret_cast<XboxD3D12ResourceProxy*>(pDst);
        auto* src = reinterpret_cast<XboxD3D12ResourceProxy*>(pSrc);
        if (dst->pc_res && src->pc_res) {
            obj->pc_list->CopyResource(dst->pc_res, src->pc_res);
        }
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListIASetPrimitiveTopology(void* self, uintptr_t topo, uintptr_t, uintptr_t) {
    static std::atomic<uint64_t> topo_cnt{0};
    const uint64_t n = topo_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (TraceEnabled() && n <= 8) {
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::IASetPrimitiveTopology(#" << n
                  << ", self=" << self << ", topo=" << topo << ")\n";
    }
    if (obj->pc_list) {
        obj->pc_list->IASetPrimitiveTopology(static_cast<D3D12_PRIMITIVE_TOPOLOGY>(topo));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListRSSetViewports(void* self, uintptr_t num, uintptr_t pViewports, uintptr_t) {
    const uint64_t n = g_set_vp_count.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list && pViewports && num > 0) {
        auto* vp = reinterpret_cast<const D3D12_VIEWPORT*>(pViewports);
        if (TraceEnabled() && n <= 4) {
            std::cerr << "[ScarlettX][d3d12_xs] CmdList::RSSetViewports(#" << n
                      << ", num=" << num << ", vp0=(" << vp[0].TopLeftX << "," << vp[0].TopLeftY
                      << "," << vp[0].Width << "x" << vp[0].Height
                      << ", z=" << vp[0].MinDepth << ".." << vp[0].MaxDepth << "))\n";
        }
        obj->pc_list->RSSetViewports(static_cast<UINT>(num), vp);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListRSSetScissorRects(void* self, uintptr_t num, uintptr_t pRects, uintptr_t) {
    static std::atomic<uint64_t> sc_cnt{0};
    const uint64_t n = sc_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list && pRects && num > 0) {
        auto* r = reinterpret_cast<const D3D12_RECT*>(pRects);
        if (TraceEnabled() && n <= 4) {
            std::cerr << "[ScarlettX][d3d12_xs] CmdList::RSSetScissorRects(#" << n
                      << ", num=" << num << ", r0=(" << r[0].left << "," << r[0].top
                      << "," << r[0].right << "," << r[0].bottom << "))\n";
        }
        obj->pc_list->RSSetScissorRects(static_cast<UINT>(num), r);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListOMSetBlendFactor(void* self, uintptr_t pFactor, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        obj->pc_list->OMSetBlendFactor(reinterpret_cast<const FLOAT*>(pFactor));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListOMSetStencilRef(void* self, uintptr_t stencilRef, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        obj->pc_list->OMSetStencilRef(static_cast<UINT>(stencilRef));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetPipelineState(void* self, uintptr_t pPso, uintptr_t, uintptr_t) {
    g_set_pso_count.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list && pPso) {
        auto* pso = reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pPso);
        if (pso->pc_obj) {
            obj->pc_list->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(pso->pc_obj));
        }
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetComputeRootSignature(void* self, uintptr_t pRs, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        ID3D12RootSignature* pc_rs = nullptr;
        if (pRs) {
            auto* rs = reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pRs);
            pc_rs = reinterpret_cast<ID3D12RootSignature*>(rs->pc_obj);
        }
        obj->pc_list->SetComputeRootSignature(pc_rs);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetGraphicsRootSignature(void* self, uintptr_t pRs, uintptr_t, uintptr_t) {
    g_set_grs_count.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        ID3D12RootSignature* pc_rs = nullptr;
        if (pRs) {
            auto* rs = reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pRs);
            pc_rs = reinterpret_cast<ID3D12RootSignature*>(rs->pc_obj);
        }
        obj->pc_list->SetGraphicsRootSignature(pc_rs);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetComputeRootDescriptorTable(
    void* self, uintptr_t rootParamIdx, uintptr_t baseDesc, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        D3D12_GPU_DESCRIPTOR_HANDLE h{HostGpuHandle(baseDesc)};
        obj->pc_list->SetComputeRootDescriptorTable(static_cast<UINT>(rootParamIdx), h);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetGraphicsRootDescriptorTable(
    void* self, uintptr_t rootParamIdx, uintptr_t baseDesc, uintptr_t) {
    const uint64_t n = g_set_gdt_count.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (TraceEnabled() && n <= 8) {
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::SetGraphicsRootDescTable(#" << n
                  << ", param=" << rootParamIdx << ", gpu=0x" << std::hex << baseDesc
                  << ", ret=" << __builtin_return_address(0) << std::dec << ")\n";
    }
    if (rootParamIdx >= 64 || baseDesc == 0) {
        // A real root signature has at most 64 parameters; anything else is
        // a slot mismatch. Forwarding it crashes the host driver.
        return 0;
    }
    if (obj->pc_list) {
        D3D12_GPU_DESCRIPTOR_HANDLE h{HostGpuHandle(baseDesc)};
        obj->pc_list->SetGraphicsRootDescriptorTable(static_cast<UINT>(rootParamIdx), h);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetComputeRoot32BitConstant(
    void* self, uintptr_t rootParamIdx, uintptr_t srcData, uintptr_t destOffset) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        obj->pc_list->SetComputeRoot32BitConstant(
            static_cast<UINT>(rootParamIdx), static_cast<UINT>(srcData), static_cast<UINT>(destOffset));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetGraphicsRoot32BitConstant(
    void* self, uintptr_t rootParamIdx, uintptr_t srcData, uintptr_t destOffset) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        obj->pc_list->SetGraphicsRoot32BitConstant(
            static_cast<UINT>(rootParamIdx), static_cast<UINT>(srcData), static_cast<UINT>(destOffset));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetComputeRoot32BitConstants(
    void* self, uintptr_t rootParamIdx, uintptr_t numVals, uintptr_t pSrcData, uintptr_t destOffset) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list && pSrcData) {
        obj->pc_list->SetComputeRoot32BitConstants(
            static_cast<UINT>(rootParamIdx), static_cast<UINT>(numVals),
            reinterpret_cast<const void*>(pSrcData), static_cast<UINT>(destOffset));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetGraphicsRoot32BitConstants(
    void* self, uintptr_t rootParamIdx, uintptr_t numVals, uintptr_t pSrcData, uintptr_t destOffset) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list && pSrcData) {
        obj->pc_list->SetGraphicsRoot32BitConstants(
            static_cast<UINT>(rootParamIdx), static_cast<UINT>(numVals),
            reinterpret_cast<const void*>(pSrcData), static_cast<UINT>(destOffset));
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetComputeRootCBV(void* self, uintptr_t idx, uintptr_t loc, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) obj->pc_list->SetComputeRootConstantBufferView(static_cast<UINT>(idx), HostGpuVa(loc));
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetGraphicsRootCBV(void* self, uintptr_t idx, uintptr_t loc, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) obj->pc_list->SetGraphicsRootConstantBufferView(static_cast<UINT>(idx), HostGpuVa(loc));
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetComputeRootSRV(void* self, uintptr_t idx, uintptr_t loc, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) obj->pc_list->SetComputeRootShaderResourceView(static_cast<UINT>(idx), HostGpuVa(loc));
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetGraphicsRootSRV(void* self, uintptr_t idx, uintptr_t loc, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) obj->pc_list->SetGraphicsRootShaderResourceView(static_cast<UINT>(idx), HostGpuVa(loc));
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetComputeRootUAV(void* self, uintptr_t idx, uintptr_t loc, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) obj->pc_list->SetComputeRootUnorderedAccessView(static_cast<UINT>(idx), HostGpuVa(loc));
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListSetGraphicsRootUAV(void* self, uintptr_t idx, uintptr_t loc, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) obj->pc_list->SetGraphicsRootUnorderedAccessView(static_cast<UINT>(idx), HostGpuVa(loc));
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListIASetIndexBuffer(void* self, uintptr_t pView, uintptr_t, uintptr_t) {
    const uint64_t n = g_set_ib_count.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (TraceEnabled() && n <= 4 && pView) {
        auto* ib = reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(pView);
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::IASetIndexBuffer(#" << n
                  << ", loc=0x" << std::hex << ib->BufferLocation << std::dec
                  << ", size=" << ib->SizeInBytes << ", fmt=" << ib->Format << ")\n";
    }
    if (obj->pc_list) {
        if (pView) {
            D3D12_INDEX_BUFFER_VIEW ib = *reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(pView);
            ib.BufferLocation = HostGpuVa(ib.BufferLocation);
            obj->pc_list->IASetIndexBuffer(&ib);
        } else {
            obj->pc_list->IASetIndexBuffer(nullptr);
        }
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListIASetVertexBuffers(
    void* self, uintptr_t startSlot, uintptr_t numViews, uintptr_t pViews) {
    const uint64_t n = g_set_vb_count.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (TraceEnabled() && n <= 4 && pViews && numViews > 0) {
        auto* vb = reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(pViews);
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::IASetVertexBuffers(#" << n
                  << ", start=" << startSlot << ", num=" << numViews
                  << ", vb0=(loc=0x" << std::hex << vb[0].BufferLocation << std::dec
                  << ", size=" << vb[0].SizeInBytes << ", stride=" << vb[0].StrideInBytes << "))\n";
    }
    if (obj->pc_list) {
        const auto* gv = reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(pViews);
        std::vector<D3D12_VERTEX_BUFFER_VIEW> views(gv ? static_cast<size_t>(numViews) : 0);
        for (size_t i = 0; i < views.size(); ++i) {
            views[i] = gv[i];
            views[i].BufferLocation = HostGpuVa(gv[i].BufferLocation);
        }
        obj->pc_list->IASetVertexBuffers(static_cast<UINT>(startSlot), static_cast<UINT>(numViews),
                                         gv ? views.data() : nullptr);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListOMSetRenderTargets(
    void* self, uintptr_t numRTVs, uintptr_t pRTVs, uintptr_t singleHandle, uintptr_t pDSV) {
    const uint64_t n = g_om_set_rt_count.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (TraceEnabled() && n <= 6) {
        uintptr_t rtv0 = (pRTVs && numRTVs > 0) ? reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(pRTVs)[0].ptr : 0;
        uintptr_t dsv0 = pDSV ? reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(pDSV)->ptr : 0;
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::OMSetRenderTargets(#" << n
                  << ", num=" << numRTVs << ", rtv0=0x" << std::hex << rtv0
                  << ", single=" << singleHandle << ", dsv=0x" << dsv0 << std::dec << ")\n";
    }
    if (obj->pc_list) {
        // A single handle stands for a contiguous range, so translating its
        // start is enough; otherwise each entry is translated.
        const auto* rtvs = reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(pRTVs);
        D3D12_CPU_DESCRIPTOR_HANDLE host_rtvs[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        const UINT n_rtv = static_cast<UINT>(std::min<uintptr_t>(numRTVs, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT));
        const UINT n_copy = (singleHandle != 0) ? (n_rtv ? 1u : 0u) : n_rtv;
        for (UINT i = 0; rtvs && i < n_copy; ++i) host_rtvs[i].ptr = HostCpuHandle(rtvs[i].ptr);
        D3D12_CPU_DESCRIPTOR_HANDLE host_dsv{};
        if (pDSV) host_dsv.ptr = HostCpuHandle(reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(pDSV)->ptr);
        obj->pc_list->OMSetRenderTargets(
            n_rtv,
            rtvs ? host_rtvs : nullptr,
            static_cast<BOOL>(singleHandle != 0),
            pDSV ? &host_dsv : nullptr);
    }
#endif
    return 0;
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListClearDepthStencilView(
    void* self, uintptr_t dsv, uintptr_t flags, uintptr_t depthBits, uintptr_t stencil,
    uintptr_t numRects, uintptr_t pRects) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        D3D12_CPU_DESCRIPTOR_HANDLE h{HostCpuHandle(dsv)};
        // Note: on Win64 ABI float arg3 is passed in XMM3; use ClearDepthStencilView with
        // depth=1.0f fallback unless we capture XMM3 in a typed signature below.
        (void)depthBits;
        obj->pc_list->ClearDepthStencilView(
            h, static_cast<D3D12_CLEAR_FLAGS>(flags), 1.0f, static_cast<UINT8>(stencil),
            static_cast<UINT>(numRects), reinterpret_cast<const D3D12_RECT*>(pRects));
    }
#endif
    return 0;
}
void SCARLETTX_GDK_CALLBACK CmdListClearDepthStencilViewTyped(
    void* self, uintptr_t dsv, uint32_t flags, float depth, uint8_t stencil,
    uint32_t numRects, const void* pRects) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (obj->pc_list) {
        D3D12_CPU_DESCRIPTOR_HANDLE h{HostCpuHandle(dsv)};
        obj->pc_list->ClearDepthStencilView(
            h, static_cast<D3D12_CLEAR_FLAGS>(flags), depth, stencil,
            static_cast<UINT>(numRects), reinterpret_cast<const D3D12_RECT*>(pRects));
    }
#endif
}
void SCARLETTX_GDK_CALLBACK CmdListClearRenderTargetViewTyped(
    void* self, uintptr_t rtv, const float* colorRgba, uint32_t numRects, const void* pRects) {
    const uint64_t n = g_clear_rtv_count.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12GraphicsCommandListProxy*>(self);
    if (TraceEnabled() && n <= 5 && colorRgba) {
        std::cerr << "[ScarlettX][d3d12_xs] CmdList::ClearRTV(#" << n
                  << ", rtv=0x" << std::hex << rtv << std::dec
                  << ", rgba=(" << colorRgba[0] << "," << colorRgba[1] << "," << colorRgba[2] << "," << colorRgba[3]
                  << "), rects=" << numRects << ")\n";
    }
    if (obj->pc_list && colorRgba) {
        D3D12_CPU_DESCRIPTOR_HANDLE h{HostCpuHandle(rtv)};
        obj->pc_list->ClearRenderTargetView(
            h, colorRgba, static_cast<UINT>(numRects), reinterpret_cast<const D3D12_RECT*>(pRects));
    }
#endif
}
intptr_t SCARLETTX_GDK_CALLBACK CmdListNoop(void*, uintptr_t, uintptr_t, uintptr_t) { return 0; }
template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK CmdListTracedSlot(void*, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::ostringstream line;
        line << "[ScarlettX][d3d12_xs] XboxID3D12GraphicsCommandList::vtable[" << Slot
             << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
             << ", caller=0x" << reinterpret_cast<uintptr_t>(SCARLETTX_RETURN_ADDRESS())
             << std::dec << ") -> E_NOTIMPL";
#if defined(_WIN32)
        // Unknown slot: show what a pointer argument refers to, to help identify it.
        MEMORY_BASIC_INFORMATION mbi = {};
        if (a1 > 0x10000 && VirtualQuery(reinterpret_cast<void*>(a1), &mbi, sizeof(mbi)) &&
            mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            a1 + 0x80 <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
            line << " [a1:" << std::hex;
            for (int i = 0; i < 16; ++i) {
                uint64_t q = 0;
                std::memcpy(&q, reinterpret_cast<const void*>(a1 + i * 8), 8);
                line << ' ' << q;
            }
            line << std::dec << ']';
        }
#endif
        line << "\n";
        std::cerr << line.str();
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}
// Retail titles call Xbox-only methods past slot 128 (e.g. +0x418/+0x420);
// a short table makes those calls jump through unrelated memory.
constexpr size_t kCmdListSlots = 256;
template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeCmdListVtable(std::index_sequence<Is...>) {
    return {&CmdListQueryInterface, &CmdListAddRef, &CmdListRelease, &CmdListTracedSlot<static_cast<int>(Is + 3)>...};
}
std::array<ComVtableFn, kCmdListSlots> BuildCmdListVtable() {
    auto vtbl = MakeCmdListVtable(std::make_index_sequence<kCmdListSlots - 3>{});
    vtbl[6] = &CmdListSetName;
    vtbl[8] = &CmdListGetType;
    vtbl[9] = &CmdListClose;
    vtbl[10] = &CmdListReset;
    vtbl[11] = &CmdListClearState;
    vtbl[12] = reinterpret_cast<ComVtableFn>(&CmdListDrawInstanced);
    vtbl[13] = reinterpret_cast<ComVtableFn>(&CmdListDrawIndexedInstanced);
    vtbl[14] = &CmdListDispatch;
    vtbl[15] = reinterpret_cast<ComVtableFn>(&CmdListCopyBufferRegion);
    vtbl[16] = reinterpret_cast<ComVtableFn>(&CmdListCopyTextureRegion);
    // Slot 83 (+0x298) is CopyTextureRegionX: the argument list of slot 16
    // plus copy flags (0 at every observed call). Titles upload textures
    // with it.
    vtbl[83] = reinterpret_cast<ComVtableFn>(&CmdListCopyTextureRegion);
    vtbl[17] = &CmdListCopyResource;
    vtbl[20] = &CmdListIASetPrimitiveTopology;
    vtbl[21] = &CmdListRSSetViewports;
    vtbl[22] = &CmdListRSSetScissorRects;
    vtbl[23] = &CmdListOMSetBlendFactor;
    vtbl[24] = &CmdListOMSetStencilRef;
    vtbl[25] = &CmdListSetPipelineState;
    vtbl[26] = &CmdListResourceBarrier;
    // Xbox d3d12_xs shifts PC slots >= 28 by +4 (verified from Concept Destruction traces).
    vtbl[32] = &CmdListSetDescriptorHeaps;
    vtbl[33] = &CmdListSetComputeRootSignature;
    vtbl[34] = &CmdListSetGraphicsRootSignature;
    vtbl[35] = &CmdListSetComputeRootDescriptorTable;
    vtbl[36] = &CmdListSetGraphicsRootDescriptorTable;
    vtbl[37] = &CmdListSetComputeRoot32BitConstant;
    vtbl[38] = &CmdListSetGraphicsRoot32BitConstant;
    vtbl[39] = reinterpret_cast<ComVtableFn>(&CmdListSetComputeRoot32BitConstants);
    vtbl[40] = reinterpret_cast<ComVtableFn>(&CmdListSetGraphicsRoot32BitConstants);
    vtbl[41] = &CmdListSetComputeRootCBV;
    vtbl[42] = &CmdListSetGraphicsRootCBV;
    vtbl[43] = &CmdListSetComputeRootSRV;
    vtbl[44] = &CmdListSetGraphicsRootSRV;
    vtbl[45] = &CmdListSetComputeRootUAV;
    vtbl[46] = &CmdListSetGraphicsRootUAV;
    vtbl[47] = &CmdListIASetIndexBuffer;
    vtbl[48] = &CmdListIASetVertexBuffers;
    vtbl[50] = reinterpret_cast<ComVtableFn>(&CmdListOMSetRenderTargets);
    vtbl[51] = reinterpret_cast<ComVtableFn>(&CmdListClearDepthStencilViewTyped);
    vtbl[52] = reinterpret_cast<ComVtableFn>(&CmdListClearRenderTargetViewTyped);
    vtbl[60] = &CmdListNoop;  // SetMarker
    vtbl[61] = &CmdListNoop;  // BeginEvent
    vtbl[62] = &CmdListNoop;  // EndEvent
    return vtbl;
}
const auto kXboxCmdListProxyVtbl = BuildCmdListVtable();
// A later SDK (D3D12XboxCreateDevice version 0x20004) added two methods at
// slots 59 and 60, so each method after them sits two slots further on:
// CopyTextureRegionX is slot 85. Checked against the method names in the
// debug driver each SDK's titles ship with.
std::array<ComVtableFn, kCmdListSlots> BuildCmdListVtableV4() {
    const auto older = BuildCmdListVtable();
    const auto unnamed = MakeCmdListVtable(std::make_index_sequence<kCmdListSlots - 3>{});
    auto vtbl = older;
    for (size_t slot = 59; slot < vtbl.size(); ++slot) {
        const bool named = slot >= 61 && older[slot - 2] != unnamed[slot - 2];
        vtbl[slot] = named ? older[slot - 2] : unnamed[slot];
    }
    return vtbl;
}
const auto kXboxCmdListProxyVtblV4 = BuildCmdListVtableV4();
std::array<ComVtableFn, kCmdListSlots> BuildDmaCmdListVtable() {
    auto vtbl = MakeCmdListVtable(std::make_index_sequence<kCmdListSlots - 3>{});
    vtbl[6] = &CmdListSetName;
    vtbl[8] = &CmdListGetType;
    vtbl[9] = &CmdListClose;
    vtbl[10] = &CmdListReset;
    vtbl[11] = reinterpret_cast<ComVtableFn>(&CmdListCopyBufferRegion);   // +0x58
    vtbl[12] = reinterpret_cast<ComVtableFn>(&CmdListCopyTextureRegion);  // +0x60
    vtbl[15] = &CmdListResourceBarrier;                                   // +0x78
    return vtbl;
}
const auto kXboxDmaCmdListProxyVtbl = BuildDmaCmdListVtable();

struct XboxD3D12CommandQueueProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
#if defined(_WIN32)
    ID3D12CommandQueue* pc_queue = nullptr;
    ID3D12Device* pc_device = nullptr;
    HWND present_hwnd = nullptr;
    HANDLE present_window_thread = nullptr;
    HANDLE present_window_ready = nullptr;
    IDXGISwapChain3* swapchain = nullptr;
    ID3D12CommandAllocator* present_alloc = nullptr;
    ID3D12GraphicsCommandList* present_list = nullptr;
    ID3D12Fence* present_fence = nullptr;
    HANDLE present_event = nullptr;
    uint64_t present_fence_val = 0;
    UINT swap_w = 0;
    UINT swap_h = 0;
    DXGI_FORMAT swap_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool present_init_attempted = false;
#endif
};

#if defined(_WIN32)
void StopPresentWindow(XboxD3D12CommandQueueProxy* obj);
#endif

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    if (TraceEnabled() && riid) {
        const auto* g = reinterpret_cast<const uint32_t*>(riid);
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12CommandQueue::QueryInterface("
                  << std::hex << g[0] << "-" << g[1] << "-" << g[2] << "-" << g[3] << std::dec << ")\n";
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxD3D12CommandQueueProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        if (obj->present_event) CloseHandle(obj->present_event);
        if (obj->present_fence) obj->present_fence->Release();
        if (obj->present_list) obj->present_list->Release();
        if (obj->present_alloc) obj->present_alloc->Release();
        if (obj->swapchain) obj->swapchain->Release();
        StopPresentWindow(obj);
        if (obj->pc_device) obj->pc_device->Release();
        if (obj->pc_queue) obj->pc_queue->Release();
#endif
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxySetName(void* self, uintptr_t name_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    if (obj->pc_queue && name_ptr) {
        return static_cast<intptr_t>(obj->pc_queue->SetName(reinterpret_cast<LPCWSTR>(name_ptr)));
    }
#endif
    return 0;
}

template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK QueueProxyTracedSlot(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12CommandQueue::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}

template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeQueueProxyVtable(std::index_sequence<Is...>) {
    return {
        &QueueProxyQueryInterface,
        &QueueProxyAddRef,
        &QueueProxyRelease,
        &QueueProxyTracedSlot<static_cast<int>(Is + 3)>...,
    };
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyExecuteCommandLists(
    void* self, uintptr_t num, uintptr_t ppLists, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    if (!obj->pc_queue || !ppLists || num == 0) return 0;
    auto** guest_lists = reinterpret_cast<XboxD3D12GraphicsCommandListProxy**>(ppLists);
    std::vector<ID3D12CommandList*> pc_lists;
    pc_lists.reserve(num);
    for (uintptr_t i = 0; i < num; ++i) {
        if (guest_lists[i] && guest_lists[i]->pc_list) {
            pc_lists.push_back(guest_lists[i]->pc_list);
        }
    }
    if (!pc_lists.empty()) {
        g_submit_count.fetch_add(1, std::memory_order_relaxed);
        if (TraceEnabled()) {
            std::cerr << "[ScarlettX][d3d12_xs] Queue::ExecuteCommandLists(q=" << self
                      << ", count=" << pc_lists.size() << ")\n";
        }
        // Flush dirty guest textures once per presented frame, before the first
        // submit of the frame. Every-submit flushing rescanned every texture
        // for every command list the title queued.
        const uint64_t epoch = g_frame_epoch.load(std::memory_order_relaxed);
        if (epoch == 0 || g_flushed_epoch.load(std::memory_order_relaxed) != epoch) {
            LARGE_INTEGER freq{}, start{}, end{};
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&start);
            FlushGuestTextureWrites(obj->pc_queue);
            QueryPerformanceCounter(&end);
            g_flush_us.fetch_add(static_cast<uint64_t>(
                                     (end.QuadPart - start.QuadPart) * 1000000ll / freq.QuadPart),
                                 std::memory_order_relaxed);
            g_flush_count.fetch_add(1, std::memory_order_relaxed);
            g_flushed_epoch.store(epoch, std::memory_order_relaxed);
        }
        obj->pc_queue->ExecuteCommandLists(static_cast<UINT>(pc_lists.size()), pc_lists.data());
        if (obj->pc_device) {
            static std::atomic<bool> reported{false};
            const HRESULT removed = obj->pc_device->GetDeviceRemovedReason();
            if (FAILED(removed) && !reported.exchange(true)) {
                std::cerr << "[ScarlettX][d3d12_xs] device removed (0x" << std::hex << static_cast<uint32_t>(removed)
                          << std::dec << ") after submit #" << g_submit_count.load() << "\n";
                ReportDred(obj->pc_device);
                DumpD3D12InfoQueue(obj->pc_device, "ExecuteCommandLists");
            } else if (std::getenv("SCARLETTX_D3D12_DEBUG")) {
                DumpD3D12InfoQueue(obj->pc_device, "ExecuteCommandLists");
            }
        }
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyNoop(void*, uintptr_t, uintptr_t, uintptr_t) { return 0; }

intptr_t SCARLETTX_GDK_CALLBACK QueueProxySignal(void* self, uintptr_t pFence, uintptr_t val, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    if (!obj->pc_queue || !pFence) return static_cast<intptr_t>(E_INVALIDARG);
    auto* f = reinterpret_cast<XboxD3D12FenceProxy*>(pFence);
    if (!f->pc_fence) return static_cast<intptr_t>(E_INVALIDARG);
    return static_cast<intptr_t>(obj->pc_queue->Signal(f->pc_fence, val));
#else
    return 0;
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyWait(void* self, uintptr_t pFence, uintptr_t val, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    if (!obj->pc_queue || !pFence) return static_cast<intptr_t>(E_INVALIDARG);
    auto* f = reinterpret_cast<XboxD3D12FenceProxy*>(pFence);
    if (!f->pc_fence) return static_cast<intptr_t>(E_INVALIDARG);
    return static_cast<intptr_t>(obj->pc_queue->Wait(f->pc_fence, val));
#else
    return 0;
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyGetTimestampFrequency(
    void* self, uintptr_t pFreq, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    if (!obj->pc_queue || !pFreq) return static_cast<intptr_t>(E_INVALIDARG);
    const HRESULT hr = obj->pc_queue->GetTimestampFrequency(reinterpret_cast<UINT64*>(pFreq));
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12CommandQueue::GetTimestampFrequency() -> 0x"
                  << std::hex << static_cast<uint32_t>(hr) << std::dec
                  << " (freq=" << *reinterpret_cast<UINT64*>(pFreq) << ")\n";
    }
    return static_cast<intptr_t>(hr);
#else
    return 0;
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyGetClockCalibration(
    void* self, uintptr_t pGpu, uintptr_t pCpu, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    if (!obj->pc_queue || !pGpu || !pCpu) return static_cast<intptr_t>(E_INVALIDARG);
    return static_cast<intptr_t>(obj->pc_queue->GetClockCalibration(
        reinterpret_cast<UINT64*>(pGpu), reinterpret_cast<UINT64*>(pCpu)));
#else
    return 0;
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyGetDesc(void* self, uintptr_t sret_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    if (obj->pc_queue && sret_ptr) {
        *reinterpret_cast<D3D12_COMMAND_QUEUE_DESC*>(sret_ptr) = obj->pc_queue->GetDesc();
    }
#endif
    return static_cast<intptr_t>(sret_ptr);
}

#if defined(_WIN32)
constexpr UINT kMsgDestroyPresentWindow = WM_APP + 1;

// Borderless fullscreen on the window's monitor; the swap chain stretches.
void ToggleFullscreen(HWND hwnd) {
    static WINDOWPLACEMENT restore = {sizeof(restore)};
    const LONG_PTR style = GetWindowLongPtrA(hwnd, GWL_STYLE);
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi = {sizeof(mi)};
        if (!GetWindowPlacement(hwnd, &restore) ||
            !GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            return;
        }
        SetWindowLongPtrA(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtrA(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &restore);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

LRESULT CALLBACK ScarlettXPresentWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) {  // Alt+Enter
            if (!(lp & (1 << 30))) ToggleFullscreen(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        return 0;  // the title owns the lifetime of the output
    case kMsgDestroyPresentWindow:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SYSCOMMAND:
        // Alt and F10 would put the window into menu mode and take the
        // keyboard. Alt+Space, the system menu, is left alone.
        if ((wp & 0xFFF0) == SC_KEYMENU && lp != ' ') return 0;
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// The window has its own thread. DefWindowProc runs modal loops (system menu,
// moving or sizing the window) that do not return until the user is done; on
// the thread that calls PresentX that would stop the title's submissions, and
// a title treats a fence that does not advance as a lost device.
DWORD WINAPI PresentWindowThread(void* param) {
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(param);
    // Keep this module loaded for as long as the thread runs its code.
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCSTR>(&ScarlettXPresentWndProc), &self);

    const char* cls_name = "ScarlettXPresentWindowClass";
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ScarlettXPresentWndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = cls_name;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    RegisterClassExA(&wc);

    RECT rc{0, 0, 1280, 720};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    const HWND hwnd = CreateWindowExA(
        0, cls_name, "ScarlettX - Xbox Series X|S",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    obj->present_hwnd = hwnd;
    SetEvent(obj->present_window_ready);  // obj is not used past this point

    if (hwnd) {
        MSG msg = {};
        while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    if (self) FreeLibraryAndExitThread(self, 0);
    return 0;
}

void StartPresentWindow(XboxD3D12CommandQueueProxy* obj) {
    obj->present_window_ready = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!obj->present_window_ready) return;
    obj->present_window_thread = CreateThread(nullptr, 0, &PresentWindowThread, obj, 0, nullptr);
    if (obj->present_window_thread) WaitForSingleObject(obj->present_window_ready, INFINITE);
}

void StopPresentWindow(XboxD3D12CommandQueueProxy* obj) {
    if (obj->present_window_thread) {
        if (obj->present_hwnd) PostMessageA(obj->present_hwnd, kMsgDestroyPresentWindow, 0, 0);
        // The thread no longer touches obj; do not hang a Release on it.
        WaitForSingleObject(obj->present_window_thread, 5000);
        CloseHandle(obj->present_window_thread);
        obj->present_window_thread = nullptr;
    }
    if (obj->present_window_ready) {
        CloseHandle(obj->present_window_ready);
        obj->present_window_ready = nullptr;
    }
    obj->present_hwnd = nullptr;
}

DXGI_FORMAT NormalizeSwapChainFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

void DumpResourceToBmp(
    XboxD3D12CommandQueueProxy* obj,
    ID3D12Resource* src_res,
    D3D12_RESOURCE_STATES cur_state,
    const std::string& path) {
    if (!obj || !obj->pc_device || !obj->pc_queue || !obj->present_alloc || !obj->present_list || !src_res) return;
    D3D12_RESOURCE_DESC desc = src_res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width == 0 || desc.Height == 0) return;
    if (desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    else if (desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 total_bytes = 0;
    obj->pc_device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &num_rows, &row_size, &total_bytes);
    if (total_bytes == 0 || fp.Footprint.RowPitch == 0) return;

    D3D12_HEAP_PROPERTIES rb_heap = {};
    rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb_desc = {};
    rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb_desc.Width = total_bytes;
    rb_desc.Height = 1;
    rb_desc.DepthOrArraySize = 1;
    rb_desc.MipLevels = 1;
    rb_desc.SampleDesc.Count = 1;
    rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* rb = nullptr;
    if (FAILED(obj->pc_device->CreateCommittedResource(
            &rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&rb))) || !rb) {
        std::cerr << "[ScarlettX][d3d12_xs] frame dump: readback buffer creation failed (" << path << ")" << std::endl;
        return;
    }

    obj->present_alloc->Reset();
    obj->present_list->Reset(obj->present_alloc, nullptr);

    if (cur_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src_res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = cur_state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        obj->present_list->ResourceBarrier(1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
    dst_loc.pResource = rb;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src_loc = {};
    src_loc.pResource = src_res;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;
    obj->present_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    if (cur_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src_res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = cur_state;
        obj->present_list->ResourceBarrier(1, &b);
    }

    if (SUCCEEDED(obj->present_list->Close())) {
        ID3D12CommandList* lists[] = {obj->present_list};
        obj->pc_queue->ExecuteCommandLists(1, lists);
        const uint64_t val = ++obj->present_fence_val;
        obj->pc_queue->Signal(obj->present_fence, val);
        if (obj->present_fence->GetCompletedValue() < val) {
            obj->present_fence->SetEventOnCompletion(val, obj->present_event);
            WaitForSingleObject(obj->present_event, 5000);
        }

        void* mapped = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(total_bytes)};
        if (SUCCEEDED(rb->Map(0, &range, &mapped)) && mapped) {
            const uint32_t w = static_cast<uint32_t>(desc.Width);
            const uint32_t h = desc.Height;
            const uint32_t bmp_stride = ((w * 3u + 3u) & ~3u);
            const uint32_t pixel_bytes = bmp_stride * h;
            const uint32_t file_size = 54u + pixel_bytes;

            uint8_t hdr[54] = {};
            hdr[0] = 'B';
            hdr[1] = 'M';
            std::memcpy(hdr + 2, &file_size, 4);
            const uint32_t off = 54u;
            std::memcpy(hdr + 10, &off, 4);
            const uint32_t dib = 40u;
            std::memcpy(hdr + 14, &dib, 4);
            std::memcpy(hdr + 18, &w, 4);
            std::memcpy(hdr + 22, &h, 4);
            const uint16_t planes = 1u;
            const uint16_t bpp = 24u;
            std::memcpy(hdr + 26, &planes, 2);
            std::memcpy(hdr + 28, &bpp, 2);
            std::memcpy(hdr + 34, &pixel_bytes, 4);

            const bool is_bgra = (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                                  desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                                  desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS);
            std::ofstream out(path, std::ios::binary);
            if (!out) {
                std::cerr << "[ScarlettX][d3d12_xs] frame dump: cannot open " << path << std::endl;
            }
            if (out) {
                out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
                std::vector<uint8_t> row_buf(bmp_stride, 0);
                const auto* base = static_cast<const uint8_t*>(mapped);
                uint64_t non_black = 0;
                for (uint32_t y = 0; y < h; ++y) {
                    const uint32_t src_y = h - 1u - y;
                    const uint8_t* srow = base + static_cast<size_t>(src_y) * fp.Footprint.RowPitch;
                    for (uint32_t x = 0; x < w; ++x) {
                        const uint8_t c0 = srow[x * 4 + 0];
                        const uint8_t c1 = srow[x * 4 + 1];
                        const uint8_t c2 = srow[x * 4 + 2];
                        const uint8_t r = is_bgra ? c2 : c0;
                        const uint8_t g = c1;
                        const uint8_t b = is_bgra ? c0 : c2;
                        row_buf[x * 3 + 0] = b;
                        row_buf[x * 3 + 1] = g;
                        row_buf[x * 3 + 2] = r;
                        if (r || g || b) ++non_black;
                    }
                    out.write(reinterpret_cast<const char*>(row_buf.data()), static_cast<std::streamsize>(bmp_stride));
                }
                std::cerr << "[ScarlettX][d3d12_xs] Dumped frame to " << path
                          << " (" << w << "x" << h << ", fmt=" << desc.Format
                          << ", non_black_pixels=" << non_black << "/" << (static_cast<uint64_t>(w) * h) << ")\n";
            }
            D3D12_RANGE empty_range{0, 0};
            rb->Unmap(0, &empty_range);
        }
    }
    rb->Release();
}

void EnsurePresentSwapChain(XboxD3D12CommandQueueProxy* obj, UINT w, UINT h, DXGI_FORMAT src_fmt) {
    if (!obj || obj->present_init_attempted || !obj->pc_device || !obj->pc_queue) return;
    obj->present_init_attempted = true;

    StartPresentWindow(obj);
    if (!obj->present_hwnd) return;

    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory))) || !factory) {
        return;
    }

    obj->swap_w = (w > 0) ? w : 1920;
    obj->swap_h = (h > 0) ? h : 1080;
    obj->swap_fmt = NormalizeSwapChainFormat(src_fmt);

    DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
    sc_desc.Width = obj->swap_w;
    sc_desc.Height = obj->swap_h;
    sc_desc.Format = obj->swap_fmt;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = 3;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.Scaling = DXGI_SCALING_STRETCH;

    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = factory->CreateSwapChainForHwnd(
        obj->pc_queue, obj->present_hwnd, &sc_desc, nullptr, nullptr, &sc1);
    // DXGI's own Alt+Enter switch leaves every Present failing.
    factory->MakeWindowAssociation(obj->present_hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->Release();
    if (FAILED(hr) || !sc1) {
        std::cerr << "[ScarlettX][d3d12_xs] CreateSwapChainForHwnd failed hr=0x"
                  << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
        return;
    }
    sc1->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&obj->swapchain));
    sc1->Release();

    obj->pc_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
        reinterpret_cast<void**>(&obj->present_alloc));
    if (obj->present_alloc) {
        obj->pc_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, obj->present_alloc, nullptr,
            __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&obj->present_list));
        if (obj->present_list) obj->present_list->Close();
    }
    obj->pc_device->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&obj->present_fence));
    obj->present_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    std::cerr << "[ScarlettX][d3d12_xs] SwapChain created (" << obj->swap_w << "x" << obj->swap_h
              << ", fmt=" << obj->swap_fmt << ", sc=" << obj->swapchain << ")\n";
}
#endif

intptr_t SCARLETTX_GDK_CALLBACK QueueProxyPresentX(
    void* self, uintptr_t planeCount, uintptr_t pPlaneParams, uintptr_t pPresentParams) {
    static std::atomic<uint64_t> present_count{0};
    const uint64_t n = present_count.fetch_add(1, std::memory_order_relaxed) + 1;
    g_frame_epoch.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12CommandQueueProxy*>(self);
    XboxD3D12ResourceProxy* src_proxy = nullptr;
    if (pPlaneParams && planeCount > 0) {
        const auto* qwords = reinterpret_cast<const uint64_t*>(pPlaneParams);
        if (qwords[1] > 0 && qwords[2] != 0) {
            auto* cand = reinterpret_cast<XboxD3D12ResourceProxy*>(qwords[2]);
            if (cand->lpVtbl == kXboxResourceProxyVtbl.data()) {
                src_proxy = cand;
            } else {
                auto* deref = *reinterpret_cast<XboxD3D12ResourceProxy* const*>(qwords[2]);
                if (deref && deref->lpVtbl == kXboxResourceProxyVtbl.data()) {
                    src_proxy = deref;
                }
            }
        }
    }

    if (src_proxy && src_proxy->pc_res && obj && obj->pc_device && obj->pc_queue) {
        const D3D12_RESOURCE_DESC sdesc = src_proxy->pc_res->GetDesc();
        EnsurePresentSwapChain(obj, static_cast<UINT>(sdesc.Width), sdesc.Height, sdesc.Format);

        const auto cur_state = static_cast<D3D12_RESOURCE_STATES>(
            src_proxy->tracked_state.load(std::memory_order_relaxed));

        if (obj->swapchain && obj->present_alloc && obj->present_list && obj->present_fence && obj->present_event) {
            const UINT bb_idx = obj->swapchain->GetCurrentBackBufferIndex();
            ID3D12Resource* bb = nullptr;
            if (SUCCEEDED(obj->swapchain->GetBuffer(bb_idx, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&bb))) && bb) {
                if (obj->present_fence->GetCompletedValue() < obj->present_fence_val) {
                    obj->present_fence->SetEventOnCompletion(obj->present_fence_val, obj->present_event);
                    WaitForSingleObject(obj->present_event, 2000);
                }
                obj->present_alloc->Reset();
                obj->present_list->Reset(obj->present_alloc, nullptr);

                D3D12_RESOURCE_BARRIER pre[2] = {};
                UINT pre_n = 0;
                if (cur_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
                    pre[pre_n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    pre[pre_n].Transition.pResource = src_proxy->pc_res;
                    pre[pre_n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    pre[pre_n].Transition.StateBefore = cur_state;
                    pre[pre_n].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    ++pre_n;
                }
                pre[pre_n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                pre[pre_n].Transition.pResource = bb;
                pre[pre_n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                pre[pre_n].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                pre[pre_n].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                ++pre_n;
                obj->present_list->ResourceBarrier(pre_n, pre);

                obj->present_list->CopyResource(bb, src_proxy->pc_res);

                D3D12_RESOURCE_BARRIER post[2] = {};
                UINT post_n = 0;
                if (cur_state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
                    post[post_n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    post[post_n].Transition.pResource = src_proxy->pc_res;
                    post[post_n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    post[post_n].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    post[post_n].Transition.StateAfter = cur_state;
                    ++post_n;
                }
                post[post_n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                post[post_n].Transition.pResource = bb;
                post[post_n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                post[post_n].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                post[post_n].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                ++post_n;
                obj->present_list->ResourceBarrier(post_n, post);

                if (SUCCEEDED(obj->present_list->Close())) {
                    ID3D12CommandList* lists[] = {obj->present_list};
                    obj->pc_queue->ExecuteCommandLists(1, lists);
                    const HRESULT present_hr = obj->swapchain->Present(0, 0);
                    if (present_hr != S_OK) {
                        // The game keeps running when the output is lost, so say so.
                        static std::atomic<uint64_t> not_ok{0};
                        const uint64_t k = not_ok.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (k == 1 || (k % 1000) == 0) {
                            std::cerr << "[ScarlettX][d3d12_xs] Present returned 0x" << std::hex
                                      << static_cast<uint32_t>(present_hr) << std::dec << " (frame " << n << ", "
                                      << k << " so far)\n";
                        }
                    }
                    const uint64_t val = ++obj->present_fence_val;
                    obj->pc_queue->Signal(obj->present_fence, val);
                }
                bb->Release();
            }
        }

        if (const char* dump_path = std::getenv("SCARLETTX_DUMP_FRAME"); dump_path && dump_path[0] != '\0') {
            // Frames to dump: SCARLETTX_DUMP_FRAME_AT="5,30,90" (the default).
            static const std::vector<uint64_t> dump_frames = [] {
                std::vector<uint64_t> frames;
                const char* at = std::getenv("SCARLETTX_DUMP_FRAME_AT");
                std::istringstream list(at && at[0] ? at : "5,30,90");
                for (std::string item; std::getline(list, item, ',');) {
                    char* end = nullptr;
                    const unsigned long long v = std::strtoull(item.c_str(), &end, 10);
                    if (end != item.c_str()) frames.push_back(v);
                }
                return frames;
            }();
            if (std::find(dump_frames.begin(), dump_frames.end(), static_cast<uint64_t>(n)) != dump_frames.end()) {
                DumpResourceToBmp(obj, src_proxy->pc_res, cur_state, std::string(dump_path) + "_" + std::to_string(n) + ".bmp");
            }
        }
    }
#endif
    if (TraceEnabled() && (n <= 10 || (n % 60) == 0)) {
        std::cerr << "[ScarlettX][d3d12_xs] Queue::PresentX(#" << n
                  << ", q=" << self << ", src=" << src_proxy
                  << ", planes=" << planeCount
                  << ", draws=" << g_draw_instanced_count.load(std::memory_order_relaxed)
                  << "/" << g_draw_indexed_count.load(std::memory_order_relaxed)
                  << ", clears=" << g_clear_rtv_count.load(std::memory_order_relaxed)
                  << ", setRT=" << g_om_set_rt_count.load(std::memory_order_relaxed)
                  << ") -> S_OK\n";
    }
    MaybePrintStats(n);
    return 0;
}

std::array<ComVtableFn, 64> BuildQueueProxyVtable() {
    auto vtbl = MakeQueueProxyVtable(std::make_index_sequence<61>{});
    vtbl[6] = &QueueProxySetName;
    vtbl[10] = &QueueProxyExecuteCommandLists;
    vtbl[11] = &QueueProxyNoop;  // SetMarker
    vtbl[12] = &QueueProxyNoop;  // BeginEvent
    vtbl[13] = &QueueProxyNoop;  // EndEvent
    vtbl[14] = &QueueProxySignal;
    vtbl[15] = &QueueProxyWait;
    vtbl[16] = &QueueProxyGetTimestampFrequency;
    vtbl[17] = &QueueProxyGetClockCalibration;
    vtbl[18] = &QueueProxyGetDesc;
    vtbl[30] = &QueueProxyPresentX;
    return vtbl;
}

const auto kXboxCommandQueueProxyVtbl = BuildQueueProxyVtable();

// Titles built against a later SDK (D3D12XboxCreateDevice version 0x20004,
// against 0x20001 for the table above) find two more methods ahead of
// Signal. Only the slots a title was seen calling have a name here.
std::array<ComVtableFn, 64> BuildQueueProxyVtableV4() {
    auto vtbl = MakeQueueProxyVtable(std::make_index_sequence<61>{});
    vtbl[6] = &QueueProxySetName;
    vtbl[10] = &QueueProxyExecuteCommandLists;
    vtbl[16] = &QueueProxySignal;    // +0x80, observed at 0x1404f6166
    vtbl[32] = &QueueProxyPresentX;  // +0x100, observed at 0x1404f60d3
    return vtbl;
}

const auto kXboxCommandQueueProxyVtblV4 = BuildQueueProxyVtableV4();

struct XboxD3D12DeviceProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    uint32_t sdk_version = 0;
#if defined(_WIN32)
    ID3D12Device* pc_device = nullptr;
    IDXGIAdapter1* pc_adapter = nullptr;
#endif
};

struct XboxDXGIDeviceProxy {
    const ComVtableFn* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    XboxD3D12DeviceProxy* parent_device = nullptr;
};

intptr_t SCARLETTX_GDK_CALLBACK DxgiDevProxyQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxDXGIDeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    if (TraceEnabled() && riid) {
        const auto* g = reinterpret_cast<const uint32_t*>(riid);
        std::cerr << "[ScarlettX][d3d12_xs] XboxIDXGIDevice::QueryInterface("
                  << std::hex << g[0] << "-" << g[1] << "-" << g[2] << "-" << g[3] << std::dec << ")\n";
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK DxgiDevProxyAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxDXGIDeviceProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}

intptr_t SCARLETTX_GDK_CALLBACK DxgiDevProxyRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxDXGIDeviceProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
        if (obj->parent_device) {
            obj->parent_device->lpVtbl[2](obj->parent_device, 0, 0, 0);
        }
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}

XboxDXGIAdapterProxy* NewAdapterProxy(const XboxDXGIDeviceProxy* obj) {
    auto* adapter_proxy = new XboxDXGIAdapterProxy();
    adapter_proxy->lpVtbl = kXboxDXGIAdapterProxyVtbl.data();
#if defined(_WIN32)
    if (obj->parent_device && obj->parent_device->pc_adapter) {
        adapter_proxy->pc_adapter = obj->parent_device->pc_adapter;
        adapter_proxy->pc_adapter->AddRef();
    }
#endif
    return adapter_proxy;
}

intptr_t SCARLETTX_GDK_CALLBACK DxgiDevProxyGetParent(void* self, uintptr_t riid, uintptr_t ppParent, uintptr_t) {
    auto* obj = static_cast<XboxDXGIDeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppParent);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    *out = nullptr;
    if (TraceEnabled() && riid) {
        const auto* g = reinterpret_cast<const uint32_t*>(riid);
        std::cerr << "[ScarlettX][d3d12_xs] XboxIDXGIDevice::GetParent("
                  << std::hex << g[0] << "-" << g[1] << "-" << g[2] << "-" << g[3] << std::dec << ") -> S_OK\n";
    }
    *out = NewAdapterProxy(obj);
    return 0;  // S_OK
}

// IDXGIDevice::GetAdapter (+0x38), observed at 0x1404f5274 in a title.
intptr_t SCARLETTX_GDK_CALLBACK DxgiDevProxyGetAdapter(void* self, uintptr_t ppAdapter, uintptr_t, uintptr_t) {
    auto** out = reinterpret_cast<void**>(ppAdapter);
    if (!out) return static_cast<intptr_t>(0x80004003u);
    *out = NewAdapterProxy(static_cast<XboxDXGIDeviceProxy*>(self));
    return 0;
}

template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK DxgiDevProxyTracedSlot(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxIDXGIDevice::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
}

template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeDxgiDevProxyVtable(std::index_sequence<Is...>) {
    return {
        &DxgiDevProxyQueryInterface,
        &DxgiDevProxyAddRef,
        &DxgiDevProxyRelease,
        &DxgiDevProxyTracedSlot<static_cast<int>(Is + 3)>...,
    };
}

std::array<ComVtableFn, 24> BuildDxgiDevProxyVtable() {
    auto vtbl = MakeDxgiDevProxyVtable(std::make_index_sequence<21>{});
    vtbl[6] = &DxgiDevProxyGetParent;  // IDXGIObject::GetParent (+0x30)
    vtbl[7] = &DxgiDevProxyGetAdapter;
    return vtbl;
}

const auto kXboxDXGIDeviceProxyVtbl = BuildDxgiDevProxyVtable();

intptr_t SCARLETTX_GDK_CALLBACK ProxyQueryInterface(void* self, uintptr_t riid, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return static_cast<intptr_t>(0x80004003u);  // E_POINTER
    if (TraceEnabled() && riid) {
        const auto* g = reinterpret_cast<const uint32_t*>(riid);
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::QueryInterface("
                  << std::hex << g[0] << "-" << g[1] << "-" << g[2] << "-" << g[3] << std::dec << ")\n";
    }
    if (ProxyGuidEquals(riid, kIidIDXGIDevice) || ProxyGuidEquals(riid, kIidIDXGIDevice1)) {
        auto* dxgi = new XboxDXGIDeviceProxy();
        dxgi->lpVtbl = kXboxDXGIDeviceProxyVtbl.data();
        dxgi->parent_device = obj;
        obj->ref_count.fetch_add(1, std::memory_order_relaxed);
        *out = dxgi;
        return 0;  // S_OK
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return 0;  // S_OK
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    return static_cast<intptr_t>(
        static_cast<XboxD3D12DeviceProxy*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
#if defined(_WIN32)
        if (obj->pc_device) obj->pc_device->Release();
        if (obj->pc_adapter) obj->pc_adapter->Release();
#endif
        delete obj;
    }
    return static_cast<intptr_t>(rem);
}

intptr_t SCARLETTX_GDK_CALLBACK ProxySetName(void* self, uintptr_t name_ptr, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj->pc_device && name_ptr) {
        return static_cast<intptr_t>(obj->pc_device->SetName(reinterpret_cast<LPCWSTR>(name_ptr)));
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyGetNodeCount(void* self, uintptr_t, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj->pc_device) {
        return static_cast<intptr_t>(obj->pc_device->GetNodeCount());
    }
#endif
    return 1;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateCommandQueue(void* self, uintptr_t pDesc, uintptr_t riid, uintptr_t ppQueue) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppQueue);
    if (!pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    const auto* desc = reinterpret_cast<const D3D12_COMMAND_QUEUE_DESC*>(pDesc);
    D3D12_COMMAND_QUEUE_DESC pc_desc = *desc;
    if (pc_desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT &&
        pc_desc.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
        pc_desc.Type != D3D12_COMMAND_LIST_TYPE_COPY) {
        pc_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    }
    pc_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    pc_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    pc_desc.NodeMask = 0;
    ID3D12CommandQueue* pc_q = nullptr;
    const HRESULT hr = obj->pc_device->CreateCommandQueue(
        &pc_desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&pc_q));
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateCommandQueue(type="
                  << static_cast<int>(desc->Type) << ", riid=" << std::hex << riid
                  << ") -> 0x" << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_q) return static_cast<intptr_t>(hr);
    auto* q_proxy = new XboxD3D12CommandQueueProxy();
    q_proxy->lpVtbl = obj->sdk_version >= 0x20004u ? kXboxCommandQueueProxyVtblV4.data()
                                                   : kXboxCommandQueueProxyVtbl.data();
    q_proxy->pc_queue = pc_q;
    q_proxy->pc_device = obj->pc_device;
    if (q_proxy->pc_device) q_proxy->pc_device->AddRef();
    *out = q_proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateCommandQueueX(void* self, uintptr_t pDesc, uintptr_t riid, uintptr_t ppQueue) {
    if (TraceEnabled()) {
        const uint32_t raw_type = pDesc ? *reinterpret_cast<const uint32_t*>(pDesc) : 0;
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::vtable[48]/+0x180(raw_type=0x"
                  << std::hex << raw_type << std::dec << ")\n";
    }
    return ProxyCreateCommandQueue(self, pDesc, riid, ppQueue);
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCheckFeatureSupport(
    void* self, uintptr_t feature_raw, uintptr_t data_ptr, uintptr_t data_size_raw) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (!obj || !obj->pc_device || !data_ptr) return static_cast<intptr_t>(E_INVALIDARG);
    const auto feature = static_cast<D3D12_FEATURE>(feature_raw);
    const auto data_size = static_cast<UINT>(data_size_raw);
    HRESULT hr = obj->pc_device->CheckFeatureSupport(
        feature, reinterpret_cast<void*>(data_ptr), data_size);
    if (FAILED(hr) && feature == D3D12_FEATURE_D3D12_OPTIONS &&
        data_size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS)) {
        // Xbox D3D12_FEATURE_DATA_D3D12_OPTIONS may differ slightly in trailing size from PC.
        D3D12_FEATURE_DATA_D3D12_OPTIONS pc_opts{};
        hr = obj->pc_device->CheckFeatureSupport(feature, &pc_opts, sizeof(pc_opts));
        if (SUCCEEDED(hr)) {
            std::memset(reinterpret_cast<void*>(data_ptr), 0, data_size);
            const size_t copy_bytes =
                data_size < sizeof(pc_opts) ? data_size : sizeof(pc_opts);
            std::memcpy(reinterpret_cast<void*>(data_ptr), &pc_opts, copy_bytes);
        }
    }
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CheckFeatureSupport(feature="
                  << static_cast<uint32_t>(feature) << ", size=0x" << std::hex << data_size
                  << ") -> 0x" << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    return static_cast<intptr_t>(hr);
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateDescriptorHeap(
    void* self, uintptr_t pDesc, uintptr_t /*riid*/, uintptr_t ppvHeap) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppvHeap);
    if (!obj || !obj->pc_device || !pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    const auto* guest_desc = reinterpret_cast<const D3D12_DESCRIPTOR_HEAP_DESC*>(pDesc);
    D3D12_DESCRIPTOR_HEAP_DESC pc_desc = *guest_desc;
    if (static_cast<uint32_t>(pc_desc.Type) >= 4) {
        pc_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    }
    if (pc_desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ||
        pc_desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV) {
        pc_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    }
    ID3D12DescriptorHeap* pc_heap = nullptr;
    HRESULT hr = obj->pc_device->CreateDescriptorHeap(&pc_desc, IID_PPV_ARGS(&pc_heap));
    CheckDeviceRemoved(obj->pc_device, "CreateDescriptorHeap");
    if (FAILED(hr) && (pc_desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) {
        // The console allows larger shader-visible heaps than PC drivers
        // (resource binding tier 3 guarantees 1,000,000 view descriptors;
        // samplers are capped at 2048). Retry at the host maximum.
        const UINT cap = pc_desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 2048u : 1000000u;
        if (pc_desc.NumDescriptors > cap) {
            std::cerr << "[ScarlettX][d3d12_xs] CreateDescriptorHeap: " << pc_desc.NumDescriptors
                      << " shader-visible descriptors exceed the host limit, clamped to " << cap << "\n";
            pc_desc.NumDescriptors = cap;
            hr = obj->pc_device->CreateDescriptorHeap(&pc_desc, IID_PPV_ARGS(&pc_heap));
        }
    }
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateDescriptorHeap(type="
                  << static_cast<uint32_t>(guest_desc->Type)
                  << ", num=" << guest_desc->NumDescriptors
                  << ", flags=0x" << std::hex << static_cast<uint32_t>(guest_desc->Flags)
                  << ") -> 0x" << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_heap) {
        DumpD3D12InfoQueue(obj->pc_device, "CreateDescriptorHeap");
        return static_cast<intptr_t>(hr);
    }
    auto* proxy = new XboxD3D12DescriptorHeapProxy();
    proxy->lpVtbl = kXboxD3D12DescriptorHeapProxyVtbl.data();
    proxy->pc_heap = pc_heap;
    proxy->orig_desc = *guest_desc;
    {
        // Xbox extension of the desc: +0x10 placement address, +0x18 size in
        // bytes. Only trusted when it is consistent with the descriptor count.
        const auto* ext = reinterpret_cast<const uint64_t*>(pDesc + sizeof(D3D12_DESCRIPTOR_HEAP_DESC));
        const uint64_t base = ext[0], size = ext[1];
        const UINT inc = obj->pc_device->GetDescriptorHandleIncrementSize(pc_desc.Type);
        if (base && (base & 7) == 0 && size >= static_cast<uint64_t>(guest_desc->NumDescriptors) * 8 &&
            size <= static_cast<uint64_t>(guest_desc->NumDescriptors) * 256) {
            proxy->guest_base = static_cast<uintptr_t>(base);
            // Handles past the clamped host heap would land outside it.
            proxy->guest_size = static_cast<uintptr_t>(std::min<uint64_t>(size, uint64_t{pc_desc.NumDescriptors} * inc));
            const UINT64 gpu = (pc_desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
                                   ? pc_heap->GetGPUDescriptorHandleForHeapStart().ptr
                                   : 0;
            RegisterPlacedDescriptorHeap(proxy->guest_base, proxy->guest_size,
                                         pc_heap->GetCPUDescriptorHandleForHeapStart().ptr, gpu);
            if (TraceEnabled()) {
                std::cerr << "[ScarlettX][d3d12_xs] descriptor heap placed at 0x" << std::hex << base
                          << " (+0x" << size << ", host inc " << std::dec << inc << ")\n";
            }
        }
    }
    *out = proxy;
    return 0;  // S_OK
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyGetDescriptorHandleIncrementSize(
    void* self, uintptr_t heap_type_raw, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (!obj || !obj->pc_device) return 32;
    auto pc_type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(heap_type_raw);
    if (heap_type_raw >= 4) {
        pc_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    }
    const UINT inc = obj->pc_device->GetDescriptorHandleIncrementSize(pc_type);
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::GetDescriptorHandleIncrementSize(type="
                  << static_cast<uint32_t>(heap_type_raw) << ") -> " << inc << "\n";
    }
    return static_cast<intptr_t>(inc);
#else
    return 32;
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxySlot69XboxSetting(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::vtable[69]/+0x228(id=0x"
                  << std::hex << a1 << ", val=0x" << a2 << std::dec << ") -> S_OK\n";
    }
    return 0;  // S_OK
}

template <int Slot>
intptr_t SCARLETTX_GDK_CALLBACK ProxyTracedSlot(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        uintptr_t norm_ret = 0;
#if defined(_WIN32)
        // Titles are mapped at their preferred base, so the raw address is
        // directly comparable with a disassembly of the title.
        norm_ret = reinterpret_cast<uintptr_t>(SCARLETTX_RETURN_ADDRESS());
#endif
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << ", ret=0x" << norm_ret
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));  // E_NOTIMPL
}

template <size_t... Is>
constexpr std::array<ComVtableFn, sizeof...(Is) + 3> MakeProxyVtable(std::index_sequence<Is...>) {
    return {
        &ProxyQueryInterface,
        &ProxyAddRef,
        &ProxyRelease,
        &ProxyTracedSlot<static_cast<int>(Is + 3)>...,
    };
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateCommandAllocator(
    void* self, uintptr_t type_raw, uintptr_t /*riid*/, uintptr_t ppAlloc) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppAlloc);
    if (!obj || !obj->pc_device || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    auto type = static_cast<D3D12_COMMAND_LIST_TYPE>(type_raw);
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT &&
        type != D3D12_COMMAND_LIST_TYPE_BUNDLE &&
        type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
        type != D3D12_COMMAND_LIST_TYPE_COPY) {
        type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    }
    ID3D12CommandAllocator* pc_alloc = nullptr;
    const HRESULT hr = obj->pc_device->CreateCommandAllocator(type, IID_PPV_ARGS(&pc_alloc));
    CheckDeviceRemoved(obj->pc_device, "CreateCommandAllocator");
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateCommandAllocator(type="
                  << static_cast<uint32_t>(type_raw) << ") -> 0x"
                  << std::hex << static_cast<uint32_t>(hr) << ", pc_alloc=" << pc_alloc;
        if (FAILED(hr)) {
            std::cerr << ", removed_reason=0x"
                      << static_cast<uint32_t>(obj->pc_device->GetDeviceRemovedReason());
        }
        std::cerr << std::dec << "\n";
        if (FAILED(hr)) DumpD3D12InfoQueue(obj->pc_device, "CreateCommandAllocator");
    }
    if (FAILED(hr) || !pc_alloc) return static_cast<intptr_t>(hr);
    auto* proxy = new XboxD3D12CommandAllocatorProxy();
    proxy->lpVtbl = kXboxCommandAllocatorProxyVtbl.data();
    proxy->pc_alloc = pc_alloc;
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateGraphicsPipelineState(
    void* self, uintptr_t pDesc, uintptr_t /*riid*/, uintptr_t ppPso) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppPso);
    if (!obj || !obj->pc_device || !pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = *reinterpret_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(pDesc);
    if (desc.pRootSignature) {
        desc.pRootSignature = reinterpret_cast<ID3D12RootSignature*>(
            reinterpret_cast<XboxD3D12SimpleObjectProxy*>(desc.pRootSignature)->pc_obj);
    }
    ID3D12PipelineState* pc_pso = nullptr;
    const HRESULT hr = obj->pc_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pc_pso));
    CheckDeviceRemoved(obj->pc_device, "CreateGraphicsPipelineState");
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateGraphicsPipelineState("
                  << "vs=" << desc.VS.BytecodeLength << ", ps=" << desc.PS.BytecodeLength
                  << ", rtvs=" << desc.NumRenderTargets << ", rtv0=" << desc.RTVFormats[0]
                  << ", dsv=" << desc.DSVFormat << ", depth=" << desc.DepthStencilState.DepthEnable
                  << ", cull=" << desc.RasterizerState.CullMode
                  << ", topo=" << desc.PrimitiveTopologyType
                  << ", blend0=" << desc.BlendState.RenderTarget[0].BlendEnable
                  << ") -> 0x"
                  << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_pso) return static_cast<intptr_t>(hr);
    auto* proxy = new XboxD3D12SimpleObjectProxy();
    proxy->lpVtbl = kXboxSimpleObjProxyVtbl.data();
    proxy->pc_obj = pc_pso;
    proxy->kind = "PipelineState";
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

// Xbox device slot 61 (+0x1e8): writes a sampler descriptor from an 0x54-byte
// console sampler description: (const desc*, D3D12_CPU_DESCRIPTOR_HANDLE, ?).
// The layout is inferred from the code a retail title builds it with, so each
// field is used only when its value is valid for PC; the rest is defaulted.
intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateSamplerX(void* self, uintptr_t pDesc, uintptr_t dest, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (!obj || !obj->pc_device || !dest) return 0;
    D3D12_SAMPLER_DESC sd{};
    sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sd.MaxLOD = D3D12_FLOAT32_MAX;
    if (pDesc) {
        const auto* u = reinterpret_cast<const uint32_t*>(pDesc);
        const auto* f = reinterpret_cast<const float*>(pDesc);
        if (u[0] <= 0x1d5) sd.Filter = static_cast<D3D12_FILTER>(u[0]);
        auto addr = [](uint32_t v, D3D12_TEXTURE_ADDRESS_MODE& m) {
            if (v >= 1 && v <= 5) m = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(v);
        };
        addr(u[1], sd.AddressU);
        addr(u[2], sd.AddressV);
        addr(u[3], sd.AddressW);
        if (u[5] >= 1 && u[5] <= 8) sd.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(u[5]);
        const uint32_t aniso = u[0x34 / 4];
        if (aniso >= 1 && aniso <= 16) sd.MaxAnisotropy = aniso;
        const float min_lod = f[0x24 / 4], max_lod = f[0x28 / 4], bias = f[0x30 / 4];
        if (min_lod >= 0.0f && min_lod <= 16.0f) sd.MinLOD = min_lod;
        if (max_lod >= sd.MinLOD && max_lod <= D3D12_FLOAT32_MAX) sd.MaxLOD = max_lod;
        if (bias >= -16.0f && bias <= 15.99f) sd.MipLODBias = bias;
        for (int i = 0; i < 4; ++i) {
            const float c = f[0x44 / 4 + i];
            sd.BorderColor[i] = (c >= 0.0f && c <= 1.0f) ? c : 0.0f;
        }
        // Anisotropic filters need MaxAnisotropy >= 1, comparison filters a func.
        const bool comparison = (sd.Filter & 0x80) != 0;
        if (comparison && sd.ComparisonFunc == D3D12_COMPARISON_FUNC_NEVER) {
            sd.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        }
    }
    obj->pc_device->CreateSampler(&sd, D3D12_CPU_DESCRIPTOR_HANDLE{HostCpuHandle(dest)});
    CheckDeviceRemoved(obj->pc_device, "CreateSamplerX");
    static std::atomic<int> logged{0};
    if (TraceEnabled() && logged.fetch_add(1) < 4) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateSamplerX(filter=0x" << std::hex << sd.Filter
                  << std::dec << ", addr=" << sd.AddressU << "/" << sd.AddressV << "/" << sd.AddressW
                  << ", aniso=" << sd.MaxAnisotropy << ")\n";
    }
#endif
    return 0;
}

// Size of one subobject in a pipeline state stream, as laid out by d3dx12's
// alignas(void*) CD3DX12_PIPELINE_STATE_STREAM_* wrappers: a 4-byte type,
// the inner struct at its natural alignment, the whole rounded up to 8.
// 0 for types this layer does not know.
size_t PsoSubobjectSize(uint32_t type) {
    auto wrap = [](size_t inner_align, size_t inner_size) {
        const size_t off = inner_align >= 8 ? 8 : 4;
        return (off + inner_size + 7) & ~size_t{7};
    };
    switch (type) {
        case 0: return wrap(8, sizeof(void*));                             // ROOT_SIGNATURE
        case 1: case 2: case 3: case 4: case 5: case 6:                     // VS PS DS HS GS CS
        case 24: case 25:                                                   // AS MS
            return wrap(8, sizeof(D3D12_SHADER_BYTECODE));
        case 7: return wrap(8, sizeof(D3D12_STREAM_OUTPUT_DESC));
        case 8: return wrap(4, sizeof(D3D12_BLEND_DESC));
        case 9: return wrap(4, sizeof(UINT));                              // SAMPLE_MASK
        case 10: return wrap(4, sizeof(D3D12_RASTERIZER_DESC));
        case 11: return wrap(4, sizeof(D3D12_DEPTH_STENCIL_DESC));
        case 12: return wrap(8, sizeof(D3D12_INPUT_LAYOUT_DESC));
        case 13: return wrap(4, sizeof(UINT));                             // IB_STRIP_CUT_VALUE
        case 14: return wrap(4, sizeof(UINT));                             // PRIMITIVE_TOPOLOGY
        case 15: return wrap(4, sizeof(D3D12_RT_FORMAT_ARRAY));
        case 16: return wrap(4, sizeof(DXGI_FORMAT));                      // DEPTH_STENCIL_FORMAT
        case 17: return wrap(4, sizeof(DXGI_SAMPLE_DESC));
        case 18: return wrap(4, sizeof(UINT));                             // NODE_MASK
        case 19: return wrap(8, sizeof(D3D12_CACHED_PIPELINE_STATE));
        case 20: return wrap(4, sizeof(UINT));                             // FLAGS
        case 21: return wrap(4, sizeof(D3D12_DEPTH_STENCIL_DESC1));
        case 22: return wrap(8, sizeof(D3D12_VIEW_INSTANCING_DESC));
        default: return 0;
    }
}

// Xbox device slot 86 (+0x2b0): ID3D12Device2::CreatePipelineState, checked
// by the riid a retail title passes (IID_ID3D12PipelineState). The stream is
// copied with root signature proxies replaced by host objects; subobjects of
// unknown type end the stream (they are logged).
intptr_t SCARLETTX_GDK_CALLBACK ProxyCreatePipelineStateStream(void* self, uintptr_t pDesc, uintptr_t /*riid*/,
                                                               uintptr_t ppPso) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppPso);
    if (!obj || !obj->pc_device || !pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    const auto* sd = reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_DESC*>(pDesc);
    const auto* src = static_cast<const uint8_t*>(sd->pPipelineStateSubobjectStream);
    std::vector<uint8_t> copy;
    copy.reserve(sd->SizeInBytes);
    size_t pos = 0;
    size_t shader_bytes = 0;
    bool has_ms = false, has_cs = false;
    while (src && pos + 4 <= sd->SizeInBytes) {
        uint32_t type = 0;
        std::memcpy(&type, src + pos, 4);
        const size_t size = PsoSubobjectSize(type);
        if (!size || pos + size > sd->SizeInBytes) {
            static std::atomic<int> warned{0};
            if (warned.fetch_add(1) < 8) {
                std::cerr << "[ScarlettX][d3d12_xs] CreatePipelineState: subobject type " << type << " at +" << pos
                          << " of " << sd->SizeInBytes << " not understood; rest of the stream dropped\n";
            }
            break;
        }
        const size_t at = copy.size();
        copy.insert(copy.end(), src + pos, src + pos + size);
        if (type == 0) {
            void* rs = nullptr;
            std::memcpy(&rs, &copy[at + 8], sizeof rs);
            if (rs) {
                rs = reinterpret_cast<XboxD3D12SimpleObjectProxy*>(rs)->pc_obj;
                std::memcpy(&copy[at + 8], &rs, sizeof rs);
            }
        } else if (type <= 6 || type == 24 || type == 25) {
            D3D12_SHADER_BYTECODE bc{};
            std::memcpy(&bc, &copy[at + 8], sizeof bc);
            shader_bytes += bc.BytecodeLength;
            static std::atomic<int> dumped{0};
            if (bc.pShaderBytecode && bc.BytecodeLength >= 32 && dumped.fetch_add(1) < 4) {
                // DXBC container: "DXBC", hash[16], version, size, part count, offsets; parts are FourCC-tagged.
                const auto* b = static_cast<const uint8_t*>(bc.pShaderBytecode);
                std::cerr << "[ScarlettX][d3d12_xs] shader type " << type << " len=" << bc.BytecodeLength
                          << " magic='" << std::string(reinterpret_cast<const char*>(b), 4) << "' hash=";
                for (int i = 4; i < 20; ++i) {
                    static const char* hx = "0123456789abcdef";
                    std::cerr << hx[b[i] >> 4] << hx[b[i] & 15];
                }
                uint32_t ver = 0, total = 0;
                std::memcpy(&ver, b + 20, 4);
                std::memcpy(&total, b + 24, 4);
                std::cerr << " ver=0x" << std::hex << ver << std::dec << " size=" << total;
                uint32_t parts = 0;
                std::memcpy(&parts, b + 28, 4);
                for (uint32_t i = 0; i < parts && i < 16 && 32 + 4 * i + 4 <= bc.BytecodeLength; ++i) {
                    uint32_t off = 0;
                    std::memcpy(&off, b + 32 + 4 * i, 4);
                    if (off + 8 <= bc.BytecodeLength) {
                        uint32_t psize = 0;
                        std::memcpy(&psize, b + off + 4, 4);
                        std::cerr << " " << std::string(reinterpret_cast<const char*>(b + off), 4) << ":" << psize;
                    }
                }
                std::cerr << "\n";
                if (const char* dir = std::getenv("SCARLETTX_DUMP_SHADERS"); dir && *dir) {
                    const std::string path = std::string(dir) + "\\shader_" + std::to_string(dumped.load()) + "_t" +
                                             std::to_string(type) + ".dxbc";
                    if (FILE* fp = std::fopen(path.c_str(), "wb")) {
                        std::fwrite(b, 1, bc.BytecodeLength, fp);
                        std::fclose(fp);
                    }
                }
            }
            has_ms |= (type == 25 && bc.BytecodeLength);
            has_cs |= (type == 6 && bc.BytecodeLength);
        }
        pos += size;
    }
    ID3D12Device2* dev2 = nullptr;
    HRESULT hr = obj->pc_device->QueryInterface(IID_PPV_ARGS(&dev2));
    ID3D12PipelineState* pc_pso = nullptr;
    if (SUCCEEDED(hr) && dev2) {
        D3D12_PIPELINE_STATE_STREAM_DESC hd{copy.size(), copy.data()};
        hr = dev2->CreatePipelineState(&hd, IID_PPV_ARGS(&pc_pso));
        dev2->Release();
    }
    CheckDeviceRemoved(obj->pc_device, "CreatePipelineState");
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreatePipelineState(stream=" << sd->SizeInBytes
                  << "->" << copy.size() << ", shaders=" << shader_bytes << (has_ms ? ", mesh" : "")
                  << (has_cs ? ", compute" : "") << ") -> 0x" << std::hex << static_cast<uint32_t>(hr) << std::dec
                  << "\n";
    }
    if (FAILED(hr) || !pc_pso) return static_cast<intptr_t>(hr);
    auto* proxy = new XboxD3D12SimpleObjectProxy();
    proxy->lpVtbl = kXboxSimpleObjProxyVtbl.data();
    proxy->pc_obj = pc_pso;
    proxy->kind = "PipelineState";
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateComputePipelineState(
    void* self, uintptr_t pDesc, uintptr_t /*riid*/, uintptr_t ppPso) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppPso);
    if (!obj || !obj->pc_device || !pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = *reinterpret_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(pDesc);
    if (desc.pRootSignature) {
        desc.pRootSignature = reinterpret_cast<ID3D12RootSignature*>(
            reinterpret_cast<XboxD3D12SimpleObjectProxy*>(desc.pRootSignature)->pc_obj);
    }
    ID3D12PipelineState* pc_pso = nullptr;
    const HRESULT hr = obj->pc_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pc_pso));
    CheckDeviceRemoved(obj->pc_device, "CreateComputePipelineState");
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateComputePipelineState() -> 0x"
                  << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_pso) return static_cast<intptr_t>(hr);
    auto* proxy = new XboxD3D12SimpleObjectProxy();
    proxy->lpVtbl = kXboxSimpleObjProxyVtbl.data();
    proxy->pc_obj = pc_pso;
    proxy->kind = "PipelineState";
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateCommandList(
    void* self, uintptr_t nodeMask, uintptr_t type_raw, uintptr_t pAlloc,
    uintptr_t pInitialState, uintptr_t /*riid*/, uintptr_t ppList) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppList);
    if (!obj || !obj->pc_device || !pAlloc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    auto type = static_cast<D3D12_COMMAND_LIST_TYPE>(type_raw);
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT &&
        type != D3D12_COMMAND_LIST_TYPE_BUNDLE &&
        type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
        type != D3D12_COMMAND_LIST_TYPE_COPY) {
        type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    }
    auto* alloc_proxy = reinterpret_cast<XboxD3D12CommandAllocatorProxy*>(pAlloc);
    ID3D12PipelineState* pc_pso = nullptr;
    if (pInitialState) {
        pc_pso = reinterpret_cast<ID3D12PipelineState*>(
            reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pInitialState)->pc_obj);
    }
    ID3D12GraphicsCommandList* pc_list = nullptr;
    const HRESULT hr = obj->pc_device->CreateCommandList(
        static_cast<UINT>(nodeMask), type, alloc_proxy->pc_alloc, pc_pso, IID_PPV_ARGS(&pc_list));
    CheckDeviceRemoved(obj->pc_device, "CreateCommandList");
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateCommandList(type="
                  << static_cast<uint32_t>(type_raw) << ") -> 0x"
                  << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_list) return static_cast<intptr_t>(hr);
    auto* proxy = new XboxD3D12GraphicsCommandListProxy();
    proxy->lpVtbl = (obj->sdk_version >= 0x20004u ? kXboxCmdListProxyVtblV4 : kXboxCmdListProxyVtbl).data();
    proxy->pc_list = pc_list;
    proxy->pc_device = obj->pc_device;
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateRootSignature(
    void* self, uintptr_t nodeMask, uintptr_t pBlob, uintptr_t blobLen,
    uintptr_t /*riid*/, uintptr_t ppRootSig) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppRootSig);
    if (!obj || !obj->pc_device || !pBlob || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    ID3D12RootSignature* pc_rs = nullptr;
    const HRESULT hr = obj->pc_device->CreateRootSignature(
        static_cast<UINT>(nodeMask), reinterpret_cast<const void*>(pBlob),
        static_cast<SIZE_T>(blobLen), IID_PPV_ARGS(&pc_rs));
    CheckDeviceRemoved(obj->pc_device, "CreateRootSignature");
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateRootSignature(len="
                  << blobLen << ") -> 0x" << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_rs) {
        const HRESULT removed = obj->pc_device->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            std::cerr << "[ScarlettX][d3d12_xs] device removed, reason 0x" << std::hex
                      << static_cast<uint32_t>(removed) << std::dec << "\n";
            ReportDred(obj->pc_device);
            DumpD3D12InfoQueue(obj->pc_device, "CreateRootSignature");
        }
        return static_cast<intptr_t>(hr);
    }
    auto* proxy = new XboxD3D12SimpleObjectProxy();
    proxy->lpVtbl = kXboxSimpleObjProxyVtbl.data();
    proxy->pc_obj = pc_rs;
    proxy->kind = "RootSignature";
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateConstantBufferView(
    void* self, uintptr_t pDesc, uintptr_t destDesc, uintptr_t) {
    static std::atomic<uint64_t> cbv_cnt{0};
    const uint64_t n = cbv_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device) {
        auto* d = reinterpret_cast<const D3D12_CONSTANT_BUFFER_VIEW_DESC*>(pDesc);
        if (TraceEnabled() && n <= 8) {
            std::cerr << "[ScarlettX][d3d12_xs] Device::CreateCBV(#" << n
                      << ", loc=0x" << std::hex << (d ? d->BufferLocation : 0) << std::dec
                      << ", size=" << (d ? d->SizeInBytes : 0)
                      << ", dst=0x" << std::hex << destDesc << std::dec << ")\n";
        }
        D3D12_CPU_DESCRIPTOR_HANDLE h{HostCpuHandle(destDesc)};
        if (d) {
            D3D12_CONSTANT_BUFFER_VIEW_DESC hd = *d;
            hd.BufferLocation = HostGpuVa(d->BufferLocation);
            // PC requires CBV sizes in multiples of 256 bytes (the console
            // accepts smaller ones); an unaligned size removes the device.
            hd.SizeInBytes = (hd.SizeInBytes + 255u) & ~255u;
            if (hd.BufferLocation & 255u) {
                static std::atomic<int> warned{0};
                if (warned.fetch_add(1) < 8) {
                    std::cerr << "[ScarlettX][d3d12_xs] CBV location 0x" << std::hex << hd.BufferLocation << std::dec
                              << " is not 256-byte aligned\n";
                }
            }
            obj->pc_device->CreateConstantBufferView(&hd, h);
            CheckDeviceRemoved(obj->pc_device, "CreateCBV");
        } else {
            obj->pc_device->CreateConstantBufferView(nullptr, h);
            CheckDeviceRemoved(obj->pc_device, "CreateCBV(null)");
        }
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateShaderResourceView(
    void* self, uintptr_t pResource, uintptr_t pDesc, uintptr_t destDesc) {
    static std::atomic<uint64_t> srv_cnt{0};
    const uint64_t n = srv_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device) {
        auto* res_proxy = pResource ? reinterpret_cast<XboxD3D12ResourceProxy*>(pResource) : nullptr;
        ID3D12Resource* pc_res = res_proxy ? res_proxy->pc_res : nullptr;
        auto* d = reinterpret_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(pDesc);
        D3D12_SHADER_RESOURCE_VIEW_DESC synth_desc = {};
        if (!d && res_proxy && res_proxy->promoted_typeless && res_proxy->guest_format != DXGI_FORMAT_UNKNOWN) {
            synth_desc.Format = res_proxy->guest_format;
            synth_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            synth_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            synth_desc.Texture2D.MipLevels = ~0u;
            d = &synth_desc;
        }
        if (TraceEnabled() && n <= 8) {
            std::cerr << "[ScarlettX][d3d12_xs] Device::CreateSRV(#" << n
                      << ", res=0x" << std::hex << pResource << std::dec
                      << ", fmt=" << (d ? static_cast<int>(d->Format) : -1)
                      << ", dim=" << (d ? static_cast<int>(d->ViewDimension) : -1)
                      << ", dst=0x" << std::hex << destDesc << std::dec << ")\n";
        }
        D3D12_CPU_DESCRIPTOR_HANDLE h{HostCpuHandle(destDesc)};
        obj->pc_device->CreateShaderResourceView(pc_res, d, h);
        CheckDeviceRemoved(obj->pc_device, "CreateSRV");
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateUnorderedAccessView(
    void* self, uintptr_t pResource, uintptr_t pCounter, uintptr_t pDesc, uintptr_t destDesc) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device) {
        ID3D12Resource* pc_res = pResource ? reinterpret_cast<XboxD3D12ResourceProxy*>(pResource)->pc_res : nullptr;
        ID3D12Resource* pc_cnt = pCounter ? reinterpret_cast<XboxD3D12ResourceProxy*>(pCounter)->pc_res : nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE h{HostCpuHandle(destDesc)};
        obj->pc_device->CreateUnorderedAccessView(
            pc_res, pc_cnt, reinterpret_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC*>(pDesc), h);
        CheckDeviceRemoved(obj->pc_device, "CreateUAV");
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateRenderTargetView(
    void* self, uintptr_t pResource, uintptr_t pDesc, uintptr_t destDesc) {
    static std::atomic<uint64_t> rtv_cnt{0};
    const uint64_t n = rtv_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device) {
        auto* res_proxy = pResource ? reinterpret_cast<XboxD3D12ResourceProxy*>(pResource) : nullptr;
        ID3D12Resource* pc_res = res_proxy ? res_proxy->pc_res : nullptr;
        auto* d = reinterpret_cast<const D3D12_RENDER_TARGET_VIEW_DESC*>(pDesc);
        D3D12_RENDER_TARGET_VIEW_DESC synth_desc = {};
        if (!d && res_proxy && res_proxy->promoted_typeless && res_proxy->guest_format != DXGI_FORMAT_UNKNOWN) {
            synth_desc.Format = res_proxy->guest_format;
            synth_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            d = &synth_desc;
        }
        if (TraceEnabled() && n <= 12) {
            std::cerr << "[ScarlettX][d3d12_xs] Device::CreateRTV(#" << n
                      << ", res=0x" << std::hex << pResource << std::dec
                      << ", fmt=" << (d ? static_cast<int>(d->Format) : -1)
                      << ", dim=" << (d ? static_cast<int>(d->ViewDimension) : -1)
                      << ", dst=0x" << std::hex << destDesc << std::dec << ")\n";
        }
        D3D12_CPU_DESCRIPTOR_HANDLE h{HostCpuHandle(destDesc)};
        obj->pc_device->CreateRenderTargetView(pc_res, d, h);
        CheckDeviceRemoved(obj->pc_device, "CreateRTV");
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateDepthStencilView(
    void* self, uintptr_t pResource, uintptr_t pDesc, uintptr_t destDesc) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device) {
        ID3D12Resource* pc_res = pResource ? reinterpret_cast<XboxD3D12ResourceProxy*>(pResource)->pc_res : nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE h{HostCpuHandle(destDesc)};
        obj->pc_device->CreateDepthStencilView(
            pc_res, reinterpret_cast<const D3D12_DEPTH_STENCIL_VIEW_DESC*>(pDesc), h);
        CheckDeviceRemoved(obj->pc_device, "CreateDSV");
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateSampler(
    void* self, uintptr_t pDesc, uintptr_t destDesc, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device && pDesc) {
        D3D12_CPU_DESCRIPTOR_HANDLE h{HostCpuHandle(destDesc)};
        obj->pc_device->CreateSampler(reinterpret_cast<const D3D12_SAMPLER_DESC*>(pDesc), h);
        CheckDeviceRemoved(obj->pc_device, "CreateSampler");
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCopyDescriptors(
    void* self, uintptr_t numDstRanges, uintptr_t pDstStarts, uintptr_t pDstSizes,
    uintptr_t numSrcRanges, uintptr_t pSrcStarts, uintptr_t pSrcSizes, uintptr_t type_raw) {
    static std::atomic<uint64_t> cd_cnt{0};
    const uint64_t n = cd_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device) {
        auto pc_type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(type_raw);
        if (type_raw >= 4) pc_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        if (TraceEnabled() && n <= 8) {
            std::cerr << "[ScarlettX][d3d12_xs] Device::CopyDescriptors(#" << n
                      << ", dstRanges=" << numDstRanges << ", srcRanges=" << numSrcRanges
                      << ", type=" << static_cast<uint32_t>(pc_type) << ")\n";
        }
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> dst(static_cast<size_t>(numDstRanges));
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> src(static_cast<size_t>(numSrcRanges));
        const auto* gd = reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(pDstStarts);
        const auto* gs = reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(pSrcStarts);
        for (size_t i = 0; gd && i < dst.size(); ++i) dst[i].ptr = HostCpuHandle(gd[i].ptr);
        for (size_t i = 0; gs && i < src.size(); ++i) src[i].ptr = HostCpuHandle(gs[i].ptr);
        obj->pc_device->CopyDescriptors(
            static_cast<UINT>(numDstRanges),
            gd ? dst.data() : nullptr,
            reinterpret_cast<const UINT*>(pDstSizes),
            static_cast<UINT>(numSrcRanges),
            gs ? src.data() : nullptr,
            reinterpret_cast<const UINT*>(pSrcSizes),
            pc_type);
        CheckDeviceRemoved(obj->pc_device, "CopyDescriptors");
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCopyDescriptorsSimple(
    void* self, uintptr_t numDescs, uintptr_t destStart, uintptr_t srcStart, uintptr_t type_raw) {
    static std::atomic<uint64_t> cds_cnt{0};
    const uint64_t n = cds_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device) {
        D3D12_CPU_DESCRIPTOR_HANDLE dst{HostCpuHandle(destStart)};
        D3D12_CPU_DESCRIPTOR_HANDLE src{HostCpuHandle(srcStart)};
        auto pc_type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(type_raw);
        if (type_raw >= 4) pc_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        if (TraceEnabled() && n <= 12) {
            std::cerr << "[ScarlettX][d3d12_xs] Device::CopyDescriptorsSimple(#" << n
                      << ", num=" << numDescs << ", dst=0x" << std::hex << destStart
                      << ", src=0x" << srcStart << std::dec
                      << ", type=" << static_cast<uint32_t>(pc_type) << ")\n";
        }
        obj->pc_device->CopyDescriptorsSimple(static_cast<UINT>(numDescs), dst, src, pc_type);
        CheckDeviceRemoved(obj->pc_device, "CopyDescriptorsSimple");
    }
#endif
    return 0;
}

extern const std::array<ComVtableFn, 256> kXboxDeviceProxyVtbl;

intptr_t SCARLETTX_GDK_CALLBACK ProxyGetResourceAllocationInfo(
    void* arg0, uintptr_t arg1, uintptr_t visibleMask, uintptr_t numDescs, uintptr_t pDescs) {
#if defined(_WIN32)
    XboxD3D12DeviceProxy* obj = nullptr;
    uintptr_t sret_ptr = 0;
    if (arg0 && *reinterpret_cast<const void* const*>(arg0) == kXboxDeviceProxyVtbl.data()) {
        obj = static_cast<XboxD3D12DeviceProxy*>(arg0);
        sret_ptr = arg1;
    } else if (arg1 && *reinterpret_cast<const void* const*>(arg1) == kXboxDeviceProxyVtbl.data()) {
        obj = reinterpret_cast<XboxD3D12DeviceProxy*>(arg1);
        sret_ptr = reinterpret_cast<uintptr_t>(arg0);
    }
    if (obj && obj->pc_device && sret_ptr && pDescs && numDescs > 0 && numDescs <= 256) {
        std::vector<D3D12_RESOURCE_DESC> descs(static_cast<size_t>(numDescs));
        const auto* src = reinterpret_cast<const D3D12_RESOURCE_DESC*>(pDescs);
        for (size_t i = 0; i < descs.size(); ++i) {
            descs[i] = src[i];
            descs[i].Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<uint32_t>(descs[i].Flags) & 0x3Fu);
            if ((descs[i].Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) &&
                !(descs[i].Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
                descs[i].Flags = static_cast<D3D12_RESOURCE_FLAGS>(
                    descs[i].Flags & ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
            }
        }
        *reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(sret_ptr) =
            obj->pc_device->GetResourceAllocationInfo(
                static_cast<UINT>(visibleMask), static_cast<UINT>(descs.size()), descs.data());
    }
    return static_cast<intptr_t>(sret_ptr);
#else
    return static_cast<intptr_t>(arg1);
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyGetCustomHeapProperties(
    void* arg0, uintptr_t arg1, uintptr_t nodeMask, uintptr_t heapType) {
#if defined(_WIN32)
    XboxD3D12DeviceProxy* obj = nullptr;
    uintptr_t sret_ptr = 0;
    if (arg0 && *reinterpret_cast<const void* const*>(arg0) == kXboxDeviceProxyVtbl.data()) {
        obj = static_cast<XboxD3D12DeviceProxy*>(arg0);
        sret_ptr = arg1;
    } else if (arg1 && *reinterpret_cast<const void* const*>(arg1) == kXboxDeviceProxyVtbl.data()) {
        obj = reinterpret_cast<XboxD3D12DeviceProxy*>(arg1);
        sret_ptr = reinterpret_cast<uintptr_t>(arg0);
    }
    if (obj && obj->pc_device && sret_ptr) {
        *reinterpret_cast<D3D12_HEAP_PROPERTIES*>(sret_ptr) =
            obj->pc_device->GetCustomHeapProperties(
                static_cast<UINT>(nodeMask), static_cast<D3D12_HEAP_TYPE>(heapType));
    }
    return static_cast<intptr_t>(sret_ptr);
#else
    return static_cast<intptr_t>(arg1);
#endif
}

// ID3D12Device::CreateHeap. The title passes IID_ID3D12Heap as riid.
intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateHeap(void* self, uintptr_t pDesc, uintptr_t riid, uintptr_t ppvHeap) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppvHeap);
    if (!obj || !pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    if (riid && !ProxyGuidEquals(riid, kIidID3D12Heap)) return static_cast<intptr_t>(E_NOINTERFACE);
    auto* proxy = new (std::nothrow) XboxD3D12HeapProxy();
    if (!proxy) return static_cast<intptr_t>(E_OUTOFMEMORY);
    proxy->lpVtbl = kXboxHeapProxyVtbl.data();
    proxy->desc = *reinterpret_cast<const D3D12_HEAP_DESC*>(pDesc);
    if (TraceEnabled()) {
        const auto& d = proxy->desc;
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateHeap(size=0x" << std::hex << d.SizeInBytes
                  << ", type=" << std::dec << static_cast<uint32_t>(d.Properties.Type) << "/"
                  << static_cast<uint32_t>(d.Properties.CPUPageProperty) << "/"
                  << static_cast<uint32_t>(d.Properties.MemoryPoolPreference) << ", align=0x" << std::hex
                  << d.Alignment << ", flags=0x" << static_cast<uint32_t>(d.Flags) << std::dec << ") -> S_OK (proxy="
                  << proxy << ")\n";
    }
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

// Xbox device slot 74 (+0x250): a heap over title memory the title backed
// with XMemMapPhysicalPages. Call site in a retail title:
//   (void* address, UINT numPages64K, ID3D12Heap** ppHeap) -> HRESULT
// Like CreateHeap here, the proxy only records the description; resources
// placed in it get host memory of their own, so the title's CPU writes to the
// range are not seen through them.
intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateHeapFromPhysicalPagesX(void* self, uintptr_t address, uintptr_t numPages,
                                                                  uintptr_t ppvHeap) {
#if defined(_WIN32)
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = static_cast<UINT64>(static_cast<uint32_t>(numPages)) << 16;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    hd.Flags = D3D12_HEAP_FLAG_NONE;
    const intptr_t hr = ProxyCreateHeap(self, reinterpret_cast<uintptr_t>(&hd), 0, ppvHeap);
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateHeapFromPhysicalPagesX(va=0x" << std::hex
                  << address << ", size=0x" << hd.SizeInBytes << ") -> 0x" << static_cast<uint32_t>(hr) << std::dec
                  << "\n";
    }
    return hr;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateCommittedResource(
    void* self, uintptr_t pHeapProps, uintptr_t heapFlags, uintptr_t pDesc,
    uintptr_t initialState, uintptr_t pClearVal, uintptr_t /*riid*/, uintptr_t ppvRes) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppvRes);
    if (!obj || !obj->pc_device || !pHeapProps || !pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    D3D12_HEAP_PROPERTIES hp = *reinterpret_cast<const D3D12_HEAP_PROPERTIES*>(pHeapProps);
    const D3D12_HEAP_PROPERTIES orig_hp = hp;
    if (hp.Type == D3D12_HEAP_TYPE_CUSTOM) {
        hp.Type = (hp.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE)
                      ? D3D12_HEAP_TYPE_UPLOAD
                      : (hp.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK
                             ? D3D12_HEAP_TYPE_READBACK
                             : D3D12_HEAP_TYPE_DEFAULT);
        hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    }
    D3D12_RESOURCE_DESC desc = *reinterpret_cast<const D3D12_RESOURCE_DESC*>(pDesc);
    const auto orig_flags = static_cast<uint32_t>(desc.Flags);
    // Strip Xbox-only resource and heap flags not valid on PC D3D12.
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(orig_flags & 0x3Fu);
    if (hp.Type == D3D12_HEAP_TYPE_UPLOAD || hp.Type == D3D12_HEAP_TYPE_READBACK) {
        // PC D3D12 forbids ALLOW_RENDER_TARGET / ALLOW_DEPTH_STENCIL / ALLOW_UNORDERED_ACCESS on UPLOAD/READBACK heaps.
        desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
            static_cast<uint32_t>(desc.Flags) & ~(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                                  D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
    }
    if ((static_cast<uint32_t>(desc.Flags) & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0u) {
        // PC D3D12 forbids DENY_SHADER_RESOURCE without ALLOW_DEPTH_STENCIL (Xbox permits it on buffers/textures).
        desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
            static_cast<uint32_t>(desc.Flags) & ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    }
    D3D12_RESOURCE_STATES pc_state = SanitizePcResourceState(
        static_cast<D3D12_RESOURCE_STATES>(static_cast<uint32_t>(initialState)), desc.Dimension);
    if (hp.Type == D3D12_HEAP_TYPE_UPLOAD) {
        pc_state = D3D12_RESOURCE_STATE_GENERIC_READ;
    } else if (hp.Type == D3D12_HEAP_TYPE_READBACK) {
        pc_state = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    const DXGI_FORMAT orig_fmt = desc.Format;
    bool promoted_typeless = false;
    const bool is_display_or_rt =
        ((heapFlags & D3D12_HEAP_FLAG_ALLOW_DISPLAY) != 0) ||
        ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0);
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        hp.Type == D3D12_HEAP_TYPE_DEFAULT && !is_display_or_rt) {
        switch (desc.Format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
                promoted_typeless = true;
                break;
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
                promoted_typeless = true;
                break;
            case DXGI_FORMAT_BC1_UNORM:
            case DXGI_FORMAT_BC1_UNORM_SRGB:
                desc.Format = DXGI_FORMAT_BC1_TYPELESS;
                promoted_typeless = true;
                break;
            case DXGI_FORMAT_BC2_UNORM:
            case DXGI_FORMAT_BC2_UNORM_SRGB:
                desc.Format = DXGI_FORMAT_BC2_TYPELESS;
                promoted_typeless = true;
                break;
            case DXGI_FORMAT_BC3_UNORM:
            case DXGI_FORMAT_BC3_UNORM_SRGB:
                desc.Format = DXGI_FORMAT_BC3_TYPELESS;
                promoted_typeless = true;
                break;
            case DXGI_FORMAT_BC7_UNORM:
            case DXGI_FORMAT_BC7_UNORM_SRGB:
                desc.Format = DXGI_FORMAT_BC7_TYPELESS;
                promoted_typeless = true;
                break;
            default:
                break;
        }
    }
    const D3D12_CLEAR_VALUE* pc_clear_val = promoted_typeless
        ? nullptr
        : reinterpret_cast<const D3D12_CLEAR_VALUE*>(pClearVal);
    const auto pc_heap_flags = static_cast<D3D12_HEAP_FLAGS>(heapFlags & 0x3FFFu);
    ID3D12Resource* pc_res = nullptr;
    const HRESULT hr = obj->pc_device->CreateCommittedResource(
        &hp, pc_heap_flags, &desc, pc_state,
        pc_clear_val, IID_PPV_ARGS(&pc_res));
    CheckDeviceRemoved(obj->pc_device, "CreateCommittedResource");
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateCommittedResource(dim="
                  << static_cast<uint32_t>(desc.Dimension) << ", w=" << desc.Width
                  << ", h=" << desc.Height << ", hp=" << static_cast<uint32_t>(orig_hp.Type)
                  << "/" << static_cast<uint32_t>(orig_hp.CPUPageProperty)
                  << "/" << static_cast<uint32_t>(orig_hp.MemoryPoolPreference)
                  << ", hflags=0x" << std::hex << heapFlags
                  << ", rflags=0x" << orig_flags
                  << ", state=0x" << initialState
                  << ", align=0x" << desc.Alignment
                  << ", layout=" << std::dec << static_cast<uint32_t>(desc.Layout)
                  << ", fmt=" << static_cast<uint32_t>(orig_fmt)
                  << "->" << static_cast<uint32_t>(desc.Format)
                  << ") -> 0x" << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_res) return static_cast<intptr_t>(hr);
    auto* proxy = new XboxD3D12ResourceProxy();
    proxy->lpVtbl = kXboxResourceProxyVtbl.data();
    proxy->device = obj;
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    proxy->pc_res = pc_res;
    proxy->tracked_state.store(static_cast<uint32_t>(pc_state), std::memory_order_relaxed);
    proxy->guest_format = orig_fmt;
    proxy->promoted_typeless = promoted_typeless;
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

// ID3D12Device::CreatePlacedResource. `pHeap` is a heap proxy from
// CreateHeap; the resource gets host memory of its own with the heap's
// properties (see XboxD3D12HeapProxy), so `heapOffset` only matters to the
// title's own bookkeeping.
intptr_t SCARLETTX_GDK_CALLBACK ProxyCreatePlacedResource(
    void* self, uintptr_t pHeap, uintptr_t heapOffset, uintptr_t pDesc,
    uintptr_t initialState, uintptr_t pClearVal, uintptr_t riid, uintptr_t ppvRes) {
#if defined(_WIN32)
    auto* heap = reinterpret_cast<XboxD3D12HeapProxy*>(pHeap);
    auto** out = reinterpret_cast<void**>(ppvRes);
    if (out) *out = nullptr;
    if (!heap || heap->lpVtbl != kXboxHeapProxyVtbl.data() || !pDesc || !out) {
        return static_cast<intptr_t>(E_INVALIDARG);
    }
    // The category restrictions describe the heap, not a committed resource.
    const uint32_t category_flags = D3D12_HEAP_FLAG_DENY_BUFFERS | D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES |
                                    D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
    const uintptr_t heap_flags = static_cast<uint32_t>(heap->desc.Flags) & ~category_flags;
    const intptr_t hr = ProxyCreateCommittedResource(
        self, reinterpret_cast<uintptr_t>(&heap->desc.Properties), heap_flags, pDesc, initialState, pClearVal, riid,
        ppvRes);
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreatePlacedResource(heap=" << heap << ", offset=0x"
                  << std::hex << heapOffset << ") -> 0x" << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    return hr;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

// --- GPU access to title physical pages -------------------------------------
// xgameruntime.dll backs XMemMapPhysicalPages with views of pagefile sections.
// A buffer the title places over such memory is built from those same
// sections, so GPU and CPU see one copy: a placed resource when the range is
// one contiguous run of a section, otherwise a reserved resource whose 64 KB
// tiles are mapped run by run (the GPU-side twin of the physical page map).
struct ScxXMemBackingRun {
    HANDLE section;
    uint64_t offset;
    uint64_t va;
    uint64_t length;
};
using ScxXMemQueryBackingFn = size_t (*)(uint64_t, uint64_t, ScxXMemBackingRun*, size_t);

ScxXMemQueryBackingFn XMemQueryBacking() {
    static const ScxXMemQueryBackingFn fn = [] {
        HMODULE m = GetModuleHandleA("xgameruntime.dll");
        return m ? reinterpret_cast<ScxXMemQueryBackingFn>(GetProcAddress(m, "ScxXMemQueryBacking")) : nullptr;
    }();
    return fn;
}

std::mutex g_section_heaps_mutex;
std::map<HANDLE, ID3D12Heap*> g_section_heaps;

ID3D12Heap* SectionHeap(ID3D12Device* dev, HANDLE section) {
    std::lock_guard<std::mutex> lock(g_section_heaps_mutex);
    auto it = g_section_heaps.find(section);
    if (it != g_section_heaps.end()) return it->second;
    ID3D12Device3* dev3 = nullptr;
    ID3D12Heap* heap = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev3))) && dev3) {
        const HRESULT hr = dev3->OpenExistingHeapFromFileMapping(section, IID_PPV_ARGS(&heap));
        if (FAILED(hr)) {
            std::cerr << "[ScarlettX][d3d12_xs] OpenExistingHeapFromFileMapping failed 0x" << std::hex
                      << static_cast<uint32_t>(hr) << std::dec << "\n";
            heap = nullptr;
        }
        dev3->Release();
    }
    g_section_heaps[section] = heap;  // failures are cached too
    return heap;
}

HRESULT CreateBufferOverGuestPages(ID3D12Device* dev, uintptr_t address, const D3D12_RESOURCE_DESC& desc,
                                   ID3D12Resource** out, const char** path) {
    auto query = XMemQueryBacking();
    if (!query || (address & 0xFFFF) != 0) return E_NOTIMPL;
    const uint64_t size = (desc.Width + 0xFFFF) & ~uint64_t{0xFFFF};
    std::vector<ScxXMemBackingRun> runs(static_cast<size_t>(size >> 16));
    const size_t n = query(address, size, runs.data(), runs.size());
    if (!n) return E_NOTIMPL;  // not (fully) physical-page memory
    D3D12_RESOURCE_DESC bd = desc;
    bd.Alignment = 0;
    bd.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        (static_cast<uint32_t>(desc.Flags) & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) |
        D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
    if (n == 1) {
        ID3D12Heap* heap = SectionHeap(dev, runs[0].section);
        if (!heap) return E_FAIL;
        const HRESULT hr = dev->CreatePlacedResource(heap, runs[0].offset, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(out));
        CheckDeviceRemoved(dev, "SectionPlacedBuffer");
        if (SUCCEEDED(hr)) *path = "section-placed";
        return hr;
    }
    // UpdateTileMappings rejects cross-adapter heaps, so a range made of
    // several runs cannot be one GPU resource.
    std::cerr << "[ScarlettX][d3d12_xs] buffer at 0x" << std::hex << address << " (+0x" << bd.Width << ") spans "
              << std::dec << n << " section runs; not shareable with the GPU\n";
    return E_NOTIMPL;
}

// --- lazily created GPU views of title memory --------------------------------
std::atomic<ID3D12Device*> g_host_device{nullptr};
std::mutex g_resolve_mutex;
std::vector<ID3D12Resource*> g_guest_view_buffers;      // kept alive for the process
std::map<uint64_t, uint64_t> g_unresolvable;           // 64 KB-aligned pages already tried

using ScxXMemRegionOfFn = int (*)(uint64_t, uint64_t*, uint64_t*);

bool PlaceOneBufferOnHeap(ID3D12Device* dev, ID3D12Heap* heap, uint64_t heap_offset, uint64_t width,
                          uint64_t guest_base);

// D3D12 resources are limited to 2 GB (views on larger ones remove the
// device). Larger ranges are covered by 1 GB buffers every 512 MB, so any
// view of up to 512 MB lies inside one of them; lookups pick the last chunk
// starting at or below an address.
bool PlaceBufferOnHeap(ID3D12Device* dev, ID3D12Heap* heap, uint64_t heap_offset, uint64_t width, uint64_t guest_base) {
    constexpr uint64_t kChunk = 1ull << 30, kStep = 1ull << 29;
    if (width <= kChunk) return PlaceOneBufferOnHeap(dev, heap, heap_offset, width, guest_base);
    bool ok = true;
    for (uint64_t off = 0; off < width; off += kStep) {
        const uint64_t len = std::min(kChunk, width - off);
        ok = PlaceOneBufferOnHeap(dev, heap, heap_offset + off, len, guest_base + off) && ok;
        if (off + len == width) break;
    }
    return ok;
}

bool PlaceOneBufferOnHeap(ID3D12Device* dev, ID3D12Heap* heap, uint64_t heap_offset, uint64_t width,
                          uint64_t guest_base) {
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = width;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    ID3D12Resource* res = nullptr;
    const HRESULT hr = dev->CreatePlacedResource(heap, heap_offset, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&res));
    CheckDeviceRemoved(dev, "PlaceBufferOnHeap");
    if (FAILED(hr) || !res) {
        std::cerr << "[ScarlettX][d3d12_xs] GPU view of title memory 0x" << std::hex << guest_base << " (+0x" << width
                  << ") failed 0x" << static_cast<uint32_t>(hr) << std::dec << "\n";
        return false;
    }
    g_guest_view_buffers.push_back(res);
    RegisterGuestGpuRange(guest_base, width, res->GetGPUVirtualAddress());
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] GPU view of title memory 0x" << std::hex << guest_base << " (+0x" << width
                  << ") -> 0x" << res->GetGPUVirtualAddress() << std::dec << "\n";
    }
    return true;
}

bool ResolveGuestGpuVa(uint64_t va) {
    ID3D12Device* dev = g_host_device.load(std::memory_order_acquire);
    if (!dev) return false;
    std::lock_guard<std::mutex> lock(g_resolve_mutex);
    const uint64_t page = va & ~uint64_t{0xFFFF};
    if (g_unresolvable.count(page)) return false;
    D3D12_GPU_VIRTUAL_ADDRESS dummy = 0;
    if (LookupGuestGpuVa(va, &dummy)) return true;  // resolved by another thread meanwhile
    bool ok = false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(va), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT) {
        if (mbi.Type == MEM_PRIVATE) {
            // Ordinary title allocation: wrap the whole host allocation.
            ID3D12Device3* dev3 = nullptr;
            ID3D12Heap* heap = nullptr;
            if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev3))) && dev3) {
                if (SUCCEEDED(dev3->OpenExistingHeapFromAddress(mbi.AllocationBase, IID_PPV_ARGS(&heap))) && heap) {
                    const uint64_t base = reinterpret_cast<uint64_t>(mbi.AllocationBase);
                    ok = PlaceBufferOnHeap(dev, heap, 0, heap->GetDesc().SizeInBytes, base);
                    heap->Release();
                }
                dev3->Release();
            }
        } else if (mbi.Type == MEM_MAPPED) {
            // Physical-page views: one buffer per contiguous section run of
            // the XMem reservation holding va.
            static const auto region_of = [] {
                HMODULE m = GetModuleHandleA("xgameruntime.dll");
                return m ? reinterpret_cast<ScxXMemRegionOfFn>(GetProcAddress(m, "ScxXMemRegionOf")) : nullptr;
            }();
            static const auto sparse = [] {
                HMODULE m = GetModuleHandleA("xgameruntime.dll");
                return m ? reinterpret_cast<ScxXMemQueryBackingFn>(GetProcAddress(m, "ScxXMemQueryBackingSparse"))
                         : nullptr;
            }();
            uint64_t rbase = 0, rsize = 0;
            if (region_of && sparse && region_of(va, &rbase, &rsize)) {
                std::vector<ScxXMemBackingRun> runs(static_cast<size_t>(std::min<uint64_t>(rsize >> 16, 65536)));
                const size_t n = sparse(rbase, rsize, runs.data(), runs.size());
                for (size_t i = 0; i < n; ++i) {
                    if (LookupGuestGpuVa(runs[i].va, &dummy)) continue;
                    if (ID3D12Heap* heap = SectionHeap(dev, runs[i].section)) {
                        if (PlaceBufferOnHeap(dev, heap, runs[i].offset, runs[i].length, runs[i].va) &&
                            va >= runs[i].va && va < runs[i].va + runs[i].length) {
                            ok = true;
                        }
                    }
                }
            }
        }
    }
    if (!ok) g_unresolvable[page] = 1;
    return ok;
}

// Enhanced-barrier layout -> legacy resource state, for titles that create
// resources with an initial layout while the rest of this layer tracks states.
uint32_t LegacyStateForLayout(uint32_t layout) {
    switch (layout) {
        case 1: case 19: case 25: return D3D12_RESOURCE_STATE_GENERIC_READ;            // GENERIC_READ
        case 2: return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case 3: case 20: case 26: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case 4: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case 5: return D3D12_RESOURCE_STATE_DEPTH_READ;
        case 6: case 21: case 27:
            return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case 7: case 22: case 28: return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case 8: case 23: case 29: return D3D12_RESOURCE_STATE_COPY_DEST;
        case 9: return D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        case 10: return D3D12_RESOURCE_STATE_RESOLVE_DEST;
        case 11: return D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
        default: return D3D12_RESOURCE_STATE_COMMON;  // COMMON/PRESENT/UNDEFINED/queue-common
    }
}

// Xbox device slot 127 (+0x3f8): ID3D12Device10::CreateCommittedResource3.
// Order from a retail title's call site (heap flags 0x8 = ALLOW_DISPLAY):
//   (pHeapProps, heapFlags, const D3D12_RESOURCE_DESC1*, initialLayout,
//    pClearValue, pProtectedSession, numCastableFormats, pCastableFormats, riid, ppv)
// D3D12_RESOURCE_DESC1 starts with the D3D12_RESOURCE_DESC fields.
intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateCommittedResource3(
    void* self, uintptr_t pHeapProps, uintptr_t heapFlags, uintptr_t pDesc1, uintptr_t initialLayout,
    uintptr_t pClearVal, uintptr_t /*pProtected*/, uintptr_t /*numCastable*/, uintptr_t /*pCastable*/,
    uintptr_t riid, uintptr_t ppvRes) {
    const uint32_t state = LegacyStateForLayout(static_cast<uint32_t>(initialLayout));
    const intptr_t hr = ProxyCreateCommittedResource(self, pHeapProps, heapFlags, pDesc1, state, pClearVal, riid,
                                                     ppvRes);
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateCommittedResource3(layout="
                  << static_cast<uint32_t>(initialLayout) << " -> state=0x" << std::hex << state << ") -> 0x"
                  << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    return hr;
}

// Xbox device slot 133 (+0x428): a resource placed at a virtual address the
// title reserved and backed itself (XMemVirtualAlloc + XMemMapPhysicalPages).
// Argument order from a retail title's call site:
//   (address, const D3D12_RESOURCE_DESC*, initialState, const D3D12_CLEAR_VALUE*, riid, ppv)
// Buffers wrap the title's memory with OpenExistingHeapFromAddress so CPU
// writes are what the GPU reads. Anything else gets memory of its own.
intptr_t SCARLETTX_GDK_CALLBACK ProxyCreatePlacedResourceX(
    void* self, uintptr_t address, uintptr_t pDesc, uintptr_t initialState,
    uintptr_t pClearVal, uintptr_t riid, uintptr_t ppvRes) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppvRes);
    if (!obj || !obj->pc_device || !pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    D3D12_RESOURCE_DESC desc = *reinterpret_cast<const D3D12_RESOURCE_DESC*>(pDesc);
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<uint32_t>(desc.Flags) & 0x3Fu &
                                                   ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    HRESULT hr = E_FAIL;
    ID3D12Resource* pc_res = nullptr;
    const char* path = "committed";
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && address) {
        hr = CreateBufferOverGuestPages(obj->pc_device, address, desc, &pc_res, &path);
    }
    if (!pc_res && desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && address) {
        ID3D12Device3* dev3 = nullptr;
        if (SUCCEEDED(obj->pc_device->QueryInterface(IID_PPV_ARGS(&dev3))) && dev3) {
            ID3D12Heap* heap = nullptr;
            hr = dev3->OpenExistingHeapFromAddress(reinterpret_cast<const void*>(address), IID_PPV_ARGS(&heap));
            if (SUCCEEDED(hr) && heap) {
                D3D12_RESOURCE_DESC bdesc = desc;
                bdesc.Alignment = 0;
                bdesc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
                    (static_cast<uint32_t>(bdesc.Flags) & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) |
                    D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
                const D3D12_HEAP_DESC hd = heap->GetDesc();
                // The heap covers only the host allocation holding `address`;
                // a buffer longer than that would let views run past its end.
                hr = bdesc.Width <= hd.SizeInBytes
                         ? obj->pc_device->CreatePlacedResource(heap, 0, &bdesc, D3D12_RESOURCE_STATE_COMMON,
                                                                nullptr, IID_PPV_ARGS(&pc_res))
                         : E_INVALIDARG;
                heap->Release();  // the placed resource keeps it alive
                if (SUCCEEDED(hr)) path = "existing-heap";
            }
            dev3->Release();
        }
    }
    if (!pc_res) {
        // Fallback: host memory of its own (the title's writes are not seen).
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
        const uintptr_t state = hp.Type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                                   : static_cast<uint32_t>(initialState);
        const intptr_t r = ProxyCreateCommittedResource(self, reinterpret_cast<uintptr_t>(&hp), 0,
                                                        reinterpret_cast<uintptr_t>(&desc), state, pClearVal, riid,
                                                        ppvRes);
        if (TraceEnabled()) {
            std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreatePlacedResourceX(va=0x" << std::hex << address
                      << ", dim=" << std::dec << static_cast<uint32_t>(desc.Dimension) << ", w=" << desc.Width
                      << ") fallback committed -> 0x" << std::hex << static_cast<uint32_t>(r) << " (open hr=0x"
                      << static_cast<uint32_t>(hr) << ")" << std::dec << "\n";
        }
        return r;
    }
    auto* proxy = new XboxD3D12ResourceProxy();
    proxy->lpVtbl = kXboxResourceProxyVtbl.data();
    proxy->device = obj;
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    proxy->pc_res = pc_res;
    proxy->tracked_state.store(static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON), std::memory_order_relaxed);
    proxy->guest_format = desc.Format;
    *out = proxy;
    if (address && desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        RegisterGuestGpuRange(address, desc.Width, pc_res->GetGPUVirtualAddress());
    }
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreatePlacedResourceX(va=0x" << std::hex << address
                  << ", w=0x" << desc.Width << ") -> " << path << std::dec << "\n";
    }
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateFence(
    void* self, uintptr_t initialValue, uintptr_t flags, uintptr_t /*riid*/, uintptr_t ppFence) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppFence);
    if (!obj || !obj->pc_device || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    ID3D12Fence* pc_fence = nullptr;
    const HRESULT hr = obj->pc_device->CreateFence(
        initialValue, static_cast<D3D12_FENCE_FLAGS>(flags), IID_PPV_ARGS(&pc_fence));
    CheckDeviceRemoved(obj->pc_device, "CreateFence");
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateFence(init="
                  << initialValue << ", flags=0x" << std::hex << flags << ") -> 0x"
                  << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_fence) return static_cast<intptr_t>(hr);
    auto* proxy = new XboxD3D12FenceProxy();
    proxy->lpVtbl = kXboxFenceProxyVtbl.data();
    proxy->pc_fence = pc_fence;
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyGetDeviceRemovedReason(void* self, uintptr_t, uintptr_t, uintptr_t) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device) return static_cast<intptr_t>(obj->pc_device->GetDeviceRemovedReason());
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyGetCopyableFootprints(
    void* self, uintptr_t pDesc, uintptr_t firstSub, uintptr_t numSub, uintptr_t baseOffset,
    uintptr_t pLayouts, uintptr_t pNumRows, uintptr_t pRowSizes, uintptr_t pTotalBytes) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    if (obj && obj->pc_device && pDesc) {
        obj->pc_device->GetCopyableFootprints(
            reinterpret_cast<const D3D12_RESOURCE_DESC*>(pDesc),
            static_cast<UINT>(firstSub), static_cast<UINT>(numSub), baseOffset,
            reinterpret_cast<D3D12_PLACED_SUBRESOURCE_FOOTPRINT*>(pLayouts),
            reinterpret_cast<UINT*>(pNumRows),
            reinterpret_cast<UINT64*>(pRowSizes),
            reinterpret_cast<UINT64*>(pTotalBytes));
    }
#endif
    return 0;
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateQueryHeap(
    void* self, uintptr_t pDesc, uintptr_t /*riid*/, uintptr_t ppvHeap) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppvHeap);
    if (!obj || !obj->pc_device || !pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    ID3D12QueryHeap* pc_qh = nullptr;
    const HRESULT hr = obj->pc_device->CreateQueryHeap(
        reinterpret_cast<const D3D12_QUERY_HEAP_DESC*>(pDesc), IID_PPV_ARGS(&pc_qh));
    CheckDeviceRemoved(obj->pc_device, "CreateQueryHeap");
    if (FAILED(hr) || !pc_qh) return static_cast<intptr_t>(hr);
    auto* proxy = new XboxD3D12SimpleObjectProxy();
    proxy->lpVtbl = kXboxSimpleObjProxyVtbl.data();
    proxy->pc_obj = pc_qh;
    proxy->kind = "QueryHeap";
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateCommandSignature(
    void* self, uintptr_t pDesc, uintptr_t pRootSig, uintptr_t /*riid*/, uintptr_t ppvSig) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppvSig);
    if (!obj || !obj->pc_device || !pDesc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    ID3D12RootSignature* pc_rs = pRootSig
        ? reinterpret_cast<ID3D12RootSignature*>(reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pRootSig)->pc_obj)
        : nullptr;
    ID3D12CommandSignature* pc_sig = nullptr;
    // Console-only argument types (>= 100) are mapped to their PC equivalent.
    // 105 is used with a 12-byte stride, i.e. three thread-group counts; it is
    // taken to be the console's mesh dispatch (an assumption, not verified).
    D3D12_COMMAND_SIGNATURE_DESC sig = *reinterpret_cast<const D3D12_COMMAND_SIGNATURE_DESC*>(pDesc);
    std::vector<D3D12_INDIRECT_ARGUMENT_DESC> args(sig.pArgumentDescs, sig.pArgumentDescs + sig.NumArgumentDescs);
    for (auto& a : args) {
        const auto t = static_cast<uint32_t>(a.Type);
        if (t == 105) a.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
        else if (t >= 100) {
            std::cerr << "[ScarlettX][d3d12_xs] CreateCommandSignature: unknown console argument type " << t << "\n";
        }
    }
    sig.pArgumentDescs = args.data();
    const HRESULT hr = obj->pc_device->CreateCommandSignature(&sig, pc_rs, IID_PPV_ARGS(&pc_sig));
    CheckDeviceRemoved(obj->pc_device, "CreateCommandSignature");
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateCommandSignature() -> 0x"
                  << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_sig) {
        DumpD3D12InfoQueue(obj->pc_device, "CreateCommandSignature");
        const auto* d = reinterpret_cast<const D3D12_COMMAND_SIGNATURE_DESC*>(pDesc);
        std::cerr << "[ScarlettX][d3d12_xs] CreateCommandSignature failed: stride=" << d->ByteStride << " args=" << d->NumArgumentDescs << " rs=" << pc_rs;
        for (UINT i = 0; i < d->NumArgumentDescs; ++i) std::cerr << " [" << static_cast<int>(d->pArgumentDescs[i].Type) << "]";
        std::cerr << "\n";
        return static_cast<intptr_t>(hr);
    }
    auto* proxy = new XboxD3D12SimpleObjectProxy();
    proxy->lpVtbl = kXboxSimpleObjProxyVtbl.data();
    proxy->pc_obj = pc_sig;
    proxy->kind = "CommandSignature";
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

// Provisional Xbox device slot 68 (+0x220), observed at 0x1405383b0 with a stack struct ptr; return value ignored.
intptr_t SCARLETTX_GDK_CALLBACK ProxySlot68XboxInfo(
    void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::vtable[68](a1=0x" << std::hex << a1
                  << ", a2=0x" << a2 << ", a3=0x" << a3 << std::dec << ") -> S_OK (provisional)\n";
    }
    return 0;
}

// Verified Xbox device slot 73 (+0x248) from 0x140094a4c..0x140094a88:
// CreateCommandListX(const D3D12XBOX_COMMAND_LIST_DESC* pDesc, ID3D12CommandAllocator* pAlloc,
//                    ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList)
intptr_t SCARLETTX_GDK_CALLBACK ProxyCreateCommandListX(
    void* self, uintptr_t pDesc, uintptr_t pAlloc, uintptr_t pInitialState,
    uintptr_t /*riid*/, uintptr_t ppList) {
#if defined(_WIN32)
    auto* obj = static_cast<XboxD3D12DeviceProxy*>(self);
    auto** out = reinterpret_cast<void**>(ppList);
    if (!obj || !obj->pc_device || !pDesc || !pAlloc || !out) return static_cast<intptr_t>(E_INVALIDARG);
    *out = nullptr;
    const uint32_t type_raw = *reinterpret_cast<const uint32_t*>(pDesc);
    auto type = static_cast<D3D12_COMMAND_LIST_TYPE>(type_raw);
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT &&
        type != D3D12_COMMAND_LIST_TYPE_BUNDLE &&
        type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
        type != D3D12_COMMAND_LIST_TYPE_COPY) {
        type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    }
    auto* alloc_proxy = reinterpret_cast<XboxD3D12CommandAllocatorProxy*>(pAlloc);
    ID3D12PipelineState* pc_pso = nullptr;
    if (pInitialState) {
        pc_pso = reinterpret_cast<ID3D12PipelineState*>(
            reinterpret_cast<XboxD3D12SimpleObjectProxy*>(pInitialState)->pc_obj);
    }
    ID3D12GraphicsCommandList* pc_list = nullptr;
    const HRESULT hr = obj->pc_device->CreateCommandList(
        0u, type, alloc_proxy->pc_alloc, pc_pso, IID_PPV_ARGS(&pc_list));
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateCommandListX(type="
                  << type_raw << ") -> 0x" << std::hex << static_cast<uint32_t>(hr) << std::dec << "\n";
    }
    if (FAILED(hr) || !pc_list) return static_cast<intptr_t>(hr);
    auto* proxy = new XboxD3D12GraphicsCommandListProxy();
    proxy->lpVtbl = (type_raw == 16u) ? kXboxDmaCmdListProxyVtbl.data()
                                      : (obj->sdk_version >= 0x20004u ? kXboxCmdListProxyVtblV4 : kXboxCmdListProxyVtbl).data();
    proxy->pc_list = pc_list;
    proxy->pc_device = obj->pc_device;
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::CreateCommandListX(type="
                  << type_raw << ") -> 0x0 (proxy=" << proxy << ", alloc=" << alloc_proxy << ")\n";
    }
    *out = proxy;
    return 0;
#else
    return static_cast<intptr_t>(static_cast<int32_t>(0x80004001u));
#endif
}

// Observed Xbox device slot 78 (+0x270) at 0x14052f501: SetFrameIntervalX(pOutput, intervalUs=0x411b, period=2, flags=0).
std::atomic<uint32_t> g_frame_interval_us{0};

intptr_t SCARLETTX_GDK_CALLBACK ProxySetFrameIntervalX(
    void* /*self*/, uintptr_t pOutput, uintptr_t intervalUs, uintptr_t period) {
    // The period is the allowed latency in intervals, not a rate divisor.
    g_frame_interval_us.store(intervalUs <= 1000000 ? static_cast<uint32_t>(intervalUs) : 0,
                              std::memory_order_relaxed);
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::SetFrameIntervalX(out=0x" << std::hex << pOutput
                  << ", interval_us=" << std::dec << intervalUs << ", period=" << period << ") -> S_OK\n";
    }
    return 0;
}

// Observed Xbox device slot 79 (+0x278) at 0x14052f51e: ScheduleFrameEventX(eventType, offset, pEvent, flags).
intptr_t SCARLETTX_GDK_CALLBACK ProxyScheduleFrameEventX(
    void* /*self*/, uintptr_t eventType, uintptr_t offset, uintptr_t pEvent) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::ScheduleFrameEventX(type=" << eventType
                  << ", offset=" << offset << ", event=0x" << std::hex << pEvent << std::dec << ") -> S_OK\n";
    }
    return 0;
}

// Observed Xbox device slot 80 (+0x280) at 0x14008ced6:
//   WaitFrameEventX(eventType=0, timeoutMs=INFINITE, pEvent=NULL, flags=0, pToken=&out_u64)
std::atomic<uint64_t> g_frame_pipeline_token{1};

// Blocks until the next frame origin, one interval after the previous one.
void WaitForFrameOrigin() {
#if defined(_WIN32)
    // --fcap: a frame rate to hold instead of the title's, 0 for no cap.
    static const long fcap = [] {
        const char* v = std::getenv("SCARLETTX_FCAP");
        return v && v[0] != '\0' ? std::strtol(v, nullptr, 10) : -1L;
    }();
    const uint32_t interval_us = fcap > 0 ? static_cast<uint32_t>(1000000 / fcap)
                                          : g_frame_interval_us.load(std::memory_order_relaxed);
    if (fcap == 0 || interval_us == 0) return;

    static std::mutex mu;
    static int64_t next = 0;
    static const int64_t freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    static const HANDLE timer = [] {
        HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* high resolution */, TIMER_ALL_ACCESS);
        return t ? t : CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }();

    std::lock_guard<std::mutex> lock(mu);
    const int64_t interval = freq * interval_us / 1000000;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (now.QuadPart > next + interval) next = now.QuadPart;  // fell behind, start over
    if (now.QuadPart < next) {
        LARGE_INTEGER due{};
        due.QuadPart = -((next - now.QuadPart) * 10000000 / freq);
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(static_cast<DWORD>((next - now.QuadPart) * 1000 / freq));
        }
    }
    next += interval;
#endif
}

intptr_t SCARLETTX_GDK_CALLBACK ProxyWaitFrameEventX(
    void* /*self*/, uintptr_t eventType, uintptr_t timeoutMs, uintptr_t pEvent,
    uintptr_t flags, uintptr_t pToken) {
    if (eventType == 0) WaitForFrameOrigin();
    uint64_t tok = g_frame_pipeline_token.fetch_add(1, std::memory_order_relaxed);
    if (pToken) {
        *reinterpret_cast<uint64_t*>(pToken) = tok;
    }
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] XboxID3D12Device::WaitFrameEventX(type=" << eventType
                  << ", timeout=0x" << std::hex << timeoutMs << ", event=0x" << pEvent
                  << ", flags=0x" << flags << std::dec << ") -> S_OK (tok=" << tok << ")\n";
    }
    return 0;
}

std::array<ComVtableFn, 256> BuildXboxDeviceProxyVtable() {
    auto vtbl = MakeProxyVtable(std::make_index_sequence<253>{});
    vtbl[133] = reinterpret_cast<ComVtableFn>(&ProxyCreatePlacedResourceX);                  // +0x428
    vtbl[127] = reinterpret_cast<ComVtableFn>(&ProxyCreateCommittedResource3);               // +0x3f8
    vtbl[74] = &ProxyCreateHeapFromPhysicalPagesX;                                         // +0x250
    vtbl[86] = &ProxyCreatePipelineStateStream;                                           // +0x2b0
    vtbl[61] = &ProxyCreateSamplerX;                                                      // +0x1e8
    vtbl[6] = &ProxySetName;                                                           // +0x30
    vtbl[7] = &ProxyGetNodeCount;                                                      // +0x38
    vtbl[8] = &ProxyCreateCommandQueue;                                                // +0x40
    vtbl[9] = &ProxyCreateCommandAllocator;                                            // +0x48
    vtbl[10] = &ProxyCreateGraphicsPipelineState;                                      // +0x50
    vtbl[11] = &ProxyCreateComputePipelineState;                                       // +0x58
    vtbl[12] = reinterpret_cast<ComVtableFn>(&ProxyCreateCommandList);                 // +0x60
    vtbl[13] = &ProxyCheckFeatureSupport;                                              // +0x68
    vtbl[14] = &ProxyCreateDescriptorHeap;                                             // +0x70
    vtbl[15] = &ProxyGetDescriptorHandleIncrementSize;                                 // +0x78
    vtbl[16] = reinterpret_cast<ComVtableFn>(&ProxyCreateRootSignature);               // +0x80
    vtbl[17] = &ProxyCreateConstantBufferView;                                         // +0x88
    vtbl[18] = &ProxyCreateShaderResourceView;                                         // +0x90
    vtbl[19] = reinterpret_cast<ComVtableFn>(&ProxyCreateUnorderedAccessView);         // +0x98
    vtbl[20] = &ProxyCreateRenderTargetView;                                           // +0xa0
    vtbl[21] = &ProxyCreateDepthStencilView;                                           // +0xa8
    vtbl[22] = &ProxyCreateSampler;                                                    // +0xb0
    vtbl[23] = reinterpret_cast<ComVtableFn>(&ProxyCopyDescriptors);                   // +0xb8
    vtbl[24] = reinterpret_cast<ComVtableFn>(&ProxyCopyDescriptorsSimple);             // +0xc0
    vtbl[25] = reinterpret_cast<ComVtableFn>(&ProxyGetResourceAllocationInfo);         // +0xc8
    vtbl[26] = &ProxyGetCustomHeapProperties;                                          // +0xd0
    vtbl[27] = reinterpret_cast<ComVtableFn>(&ProxyCreateCommittedResource);           // +0xd8
    vtbl[28] = &ProxyCreateHeap;                                                       // +0xe0
    vtbl[29] = reinterpret_cast<ComVtableFn>(&ProxyCreatePlacedResource);              // +0xe8
    vtbl[36] = reinterpret_cast<ComVtableFn>(&ProxyCreateFence);                       // +0x120
    vtbl[37] = &ProxyGetDeviceRemovedReason;                                           // +0x128
    vtbl[38] = reinterpret_cast<ComVtableFn>(&ProxyGetCopyableFootprints);             // +0x130
    vtbl[39] = &ProxyCreateQueryHeap;                                                  // +0x138
    vtbl[41] = reinterpret_cast<ComVtableFn>(&ProxyCreateCommandSignature);            // +0x148
    vtbl[48] = &ProxyCreateCommandQueueX;                                              // +0x180
    vtbl[68] = &ProxySlot68XboxInfo;                                                   // +0x220
    vtbl[69] = &ProxySlot69XboxSetting;                                                // +0x228
    vtbl[73] = reinterpret_cast<ComVtableFn>(&ProxyCreateCommandListX);                // +0x248
    vtbl[78] = &ProxySetFrameIntervalX;                                                // +0x270
    vtbl[79] = &ProxyScheduleFrameEventX;                                              // +0x278
    vtbl[80] = reinterpret_cast<ComVtableFn>(&ProxyWaitFrameEventX);                   // +0x280
    return vtbl;
}

const std::array<ComVtableFn, 256> kXboxDeviceProxyVtbl = BuildXboxDeviceProxyVtable();

}  // namespace

extern "C" {

// ---------------------------------------------------------------------------
// Verified Xbox Series X|S d3d12_xs.dll entry points
// ---------------------------------------------------------------------------
void SCARLETTX_GDK_CALLBACK D3D12XboxSetProcessDebugFlags(uint32_t flags) {
    g_process_debug_flags.store(flags, std::memory_order_relaxed);
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][d3d12_xs] D3D12XboxSetProcessDebugFlags(0x"
                  << std::hex << flags << std::dec << ")\n";
    }
}

uint32_t SCARLETTX_GDK_CALLBACK D3D12XboxGetProcessDebugFlags(void) {
    return g_process_debug_flags.load(std::memory_order_relaxed);
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XboxCreateDevice(
    void* pAdapter,
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* pParameters,
    const void* riid,
    void** ppvDevice) {
#if defined(_WIN32)
    if (!riid || !ppvDevice) {
        return static_cast<int32_t>(E_INVALIDARG);
    }
    *ppvDevice = nullptr;

    if (TraceEnabled()) {
        const auto* g = reinterpret_cast<const uint32_t*>(riid);
        std::cerr << "[ScarlettX][d3d12_xs] D3D12XboxCreateDevice(adapter=" << pAdapter
                  << ", params=" << pParameters
                  << ", riid=" << std::hex << g[0] << "-" << g[1] << "-" << g[2] << "-" << g[3]
                  << std::dec << ")\n";
    }

    const bool prefer_warp =
        PreferWarpFromEnv(pParameters ? (pParameters->PreferWarp != 0) : true);
    const uint32_t dbg_flags =
        (pParameters ? pParameters->ProcessDebugFlags : 0u) |
        g_process_debug_flags.load(std::memory_order_relaxed);
    const bool debug_layer = (dbg_flags & D3D12XBOX_PROCESS_DEBUG_FLAG_DEBUG) != 0;

    const char* env_d3d_debug = std::getenv("SCARLETTX_D3D12_DEBUG");
    const bool force_debug_layer = env_d3d_debug && env_d3d_debug[0] == '1';
    if ((debug_layer && !RealTitleMode()) || force_debug_layer) {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(CallSystemD3D12GetDebugInterface(__uuidof(ID3D12Debug), reinterpret_cast<void**>(&dbg))) && dbg) {
            dbg->EnableDebugLayer();
            dbg->Release();
        }
    }
    if (const char* dred = std::getenv("SCARLETTX_DRED"); dred && dred[0] == '1') {
        // Device Removed Extended Data: breadcrumbs and GPU page-fault info,
        // reported by ReportDred() when the device is found removed.
        ID3D12DeviceRemovedExtendedDataSettings* s = nullptr;
        if (SUCCEEDED(CallSystemD3D12GetDebugInterface(__uuidof(ID3D12DeviceRemovedExtendedDataSettings),
                                                       reinterpret_cast<void**>(&s))) && s) {
            s->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            s->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            s->Release();
            std::cerr << "[ScarlettX][d3d12_xs] DRED enabled\n";
        }
    }

    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) {
        return static_cast<int32_t>(hr);
    }

    IDXGIAdapter1* adapter = nullptr;
    if (prefer_warp) {
        hr = factory->EnumWarpAdapter(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
    } else {
        if (pAdapter) {
            adapter = static_cast<IDXGIAdapter1*>(pAdapter);
            adapter->AddRef();
            DXGI_ADAPTER_DESC1 requested{};
            if (SUCCEEDED(adapter->GetDesc1(&requested)) &&
                (requested.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                adapter->Release();
                adapter = nullptr;
            }
        }
        if (!adapter) {
            IDXGIFactory6* factory6 = nullptr;
            if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory6),
                                                  reinterpret_cast<void**>(&factory6))) &&
                factory6) {
                factory6->EnumAdapterByGpuPreference(
                    0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                    reinterpret_cast<void**>(&adapter));
                factory6->Release();
            }
        }
        if (!adapter) {
            hr = factory->EnumAdapters1(0, &adapter);
        }
        if (FAILED(hr) || !adapter) {
            hr = factory->EnumWarpAdapter(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
        }
    }

    {
        DXGI_ADAPTER_DESC1 chosen{};
        if (adapter && SUCCEEDED(adapter->GetDesc1(&chosen))) {
            std::fprintf(stderr, "[ScarlettX][d3d12] Adapter: %ls%s\n", chosen.Description,
                         (chosen.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (WARP/software)"
                                                                     : " (hardware)");
            std::fflush(stderr);
        } else {
            std::fprintf(stderr, "[ScarlettX][d3d12] Adapter: none\n");
            std::fflush(stderr);
        }
    }

    if (RealTitleMode()) {
        // Real Scarlett guest binary: NEVER hand back a raw PC ID3D12Device* vtable.
        ID3D12Device* pc_dev = nullptr;
        hr = CallSystemD3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device),
                                         reinterpret_cast<void**>(&pc_dev));
        if (FAILED(hr)) {
            hr = CallSystemD3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                             reinterpret_cast<void**>(&pc_dev));
        }
        factory->Release();
        if (FAILED(hr) || !pc_dev) {
            if (adapter) adapter->Release();
            return static_cast<intptr_t>(hr);
        }
        auto* proxy = new XboxD3D12DeviceProxy();
        proxy->lpVtbl = kXboxDeviceProxyVtbl.data();
        proxy->sdk_version = pParameters ? pParameters->Version : 0;
        g_host_device.store(pc_dev, std::memory_order_release);
        if (const char* b = std::getenv("SCARLETTX_D3D12_BREAK"); b && b[0] == '1') {
            // With the debug layer on: raise a breakpoint inside the first
            // invalid call, so a crash log shows who made it.
            ID3D12InfoQueue* q = nullptr;
            if (SUCCEEDED(pc_dev->QueryInterface(__uuidof(ID3D12InfoQueue), reinterpret_cast<void**>(&q))) && q) {
                q->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
                q->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
                q->Release();
            }
        }
        if (TraceEnabled()) {
            std::cerr << "[ScarlettX][d3d12_xs] device SDK version 0x" << std::hex << proxy->sdk_version
                      << std::dec << "\n";
        }
        proxy->pc_device = pc_dev;
        proxy->pc_adapter = adapter;  // transfers ownership
        *ppvDevice = proxy;
        return S_OK;
    }

    const IID target_iid = *reinterpret_cast<const IID*>(riid);
    hr = CallSystemD3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, target_iid, ppvDevice);
    if (FAILED(hr)) {
        hr = CallSystemD3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, target_iid, ppvDevice);
    }

    if (adapter) adapter->Release();
    factory->Release();
    return static_cast<int32_t>(hr);
#else
    (void)pAdapter;
    (void)pParameters;
    (void)riid;
    (void)ppvDevice;
    return XGR_E_FAIL;
#endif
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XsCreateDevice(
    void* pAdapter,
    uint32_t minimumFeatureLevel,
    const void* riid,
    void** ppDevice) {
#if defined(_WIN32)
    if (!riid || !ppDevice) {
        return static_cast<int32_t>(E_INVALIDARG);
    }
    const IID target_iid = *reinterpret_cast<const IID*>(riid);
    return static_cast<int32_t>(CallSystemD3D12CreateDevice(
        static_cast<IUnknown*>(pAdapter),
        static_cast<D3D_FEATURE_LEVEL>(minimumFeatureLevel),
        target_iid,
        ppDevice));
#else
    (void)pAdapter; (void)minimumFeatureLevel; (void)riid; (void)ppDevice;
    return XGR_E_FAIL;
#endif
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XsGetDebugInterface(const void* riid, void** ppvDebug) {
#if defined(_WIN32)
    if (!riid || !ppvDebug) {
        return static_cast<int32_t>(E_INVALIDARG);
    }
    const IID target_iid = *reinterpret_cast<const IID*>(riid);
    return static_cast<int32_t>(CallSystemD3D12GetDebugInterface(target_iid, ppvDebug));
#else
    (void)riid; (void)ppvDebug;
    return XGR_E_FAIL;
#endif
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XsSerializeRootSignature(
    const void* pRootSignature,
    uint32_t version,
    void** ppBlob,
    void** ppErrorBlob) {
#if defined(_WIN32)
    if (!pRootSignature || !ppBlob) {
        return static_cast<int32_t>(E_INVALIDARG);
    }
    return static_cast<int32_t>(CallSystemD3D12SerializeRootSignature(
        static_cast<const D3D12_ROOT_SIGNATURE_DESC*>(pRootSignature),
        static_cast<D3D_ROOT_SIGNATURE_VERSION>(version),
        reinterpret_cast<ID3DBlob**>(ppBlob),
        reinterpret_cast<ID3DBlob**>(ppErrorBlob)));
#else
    (void)pRootSignature; (void)version; (void)ppBlob; (void)ppErrorBlob;
    return XGR_E_FAIL;
#endif
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XsSerializeVersionedRootSignature(
    const void* pRootSignature,
    void** ppBlob,
    void** ppErrorBlob) {
#if defined(_WIN32)
    if (!pRootSignature || !ppBlob) {
        return static_cast<int32_t>(E_INVALIDARG);
    }
    return static_cast<int32_t>(CallSystemD3D12SerializeVersionedRootSignature(
        static_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*>(pRootSignature),
        reinterpret_cast<ID3DBlob**>(ppBlob),
        reinterpret_cast<ID3DBlob**>(ppErrorBlob)));
#else
    (void)pRootSignature; (void)ppBlob; (void)ppErrorBlob;
    return XGR_E_FAIL;
#endif
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XsCreateRootSignatureDeserializer(
    const void* pSrcData,
    size_t srcDataSizeInBytes,
    const void* pRootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer) {
#if defined(_WIN32)
    if (!pSrcData || !pRootSignatureDeserializerInterface || !ppRootSignatureDeserializer) {
        return static_cast<int32_t>(E_INVALIDARG);
    }
    const IID target_iid = *reinterpret_cast<const IID*>(pRootSignatureDeserializerInterface);
    return static_cast<int32_t>(CallSystemD3D12CreateRootSignatureDeserializer(
        pSrcData,
        srcDataSizeInBytes,
        target_iid,
        ppRootSignatureDeserializer));
#else
    (void)pSrcData; (void)srcDataSizeInBytes;
    (void)pRootSignatureDeserializerInterface; (void)ppRootSignatureDeserializer;
    return XGR_E_FAIL;
#endif
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XsEnableExperimentalFeatures(
    uint32_t numFeatures,
    const void* pIIDs,
    void* pConfigurationStructs,
    uint32_t* pConfigurationStructSizes) {
#if defined(_WIN32)
    HRESULT hr = D3D12EnableExperimentalFeatures(
        numFeatures,
        static_cast<const IID*>(pIIDs),
        pConfigurationStructs,
        pConfigurationStructSizes);
    // On headless WARP/CI without developer mode, swallow E_NOINTERFACE gracefully.
    if (hr == E_NOINTERFACE) {
        return S_OK;
    }
    return static_cast<int32_t>(hr);
#else
    (void)numFeatures; (void)pIIDs; (void)pConfigurationStructs; (void)pConfigurationStructSizes;
    return XGR_S_OK;
#endif
}

// ---------------------------------------------------------------------------
// ScarlettX synthetic handle-based device, frame pipeline & DXR test helpers
// ---------------------------------------------------------------------------
int32_t D3D12XboxCreateSyntheticDevice(const D3D12XBOX_CREATE_DEVICE_PARAMETERS* params,
                                       uint64_t* out_device_handle) {
    if (!out_device_handle) {
        return -1;
    }
    const bool prefer_warp = PreferWarpFromEnv(params ? (params->PreferWarp != 0) : true);
    const bool debug_layer = params ? (params->ProcessDebugFlags != 0) : false;

    auto state = std::make_unique<D3D12XsDeviceState>();
    state->backend = std::make_unique<scarlettx::D3D12Backend>();
    auto st = state->backend->Initialize(prefer_warp, debug_layer);
    if (!st.ok()) {
        return static_cast<int32_t>(st.code);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t handle = g_next_handle++;
    g_devices[handle] = std::move(state);
    *out_device_handle = handle;
    return 0;
}

int32_t D3D12XboxDestroyDevice(uint64_t device_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    g_devices.erase(it);
    return 0;
}

int32_t D3D12XboxGetNativeDevice(uint64_t device_handle, void** out_id3d12_device) {
    if (!out_id3d12_device) {
        return -1;
    }
    *out_id3d12_device = nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    void* dev = it->second->backend->NativeDevice();
    if (!dev) {
        return -3;
    }
#if defined(_WIN32)
    static_cast<ID3D12Device*>(dev)->AddRef();
#endif
    *out_id3d12_device = dev;
    return 0;
}

int32_t D3D12XboxQueryCapabilities(uint64_t device_handle, D3D12XBOX_CAPABILITIES* out_caps) {
    if (!out_caps) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    const auto& info = it->second->backend->AdapterInfo();
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->RaytracingTier = info.raytracing_tier;
    out_caps->IsWarpAdapter = info.is_warp ? 1u : 0u;
    out_caps->MaxRenderWidth = 3840;
    out_caps->MaxRenderHeight = 2160;
    std::strncpy(out_caps->AdapterName, info.description.c_str(), sizeof(out_caps->AdapterName) - 1);
    return 0;
}

int32_t D3D12XboxQueryRaytracingCapabilities(uint64_t device_handle,
                                             D3D12XBOX_RAYTRACING_CAPABILITIES* out_rt_caps) {
    if (!out_rt_caps) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    const auto& info = it->second->backend->AdapterInfo();
    std::memset(out_rt_caps, 0, sizeof(*out_rt_caps));
    out_rt_caps->HardwareRaytracingTier = info.raytracing_tier;
    out_rt_caps->ActiveBackend = (info.raytracing_tier >= 10)
        ? D3D12XBOX_RT_BACKEND_HARDWARE_DXR
        : D3D12XBOX_RT_BACKEND_SOFTWARE_BVH;
    out_rt_caps->SupportsInlineRaytracing = (info.raytracing_tier >= 11) ? 1u : 0u;
    out_rt_caps->MaxRecursionDepth = 31;
    std::strncpy(out_rt_caps->AdapterName, info.description.c_str(), sizeof(out_rt_caps->AdapterName) - 1);
    return 0;
}

int32_t D3D12XboxSetFrameIntervalX(uint64_t device_handle, uint32_t interval, uint32_t flags) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    it->second->frame_interval = interval;
    it->second->frame_interval_flags = flags;
    return 0;
}

int32_t D3D12XboxScheduleFrameEventX(uint64_t device_handle,
                                     uint32_t frame_event,
                                     uint64_t offset_us,
                                     void* event_handle) {
    (void)frame_event;
    (void)offset_us;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
#if defined(_WIN32)
    if (event_handle) {
        SetEvent(static_cast<HANDLE>(event_handle));
    }
#else
    (void)event_handle;
#endif
    return 0;
}

int32_t D3D12XboxWaitFrameEventX(uint64_t device_handle, uint64_t* out_frame_token) {
    return D3D12XboxWaitFrameEventXEx(device_handle, D3D12XBOX_FRAME_EVENT_ORIGIN, 0, out_frame_token);
}

int32_t D3D12XboxWaitFrameEventXEx(uint64_t device_handle,
                                   uint32_t frame_event,
                                   uint32_t timeout_ms,
                                   D3D12XBOX_FRAME_PIPELINE_TOKEN* out_frame_token) {
    (void)frame_event;
    (void)timeout_ms;
    if (!out_frame_token) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    const uint64_t tok = it->second->next_frame_token++;
    it->second->last_issued_token = tok;
    *out_frame_token = tok;
    return 0;
}

int32_t D3D12XboxRenderTestTriangle(uint64_t device_handle,
                                    uint32_t width,
                                    uint32_t height,
                                    const char* optional_ppm_path,
                                    uint64_t* out_pixel_hash) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    scarlettx::FrameBufferRgba8 fb;
    auto st = it->second->backend->RenderTestTriangle(width, height, fb);
    if (!st.ok()) {
        return static_cast<int32_t>(st.code);
    }
    if (optional_ppm_path && *optional_ppm_path) {
        fb.WritePpm(optional_ppm_path);
    }
    if (out_pixel_hash) {
        *out_pixel_hash = fb.ComputeFnv1aHash();
    }
    return 0;
}

int32_t D3D12XboxPresentX(uint64_t device_handle, const D3D12XBOX_PRESENT_PLANE_PARAMETERS* plane) {
    if (!plane) {
        return -1;
    }
    D3D12XBOX_PRESENT_PARAMETERS params{};
    params.Token = plane->Token;
    params.PlaneCount = 1;
    params.Flags = D3D12XBOX_PRESENT_FLAG_NONE;
    params.Planes = plane;
    params.CommandQueue = nullptr;
    return D3D12XboxPresentXParams(device_handle, &params);
}

int32_t D3D12XboxPresentXParams(uint64_t device_handle, const D3D12XBOX_PRESENT_PARAMETERS* params) {
    if (!params) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    if (dev.suspended) {
        return -4;
    }

#if defined(_WIN32)
    if (params->CommandQueue) {
        auto* queue = static_cast<ID3D12CommandQueue*>(params->CommandQueue);
        auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
        if (native_dev) {
            ID3D12Fence* fence = nullptr;
            if (SUCCEEDED(native_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                                  reinterpret_cast<void**>(&fence))) && fence) {
                HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (ev) {
                    if (SUCCEEDED(queue->Signal(fence, 1))) {
                        if (fence->GetCompletedValue() < 1) {
                            fence->SetEventOnCompletion(1, ev);
                            WaitForSingleObject(ev, 5000);
                        }
                    }
                    CloseHandle(ev);
                }
                fence->Release();
            }
        }
    }
#endif

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - dev.last_present_time).count());
    dev.last_frame_duration_us = std::max<uint64_t>(1, elapsed_us);
    dev.last_present_time = now;
    dev.last_presented_token = params->Token;
    dev.presented_frame_count++;
    return 0;
}

int32_t D3D12XboxGetFrameStatisticsX(uint64_t device_handle,
                                     D3D12XBOX_FRAME_STATISTICS* out_stats) {
    if (!out_stats) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    const auto& dev = *it->second;
    out_stats->PresentedFrameCount = dev.presented_frame_count;
    out_stats->LastPresentedToken = dev.last_presented_token;
    out_stats->LastIssuedToken = dev.last_issued_token;
    out_stats->IntervalHz = IntervalToHz(dev.frame_interval);
    out_stats->Suspended = dev.suspended ? 1u : 0u;
    out_stats->LastFrameDurationUs = dev.last_frame_duration_us;
    return 0;
}

int32_t D3D12XboxSuspendX(uint64_t device_handle, uint32_t flags) {
    (void)flags;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    it->second->suspended = true;
    return 0;
}

int32_t D3D12XboxResumeX(uint64_t device_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    it->second->suspended = false;
    return 0;
}

int32_t D3D12XboxGetResourceAllocationInfo(uint64_t device_handle,
                                           uint64_t width,
                                           uint32_t height,
                                           uint32_t dxgi_format,
                                           uint64_t* out_size_bytes,
                                           uint64_t* out_alignment) {
    if (!out_size_bytes || !out_alignment || width == 0 || height == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
#if defined(_WIN32)
    auto* dev = static_cast<ID3D12Device*>(it->second->backend->NativeDevice());
    if (dev) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = static_cast<DXGI_FORMAT>(dxgi_format);
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        const D3D12_RESOURCE_ALLOCATION_INFO info = dev->GetResourceAllocationInfo(0, 1, &desc);
        *out_size_bytes = info.SizeInBytes;
        *out_alignment = info.Alignment;
        return 0;
    }
#endif
    (void)dxgi_format;
    *out_alignment = 65536;
    *out_size_bytes = AlignUp(width * static_cast<uint64_t>(height) * 4, 65536);
    return 0;
}

int32_t D3D12XboxGetRaytracingAccelerationStructurePrebuildInfo(
    uint64_t device_handle,
    const D3D12XBOX_RT_AS_INPUTS* inputs,
    D3D12XBOX_RT_AS_PREBUILD_INFO* out_info) {
    if (!inputs || !out_info) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }

#if defined(_WIN32)
    const auto& adapter = it->second->backend->AdapterInfo();
    auto* dev5 = static_cast<ID3D12Device5*>(it->second->backend->NativeDevice5());
    if (dev5 && adapter.raytracing_tier >= 10) {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS dxr_inputs{};
        dxr_inputs.Type = (inputs->Type == D3D12XBOX_AS_TYPE_TOP_LEVEL)
            ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL
            : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        dxr_inputs.Flags = static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(inputs->Flags);
        dxr_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

        D3D12_RAYTRACING_GEOMETRY_DESC geom{};
        if (inputs->Type == D3D12XBOX_AS_TYPE_BOTTOM_LEVEL) {
            geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            geom.Triangles.VertexCount = inputs->NumDescs * 3;
            dxr_inputs.NumDescs = 1;
            dxr_inputs.pGeometryDescs = &geom;
        } else {
            dxr_inputs.NumDescs = inputs->NumDescs;
        }

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        dev5->GetRaytracingAccelerationStructurePrebuildInfo(&dxr_inputs, &info);
        out_info->ResultDataMaxSizeInBytes = info.ResultDataMaxSizeInBytes;
        out_info->ScratchDataSizeInBytes = info.ScratchDataSizeInBytes;
        out_info->UpdateScratchDataSizeInBytes = info.UpdateScratchDataSizeInBytes;
        // Hosts that update in place (current WARP) report 0 here. Keep the size
        // non-zero when updates were requested so the guest can allocate the buffer.
        if ((inputs->Flags & D3D12XBOX_AS_BUILD_FLAG_ALLOW_UPDATE) &&
            out_info->UpdateScratchDataSizeInBytes == 0) {
            out_info->UpdateScratchDataSizeInBytes = 256;
        }
        return 0;
    }
#endif

    // Deterministic Software BVH prebuild sizing (256-byte aligned like DXR)
    const uint64_t n = std::max<uint64_t>(1, inputs->NumDescs);
    const uint64_t node_bytes = (2 * n) * 64;
    const uint64_t prim_bytes = n * sizeof(SCARLETT_BVH_TRIANGLE);
    out_info->ResultDataMaxSizeInBytes = AlignUp(node_bytes + prim_bytes + 256, 256);
    out_info->ScratchDataSizeInBytes = AlignUp(n * 128 + 256, 256);
    out_info->UpdateScratchDataSizeInBytes =
        (inputs->Flags & D3D12XBOX_AS_BUILD_FLAG_ALLOW_UPDATE)
            ? AlignUp(n * 64 + 256, 256)
            : 0;
    return 0;
}

int32_t D3D12XboxBuildRaytracingAccelerationStructure(
    uint64_t device_handle,
    const D3D12XBOX_RT_AS_BUILD_DESC* desc,
    uint64_t* out_as_handle) {
    if (!desc || !out_as_handle || desc->Inputs.NumDescs == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto entry = std::make_unique<AccelerationStructureEntry>();
    entry->type = desc->Inputs.Type;
    entry->flags = desc->Inputs.Flags;

    if (desc->Inputs.Type == D3D12XBOX_AS_TYPE_BOTTOM_LEVEL) {
        if (!desc->Triangles) {
            return -1;
        }
        entry->triangles.reserve(desc->Inputs.NumDescs);
        for (uint32_t i = 0; i < desc->Inputs.NumDescs; ++i) {
            const auto& src = desc->Triangles[i];
            scarlettx::RtTriangle tri{};
            tri.v0 = {src.v0[0], src.v0[1], src.v0[2]};
            tri.v1 = {src.v1[0], src.v1[1], src.v1[2]};
            tri.v2 = {src.v2[0], src.v2[1], src.v2[2]};
            tri.color = {src.color[0], src.color[1], src.color[2]};
            tri.reflectivity = src.reflectivity;
            entry->triangles.push_back(tri);
        }
        auto st = entry->bvh.Build(entry->triangles);
        if (!st.ok()) {
            return static_cast<int32_t>(st.code);
        }
    } else if (desc->Inputs.Type == D3D12XBOX_AS_TYPE_TOP_LEVEL) {
        if (!desc->BlasHandles) {
            return -1;
        }
        std::vector<scarlettx::RtTriangle> combined;
        for (uint32_t i = 0; i < desc->Inputs.NumDescs; ++i) {
            auto bit = dev.accel_structs.find(desc->BlasHandles[i]);
            if (bit == dev.accel_structs.end() ||
                bit->second->type != D3D12XBOX_AS_TYPE_BOTTOM_LEVEL) {
                return -2;
            }
            combined.insert(combined.end(),
                            bit->second->triangles.begin(),
                            bit->second->triangles.end());
        }
        entry->triangles = combined;
        auto st = entry->bvh.Build(entry->triangles);
        if (!st.ok()) {
            return static_cast<int32_t>(st.code);
        }
    } else {
        return -1;
    }

    const uint64_t handle = dev.next_as_handle++;
    dev.accel_structs[handle] = std::move(entry);
    *out_as_handle = handle;
    return 0;
}

int32_t D3D12XboxDestroyRaytracingAccelerationStructure(uint64_t device_handle, uint64_t as_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    return it->second->accel_structs.erase(as_handle) > 0 ? 0 : -2;
}

// ---------------------------------------------------------------------------
// Milestone 6: Shader Inspection, Root Signatures, Descriptor Heaps & PSOs
// ---------------------------------------------------------------------------
int32_t D3D12XboxInspectShader(
    const D3D12XBOX_SHADER_BYTECODE* shader,
    D3D12XBOX_SHADER_INSPECTION* out_info) {
    if (!shader || !shader->Bytecode || shader->BytecodeLength == 0 || !out_info) {
        return -1;
    }
    std::memset(out_info, 0, sizeof(*out_info));
    const auto* bytes = static_cast<const uint8_t*>(shader->Bytecode);
    std::vector<uint8_t> buf(bytes, bytes + shader->BytecodeLength);
    scarlettx::ShaderModuleInfo info{};
    auto st = scarlettx::InspectShaderContainer(buf, info);
    if (!st.ok()) {
        return static_cast<int32_t>(st.code);
    }
    out_info->ContainerKind = static_cast<uint32_t>(info.kind);
    out_info->Stage = static_cast<uint32_t>(info.stage);
    out_info->ShaderModelMajor = info.shader_model_major;
    out_info->ShaderModelMinor = info.shader_model_minor;
    out_info->ChunkCount = info.chunk_count;
    out_info->InputParamCount = info.input_param_count;
    out_info->OutputParamCount = info.output_param_count;
    out_info->HasRaytracingExports = info.has_raytracing_exports ? 1u : 0u;
    out_info->HasEmbeddedRootSignature = info.has_embedded_root_signature ? 1u : 0u;
    out_info->HasXboxPrivateChunks = info.has_xbox_private_chunks ? 1u : 0u;
    out_info->ContentHash = info.content_hash;
    return 0;
}

int32_t D3D12XboxCreateRootSignature(
    uint64_t device_handle,
    const D3D12XBOX_ROOT_SIGNATURE_DESC* desc,
    uint64_t* out_root_sig_handle) {
    if (!desc || !out_root_sig_handle) {
        return -1;
    }
    *out_root_sig_handle = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto entry = std::make_unique<RootSignatureEntry>();
    entry->desc = *desc;

#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
    if (!native_dev) {
        return -3;
    }
    std::vector<D3D12_ROOT_PARAMETER> params;
    if (desc->NumRootConstants32Bit > 0) {
        entry->constants_param_idx = static_cast<int32_t>(params.size());
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p.Constants.ShaderRegister = 0;
        p.Constants.RegisterSpace = 0;
        p.Constants.Num32BitValues = desc->NumRootConstants32Bit;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    for (uint32_t i = 0; i < desc->NumCbvs; ++i) {
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p.Descriptor.ShaderRegister = (desc->NumRootConstants32Bit > 0 ? 1u : 0u) + i;
        p.Descriptor.RegisterSpace = 0;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    for (uint32_t i = 0; i < desc->NumSrvs; ++i) {
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p.Descriptor.ShaderRegister = i;
        p.Descriptor.RegisterSpace = 0;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    for (uint32_t i = 0; i < desc->NumUavs; ++i) {
        if (i == 0) {
            entry->uav0_param_idx = static_cast<int32_t>(params.size());
        }
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p.Descriptor.ShaderRegister = i;
        p.Descriptor.RegisterSpace = 0;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = static_cast<UINT>(params.size());
    rs_desc.pParameters = params.empty() ? nullptr : params.data();
    rs_desc.NumStaticSamplers = 0;
    rs_desc.pStaticSamplers = nullptr;
    rs_desc.Flags = desc->Flags
        ? static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(desc->Flags)
        : D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* sig_blob = nullptr;
    ID3DBlob* err_blob = nullptr;
    HRESULT hr = CallSystemD3D12SerializeRootSignature(
        &rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &err_blob);
    if (err_blob) err_blob->Release();
    if (FAILED(hr) || !sig_blob) {
        if (sig_blob) sig_blob->Release();
        return static_cast<int32_t>(hr);
    }

    hr = native_dev->CreateRootSignature(
        0,
        sig_blob->GetBufferPointer(),
        sig_blob->GetBufferSize(),
        __uuidof(ID3D12RootSignature),
        reinterpret_cast<void**>(&entry->native));
    sig_blob->Release();
    if (FAILED(hr) || !entry->native) {
        return static_cast<int32_t>(hr);
    }
#endif

    const uint64_t handle = dev.next_root_sig_handle++;
    dev.root_signatures[handle] = std::move(entry);
    *out_root_sig_handle = handle;
    return 0;
}

int32_t D3D12XboxDestroyRootSignature(uint64_t device_handle, uint64_t root_sig_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    return it->second->root_signatures.erase(root_sig_handle) > 0 ? 0 : -2;
}

int32_t D3D12XboxCreateDescriptorHeap(
    uint64_t device_handle,
    const D3D12XBOX_DESCRIPTOR_HEAP_DESC* desc,
    uint64_t* out_heap_handle) {
    if (!desc || desc->NumDescriptors == 0 || !out_heap_handle) {
        return -1;
    }
    *out_heap_handle = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto entry = std::make_unique<DescriptorHeapEntry>();
    entry->desc = *desc;

#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
    if (!native_dev) {
        return -3;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hdesc{};
    hdesc.Type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(desc->Type);
    hdesc.NumDescriptors = desc->NumDescriptors;
    hdesc.Flags = (desc->ShaderVisible &&
                   (desc->Type == D3D12XBOX_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
                    desc->Type == D3D12XBOX_DESCRIPTOR_HEAP_TYPE_SAMPLER))
                      ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                      : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hdesc.NodeMask = 0;
    HRESULT hr = native_dev->CreateDescriptorHeap(
        &hdesc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&entry->native));
    if (FAILED(hr) || !entry->native) {
        return static_cast<int32_t>(hr);
    }
#endif

    const uint64_t handle = dev.next_heap_handle++;
    dev.descriptor_heaps[handle] = std::move(entry);
    *out_heap_handle = handle;
    return 0;
}

int32_t D3D12XboxDestroyDescriptorHeap(uint64_t device_handle, uint64_t heap_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    return it->second->descriptor_heaps.erase(heap_handle) > 0 ? 0 : -2;
}

uint32_t D3D12XboxGetDescriptorHandleIncrementSize(uint64_t device_handle, uint32_t heap_type) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return 0;
    }
#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(it->second->backend->NativeDevice());
    if (native_dev) {
        return native_dev->GetDescriptorHandleIncrementSize(
            static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(heap_type));
    }
#endif
    (void)heap_type;
    return 32;
}

int32_t D3D12XboxCreateGraphicsPipelineState(
    uint64_t device_handle,
    const D3D12XBOX_GRAPHICS_PSO_DESC* desc,
    uint64_t* out_pso_handle) {
    if (!desc || !out_pso_handle || !desc->VS.Bytecode || desc->VS.BytecodeLength == 0 ||
        !desc->PS.Bytecode || desc->PS.BytecodeLength == 0) {
        return -1;
    }
    *out_pso_handle = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto rs_it = dev.root_signatures.find(desc->RootSignatureHandle);
    if (rs_it == dev.root_signatures.end()) {
        return -2;
    }

    std::vector<uint8_t> vs_in(
        static_cast<const uint8_t*>(desc->VS.Bytecode),
        static_cast<const uint8_t*>(desc->VS.Bytecode) + desc->VS.BytecodeLength);
    std::vector<uint8_t> ps_in(
        static_cast<const uint8_t*>(desc->PS.Bytecode),
        static_cast<const uint8_t*>(desc->PS.Bytecode) + desc->PS.BytecodeLength);

    scarlettx::ShaderTranslationResult vs_tr{};
    scarlettx::ShaderTranslationResult ps_tr{};
    if (auto st = scarlettx::TranslateXboxShaderToPc(vs_in, vs_tr); !st.ok()) {
        return static_cast<int32_t>(st.code);
    }
    if (auto st = scarlettx::TranslateXboxShaderToPc(ps_in, ps_tr); !st.ok()) {
        return static_cast<int32_t>(st.code);
    }

    const uint32_t rtv_fmt = desc->RtvFormat ? desc->RtvFormat : 28u;  // DXGI_FORMAT_R8G8B8A8_UNORM
    const uint64_t pso_key = vs_tr.translated_hash ^ (ps_tr.translated_hash * 1315423911ull) ^
                             (desc->RootSignatureHandle << 1) ^ static_cast<uint64_t>(rtv_fmt);

    auto entry = std::make_unique<PipelineStateEntry>();
    entry->is_compute = false;
    entry->root_sig_handle = desc->RootSignatureHandle;
    entry->rtv_format = rtv_fmt;
    entry->pso_key = pso_key;

#if defined(_WIN32)
    auto cache_it = dev.pso_cache.find(pso_key);
    if (cache_it != dev.pso_cache.end() && cache_it->second) {
        ++dev.pso_cache_hits;
        entry->native = cache_it->second;
        entry->native->AddRef();
    } else {
        ++dev.pso_cache_misses;
        auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
        if (!native_dev || !rs_it->second->native) {
            return -3;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = rs_it->second->native;
        pso_desc.VS = {vs_tr.pc_bytecode.data(), vs_tr.pc_bytecode.size()};
        pso_desc.PS = {ps_tr.pc_bytecode.data(), ps_tr.pc_bytecode.size()};
        pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
        pso_desc.BlendState.IndependentBlendEnable = FALSE;
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.DepthStencilState.DepthEnable = FALSE;
        pso_desc.DepthStencilState.StencilEnable = FALSE;
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = static_cast<DXGI_FORMAT>(rtv_fmt);
        pso_desc.SampleDesc.Count = desc->SampleCount ? desc->SampleCount : 1u;

        HRESULT hr = native_dev->CreateGraphicsPipelineState(
            &pso_desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&entry->native));
        if (FAILED(hr) || !entry->native) {
            return static_cast<int32_t>(hr);
        }
        entry->native->AddRef();
        dev.pso_cache[pso_key] = entry->native;
    }
#endif

    const uint64_t handle = dev.next_pso_handle++;
    dev.psos[handle] = std::move(entry);
    *out_pso_handle = handle;
    return 0;
}

int32_t D3D12XboxCreateComputePipelineState(
    uint64_t device_handle,
    const D3D12XBOX_COMPUTE_PSO_DESC* desc,
    uint64_t* out_pso_handle) {
    if (!desc || !out_pso_handle || !desc->CS.Bytecode || desc->CS.BytecodeLength == 0) {
        return -1;
    }
    *out_pso_handle = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto rs_it = dev.root_signatures.find(desc->RootSignatureHandle);
    if (rs_it == dev.root_signatures.end()) {
        return -2;
    }

    std::vector<uint8_t> cs_in(
        static_cast<const uint8_t*>(desc->CS.Bytecode),
        static_cast<const uint8_t*>(desc->CS.Bytecode) + desc->CS.BytecodeLength);
    scarlettx::ShaderTranslationResult cs_tr{};
    if (auto st = scarlettx::TranslateXboxShaderToPc(cs_in, cs_tr); !st.ok()) {
        return static_cast<int32_t>(st.code);
    }

    const uint64_t pso_key = cs_tr.translated_hash ^ (desc->RootSignatureHandle << 3) ^ 0xC0DE0001ull;
    auto entry = std::make_unique<PipelineStateEntry>();
    entry->is_compute = true;
    entry->root_sig_handle = desc->RootSignatureHandle;
    entry->pso_key = pso_key;

#if defined(_WIN32)
    auto cache_it = dev.pso_cache.find(pso_key);
    if (cache_it != dev.pso_cache.end() && cache_it->second) {
        ++dev.pso_cache_hits;
        entry->native = cache_it->second;
        entry->native->AddRef();
    } else {
        ++dev.pso_cache_misses;
        auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
        if (!native_dev || !rs_it->second->native) {
            return -3;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = rs_it->second->native;
        pso_desc.CS = {cs_tr.pc_bytecode.data(), cs_tr.pc_bytecode.size()};
        HRESULT hr = native_dev->CreateComputePipelineState(
            &pso_desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&entry->native));
        if (FAILED(hr) || !entry->native) {
            return static_cast<int32_t>(hr);
        }
        entry->native->AddRef();
        dev.pso_cache[pso_key] = entry->native;
    }
#endif

    const uint64_t handle = dev.next_pso_handle++;
    dev.psos[handle] = std::move(entry);
    *out_pso_handle = handle;
    return 0;
}

int32_t D3D12XboxDestroyPipelineState(uint64_t device_handle, uint64_t pso_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    return it->second->psos.erase(pso_handle) > 0 ? 0 : -2;
}

int32_t D3D12XboxDispatchCompute(
    uint64_t device_handle,
    uint64_t pso_handle,
    uint32_t group_x,
    uint32_t group_y,
    uint32_t group_z,
    const uint32_t* root_constants,
    uint32_t root_constant_count,
    void* out_uav_data,
    uint32_t uav_byte_size) {
    if (group_x == 0 || group_y == 0 || group_z == 0 || !out_uav_data || uav_byte_size == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto pso_it = dev.psos.find(pso_handle);
    if (pso_it == dev.psos.end() || !pso_it->second->is_compute) {
        return -2;
    }
    auto rs_it = dev.root_signatures.find(pso_it->second->root_sig_handle);
    if (rs_it == dev.root_signatures.end()) {
        return -2;
    }

#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
    if (!native_dev || !pso_it->second->native || !rs_it->second->native) {
        return -3;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12Resource* uav_buf = nullptr;
    ID3D12Resource* rb_buf = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;

    auto cleanup = [&]() {
        if (ev) CloseHandle(ev);
        if (fence) fence->Release();
        if (rb_buf) rb_buf->Release();
        if (uav_buf) uav_buf->Release();
        if (cmd) cmd->Release();
        if (alloc) alloc->Release();
        if (queue) queue->Release();
    };

    const uint64_t aligned_bytes = AlignUp(uav_byte_size, 256);

    HRESULT hr = native_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    hr = native_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&alloc));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    hr = native_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, pso_it->second->native, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&cmd));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    D3D12_HEAP_PROPERTIES def_heap{};
    def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC uav_desc{};
    uav_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uav_desc.Width = aligned_bytes;
    uav_desc.Height = 1;
    uav_desc.DepthOrArraySize = 1;
    uav_desc.MipLevels = 1;
    uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    uav_desc.SampleDesc.Count = 1;
    uav_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uav_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    hr = native_dev->CreateCommittedResource(
        &def_heap, D3D12_HEAP_FLAG_NONE, &uav_desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(&uav_buf));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    D3D12_HEAP_PROPERTIES rb_heap{};
    rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb_desc = uav_desc;
    rb_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    hr = native_dev->CreateCommittedResource(
        &rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(&rb_buf));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    cmd->SetComputeRootSignature(rs_it->second->native);
    if (rs_it->second->constants_param_idx >= 0 && root_constants && root_constant_count > 0) {
        const uint32_t n = std::min(root_constant_count, rs_it->second->desc.NumRootConstants32Bit);
        cmd->SetComputeRoot32BitConstants(
            static_cast<UINT>(rs_it->second->constants_param_idx), n, root_constants, 0);
    }
    if (rs_it->second->uav0_param_idx >= 0) {
        cmd->SetComputeRootUnorderedAccessView(
            static_cast<UINT>(rs_it->second->uav0_param_idx), uav_buf->GetGPUVirtualAddress());
    }
    cmd->Dispatch(group_x, group_y, group_z);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = uav_buf;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);
    cmd->CopyBufferRegion(rb_buf, 0, uav_buf, 0, uav_byte_size);

    hr = cmd->Close();
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    ID3D12CommandList* lists[] = {cmd};
    queue->ExecuteCommandLists(1, lists);

    hr = native_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!ev) { cleanup(); return -4; }
    hr = queue->Signal(fence, 1);
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, 5000);
    }

    void* mapped = nullptr;
    D3D12_RANGE range{0, uav_byte_size};
    hr = rb_buf->Map(0, &range, &mapped);
    if (FAILED(hr) || !mapped) { cleanup(); return static_cast<int32_t>(hr); }
    std::memcpy(out_uav_data, mapped, uav_byte_size);
    D3D12_RANGE empty_range{0, 0};
    rb_buf->Unmap(0, &empty_range);

    cleanup();
    return 0;
#else
    (void)root_constants;
    (void)root_constant_count;
    return -3;
#endif
}

int32_t D3D12XboxRenderWithPipelineState(
    uint64_t device_handle,
    uint64_t pso_handle,
    uint32_t width,
    uint32_t height,
    uint32_t vertex_count,
    const char* optional_ppm_path,
    uint64_t* out_pixel_hash) {
    if (width == 0 || height == 0 || vertex_count == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto pso_it = dev.psos.find(pso_handle);
    if (pso_it == dev.psos.end() || pso_it->second->is_compute) {
        return -2;
    }
    auto rs_it = dev.root_signatures.find(pso_it->second->root_sig_handle);
    if (rs_it == dev.root_signatures.end()) {
        return -2;
    }

#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
    if (!native_dev || !pso_it->second->native || !rs_it->second->native) {
        return -3;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12Resource* rt = nullptr;
    ID3D12Resource* rb = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;

    auto cleanup = [&]() {
        if (ev) CloseHandle(ev);
        if (fence) fence->Release();
        if (rb) rb->Release();
        if (rt) rt->Release();
        if (rtv_heap) rtv_heap->Release();
        if (cmd) cmd->Release();
        if (alloc) alloc->Release();
        if (queue) queue->Release();
    };

    HRESULT hr = native_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    hr = native_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&alloc));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    hr = native_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, pso_it->second->native, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&cmd));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    hr = native_dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&rtv_heap));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    D3D12_HEAP_PROPERTIES def_heap{};
    def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rt_desc{};
    rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_desc.Width = width;
    rt_desc.Height = height;
    rt_desc.DepthOrArraySize = 1;
    rt_desc.MipLevels = 1;
    rt_desc.Format = static_cast<DXGI_FORMAT>(pso_it->second->rtv_format);
    rt_desc.SampleDesc.Count = 1;
    rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE cv{};
    cv.Format = rt_desc.Format;
    cv.Color[0] = 0.05f;
    cv.Color[1] = 0.07f;
    cv.Color[2] = 0.11f;
    cv.Color[3] = 1.0f;

    hr = native_dev->CreateCommittedResource(
        &def_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(&rt));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    native_dev->CreateRenderTargetView(rt, nullptr, rtv_handle);

    const uint64_t row_pitch = AlignUp(static_cast<uint64_t>(width) * 4ull, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint64_t rb_size = row_pitch * height;
    D3D12_HEAP_PROPERTIES rb_heap{};
    rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb_desc{};
    rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb_desc.Width = rb_size;
    rb_desc.Height = 1;
    rb_desc.DepthOrArraySize = 1;
    rb_desc.MipLevels = 1;
    rb_desc.Format = DXGI_FORMAT_UNKNOWN;
    rb_desc.SampleDesc.Count = 1;
    rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    hr = native_dev->CreateCommittedResource(
        &rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(&rb));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    D3D12_RECT sc{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    cmd->SetGraphicsRootSignature(rs_it->second->native);
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
    cmd->ClearRenderTargetView(rtv_handle, cv.Color, 0, nullptr);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(vertex_count, 1, 0, 0);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = rt;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = rt_desc.Format;
    dst.PlacedFootprint.Footprint.Width = width;
    dst.PlacedFootprint.Footprint.Height = height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(row_pitch);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = rt;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    hr = cmd->Close();
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    ID3D12CommandList* lists[] = {cmd};
    queue->ExecuteCommandLists(1, lists);

    hr = native_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!ev) { cleanup(); return -4; }
    queue->Signal(fence, 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, 5000);
    }

    void* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(rb_size)};
    hr = rb->Map(0, &range, &mapped);
    if (FAILED(hr) || !mapped) { cleanup(); return static_cast<int32_t>(hr); }

    scarlettx::FrameBufferRgba8 frame{};
    frame.width = width;
    frame.height = height;
    frame.pixels.resize(static_cast<size_t>(width) * height * 4ull);
    const auto* src_bytes = static_cast<const uint8_t*>(mapped);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(frame.pixels.data() + static_cast<size_t>(y) * width * 4ull,
                    src_bytes + static_cast<size_t>(y) * row_pitch,
                    static_cast<size_t>(width) * 4ull);
    }
    D3D12_RANGE empty_range{0, 0};
    rb->Unmap(0, &empty_range);

    if (out_pixel_hash) {
        *out_pixel_hash = frame.ComputeFnv1aHash();
    }
    if (optional_ppm_path && optional_ppm_path[0] != '\0') {
        auto st = frame.WritePpm(optional_ppm_path);
        if (!st.ok()) {
            cleanup();
            return static_cast<int32_t>(st.code);
        }
    }
    cleanup();
    return 0;
#else
    (void)optional_ppm_path;
    (void)out_pixel_hash;
    return -3;
#endif
}

int32_t D3D12XboxCreateRaytracingPipelineState(
    uint64_t device_handle,
    const D3D12XBOX_RAYTRACING_PSO_DESC* desc,
    uint64_t* out_pso_handle) {
    if (!desc || !out_pso_handle || !desc->DxilLibrary.Bytecode ||
        desc->DxilLibrary.BytecodeLength == 0) {
        return -1;
    }
    *out_pso_handle = 0;

    const auto* bytes = static_cast<const uint8_t*>(desc->DxilLibrary.Bytecode);
    std::vector<uint8_t> raw(bytes, bytes + desc->DxilLibrary.BytecodeLength);
    scarlettx::ShaderTranslationResult translated{};
    auto st = scarlettx::TranslateXboxShaderToPc(raw, translated);
    if (!st.ok()) {
        return static_cast<int32_t>(st.code);
    }
    if (translated.source_info.kind != scarlettx::ShaderContainerKind::Dxil) {
        return static_cast<int32_t>(scarlettx::StatusCode::ShaderError);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    if (desc->GlobalRootSignatureHandle != 0 &&
        dev.root_signatures.find(desc->GlobalRootSignatureHandle) == dev.root_signatures.end()) {
        return -2;
    }

    auto entry = std::make_unique<PipelineStateEntry>();
    entry->is_raytracing = true;
    entry->root_sig_handle = desc->GlobalRootSignatureHandle;
    entry->pso_key = translated.translated_hash ^
                     (static_cast<uint64_t>(desc->MaxPayloadSizeInBytes) << 32) ^
                     desc->MaxTraceRecursionDepth;
    entry->max_payload_bytes = desc->MaxPayloadSizeInBytes;
    entry->max_attribute_bytes = desc->MaxAttributeSizeInBytes;
    entry->max_recursion_depth = desc->MaxTraceRecursionDepth;
    // Software DXR state object backing (native ID3D12StateObject requires hardware DXR Tier >= 1.0).
    ++dev.pso_cache_misses;
    const uint64_t h = dev.next_pso_handle++;
    dev.psos.emplace(h, std::move(entry));
    *out_pso_handle = h;
    return 0;
}

int32_t D3D12XboxGetPipelineCacheStats(
    uint64_t device_handle,
    D3D12XBOX_PIPELINE_CACHE_STATS* out_stats) {
    if (!out_stats) {
        return -1;
    }
    std::memset(out_stats, 0, sizeof(*out_stats));
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    const auto& dev = *it->second;
    out_stats->CacheHits = dev.pso_cache_hits;
    out_stats->CacheMisses = dev.pso_cache_misses;
    out_stats->LiveRootSignatures = static_cast<uint32_t>(dev.root_signatures.size());
    out_stats->LiveDescriptorHeaps = static_cast<uint32_t>(dev.descriptor_heaps.size());
    for (const auto& kv : dev.psos) {
        if (kv.second->is_raytracing) {
            ++out_stats->LiveRaytracingPsos;
        } else if (kv.second->is_compute) {
            ++out_stats->LiveComputePsos;
        } else {
            ++out_stats->LiveGraphicsPsos;
        }
    }
    return 0;
}

}  // extern "C"
