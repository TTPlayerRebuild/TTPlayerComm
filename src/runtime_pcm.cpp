#include "runtime_internal.h"
#include <climits>
namespace ttpcomm::runtime {
bool WaveFormat(const TtpCommPcmFormat* format,WAVEFORMATEXTENSIBLE& wave) noexcept {
    if(!format || format->size<sizeof(*format) || !format->channels || format->channels>USHRT_MAX ||
       !format->sample_rate || format->bits>64 || format->valid_bits>format->bits || !format->valid_bits ||
       format->block_align>USHRT_MAX || format->floating_point>1) return false;
    wave={};wave.Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels=static_cast<WORD>(format->channels);
    wave.Format.nSamplesPerSec=format->sample_rate;wave.Format.wBitsPerSample=static_cast<WORD>(format->bits);
    wave.Format.nBlockAlign=static_cast<WORD>(format->block_align);
    wave.Format.cbSize=sizeof(wave)-sizeof(wave.Format);
    wave.Samples.wValidBitsPerSample=static_cast<WORD>(format->valid_bits);wave.dwChannelMask=format->channel_mask;
    wave.SubFormat={static_cast<DWORD>(format->floating_point?WAVE_FORMAT_IEEE_FLOAT:WAVE_FORMAT_PCM),0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
    return pcm::Describe(wave.Format,true).supported;
}
int __cdecl PcmDecode(const TtpCommPcmFormat* format,uint32_t domain,const void* input,uint32_t size,
    double* output,uint32_t capacity,uint32_t* produced) noexcept {
    if(!produced) return TTPCOMM_INVALID;
    *produced=0;WAVEFORMATEXTENSIBLE wave{};
    if(!WaveFormat(format,wave) || domain>TTPCOMM_PCM_FULL || size%format->block_align || (size && !input)) return TTPCOMM_INVALID;
    const uint64_t count=uint64_t(size/format->block_align)*format->channels;
    if(count>capacity) return TTPCOMM_CAPACITY;
    if(!pcm::DecodeTo(input,size,wave.Format,output,capacity,domain==TTPCOMM_PCM_HALF?pcm::Domain::half:pcm::Domain::full,true))
        return TTPCOMM_INVALID;
    *produced=static_cast<uint32_t>(count);return TTPCOMM_OK;
}
int __cdecl PcmEncode(const TtpCommPcmFormat* format,const double* input,uint32_t count,
    void* output,uint32_t capacity,uint32_t* produced) noexcept {
    if(!produced) return TTPCOMM_INVALID;
    *produced=0;WAVEFORMATEXTENSIBLE wave{};
    if(!WaveFormat(format,wave) || count%format->channels || format->valid_bits!=format->bits ||
        !pcm::Describe(wave.Format).supported || (count && !input)) return TTPCOMM_INVALID;
    const uint64_t size=uint64_t(count)*(format->bits/8);
    if(size>capacity) return TTPCOMM_CAPACITY;
    if(!pcm::EncodeHalfTo(input,count,wave.Format,output,capacity)) return TTPCOMM_INVALID;
    *produced=static_cast<uint32_t>(size);return TTPCOMM_OK;
}
int __cdecl PcmGain(double* samples,uint32_t count,double gain) noexcept {
    if(count && !samples) return TTPCOMM_INVALID;
    pcm::ApplyGainHalfTo(samples,count,gain);return TTPCOMM_OK;
}
}
