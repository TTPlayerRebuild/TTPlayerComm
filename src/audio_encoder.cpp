#include "audio_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace ttpcomm::audio {
namespace {
bool Valid(const TtpCommAudioEncoderConfig* config) noexcept {
    if(!config || config->size<sizeof(*config) || config->dither>1)return false;
    const auto& f=config->format;
    if(f.size<sizeof(f) || !f.channels || f.channels>256 || !f.sample_rate ||
       f.sample_rate>768000 || !f.valid_bits || f.valid_bits>f.bits ||
       f.floating_point>1 || f.block_align!=f.channels*(f.bits/8))return false;
    if(f.floating_point)return (f.bits==32 || f.bits==64) && f.valid_bits==f.bits;
    return f.bits==8 || f.bits==16 || f.bits==24 || f.bits==32;
}
struct Encoder {
    TtpCommAudioEncoderConfig config;
    uint32_t random;
    explicit Encoder(const TtpCommAudioEncoderConfig& c) noexcept:
        config(c),random(c.seed?c.seed:0x6d2b79f5u) {}
    double uniform() noexcept {
        random^=random<<13;random^=random>>17;random^=random<<5;
        return double(random)*0x1p-32;
    }
};
// Explicit tie handling does not depend on a plugin's current x87/MXCSR
// rounding mode. The caller's floating-point control state is never changed.
double RoundEven(double value) noexcept {
    const double below=std::floor(value),fraction=value-below;
    if(fraction<0.5)return below;
    if(fraction>0.5)return below+1.0;
    return std::fmod(below,2.0)==0.0?below:below+1.0;
}
}
void* __cdecl EncoderCreate(const TtpCommAudioEncoderConfig* config) noexcept {
    return Valid(config)?new(std::nothrow) Encoder(*config):nullptr;
}
void __cdecl EncoderDestroy(void* object) noexcept {delete static_cast<Encoder*>(object);}
void __cdecl EncoderReset(void* object) noexcept {
    if(auto* e=static_cast<Encoder*>(object))e->random=e->config.seed?e->config.seed:0x6d2b79f5u;
}
int __cdecl EncoderEncode(void* object,const double* input,uint32_t frames,
    const double* gains,uint32_t gainCount,void* output,uint32_t capacity,
    TtpCommAudioEncodeResult* result) noexcept {
    if(!result || result->size<sizeof(*result))return TTPCOMM_INVALID;
    *result={sizeof(*result),0,0,0};
    auto* e=static_cast<Encoder*>(object);
    if(!e)return TTPCOMM_INVALID;
    const auto& f=e->config.format;
    if((frames && !input) || (gainCount && !gains) ||
       (gainCount && gainCount!=f.channels) || (!gainCount && gains))return TTPCOMM_INVALID;
    for(uint32_t c=0;c<gainCount;++c)if(!std::isfinite(gains[c]))return TTPCOMM_INVALID;
    const uint64_t bytes=uint64_t(frames)*f.block_align;
    const uint64_t inputBytes=uint64_t(frames)*f.channels*sizeof(double);
    if(inputBytes>std::numeric_limits<size_t>::max())return TTPCOMM_INVALID;
    if(bytes>capacity)return TTPCOMM_CAPACITY;
    if(bytes && !output)return TTPCOMM_INVALID;
    if(bytes) {
        const auto outStart=reinterpret_cast<uintptr_t>(output),inStart=reinterpret_cast<uintptr_t>(input);
        const auto maximumAddress=std::numeric_limits<uintptr_t>::max();
        if(outStart>maximumAddress-bytes || inStart>maximumAddress-inputBytes ||
           (inStart<outStart+bytes && outStart<inStart+inputBytes))return TTPCOMM_INVALID;
        if(gainCount) {
            const auto gainStart=reinterpret_cast<uintptr_t>(gains);
            const size_t gainBytes=size_t(gainCount)*sizeof(double);
            if(gainStart>maximumAddress-gainBytes ||
               (gainStart<outStart+bytes && outStart<gainStart+gainBytes))return TTPCOMM_INVALID;
        }
    }
    // A uint32 byte capacity also bounds the sample loop and index arithmetic.
    const uint32_t count=frames*f.channels,step=f.bits/8;
    auto* target=static_cast<unsigned char*>(output);
    const double scale=std::ldexp(1.0,static_cast<int>(f.valid_bits));
    const double minimum=-scale*0.5,maximum=scale*0.5-1.0;
    const uint32_t padding=f.bits-f.valid_bits;
    for(uint32_t i=0;i<count;++i,target+=step) {
        double value=input[i]*(gainCount?gains[i%f.channels]:1.0);
        if(!std::isfinite(value)) {value=0;++result->nonfinite_samples;}
        if(f.floating_point) {
            // Multiplication by two converts the internal half-amplitude
            // convention to the standard external IEEE float representation.
            const double limit=f.bits==32?double(std::numeric_limits<float>::max()):
                std::numeric_limits<double>::max();
            if(value>limit*0.5){value=limit;++result->clipped_samples;}
            else if(value<-limit*0.5){value=-limit;++result->clipped_samples;}
            else value*=2.0;
            if(f.bits==32) {const float v=static_cast<float>(value);std::memcpy(target,&v,4);}
            else std::memcpy(target,&value,8);
            continue;
        }
        value*=scale;
        if(e->config.dither)value+=e->uniform()-e->uniform();
        if(value<minimum || value>maximum)++result->clipped_samples;
        value=RoundEven(std::clamp(value,minimum,maximum));
        const auto integer=static_cast<int64_t>(value);
        const uint64_t packed=f.bits==8?
            (static_cast<uint64_t>(integer+static_cast<int64_t>(scale*0.5))<<padding):
            (static_cast<uint64_t>(integer)<<padding);
        for(uint32_t b=0;b<step;++b)target[b]=static_cast<unsigned char>(packed>>(b*8));
    }
    result->bytes=static_cast<uint32_t>(bytes);return TTPCOMM_OK;
}
int Query(uint32_t required,TtpCommAudioApi* api) noexcept {
    if(!api || api->size<sizeof(*api) || (required&~TTPCOMM_AUDIO_REQUIRED))return 0;
    const TtpCommAudioApi table{sizeof(table),TTPCOMM_AUDIO_ABI,TTPCOMM_AUDIO_REQUIRED,0,
        EncoderCreate,EncoderDestroy,EncoderEncode,EncoderReset,EffectsCreate,EffectsDestroy,
        EffectsInfo,EffectsConfigure,EffectsProcess,EffectsReset};
    *api=table;return 1;
}
}
