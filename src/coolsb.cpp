// Original ordinals 200..206: CoolSB 1.2, with TTPlayer's proportional-thumb
// extension. Hook only this process; all ordinary windows use USER32 unchanged.
#include <windows.h>
#include <tlhelp32.h>
#include <detours.h>
#include "coolscroll.h"
#include "userdefs.h"
#include "coolsb_internal.h"
#include <cstddef>

static_assert(sizeof(SCROLLBAR) == 0x350);
static_assert(sizeof(SCROLLWND) == 0x6c8);
static_assert(offsetof(SCROLLBAR, fProportionalThumb) == 0x34c);
static_assert(offsetof(SCROLLWND, sbarVert) == 0x358);
static_assert(offsetof(SCROLLWND, bPreventStyleChange) == 0x6c4);

namespace {
decltype(&EnableScrollBar) real_enable = EnableScrollBar;
decltype(&GetScrollInfo) real_info = GetScrollInfo;
decltype(&GetScrollPos) real_pos = GetScrollPos;
decltype(&GetScrollRange) real_range = GetScrollRange;
decltype(&SetScrollInfo) real_set_info = SetScrollInfo;
decltype(&SetScrollPos) real_set_pos = SetScrollPos;
decltype(&SetScrollRange) real_set_range = SetScrollRange;
decltype(&ShowScrollBar) real_show = ShowScrollBar;
volatile LONG transaction_lock;
bool installed;
struct Guard {
    Guard() { while (InterlockedCompareExchange(&transaction_lock, 1, 0)) Sleep(0); }
    ~Guard() { InterlockedExchange(&transaction_lock, 0); }
};
bool custom(HWND window, int bar, bool both = false) {
    return (bar == SB_HORZ || bar == SB_VERT || (both && bar == SB_BOTH)) &&
        GetScrollWndFromHwnd(window) != nullptr;
}
BOOL WINAPI enable(HWND w, UINT b, UINT a) {
    return custom(w, b, true) ? CoolSB_EnableScrollBar(w, b, a) : real_enable(w, b, a);
}
BOOL WINAPI info(HWND w, int b, LPSCROLLINFO s) {
    return custom(w, b) ? CoolSB_GetScrollInfo(w, b, s) : real_info(w, b, s);
}
int WINAPI pos(HWND w, int b) {
    return custom(w, b) ? CoolSB_GetScrollPos(w, b) : real_pos(w, b);
}
BOOL WINAPI range(HWND w, int b, LPINT lo, LPINT hi) {
    return custom(w, b) ? CoolSB_GetScrollRange(w, b, lo, hi) : real_range(w, b, lo, hi);
}
int WINAPI set_info(HWND w, int b, LPCSCROLLINFO s, BOOL redraw) {
    return custom(w, b) ? CoolSB_SetScrollInfo(w, b, const_cast<LPSCROLLINFO>(s), redraw) : real_set_info(w, b, s, redraw);
}
int WINAPI set_pos(HWND w, int b, int p, BOOL redraw) {
    return custom(w, b) ? CoolSB_SetScrollPos(w, b, p, redraw) : real_set_pos(w, b, p, redraw);
}
BOOL WINAPI set_range(HWND w, int b, int lo, int hi, BOOL redraw) {
    return custom(w, b) ? CoolSB_SetScrollRange(w, b, lo, hi, redraw) : real_set_range(w, b, lo, hi, redraw);
}
BOOL WINAPI show(HWND w, int b, BOOL visible) {
    return custom(w, b, true) ? CoolSB_ShowScrollBar(w, b, visible) : real_show(w, b, visible);
}
struct Hook { PVOID* original; PVOID replacement; };
Hook hooks[] = {
    {reinterpret_cast<PVOID*>(&real_enable), reinterpret_cast<PVOID>(enable)},
    {reinterpret_cast<PVOID*>(&real_info), reinterpret_cast<PVOID>(info)},
    {reinterpret_cast<PVOID*>(&real_pos), reinterpret_cast<PVOID>(pos)},
    {reinterpret_cast<PVOID*>(&real_range), reinterpret_cast<PVOID>(range)},
    {reinterpret_cast<PVOID*>(&real_set_info), reinterpret_cast<PVOID>(set_info)},
    {reinterpret_cast<PVOID*>(&real_set_pos), reinterpret_cast<PVOID>(set_pos)},
    {reinterpret_cast<PVOID*>(&real_set_range), reinterpret_cast<PVOID>(set_range)},
    {reinterpret_cast<PVOID*>(&real_show), reinterpret_cast<PVOID>(show)},
};
// Detours keeps these handles until commit/abort. Enlist every extant thread,
// so its instruction pointer is relocated safely while USER32 is patched.
struct ThreadHandles {
    HANDLE* data = nullptr;
    SIZE_T size = 0, capacity = 0;
    ~ThreadHandles() {
        for (SIZE_T i = 0; i < size; ++i) CloseHandle(data[i]);
        if (data) HeapFree(GetProcessHeap(), 0, data);
    }
    bool add(HANDLE h) {
        if (size == capacity) {
            const SIZE_T next = capacity ? capacity * 2 : 32;
            auto p = static_cast<HANDLE*>(data ? HeapReAlloc(GetProcessHeap(), 0, data, next * sizeof(HANDLE)) :
                HeapAlloc(GetProcessHeap(), 0, next * sizeof(HANDLE)));
            if (!p) return false;
            data = p; capacity = next;
        }
        data[size++] = h;
        return true;
    }
};
LONG collect_threads(ThreadHandles& handles) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return GetLastError();
    THREADENTRY32 e = {sizeof(e)};
    LONG result = NO_ERROR;
    if (!Thread32First(snapshot, &e)) result = GetLastError();
    else do {
        if (e.th32OwnerProcessID != GetCurrentProcessId() || e.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
            FALSE, e.th32ThreadID);
        if (!h) { if (GetLastError() == ERROR_INVALID_PARAMETER) continue; result = GetLastError(); break; }
        if (!handles.add(h)) { CloseHandle(h); result = ERROR_NOT_ENOUGH_MEMORY; break; }
    } while (Thread32Next(snapshot, &e));
    CloseHandle(snapshot);
    return result;
}
BOOL change(bool attach) {
    Guard guard;
    if (installed == attach) return TRUE;
    LONG error = DetourTransactionBegin();
    if (error != NO_ERROR) { SetLastError(error); return FALSE; }
    ThreadHandles handles;
    error = collect_threads(handles);
    for (const auto& h : hooks) {
        if (error != NO_ERROR) break;
        error = attach ? DetourAttach(h.original, h.replacement) : DetourDetach(h.original, h.replacement);
    }
    // Collect handles and prepare trampolines before suspending anyone. The
    // adapted Detours bookkeeping uses VirtualAlloc, avoiding CRT heap locks.
    for (SIZE_T i = 0; error == NO_ERROR && i < handles.size; ++i)
        error = DetourUpdateThread(handles.data[i]);
    if (error != NO_ERROR) DetourTransactionAbort();
    else error = DetourTransactionCommit();
    if (error != NO_ERROR) { SetLastError(error); return FALSE; }
    installed = attach;
    CoolSB_SetESBProc(reinterpret_cast<void*>(real_enable));
    return TRUE;
}
}

extern "C" BOOL WINAPI ttp_coolsb_init_app() { return change(true); }
extern "C" BOOL WINAPI ttp_coolsb_uninit_app() { return change(false); }
