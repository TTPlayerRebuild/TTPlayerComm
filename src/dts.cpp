#include <windows.h>
#include <mmreg.h>
#include <cstdint>
#include <algorithm>

namespace {
struct Bits {
    const unsigned char* data;unsigned size;bool little,packed;unsigned bit=0;bool valid=true;
    unsigned read(unsigned count) {
        unsigned result=0,wordBits=packed ? 14 : 16;
        for(unsigned i=0;i<count;++i,++bit) {
            unsigned pos=(bit/wordBits)*2;
            if(pos+1>=size){valid=false;return 0;}
            unsigned word=little ? data[pos]|(data[pos+1]<<8) : (data[pos]<<8)|data[pos+1];
            result=(result<<1)|((word>>(wordBits-1-bit%wordBits))&1);
        }
        return result;
    }
};
constexpr unsigned rates[]{0,8000,16000,32000,0,0,11025,22050,44100,0,0,12000,24000,48000,96000,192000};
constexpr unsigned bitrates[]{32000,56000,64000,96000,112000,128000,192000,224000,
    256000,320000,384000,448000,512000,576000,640000,768000,896000,1024000,
    1152000,1280000,1344000,1408000,1411200,1472000,1536000,1920000,2048000,
    3072000,3840000,1,2,3};
constexpr unsigned channels[]{1,2,2,2,2,3,3,4,4,5,2,3,3,3,3,4,4,5,5,6,128};
struct Header { unsigned length,frames,mode,rate,bitrate,bits; };
bool header(const unsigned char* p,unsigned size,Header& out) {
    if(size<8)return false;
    bool little,packed;
    if(p[0]==0x7f&&p[1]==0xfe&&p[2]==0x80&&p[3]==1){little=false;packed=false;}
    else if(p[0]==0xfe&&p[1]==0x7f&&p[2]==1&&p[3]==0x80){little=true;packed=false;}
    else if(p[0]==0x1f&&p[1]==0xff&&p[2]==0xe8&&p[3]==0&&p[4]==7&&(p[5]&0xf0)==0xf0){little=false;packed=true;}
    else if(p[0]==0xff&&p[1]==0x1f&&p[2]==0&&p[3]==0xe8&&(p[4]&0xf0)==0xf0&&p[5]==7){little=true;packed=true;}
    else return false;
    Bits b{p,size,little,packed};b.read(32);b.read(1);b.read(5);unsigned crc=b.read(1);
    out.frames=(b.read(7)+1)*32;out.length=b.read(14)+1;
    if(packed)out.length=out.length*8/14*2;
    out.mode=b.read(6);out.rate=rates[b.read(4)];out.bitrate=bitrates[b.read(5)];
    b.read(10);if(b.read(2))out.mode|=0x80;b.read(1);if(crc)b.read(16);
    b.read(1);b.read(4);b.read(2);constexpr unsigned depths[]{16,16,20,20,0,24,24,0};out.bits=depths[b.read(3)];
    return b.valid && out.rate && out.bitrate;
}
}
extern "C" int __cdecl ttp_dts_probe(const char* input,unsigned size,WAVEFORMATEX* output) noexcept {
    if(!input||!output)return 0;
    const auto* p=reinterpret_cast<const unsigned char*>(input);
    unsigned firstLength=0;int found=0;
    while(size>=8) {
        Header h{};
        if(!header(p,size,h)||!h.length||h.frames<=512){++p;--size;continue;}
        if(size<h.length)return found;
        if(!firstLength)firstLength=h.length;
        else {
            if(firstLength!=h.length)return 0;
            unsigned mode=std::min(h.mode&63,10u)+((h.mode&128)?10:0);
            output->wFormatTag=0x2001;output->nChannels=static_cast<WORD>(channels[mode]);
            output->nSamplesPerSec=h.rate;output->nAvgBytesPerSec=h.bitrate/8;
            output->nBlockAlign=static_cast<WORD>(h.length);output->wBitsPerSample=static_cast<WORD>(h.bits?h.bits:16);output->cbSize=0;
            found=1;
        }
        p+=h.length;size-=h.length;
    }
    return found;
}
