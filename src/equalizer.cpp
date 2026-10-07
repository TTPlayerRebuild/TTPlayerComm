#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <new>
#include <vector>

namespace {
constexpr int blockSize=511, fftSize=1024;
struct Complex { double re{},im{}; };
Complex multiply(Complex a,Complex b) noexcept {
    return {a.re*b.re-a.im*b.im,a.re*b.im+a.im*b.re};
}
void transform(std::array<Complex,fftSize>& values,bool inverse) noexcept {
    for(unsigned i=1,j=0;i<fftSize;++i) {
        unsigned bit=fftSize/2;
        while(j&bit){j^=bit;bit>>=1;}j^=bit;
        if(i<j)std::swap(values[i],values[j]);
    }
    for(int length=2;length<=fftSize;length*=2) {
        const double angle=(inverse?1:-1)*6.283185307179586476925286766559/length;
        const Complex step{std::cos(angle),std::sin(angle)};
        for(int start=0;start<fftSize;start+=length) {
            Complex weight{1,0};
            for(int i=0;i<length/2;++i) {
                const auto even=values[start+i];
                const auto odd=multiply(values[start+i+length/2],weight);
                values[start+i]={even.re+odd.re,even.im+odd.im};
                values[start+i+length/2]={even.re-odd.re,even.im-odd.im};
                weight=multiply(weight,step);
            }
        }
    }
    if(inverse)for(auto& value:values){value.re/=fftSize;value.im/=fftSize;}
}
double bessel(double value) noexcept {
    // 600063E0 sums terms 0..15, using integer powers and factorials.
    double sum=1,factorial=1;
    for(unsigned i=1;i<16;++i) {
        factorial*=i;
        double power=1,base=value*0.5;
        for(unsigned n=i;n;n>>=1){if(n&1)power*=base;if(n>>1)base*=base;}
        const double term=power/factorial;sum+=term*term;
    }
    return sum;
}
struct Channel {
    std::array<double,blockSize> pending{};
    std::array<double,fftSize> overlap{};
    std::vector<double> queue;
    int used{};
    bool first=true;
};
class Equalizer {
public:
    virtual ~Equalizer()=default;
    virtual bool initialize(int rate,int channels) noexcept {
        if(rate<=0 || channels<=0 || channels>256)return false;
        rate_=rate;channels_=channels;
        int bands[11]{};setParameters(bands);
        return ready_;
    }
    virtual void setParameters(const int* bands) noexcept {
        if(!bands || rate_<=0 || channels_<=0)return;
        try {
            for(int c=0;c<std::min(channels_,10);++c)
                if(!channel_[c])channel_[c]=std::make_unique<Channel>();
            constexpr double boundaries[9]={32,64,125,250,500,1000,2000,4000,8000};
            double gains[10];
            for(int i=0;i<10;++i)gains[i]=std::pow(10.0,(double(bands[i])+bands[10])*0.05);
            constexpr double beta=(96.0-8.7)*0.1102;
            const double normalizedBeta=beta/bessel(beta);
            for(int i=0;i<fftSize;++i) {
                double coefficient=0;
                if(i<blockSize) {
                    const int n=i-blockSize/2;
                    auto lowpass=[&](double frequency) {
                        // The original constant is a rounded float promoted to double.
                        const double angle=frequency*6.2831854820251465*n/rate_;
                        return (angle==0 ? 1 : std::sin(angle)/angle)/rate_*frequency*2;
                    };
                    double previous=lowpass(boundaries[0]);
                    coefficient=previous*gains[0];int band=1;
                    while(band<9 && boundaries[band]<rate_*0.5) {
                        const double next=lowpass(boundaries[band]);
                        coefficient+=(next-previous)*gains[band];previous=next;++band;
                    }
                    coefficient+=((n==0?1.0:0.0)-previous)*gains[band];
                    // Verified at 60006ECB: divide beta by I0(beta) BEFORE I0().
                    coefficient*=bessel(normalizedBeta*std::sqrt(1.0-4.0*n*n/260100.0));
                }
                kernel_[i]={coefficient,0};
            }
            transform(kernel_,false);ready_=true;
        } catch(const std::bad_alloc&) {ready_=false;}
    }
    virtual int latency() noexcept {return 0;}
    virtual bool submit(const double* samples,int count) noexcept {
        if(!ready_ || !samples || count<0)return false;
        try {
            for(int c=0;c<std::min(channels_,10);++c) {
                auto& state=*channel_[c];
                for(int i=0;i<count/channels_;++i) {
                    state.pending[state.used++]=samples[i*channels_+c];
                    if(state.used==blockSize)processBlock(state);
                }
            }
            return true;
        }catch(const std::bad_alloc&){return false;}
    }
    virtual bool receive(double* samples,int* count) noexcept {
        if(!ready_ || !count || *count<0 || (!samples&&*count))return false;
        int frames=*count/channels_;
        for(int c=0;c<std::min(channels_,10);++c)
            frames=std::min(frames,static_cast<int>(channel_[c]->queue.size()));
        for(int c=0;c<std::min(channels_,10);++c) {
            auto& queue=channel_[c]->queue;
            for(int i=0;i<frames;++i)samples[i*channels_+c]=queue[i];
            queue.erase(queue.begin(),queue.begin()+frames);
        }
        *count=frames*channels_;return true;
    }
    virtual void reset() noexcept {
        for(auto& state:channel_)if(state) {
            state->first=true;state->used=0;state->queue.clear();
            // 600068E0 intentionally leaves the overlap buffer intact.
        }
    }
private:
    void processBlock(Channel& state) {
        std::array<Complex,fftSize> work{};
        for(int i=0;i<blockSize;++i)work[i].re=state.pending[i];
        transform(work,false);
        for(int i=0;i<fftSize;++i)work[i]=multiply(work[i],kernel_[i]);
        transform(work,true);
        for(int i=0;i<fftSize-blockSize;++i)state.overlap[i]=state.overlap[i+blockSize];
        for(int i=0;i<blockSize;++i)state.overlap[i]+=work[i].re;
        for(int i=blockSize;i<fftSize;++i)state.overlap[i]=work[i].re;
        const int skip=state.first ? blockSize/2 : 0;
        state.queue.insert(state.queue.end(),state.overlap.begin()+skip,state.overlap.begin()+blockSize);
        state.first=false;state.used=0;
    }
    int rate_{},channels_{};
    bool ready_{};
    std::array<std::unique_ptr<Channel>,10> channel_;
    std::array<Complex,fftSize> kernel_{};
};
}
extern "C" void* __cdecl ttp_equalizer_create() noexcept {return new(std::nothrow) Equalizer;}
