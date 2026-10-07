#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include "replaygain_coefficients.inc"

namespace {
struct Channel {
    double x[11]{}, y[11]{}, butterX[3]{}, butterY[3]{};
    static double clean(double x) { return std::abs(x)<2.220446049250313e-16 ? 0 : x; }
    double filter(double sample,const GainCoefficients& c) {
        for(int i=10;i>0;--i) {x[i]=x[i-1];y[i]=y[i-1];}
        x[0]=sample;
        // Keep the recovered descending summation order (60003A70).
        double value=x[10]*c.b[10]-y[10]*c.a[10];
        for(int i=9;i>=2;--i)value+=x[i]*c.b[i]-y[i]*c.a[i];
        value+=x[0]*c.b[0];value+=x[1]*c.b[1]-y[1]*c.a[1];
        y[0]=clean(value);
        for(int i=2;i>0;--i){butterX[i]=butterX[i-1];butterY[i]=butterY[i-1];}
        butterX[0]=y[0];
        butterY[0]=clean((butterX[2]*c.butterB[2]-butterY[2]*c.butterA[2])+
            butterX[0]*c.butterB[0]+(butterX[1]*c.butterB[1]-butterY[1]*c.butterA[1]));
        return butterY[0];
    }
};
double histogramGain(const unsigned* bins) {
    unsigned total=0;for(int i=0;i<12000;++i)total+=bins[i];
    if(!total)return 0;
    auto remaining=static_cast<std::int64_t>(std::ceil(total*0.050000000000000044));
    int i=12000;do{--i;remaining-=bins[i];}while(i>0 && remaining>0);
    return 64.82-i*0.01;
}
// x86 MSVC scalar deleting destructor occupies slot 0, as in 600273B8.
class Analyzer {
public:
    virtual ~Analyzer() = default;
    virtual bool initialize(int rate) noexcept {
        const GainCoefficients* next=nullptr;
        for(const auto& c:gainCoefficients)if(c.rate==rate)next=&c;
        if(!next)return false;
        coefficients_=next;window_=static_cast<unsigned>(std::ceil(rate*0.05));
        std::memset(album_,0,sizeof(album_));std::memset(track_,0,sizeof(track_));
        peak_=0;resetFilters();return true;
    }
    virtual bool process(const double* samples,unsigned channels,unsigned frames) noexcept {
        if(!coefficients_ || !channels || (frames && !samples))return false;
        for(unsigned i=0;i<frames;++i,samples+=channels) {
            double l,r;
            if(channels==1)l=r=samples[0];
            else if(channels==2){l=samples[0];r=samples[1];}
            else if(channels==6){
                l=(samples[4]+samples[2])*0.7071067811865476+samples[0]+samples[3];
                r=(samples[5]+samples[2])*0.7071067811865476+samples[1]+samples[3];
            }else{l=0;for(unsigned k=0;k<channels;++k)l+=samples[k];l/=channels;r=l;}
            // Original tracks only the left/downmixed-left peak, including stereo.
            peak_=std::max(peak_,std::abs(l));
            l=left_.filter(l,*coefficients_);r=right_.filter(r,*coefficients_);
            leftSum_+=l*l;rightSum_+=r*r;
            if(++count_==window_) {
                double energy=1000*std::log10((leftSum_+rightSum_)/window_*536870912.0+1e-37);
                int bin=std::isfinite(energy) ? static_cast<int>(std::clamp(energy,0.0,11999.0)) : 11999;
                ++track_[bin];count_=0;leftSum_=rightSum_=0;
            }
        }
        return true;
    }
    virtual double trackGain() noexcept {
        double result=histogramGain(track_);
        for(int i=0;i<12000;++i){album_[i]+=track_[i];track_[i]=0;}
        resetFilters();return result;
    }
    virtual double peak() noexcept {return peak_;}
    virtual double albumGain() noexcept {return histogramGain(album_);}
private:
    void resetFilters() noexcept {left_={};right_={};count_=0;leftSum_=rightSum_=0;}
    const GainCoefficients* coefficients_{};
    Channel left_,right_;
    unsigned window_{},count_{};
    double leftSum_{},rightSum_{},peak_{};
    unsigned track_[12000]{},album_[12000]{};
};
}
extern "C" void* __cdecl ttp_replaygain_create() noexcept {return new(std::nothrow) Analyzer;}
