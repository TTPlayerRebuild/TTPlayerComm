#pragma once
#include <ttpcomm/audio_api.h>
namespace ttpcomm::audio {
void* __cdecl EncoderCreate(const TtpCommAudioEncoderConfig*) noexcept;
void __cdecl EncoderDestroy(void*) noexcept;
int __cdecl EncoderEncode(void*,const double*,uint32_t,const double*,uint32_t,
    void*,uint32_t,TtpCommAudioEncodeResult*) noexcept;
void __cdecl EncoderReset(void*) noexcept;
void* __cdecl EffectsCreate(const TtpCommAudioEffectsConfig*) noexcept;
void __cdecl EffectsDestroy(void*) noexcept;
int __cdecl EffectsInfo(void*,TtpCommAudioEffectsInfo*) noexcept;
int __cdecl EffectsConfigure(void*,const TtpCommAudioEffectsConfig*) noexcept;
int __cdecl EffectsProcess(void*,const double*,uint32_t,double*,uint32_t,uint32_t,
    TtpCommAudioProcessResult*) noexcept;
void __cdecl EffectsReset(void*) noexcept;
int Query(uint32_t,TtpCommAudioApi*) noexcept;
}
