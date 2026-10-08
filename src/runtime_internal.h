#pragma once
#include <ttpcomm/runtime_api.h>
#include <ttpcomm/pcm.h>
namespace ttpcomm::runtime {
bool WaveFormat(const TtpCommPcmFormat*, WAVEFORMATEXTENSIBLE&) noexcept;
int __cdecl PcmDecode(const TtpCommPcmFormat*,uint32_t,const void*,uint32_t,double*,uint32_t,uint32_t*) noexcept;
int __cdecl PcmEncode(const TtpCommPcmFormat*,const double*,uint32_t,void*,uint32_t,uint32_t*) noexcept;
int __cdecl PcmGain(double*,uint32_t,double) noexcept;
void* __cdecl QuantizerCreate(const TtpCommPcmFormat*, int) noexcept;
void __cdecl QuantizerDestroy(void*) noexcept;
int __cdecl QuantizerEncode(void*,const double*,uint32_t,void*,uint32_t,uint32_t*) noexcept;
void __cdecl QuantizerReset(void*) noexcept;
}
