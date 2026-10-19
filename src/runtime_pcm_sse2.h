#pragma once
#include <stdint.h>

namespace ttpcomm::runtime {
// Internal only: callers must first check PF_XMMI64_INSTRUCTIONS_AVAILABLE.
__declspec(noinline) void ApplyGainHalfSse2(double*,uint32_t,double) noexcept;
}
