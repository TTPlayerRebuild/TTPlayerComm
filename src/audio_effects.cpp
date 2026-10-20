#include "audio_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace ttpcomm::audio {
namespace {
constexpr double pi=3.1415926535897932384626433832795;
struct Complex {double re{},im{};};
Complex Multiply(Complex a,Complex b) noexcept {
    return {a.re*b.re-a.im*b.im,a.re*b.im+a.im*b.re};
}
bool Valid(const TtpCommAudioEffectsConfig* config) noexcept {
    if(!config || config->size<sizeof(*config) || config->sample_rate<1000 ||
       config->sample_rate>768000 || !config->channels || config->channels>32 ||
       (config->flags&~(TTPCOMM_AUDIO_EQ|TTPCOMM_AUDIO_SURROUND)) ||
       (config->flags&TTPCOMM_AUDIO_SURROUND &&
        (!config->surround_strength || config->surround_strength>16)) ||
       config->transition_frames>config->sample_rate*10)return false;
    for(double band:config->bands_db)
        if(!std::isfinite(band) || band<-60 || band>24)return false;
    return std::isfinite(config->preamp_db) && config->preamp_db>=-60 && config->preamp_db<=24;
}
double Bessel(double value) noexcept {
    double sum=1,term=1;
    for(unsigned n=1;n<40;++n) {
        term*=value*value/(4.0*n*n);sum+=term;
        if(term<sum*1e-16)break;
    }
    return sum;
}
class Effects {
public:
    explicit Effects(const TtpCommAudioEffectsConfig& config):config_(config) {
        // Preserve a fixed filter duration across sample rates. Unlike the
        // legacy export this uses a conventional, normalized Kaiser window.
        taps_=std::clamp(static_cast<uint32_t>(std::lround(511.0*config.sample_rate/44100.0))|1u,
            63u,16383u);
        fft_=1;while(fft_<taps_*2)fft_*=2;
        block_=fft_-taps_+1;delay_=taps_/2;
        reverse_.resize(fft_);twiddles_.resize(fft_/2);
        for(uint32_t i=0;i<fft_;++i) {
            uint32_t x=i,r=0;
            for(uint32_t n=fft_;n>1;n>>=1){r=(r<<1)|(x&1);x>>=1;}
            reverse_[i]=r;
        }
        for(uint32_t i=0;i<fft_/2;++i) {
            const double angle=-2*pi*i/fft_;
            twiddles_[i]={std::cos(angle),std::sin(angle)};
        }
        work_.resize(fft_);filtered_.resize(fft_);second_.resize(fft_);
        history_.resize(size_t(taps_-1)*config.channels);
        pending_.resize(size_t(block_)*config.channels);
        queued_.resize(size_t(block_)*config.channels);
        surround_.resize(std::max(1u,(config.sample_rate*16u+500u)/1000u));
        base_=Kernel(config);target_=base_;
        surroundTarget_=SurroundStrength(config);surroundCurrent_=surroundTarget_;
        // Stable one-pole band-pass on the delayed mono component. Only front
        // left/right are altered; all remaining channels pass through the EQ.
        highAlpha_=std::exp(-2*pi*std::min(200.0,config.sample_rate*0.45)/config.sample_rate);
        lowAlpha_=std::exp(-2*pi*std::min(7000.0,config.sample_rate*0.45)/config.sample_rate);
    }
    void Info(TtpCommAudioEffectsInfo& info) const noexcept {
        info={sizeof(info),taps_,fft_,block_,delay_,block_-1+delay_,
            static_cast<uint32_t>(surround_.size()),0};
    }
    int Configure(const TtpCommAudioEffectsConfig& config) {
        if(config.sample_rate!=config_.sample_rate || config.channels!=config_.channels)
            return TTPCOMM_INVALID;
        if(config.flags==config_.flags && config.preamp_db==config_.preamp_db &&
           config.surround_strength==config_.surround_strength &&
           std::equal(std::begin(config.bands_db),std::end(config.bands_db),std::begin(config_.bands_db))) {
            config_.transition_frames=config.transition_frames;return TTPCOMM_OK;
        }
        // Build every fallible allocation before touching current parameters.
        auto next=Kernel(config);
        if(transitionLength_) {
            const double alpha=std::min(1.0,double(transitionPosition_)/transitionLength_);
            for(uint32_t i=0;i<fft_;++i) {
                base_[i].re+=(target_[i].re-base_[i].re)*alpha;
                base_[i].im+=(target_[i].im-base_[i].im)*alpha;
            }
        }
        target_.swap(next);
        transitionLength_=config.transition_frames;transitionPosition_=0;
        if(!transitionLength_)std::copy(target_.begin(),target_.end(),base_.begin());
        surroundStart_=surroundCurrent_;surroundTarget_=SurroundStrength(config);
        surroundTransition_=config.transition_frames;surroundPosition_=0;
        if(!surroundTransition_)surroundCurrent_=surroundTarget_;
        config_=config;return TTPCOMM_OK;
    }
    int Process(const double* input,uint32_t frames,double* output,uint32_t capacity,
        uint32_t flags,TtpCommAudioProcessResult& result) noexcept {
        if((frames&&!input) || (capacity&&!output) || flags&~TTPCOMM_AUDIO_END_INPUT ||
           (ending_ && frames))return TTPCOMM_INVALID;
        const auto channels=config_.channels;
        if(uint64_t(frames)*channels>std::numeric_limits<size_t>::max()/sizeof(double) ||
           uint64_t(capacity)*channels>std::numeric_limits<size_t>::max()/sizeof(double))
            return TTPCOMM_INVALID;
        // Avoid silently corrupting pending input when callers use the same
        // buffer for both sides. Integer address arithmetic is overflow checked.
        if(frames&&capacity) {
            const auto beginIn=reinterpret_cast<uintptr_t>(input),beginOut=reinterpret_cast<uintptr_t>(output);
            const size_t inBytes=size_t(frames)*channels*sizeof(double),outBytes=size_t(capacity)*channels*sizeof(double);
            if(beginIn>std::numeric_limits<uintptr_t>::max()-inBytes ||
               beginOut>std::numeric_limits<uintptr_t>::max()-outBytes ||
               (beginIn<beginOut+outBytes && beginOut<beginIn+inBytes))return TTPCOMM_INVALID;
        }
        if(uint64_t(frames)>std::numeric_limits<uint64_t>::max()-totalInput_)return TTPCOMM_INVALID;
        for(;;) {
            if(queuePosition_<queueFrames_) {
                const uint32_t n=std::min(queueFrames_-queuePosition_,capacity-result.produced_frames);
                if(n) {
                    std::memcpy(output+size_t(result.produced_frames)*channels,
                        queued_.data()+size_t(queuePosition_)*channels,size_t(n)*channels*sizeof(double));
                    queuePosition_+=n;result.produced_frames+=n;
                }
                if(queuePosition_<queueFrames_)return TTPCOMM_OK;
                queuePosition_=queueFrames_=0;
            }
            if(ending_ && generated_==totalInput_)return TTPCOMM_END;
            if(result.consumed_frames<frames) {
                const uint32_t n=std::min(block_-pendingFrames_,frames-result.consumed_frames);
                for(size_t i=0;i<size_t(n)*channels;++i) {
                    const double sample=input[size_t(result.consumed_frames)*channels+i];
                    pending_[size_t(pendingFrames_)*channels+i]=std::isfinite(sample)?sample:0.0;
                }
                pendingFrames_+=n;result.consumed_frames+=n;totalInput_+=n;
            }
            if(result.consumed_frames==frames && (flags&TTPCOMM_AUDIO_END_INPUT))ending_=true;
            if(ending_ && !totalInput_)return TTPCOMM_END;
            if(pendingFrames_<block_ && !ending_)return TTPCOMM_OK;
            if(pendingFrames_==block_ || ending_) {
                std::fill(pending_.begin()+size_t(pendingFrames_)*channels,pending_.end(),0.0);
                Block();pendingFrames_=0;
                continue;
            }
            return TTPCOMM_OK;
        }
    }
    void Reset() noexcept {
        std::fill(history_.begin(),history_.end(),0.0);
        std::fill(pending_.begin(),pending_.end(),0.0);
        std::fill(queued_.begin(),queued_.end(),0.0);
        std::fill(surround_.begin(),surround_.end(),0.0);
        std::copy(target_.begin(),target_.end(),base_.begin());
        pendingFrames_=queueFrames_=queuePosition_=0;totalInput_=generated_=position_=0;
        ending_=false;transitionPosition_=transitionLength_=0;
        surroundCursor_=surroundPosition_=surroundTransition_=0;
        surroundCurrent_=surroundTarget_;previousMono_=highState_=lowState_=0;
    }
private:
    void Transform(std::vector<Complex>& values,bool inverse) const noexcept {
        for(uint32_t i=0;i<fft_;++i)if(i<reverse_[i])std::swap(values[i],values[reverse_[i]]);
        for(uint32_t length=2;length<=fft_;length*=2)
            for(uint32_t start=0;start<fft_;start+=length)
                for(uint32_t i=0;i<length/2;++i) {
                    auto weight=twiddles_[i*(fft_/length)];
                    if(inverse)weight.im=-weight.im;
                    const auto even=values[start+i],odd=Multiply(values[start+i+length/2],weight);
                    values[start+i]={even.re+odd.re,even.im+odd.im};
                    values[start+i+length/2]={even.re-odd.re,even.im-odd.im};
                }
        if(inverse)for(auto& value:values){value.re/=fft_;value.im/=fft_;}
    }
    std::vector<Complex> Kernel(const TtpCommAudioEffectsConfig& config) const {
        std::vector<Complex> result(fft_);
        if(!(config.flags&TTPCOMM_AUDIO_EQ)) {result[delay_].re=1;Transform(result,false);return result;}
        constexpr double frequencies[9]={32,64,125,250,500,1000,2000,4000,8000};
        double gains[10];
        for(uint32_t i=0;i<10;++i)gains[i]=std::pow(10.0,(config.bands_db[i]+config.preamp_db)/20.0);
        constexpr double beta=8.6;
        const double denominator=Bessel(beta);
        for(uint32_t i=0;i<taps_;++i) {
            const int n=static_cast<int>(i)-static_cast<int>(delay_);
            const auto lowpass=[&](double frequency) {
                frequency=std::min(frequency,config.sample_rate*0.5);
                return n?std::sin(2*pi*frequency*n/config.sample_rate)/(pi*n):2*frequency/config.sample_rate;
            };
            double previous=lowpass(frequencies[0]),value=previous*gains[0];
            for(uint32_t band=1;band<9;++band) {
                const double next=lowpass(frequencies[band]);value+=(next-previous)*gains[band];previous=next;
            }
            value+=((n?0.0:1.0)-previous)*gains[9];
            const double ratio=double(n)/delay_;
            result[i].re=value*Bessel(beta*std::sqrt(std::max(0.0,1-ratio*ratio)))/denominator;
        }
        Transform(result,false);return result;
    }
    static double SurroundStrength(const TtpCommAudioEffectsConfig& config) noexcept {
        return config.flags&TTPCOMM_AUDIO_SURROUND?config.surround_strength/64.0:0.0;
    }
    void Surround(double* samples,uint32_t frames) noexcept {
        if(config_.channels<2)return;
        for(uint32_t f=0;f<frames;++f,samples+=config_.channels) {
            if(surroundTransition_) {
                const double alpha=std::min(1.0,double(++surroundPosition_)/surroundTransition_);
                surroundCurrent_=surroundStart_+(surroundTarget_-surroundStart_)*alpha;
                if(surroundPosition_>=surroundTransition_)surroundTransition_=0;
            }
            const double delayed=surround_[surroundCursor_];
            surround_[surroundCursor_]=(samples[0]+samples[1])*0.5;
            if(++surroundCursor_==surround_.size())surroundCursor_=0;
            highState_=highAlpha_*(highState_+delayed-previousMono_);previousMono_=delayed;
            lowState_=(1-lowAlpha_)*highState_+lowAlpha_*lowState_;
            const double value=lowState_*surroundCurrent_;
            samples[0]+=value;samples[1]-=value;
        }
    }
    void Block() noexcept {
        const uint32_t channels=config_.channels,historyFrames=taps_-1;
        const uint32_t skip=position_<delay_?static_cast<uint32_t>(std::min<uint64_t>(block_,delay_-position_)):0;
        uint32_t emit=block_-skip;
        if(ending_)emit=static_cast<uint32_t>(std::min<uint64_t>(emit,totalInput_-generated_));
        const bool transition=transitionLength_ && transitionPosition_<transitionLength_;
        for(uint32_t c=0;c<channels;++c) {
            for(uint32_t i=0;i<historyFrames;++i)work_[i]={history_[size_t(c)*historyFrames+i],0};
            for(uint32_t i=0;i<block_;++i)work_[historyFrames+i]={pending_[size_t(i)*channels+c],0};
            // Save the last M-1 unfiltered input samples for overlap-save.
            for(uint32_t i=0;i<historyFrames;++i)history_[size_t(c)*historyFrames+i]=work_[fft_-historyFrames+i].re;
            Transform(work_,false);
            for(uint32_t i=0;i<fft_;++i)filtered_[i]=Multiply(work_[i],base_[i]);
            Transform(filtered_,true);
            if(transition) {
                for(uint32_t i=0;i<fft_;++i)second_[i]=Multiply(work_[i],target_[i]);
                Transform(second_,true);
            }
            for(uint32_t i=0;i<emit;++i) {
                const uint32_t offset=historyFrames+skip+i;
                double value=filtered_[offset].re;
                if(transition) {
                    const double alpha=std::min(1.0,double(transitionPosition_+skip+i+1)/transitionLength_);
                    value+=(second_[offset].re-value)*alpha;
                }
                queued_[size_t(i)*channels+c]=value;
            }
        }
        if(transition) {
            transitionPosition_+=block_;
            if(transitionPosition_>=transitionLength_) {
                std::copy(target_.begin(),target_.end(),base_.begin());
                transitionLength_=transitionPosition_=0;
            }
        }
        Surround(queued_.data(),emit);
        position_+=block_;generated_+=emit;queueFrames_=emit;queuePosition_=0;
    }
    TtpCommAudioEffectsConfig config_;
    uint32_t taps_{},fft_{},block_{},delay_{},pendingFrames_{},queueFrames_{},queuePosition_{};
    uint64_t totalInput_{},generated_{},position_{},transitionLength_{},transitionPosition_{};
    bool ending_{};
    std::vector<uint32_t> reverse_;
    std::vector<Complex> twiddles_,base_,target_,work_,filtered_,second_;
    std::vector<double> history_,pending_,queued_,surround_;
    uint32_t surroundCursor_{},surroundPosition_{},surroundTransition_{};
    double surroundCurrent_{},surroundTarget_{},surroundStart_{};
    double previousMono_{},highState_{},lowState_{},highAlpha_{},lowAlpha_{};
};
}
void* __cdecl EffectsCreate(const TtpCommAudioEffectsConfig* config) noexcept {
    if(!Valid(config))return nullptr;
    try{return new Effects(*config);}catch(...){return nullptr;}
}
void __cdecl EffectsDestroy(void* object) noexcept {delete static_cast<Effects*>(object);}
int __cdecl EffectsInfo(void* object,TtpCommAudioEffectsInfo* info) noexcept {
    if(!object || !info || info->size<sizeof(*info))return TTPCOMM_INVALID;
    static_cast<Effects*>(object)->Info(*info);return TTPCOMM_OK;
}
int __cdecl EffectsConfigure(void* object,const TtpCommAudioEffectsConfig* config) noexcept {
    if(!object || !Valid(config))return TTPCOMM_INVALID;
    try{return static_cast<Effects*>(object)->Configure(*config);}
    catch(const std::bad_alloc&){return TTPCOMM_NOMEM;}
    catch(...){return TTPCOMM_INVALID;}
}
int __cdecl EffectsProcess(void* object,const double* input,uint32_t frames,double* output,
    uint32_t capacity,uint32_t flags,TtpCommAudioProcessResult* result) noexcept {
    if(!object || !result || result->size<sizeof(*result))return TTPCOMM_INVALID;
    *result={sizeof(*result),0,0,0};
    return static_cast<Effects*>(object)->Process(input,frames,output,capacity,flags,*result);
}
void __cdecl EffectsReset(void* object) noexcept {if(object)static_cast<Effects*>(object)->Reset();}
}
