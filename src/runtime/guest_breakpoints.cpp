// Diagnostic code breakpoints for guest titles.
//
// SCARLETTX_BREAKPOINTS="0x14247934c,0x142479366" plants int3 at each address
// before the entry point runs. Each hit logs the thread, RIP and the integer
// argument/return registers, then single-steps the original instruction and
// re-arms the breakpoint. SCARLETTX_BREAKPOINT_HITS caps the log lines per
// address (default 4).

#include "guest_breakpoints.h"

#if defined(_WIN32)
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

struct Bp {
    uint8_t original = 0;
    unsigned hits = 0;
};

std::unordered_map<uintptr_t, Bp>* g_bps = nullptr;
std::mutex g_mu;
unsigned g_max_hits = 4;
bool g_crash_log = false;
// SCARLETTX_SKIP_INT3: addresses of a title's own check int3s to step over.
std::unordered_set<uintptr_t>* g_skip_int3 = nullptr;
thread_local uintptr_t t_rearm = 0;

void write_byte(uintptr_t addr, uint8_t value) {
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), 1, PAGE_EXECUTE_READWRITE, &old);
    *reinterpret_cast<volatile uint8_t*>(addr) = value;
    VirtualProtect(reinterpret_cast<void*>(addr), 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), 1);
}

// Unwound call stack (frames without unwind data are taken as leaf).
void print_stack(const CONTEXT* c) {
    CONTEXT u = *c;
    const auto stack_base = reinterpret_cast<uintptr_t>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
    for (int f = 0; f < 24 && u.Rip && u.Rsp < stack_base; ++f) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(u.Rip), &mod) && mod) {
            GetModuleFileNameA(mod, name, sizeof name);
        } else if (u.Rip >= 0x140000000ull && u.Rip < 0x150000000ull) {
            std::strcpy(name, "title");
        }
        const char* base = std::strrchr(name, '\\');
        std::fprintf(stderr, "[ScarlettX][stack]   #%d %llx %s\n", f, static_cast<unsigned long long>(u.Rip),
                     base ? base + 1 : name);
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(u.Rip, &image_base, nullptr);
        if (fe) {
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, u.Rip, fe, &u, &handler_data, &establisher, nullptr);
        } else {
            u.Rip = *reinterpret_cast<const DWORD64*>(u.Rsp);
            u.Rsp += 8;
        }
    }
    std::fflush(stderr);
}

LONG CALLBACK handler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    CONTEXT* c = ep->ContextRecord;
    if (code == EXCEPTION_BREAKPOINT) {
        const uintptr_t rip = static_cast<uintptr_t>(c->Rip);
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_bps ? g_bps->find(rip) : decltype(g_bps->end()){};
        if (!g_bps || it == g_bps->end()) {
            // A title's own int3 (failed check); RIP points at the int3.
            const bool skip = g_skip_int3 && g_skip_int3->count(rip) &&
                              *reinterpret_cast<const uint8_t*>(rip) == 0xCC;
            if (g_crash_log || skip) {
                std::fprintf(stderr, "[ScarlettX][crash] guest int3 at rip=%p tid=%lu rax=%llx rcx=%llx rdx=%llx%s\n",
                             reinterpret_cast<void*>(rip), GetCurrentThreadId(),
                             static_cast<unsigned long long>(c->Rax), static_cast<unsigned long long>(c->Rcx),
                             static_cast<unsigned long long>(c->Rdx), skip ? " (skipped)" : "");
                std::fflush(stderr);
                if (!skip) print_stack(c);
            }
            if (skip) {
                c->Rip += 1;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
        if (++it->second.hits <= g_max_hits) {
            uint64_t ret = 0;
            std::memcpy(&ret, reinterpret_cast<const void*>(c->Rsp), sizeof ret);
            std::fprintf(stderr,
                         "[ScarlettX][bp] hit %p tid=%lu [rsp]=%llx rax=%llx rcx=%llx rdx=%llx r8=%llx r9=%llx rsp=%llx "
                         "rsi=%llx rdi=%llx\n",
                         reinterpret_cast<void*>(rip), GetCurrentThreadId(), static_cast<unsigned long long>(ret),
                         static_cast<unsigned long long>(c->Rax), static_cast<unsigned long long>(c->Rcx),
                         static_cast<unsigned long long>(c->Rdx), static_cast<unsigned long long>(c->R8),
                         static_cast<unsigned long long>(c->R9), static_cast<unsigned long long>(c->Rsp),
                         static_cast<unsigned long long>(c->Rsi), static_cast<unsigned long long>(c->Rdi));
            print_stack(c);
        }
        write_byte(rip, it->second.original);
        t_rearm = rip;
        c->EFlags |= 0x100;  // trap after the original instruction
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION && g_crash_log) {
        static std::atomic<int> reported{0};
        if (reported.fetch_add(1) < 8) {
            const auto* er = ep->ExceptionRecord;
            uint64_t ret = 0;
            if (c->Rsp) std::memcpy(&ret, reinterpret_cast<const void*>(c->Rsp), sizeof ret);
            std::fprintf(stderr,
                         "[ScarlettX][crash] AV %s %p at rip=%p [rsp]=%llx tid=%lu rax=%llx rcx=%llx rdx=%llx r8=%llx "
                         "r9=%llx rbx=%llx rsi=%llx rdi=%llx\n",
                         er->ExceptionInformation[0] == 8 ? "exec" : er->ExceptionInformation[0] ? "write" : "read",
                         reinterpret_cast<void*>(er->ExceptionInformation[1]), reinterpret_cast<void*>(c->Rip),
                         static_cast<unsigned long long>(ret), GetCurrentThreadId(),
                         static_cast<unsigned long long>(c->Rax), static_cast<unsigned long long>(c->Rcx),
                         static_cast<unsigned long long>(c->Rdx), static_cast<unsigned long long>(c->R8),
                         static_cast<unsigned long long>(c->R9), static_cast<unsigned long long>(c->Rbx),
                         static_cast<unsigned long long>(c->Rsi), static_cast<unsigned long long>(c->Rdi));
            std::fflush(stderr);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == EXCEPTION_SINGLE_STEP && t_rearm) {
        write_byte(t_rearm, 0xCC);
        t_rearm = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallGuestBreakpointsFromEnv() {
    const char* list = std::getenv("SCARLETTX_BREAKPOINTS");
    const char* crash = std::getenv("SCARLETTX_CRASH_LOG");
    g_crash_log = crash && *crash && *crash != '0';
    if (const char* skip = std::getenv("SCARLETTX_SKIP_INT3"); skip && *skip) {
        g_skip_int3 = new std::unordered_set<uintptr_t>();
        for (const char* p = skip; *p;) {
            char* end = nullptr;
            const unsigned long long a = std::strtoull(p, &end, 16);
            if (end == p) break;
            if (a) g_skip_int3->insert(static_cast<uintptr_t>(a));
            p = (*end == ',') ? end + 1 : end;
        }
    }
    if ((g_crash_log || g_skip_int3) && (!list || !*list)) {
        AddVectoredExceptionHandler(1, &handler);
        return;
    }
    if (!list || !*list) return;
    if (const char* h = std::getenv("SCARLETTX_BREAKPOINT_HITS"); h && *h) {
        g_max_hits = static_cast<unsigned>(std::strtoul(h, nullptr, 10));
    }
    g_bps = new std::unordered_map<uintptr_t, Bp>();
    std::string s(list);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(pos, end - pos);
        pos = end + 1;
        const uintptr_t addr = static_cast<uintptr_t>(std::strtoull(item.c_str(), nullptr, 16));
        if (!addr || g_bps->count(addr)) continue;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) {
            std::fprintf(stderr, "[ScarlettX][bp] %p is not mapped, skipped\n", reinterpret_cast<void*>(addr));
            continue;
        }
        Bp bp;
        bp.original = *reinterpret_cast<const uint8_t*>(addr);
        (*g_bps)[addr] = bp;
        write_byte(addr, 0xCC);
    }
    AddVectoredExceptionHandler(1, &handler);
    std::fprintf(stderr, "[ScarlettX][bp] %zu breakpoint(s) armed\n", g_bps->size());
}

#else
void InstallGuestBreakpointsFromEnv() {}
#endif
