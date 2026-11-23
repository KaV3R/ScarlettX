#pragma once
#if defined(_WIN32)
#include <cstddef>
#include <windows.h>

// Reserves an address range as a host placeholder so physical pages can be
// mapped into it later. Returns nullptr if placeholders are unavailable.
void* ScxXMemReservePlaceholder(size_t size, size_t align, DWORD protect);

// Handles MEM_RESERVE_PLACEHOLDER / MEM_REPLACE_PLACEHOLDER requests, which
// host VirtualAlloc rejects.
void* ScxXMemPlaceholderAlloc(void* address, size_t size, DWORD type, DWORD protect, size_t page);

// If address lies in a placeholder reservation, commits [address, +size)
// there, stores the result in *out and returns 1. Returns 0 otherwise.
int ScxXMemCommitInPlaceholder(void* address, size_t size, DWORD protect, void** out);
#endif
