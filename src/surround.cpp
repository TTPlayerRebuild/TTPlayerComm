#include <algorithm>
#include <cmath>
#include <new>
#include <cstring>

namespace {
struct Filter { double a{},b0{},b1{}; };
Filter coefficients(int frequency,int rate,float g0,float middle,float g1) {
    // Original 60005C90 quantises filter coefficients to signed integers.
    float denominator=g0*g0+g1*g1-(middle*middle+middle*middle);
    double alpha=0;
    if(denominator!=0) {
        alpha=(double(g1*g1)-double(g0*g0))/denominator;
        alpha-=std::sqrt(alpha*alpha-1)*(alpha>=0 ? 1 : -1);
    }
    float c0=static_cast<float>(((g0-g1)*alpha+g0+g1)*0.5);
    double c1=((g0+g1)*alpha+g0-g1)*0.5;
    double omega=static_cast<float>(frequency*3.1415927f/rate)*0.5;
    float warp=static_cast<float>(std::sin(omega-0.7853981852531433)/std::sin(omega+0.7853981852531433));
    float norm=static_cast<float>(1.0/(warp*alpha+1));
    auto quantize=[](double v){return std::nearbyint(static_cast<float>(v)*1024.0f);};
    return {quantize(-(warp+alpha)*norm),quantize((warp*c1+c0)*norm),quantize((warp*c0+c1)*norm)};
}
class Surround {
public:
    virtual ~Surround()=default;
    virtual void initialize(int rate,int channels,int strength) noexcept {
        if(rate<=0 || channels<=0)return;
        channels_=channels;strength_=std::clamp(strength,1,16);
        length_=std::clamp(rate*16/1000,1,1024);
        high_=coefficients(200,rate,0,0.5f,1);
        low_=coefficients(7000,rate,1,0.75f,0);
        // 60005F93 calls __allmul, followed by a signed right shift by five.
        high_.b0=(static_cast<int>(high_.b0)*strength_)>>5;
        high_.b1=(static_cast<int>(high_.b1)*strength_)>>5;
        reset();
    }
    virtual void process(double* samples,int count) noexcept {
        if(!samples || channels_<2 || count<=0)return;
        for(int frames=count/channels_;frames>0;--frames,samples+=channels_) {
            double delayed=ring_[position_];
            ring_[position_]=(samples[0]+samples[1])*0.001953125;
            if(++position_>=length_)position_=0;
            double filtered=(delayed*high_.b0+high_.b1*previousDelay_+previousHigh_*high_.a)*0.0009765625;
            previousDelay_=delayed;
            double value=(low_.a*previousLow_+filtered*low_.b0+previousHigh_*low_.b1)*0.25+2.220446049250313e-16;
            previousLow_=value*0.00390625;previousHigh_=filtered;
            samples[0]+=value;samples[1]-=value;
        }
    }
    virtual void reset() noexcept {
        position_=0;previousDelay_=previousHigh_=previousLow_=0;
        std::memset(ring_,0,sizeof(ring_));
    }
private:
    int channels_=2,strength_=8,length_=1,position_=0;
    double previousHigh_{},previousDelay_{},previousLow_{};
    Filter high_,low_;
    double ring_[1024]{};
};
}
extern "C" void* __cdecl ttp_surround_create() noexcept {return new(std::nothrow) Surround;}
