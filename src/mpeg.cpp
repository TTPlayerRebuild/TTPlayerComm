#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "ttpcomm_layouts.h"
extern "C" {
#include "common.h"
#include "layer1.h"
#include "layer2.h"
#include "layer3.h"
#include "decode_i386.h"
#include "tabinit.h"
void lame_report_fnc(lame_report_function, const char*, ...) {}
}

namespace {
struct Header { frame fields; unsigned bytes,samples,sideBytes; };
struct Decoder {
    MPSTR mp;
    Header header;
    unsigned mode,historySize;
    bool headerReady,synchronized;
    unsigned char history[512];
};
volatile LONG tablesReady=0;
void initializeTables() noexcept {
    if(InterlockedCompareExchange(&tablesReady,1,0)==0) {
        make_decode_tables(1);hip_init_tables_layer2();hip_init_tables_layer3();
        InterlockedExchange(&tablesReady,2);
    }else while(InterlockedCompareExchange(&tablesReady,2,2)!=2)Sleep(0);
}
bool parse(const unsigned char* p,Header& h) noexcept {
    const uint32_t bits=uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];
    if((bits&0xffe00000)!=0xffe00000)return false;
    auto& f=h.fields;f={};
    f.mpeg25=(bits&0x100000)==0;f.lsf=f.mpeg25?1:(~(bits>>19)&1);
    const int frequency=(bits>>10)&3;
    f.lay=4-((bits>>17)&3);f.bitrate_index=(bits>>12)&15;
    if(frequency==3||f.lay==4||f.bitrate_index==0||f.bitrate_index==15)return false;
    f.sampling_frequency=frequency+(f.mpeg25?6:f.lsf*3);
    f.mode=(bits>>6)&3;f.mode_ext=(bits>>4)&3;f.stereo=f.mode==3?1:2;
    f.single=-1;f.error_protection=(~(bits>>16))&1;f.padding=(bits>>9)&1;
    f.extension=(bits>>8)&1;f.copyright=(bits>>3)&1;f.original=(bits>>2)&1;f.emphasis=bits&3;
    f.down_sample_sblimit=32;
    const unsigned bitrate=tabsel_123[f.lsf][f.lay-1][f.bitrate_index];
    const unsigned rate=freqs[f.sampling_frequency];
    h.samples=f.lay==1?384:(f.lay==3&&f.lsf?576:1152);
    h.bytes=f.lay==1?(bitrate*12000/rate+f.padding)*4:
        bitrate*144000/(rate<<(f.lay==3?f.lsf:0))+f.padding;
    h.sideBytes=f.lay==3?(f.lsf?(f.stereo==1?9:17):(f.stereo==1?17:32)):0;
    f.framesize=h.bytes-4;
    return h.bytes>=4&&h.bytes<=MAXFRAMESIZE;
}
int decode(Decoder& d,const unsigned char* input,unsigned bytes,double* output,unsigned& produced) noexcept {
    auto& m=d.mp;m.fr=d.header.fields;m.bitindex=0;
    const unsigned crc=m.fr.error_protection?2:0;
    if(bytes<4+crc+d.header.sideBytes || bytes>MAXFRAMESIZE)return -1;
    auto* body=m.bsspace[0]+512;
    std::memset(m.bsspace[0],0,sizeof(m.bsspace[0]));
    std::memcpy(body,input+4,bytes-4);
    m.wordpointer=body+crc;
    int count=0,result=0;
    if(m.fr.lay==3) {
        decode_layer3_sideinfo(&m);
        // 600121B0: short blocks always split at 18 Huffman pairs, including
        // 8 kHz and mixed blocks. Newer mpglib changes these historical rules.
        if(m.fr.lsf)for(int c=0;c<m.fr.stereo;++c) {
            auto& gr=m.sideinfo.ch[c].gr[0];
            if(gr.block_type==2)gr.region1start=18;
        }
        auto* main=body+crc+d.header.sideBytes;
        const unsigned mainBytes=bytes-4-crc-d.header.sideBytes;
        m.wordpointer=main;m.bitindex=0;
        unsigned back=d.mode?0:m.sideinfo.main_data_begin;
        unsigned requiredBits=0;bool valid=true;
        for(int c=0;c<m.fr.stereo;++c)for(int g=0;g<(m.fr.lsf?1:2);++g) {
            const auto& gr=m.sideinfo.ch[c].gr[g];
            requiredBits+=gr.part2_3_length;
            valid=valid&&gr.block_type<=3;
        }
        if(back>d.historySize || requiredBits>(mainBytes+back)*8 || !valid)result=-1;
        else {
            if(back)std::memcpy(main-back,d.history+d.historySize-back,back);
            m.sideinfo.main_data_begin=back;
            result=decode_layer3_frame(&m,reinterpret_cast<unsigned char*>(output),&count,
                synth_1to1_mono_unclipped,synth_1to1_unclipped);
        }
        if(!d.mode) {
            // Preserve the last 511 main-data bytes, not MPEG headers/CRC/sideinfo.
            const unsigned keep=std::min(d.historySize,511-std::min(511u,mainBytes));
            std::memmove(d.history,d.history+d.historySize-keep,keep);
            const unsigned take=std::min(511u,mainBytes);
            std::memcpy(d.history+keep,input+bytes-take,take);d.historySize=keep+take;
        }
    }else if(m.fr.lay==1)result=decode_layer1_frame(&m,reinterpret_cast<unsigned char*>(output),&count);
    else result=decode_layer2_frame(&m,reinterpret_cast<unsigned char*>(output),&count);
    produced=result>=0?static_cast<unsigned>(count):0;
    return result;
}
}
extern "C" void* __cdecl ttp_decoder_create(unsigned mode) noexcept {
    initializeTables();auto* d=static_cast<Decoder*>(std::calloc(1,sizeof(Decoder)));
    if(d){d->mode=mode;d->mp.synth_bo=1;}
    return d; // Entire state is inline: ordinal 12/MSVCRT free remains valid.
}
extern "C" void __cdecl ttp_decoder_reset(void* instance) noexcept {
    if(auto* d=static_cast<Decoder*>(instance)) {
        d->headerReady=false;d->mp.bitindex=0;d->mp.synth_bo=1;d->historySize=0;
        // Original reset does not clear hybrid/synthesis history or sync status.
    }
}
extern "C" int __cdecl ttp_decoder_process(void* instance,TtpDecoderIo* io) noexcept {
    auto* d=static_cast<Decoder*>(instance);
    if(!d||!io||!io->input||!io->output||!io->output_capacity_bytes)return -1;
    io->consumed_bytes=io->produced_bytes=0;int result=0;
    const unsigned char* input=io->input;unsigned remaining=io->input_bytes;
    do {
        if(remaining<8){result=1;break;}
        if(!d->headerReady) {
            if(!parse(input,d->header)) {
                d->synchronized=false;++input;--remaining;++io->consumed_bytes;continue;
            }
            d->headerReady=true;
        }
        unsigned frameBytes=d->mode?remaining:d->header.bytes;
        if(!d->mode) {
            if(remaining<frameBytes+8){result=1;break;}
            if(!d->synchronized) {
                Header next{};
                if(!parse(input+frameBytes,next)) {
                    d->headerReady=false;++input;--remaining;++io->consumed_bytes;continue;
                }
                d->synchronized=true;
            }
        }
        const unsigned needed=d->header.samples*d->header.fields.stereo*sizeof(double);
        if(io->output_capacity_bytes-io->produced_bytes<needed)break;
        unsigned produced=0;
        result=decode(*d,input,frameBytes,io->output+io->produced_bytes/sizeof(double),produced);
        d->headerReady=false;input+=frameBytes;remaining-=frameBytes;io->consumed_bytes+=frameBytes;
        if(result>=0)io->produced_bytes+=produced;
    }while(remaining>8);
    return io->produced_bytes?0:result;
}
