#include <kiss_fftr.h>
#include <cmath>
#include <cstdint>
#include <new>
namespace {
class Spectrum {
public:
    Spectrum() noexcept : fft_(kiss_fftr_alloc(512,0,nullptr,nullptr)) {}
    virtual ~Spectrum(){kiss_fftr_free(fft_);}
    virtual void process(std::int16_t* work,int channels) noexcept {
        if(!fft_||!work||(channels!=1&&channels!=2))return;
        for(int side=0;side<channels;++side) {
            // 600016C0: the second transform starts at byte +0x400, after the
            // first result was written. Preserve this historical workspace ABI.
            for(int i=0;i<512;++i)input_[i]=static_cast<float>(work[side*512+i*channels]);
            kiss_fftr(fft_,input_,output_);
            for(int i=0;i<256;++i) {
                const auto v=output_[i];
                // x87 kept products in extended precision before calling sqrt.
                double energy=static_cast<double>(v.r)*v.r+static_cast<double>(v.i)*v.i;
                auto magnitude=static_cast<unsigned>(std::sqrt(energy));
                work[1024+side*256+i]=static_cast<std::int16_t>(magnitude>>8);
            }
        }
    }
    bool valid() const noexcept {return fft_!=nullptr;}
private:
    kiss_fftr_cfg fft_;
    float input_[512]{};
    kiss_fft_cpx output_[257]{};
};
}
extern "C" void* __cdecl ttp_spectrum_create() noexcept {
    auto* p=new(std::nothrow) Spectrum;
    if(p&&!p->valid()){delete p;return nullptr;}return p;
}
