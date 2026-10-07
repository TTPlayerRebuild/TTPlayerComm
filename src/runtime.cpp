#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <malloc.h>

namespace {
DWORD rngLow = TLS_OUT_OF_INDEXES, rngHigh = TLS_OUT_OF_INDEXES;
// The complete 48-bit state fits in two x86 TLS values. Store high+1 so zero
// means the original default state, even when an explicit state is all zero.
// No thread-owned heap blocks can leak when unloaded with live idle threads.
std::uint64_t randomState() noexcept {
    const auto high = reinterpret_cast<std::uintptr_t>(TlsGetValue(rngHigh));
    return high ? (std::uint64_t(high - 1) << 32) |
        reinterpret_cast<std::uintptr_t>(TlsGetValue(rngLow)) : 0xffff1111330eULL;
}
void randomState(std::uint64_t state) noexcept {
    TlsSetValue(rngLow, reinterpret_cast<void*>(static_cast<std::uintptr_t>(state)));
    TlsSetValue(rngHigh, reinterpret_cast<void*>(static_cast<std::uintptr_t>((state >> 32) + 1)));
}
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        rngLow = TlsAlloc();
        if (rngLow == TLS_OUT_OF_INDEXES) return FALSE;
        rngHigh = TlsAlloc();
        if (rngHigh == TLS_OUT_OF_INDEXES) { TlsFree(rngLow); rngLow = TLS_OUT_OF_INDEXES; return FALSE; }
    }
    if (reason == DLL_PROCESS_DETACH) {
        if (rngLow != TLS_OUT_OF_INDEXES) TlsFree(rngLow);
        if (rngHigh != TLS_OUT_OF_INDEXES) TlsFree(rngHigh);
    }
    return TRUE;
}

extern "C" {
unsigned __cdecl ttpcomm_getversion() { return 0x00050700; }
int __cdecl ttp_resetstkoflw() { return _resetstkoflw(); }
void __cdecl ttp_srand48(unsigned seed) {
    randomState((std::uint64_t(seed) << 16) | 0x330e);
    // Observable side effect: the original also seeds MSVCRT's rand().
    srand(seed);
}
unsigned __cdecl ttp_lrand48() {
    const auto next = (randomState() * 0x5deece66dULL + 11) & 0xffffffffffffULL;
    randomState(next);
    return static_cast<unsigned>(next >> 17);
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
