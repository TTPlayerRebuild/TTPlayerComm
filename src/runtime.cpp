#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <malloc.h>

namespace {
DWORD rngIndex = TLS_OUT_OF_INDEXES;
struct RandomState { std::uint64_t state; };
RandomState* randomState() noexcept {
    auto* state = static_cast<RandomState*>(TlsGetValue(rngIndex));
    if (!state) {
        state = static_cast<RandomState*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(RandomState)));
        if (state) state->state = 0xffff1111330eULL; // bytes in original .tls template
        if (state && !TlsSetValue(rngIndex, state)) {
            HeapFree(GetProcessHeap(), 0, state);
            state = nullptr;
        }
    }
    return state;
}
using SecurityHandler = void(__cdecl*)(int, void*);
PVOID volatile securityHandler = nullptr;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        rngIndex = TlsAlloc();
        return rngIndex != TLS_OUT_OF_INDEXES;
    }
    if (reason == DLL_THREAD_DETACH || reason == DLL_PROCESS_DETACH) {
        if (rngIndex != TLS_OUT_OF_INDEXES) {
            if (void* p = TlsGetValue(rngIndex)) HeapFree(GetProcessHeap(), 0, p);
            TlsSetValue(rngIndex, nullptr);
            if (reason == DLL_PROCESS_DETACH) TlsFree(rngIndex);
        }
    }
    return TRUE;
}

extern "C" {
unsigned __cdecl ttpcomm_getversion() { return 0x00050700; }
int __cdecl ttp_resetstkoflw() { return _resetstkoflw(); }
SecurityHandler __cdecl ttp_set_security_error_handler(SecurityHandler handler) {
    return reinterpret_cast<SecurityHandler>(InterlockedExchangePointer(&securityHandler, reinterpret_cast<void*>(handler)));
}
void __cdecl ttp_srand48(unsigned seed) {
    if (auto* p = randomState()) p->state = (std::uint64_t(seed) << 16) | 0x330e;
    // Observable side effect: the original also seeds MSVCRT's rand().
    srand(seed);
}
unsigned __cdecl ttp_lrand48() {
    auto* p = randomState();
    if (!p) return 0;
    p->state = (p->state * 0x5deece66dULL + 11) & 0xffffffffffffULL;
    return static_cast<unsigned>(p->state >> 17);
}
void* __cdecl ttp_malloc(unsigned size) { return malloc(size); }
void __cdecl ttp_free(void* address) { free(address); }
unsigned __cdecl ttp_decoder_sample_bits() { return 64; }
BOOL __cdecl ttp_replaygain_supports_rate(int rate) {
    static constexpr int rates[]{48000,44100,32000,24000,22050,16000,12000,11025,
        8000,18900,37800,56000,64000,88200,96000,112000,128000,144000,176400,192000};
    for (int value : rates) if (rate == value) return TRUE;
    return FALSE;
}
}
