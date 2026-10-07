#include "ssrc.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
#include <vector>

namespace {
class Interface {
public:
    virtual ~Interface()=default;
    virtual bool initialize(int,int,int,bool)=0;
    virtual int process(const double*,int,double*,int)=0;
    virtual void reset()=0;
};
class Ssrc final:public Interface {
    std::unique_ptr<Resampler_base> state_;
public:
    bool initialize(int inputRate,int outputRate,int channels,bool fast) noexcept override {
        state_.reset();
        if(inputRate<=0||outputRate<=0||channels<=0||channels>256)return false;
        // The historical backend stores intermediate rates in signed int.
        const auto common=std::gcd(inputRate,outputRate);
        if(int64_t(inputRate/common)*outputRate>std::numeric_limits<int>::max()/3 ||
           inputRate>768000 || outputRate>768000)return false;
        try {Resampler_base::CONFIG config(inputRate,outputRate,channels,0,0,fast);
            state_.reset(Resampler_base::Create(config));return state_!=nullptr;
        }catch(const std::bad_alloc&){state_.reset();return false;}
    }
    int process(const double* input,int count,double* output,int capacity) noexcept override {
        if(!state_||count<0||capacity<0||(!output&&capacity))return 0;
        try {
            if(input&&count)state_->Write(input,count);
            int available=0;auto* buffer=state_->GetBuffer(&available);
            const int result=std::min(available,capacity);
            if(result)std::memcpy(output,buffer,result*sizeof(double));
            state_->Read(result);return result;
        }catch(const std::bad_alloc&){return 0;}
    }
    void reset() noexcept override {
        // 60005660 calls Finish, then clears ONLY the output queue (6000CE00).
        if(state_)try{state_->Finish();state_->ClearOutput();}catch(const std::bad_alloc&){}
    }
};
double bessel(double x) noexcept {
    double term=1,sum=1;
    for(int n=2;n<1024;n+=2){const double y=x/n;term*=y*y;sum+=term;if(term<=1e-21*sum)break;}
    return sum;
}
class Polyphase final:public Interface {
    int inputStep_{},phases_{},channels_=2,taps_{},used_{},phase_{};
    std::vector<double> filter_,history_;
public:
    bool initialize(int inputRate,int outputRate,int channels,bool) noexcept override {
        filter_.clear();history_.clear();
        if(inputRate<=0||outputRate<=0||channels<=0||channels>256)return false;
        const double ratio=double(inputRate)/outputRate;
        const int common=std::gcd(inputRate,outputRate);
        inputStep_=inputRate/common;phases_=outputRate/common;channels_=channels;
        taps_=ratio>=.5&&ratio<=2?35:ratio>=.25&&ratio<=4?40:45;
        double cutoff=1;
        if(phases_<inputStep_){
            cutoff=double(phases_)/inputStep_;
            const auto scaled=int64_t(taps_)*inputStep_/phases_;
            if(scaled>16*1024*1024)return false;
            taps_=static_cast<int>(scaled);
        }
        const auto total=int64_t(taps_)*phases_;
        if(total<=0||total>16*1024*1024||int64_t(taps_)*channels_>16*1024*1024)return false;
        try {
            filter_.assign(static_cast<size_t>(total),0);history_.assign(size_t(taps_)*channels_,0);
            const int start=(total&1)?0:1,half=static_cast<int>((total-start)/2);
            const double norm=1/bessel(16),window=1.0/((half+1.0)*(half+1.0));
            for(int flat=start,n=-half;flat<total;++flat,++n) {
                constexpr double pi=3.1415927410125732421875;
                double value=n==0?cutoff:std::sin(n*pi*(cutoff/phases_))/(n*pi)*phases_;
                value*=bessel(std::sqrt(1.0-double(n)*n*window)*16)*norm;
                filter_[size_t(flat%phases_)*taps_+flat/phases_]=value;
            }
            used_=taps_/2+1;phase_=0;return true;
        }catch(const std::bad_alloc&){filter_.clear();history_.clear();return false;}
    }
    int process(const double* input,int count,double* output,int capacity) noexcept override {
        if(!input||!output||count<=0||capacity<0||filter_.empty())return 0;
        const int frames=count/channels_,missing=taps_-used_;
        // Original short-block path returns before updating the history count.
        if(frames<=missing) {
            for(int c=0;c<channels_;++c)for(int i=0;i<frames;++i)history_[size_t(c)*taps_+used_+i]=input[i*channels_+c];
            return 0;
        }
        const int remaining=frames-missing;
        // Original ignores capacity; reject an undersized destination safely.
        const int64_t required=(int64_t(remaining)*phases_-phase_+inputStep_-1)/inputStep_;
        if(required*channels_>capacity)return 0;
        int produced=0,nextUsed=used_,nextPhase=phase_;
        for(int c=0;c<channels_;++c) {
            auto* history=history_.data()+size_t(c)*taps_;
            for(int i=0;i<missing;++i)history[used_+i]=input[i*channels_+c];
            const double* start=input+missing*channels_+c;
            int pos=0,phase=phase_;produced=0;
            while(pos<remaining) {
                const double* kernel=filter_.data()+size_t(phase)*taps_;double sum=0;
                for(int i=0;i<taps_;++i)sum+=(pos-i>=0?start[(pos-i)*channels_]:history[taps_+pos-i])*kernel[i];
                output[produced++*channels_+c]=sum;
                phase+=inputStep_;pos+=phase/phases_;phase%=phases_;
            }
            int saved=0;
            if(pos<taps_)for(int i=pos;i<taps_;++i)history[saved++]=history[i];
            for(int i=pos-std::min(pos,taps_);i<remaining;++i)history[saved++]=start[i*channels_];
            nextUsed=saved;nextPhase=phase;
        }
        used_=nextUsed;phase_=nextPhase;return produced*channels_;
    }
    void reset() noexcept override {} // 600057B0 is a bare RET.
};
}
extern "C" void* __cdecl ttp_resampler_create(int mode) noexcept {
    if(mode==1||mode==2)return new(std::nothrow) Ssrc;
    return new(std::nothrow) Polyphase;
}
