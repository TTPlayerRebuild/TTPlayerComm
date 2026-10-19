#include "runtime_pcm_sse2.h"
#include <cmath>

namespace ttpcomm::runtime {
// Keep the previous SSE2 build's expression and math intrinsic. The CRT's
// scalar tan/_CItan does not produce identical binary64 results for all inputs.
// This translation unit has /arch:SSE2 and /GL-; it contains no initializers.
__declspec(noinline) void ApplyGainHalfSse2(double* samples,uint32_t count,double gain) noexcept {
    if(gain==1.0)return;
    for(uint32_t i=0;i<count;++i) {
        double value=samples[i]*gain;
        if(value>0.5)value=(std::tan((value-0.5)*2.0)+1.0)*0.5;
        else if(value<-0.5)value=std::tan((value+0.5)*2.0)*0.5-0.5;
        samples[i]=value;
    }
}
}
