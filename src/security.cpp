#include <windows.h>
#include <intrin.h>

namespace {
using SecurityHandler = void(__cdecl*)(int, void*);
PVOID volatile handler = nullptr;
volatile LONG reporting = 0;
__declspec(noreturn) __declspec(safebuffers) void fastFail(unsigned code) {
    if (IsProcessorFeaturePresent(PF_FASTFAIL_AVAILABLE)) __fastfail(code);
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) {}
}
}

extern "C" SecurityHandler __cdecl ttp_set_security_error_handler(SecurityHandler next) {
    return reinterpret_cast<SecurityHandler>(InterlockedExchangePointer(
        &handler, reinterpret_cast<void*>(next)));
}

// x86 MSVC's __security_check_cookie branches here on a failed /GS check.
// 60010CBD / 60010D62 call the legacy callback with (1, nullptr), catch SEH,
// and terminate even if the callback returns or raises. Never resume execution.
extern "C" __declspec(noreturn) __declspec(safebuffers) void __cdecl __report_gsfailure() {
    if (InterlockedCompareExchange(&reporting, 1, 0) == 0) {
        const auto callback = reinterpret_cast<SecurityHandler>(
            InterlockedCompareExchangePointer(&handler, nullptr, nullptr));
        if (callback) {
            __try { callback(1, nullptr); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            TerminateProcess(GetCurrentProcess(), 3);
        }
    }
    // Preserve fail-fast for an absent/reentrant callback. Old systems have
    // no fast-fail facility; terminate directly without running damaged cleanup.
    fastFail(FAST_FAIL_STACK_COOKIE_CHECK_FAILURE);
}

// Keep the other reporters in MSVC's gs_report object available without pulling
// a second definition of __report_gsfailure from that same archive member.
extern "C" __declspec(noreturn) __declspec(safebuffers) void __cdecl
__report_securityfailureEx(unsigned long code, unsigned long, void**) { fastFail(code); }
extern "C" __declspec(noreturn) __declspec(safebuffers) void __cdecl
__report_securityfailure(unsigned long code) { fastFail(code); }
extern "C" __declspec(noreturn) __declspec(safebuffers) void __cdecl
__report_rangecheckfailure() { fastFail(FAST_FAIL_RANGE_CHECK_FAILURE); }
