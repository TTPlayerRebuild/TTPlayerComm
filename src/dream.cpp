// Dream's exported ABI and lifetime are recovered from 6001D85E..6001DAAD.
// Algorithm sources are adapted from the fixed Goom release at configure time.
#include <windows.h>
#include <stdint.h>
#include "dream_engine.inc"

namespace {
// The reference keeps mode transitions and the random table global. Serialize
// those shared states while keeping each image/filter/IFS lifetime independent.
volatile LONG dream_lock = 0;
struct Guard {
    Guard() { while (InterlockedCompareExchange(&dream_lock, 1, 0)) Sleep(0); }
    ~Guard() { InterlockedExchange(&dream_lock, 0); }
};
bool valid_size(unsigned width, int height) {
    return width >= 2 && height >= 2 && width <= 8192 && height <= 8192
        && uint64_t(width) * unsigned(height) <= 16777216;
}
bool buffers(Dream& d, unsigned width, int height) {
    size_t size = size_t(width) * height * 4 + 128;
    auto a = static_cast<unsigned*>(calloc(1, size));
    auto b = static_cast<unsigned*>(calloc(1, size));
    if (!a || !b) { free(a); free(b); return false; }
    free(d.pixel); free(d.back);
    d.pixel = a; d.back = b;
    d.p1 = reinterpret_cast<unsigned*>((uintptr_t(a) + 128) & ~uintptr_t(127));
    d.p2 = reinterpret_cast<unsigned*>((uintptr_t(b) + 128) & ~uintptr_t(127));
    d.buffsize = width * height;
    d.width = width; d.height = height;
    resolx = width; c_resoly = height;
    return true;
}
void destroy(Dream* d) {
    if (!d) return;
    free(d->pixel); free(d->back);
    goom_lines_free(&d->gmline1); goom_lines_free(&d->gmline2);
    delete d;
}
}

extern "C" void* __fastcall ttp_dream_create(unsigned width, int height) {
    Guard guard;
    if (!valid_size(width, height)) return nullptr;
    Dream* d = new(std::nothrow) Dream;
    if (!d) return nullptr;
    if (!buffers(*d, width, height)) { destroy(d); return nullptr; }
    for (rand_pos=0; rand_pos<0x3fff; ++rand_pos) rand_tab[rand_pos] = short(rand());
    // Original leaves the final random-table element indeterminate; define it.
    rand_tab[0x3fff] = 0;
    try {
        d->filter.resize(width, height);
        d->ifs.init_ifs(width, height);
        d->gmline1 = goom_lines_init(width, height, 1, float(height), 6, 0, float(height*2/5), 4);
        d->gmline2 = goom_lines_init(width, height, 1, 0, 6, 0, float(height/5), 1);
    } catch (...) { destroy(d); return nullptr; }
    return d;
}
extern "C" void __fastcall ttp_dream_resize(Dream* d, unsigned width, int height) {
    Guard guard;
    if (!d || !valid_size(width, height) || !buffers(*d, width, height)) return;
    try {
        if (d->failed) d->filter.prevX = -1;
        d->filter.resize(width, height);
        d->ifs.init_ifs(width, height);
        goom_lines_set_res(d->gmline1, width, height);
        goom_lines_set_res(d->gmline2, width, height);
        d->failed = false;
    } catch (...) { d->failed = true; }
}
extern "C" const uint32_t* __fastcall ttp_dream_process(Dream* d, const int16_t* left,
                                                      const int16_t* right, int mode) {
    Guard guard;
    if (!d || d->failed || !left || !right) return nullptr;
    resolx = d->width; c_resoly = d->height;
    try { return d->goom_update(left, right, mode); }
    catch (...) { d->failed = true; return nullptr; }
}
extern "C" void __fastcall ttp_dream_destroy(Dream* d) { Guard guard; destroy(d); }

