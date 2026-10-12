// Stream adapters for the existing 7z folder/filter graph. No archive writing.
#include <windows.h>
#include "CPP/7zip/Common/RegisterCodec.h"
#include "archive_seven_hooks.h"
#include <bzlib.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <zstd_errors.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <cstddef>

// BZ_NO_STDIO routes internal invariant failures here. Keep failures inside
// this operation instead of terminating the player process.
extern "C" void bz_internal_error(int){throw E_FAIL;}

namespace {
struct Allocation {size_t size;std::max_align_t alignment;};
struct Budget {
    size_t used{};const uint64_t limit=ttpcomm_seven_codec_memory_limit()+2*1024*1024;
    bool exceeded{};
    static void* Allocate(void* opaque,size_t n) {
        auto& b=*static_cast<Budget*>(opaque);
        if(n>SIZE_MAX-sizeof(Allocation) || n>b.limit || b.used>b.limit-n){b.exceeded=true;return nullptr;}
        auto* p=static_cast<Allocation*>(std::malloc(sizeof(Allocation)+n));
        if(!p)return nullptr;p->size=n;b.used+=n;return p+1;
    }
    static void Free(void* opaque,void* data) {
        if(!data)return;auto* p=static_cast<Allocation*>(data)-1;
        static_cast<Budget*>(opaque)->used-=p->size;std::free(p);
    }
    static void* BzipAllocate(void* opaque,int count,int size) {
        if(count<0 || size<0 || size && size_t(count)>SIZE_MAX/size){static_cast<Budget*>(opaque)->exceeded=true;return nullptr;}
        return Allocate(opaque,size_t(count)*size);
    }
};

class Decoder final:public ICompressCoder,public ICompressSetDecoderProperties2,
    public ICompressSetFinishMode,public ICompressGetInStreamProcessedSize {
    std::atomic<ULONG> refs_{0};bool zstd_,finish_{true};UInt64 processed_{};
public:
    explicit Decoder(bool zstd):zstd_(zstd){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** p) noexcept override {
        if(!p)return E_POINTER;*p=nullptr;
        if(id==IID_IUnknown || id==IID_ICompressCoder)*p=static_cast<ICompressCoder*>(this);
        else if(id==IID_ICompressSetDecoderProperties2)*p=static_cast<ICompressSetDecoderProperties2*>(this);
        else if(id==IID_ICompressSetFinishMode)*p=static_cast<ICompressSetFinishMode*>(this);
        else if(id==IID_ICompressGetInStreamProcessedSize)*p=static_cast<ICompressGetInStreamProcessedSize*>(this);
        else return E_NOINTERFACE;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override{return ++refs_;}
    ULONG STDMETHODCALLTYPE Release() noexcept override{const auto n=--refs_;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE SetDecoderProperties2(const Byte* p,UInt32 n) noexcept override {
        if(!zstd_)return n?E_NOTIMPL:S_OK;
        // 7-Zip-Zstandard method 04 F7 11 01: current flags byte, or the
        // historical 3/5-byte version/level properties. These do not encode a
        // dictionary; its actual window is read from each Zstandard frame.
        return (n==1 && p && p[0]==0) || n==3 || n==5?S_OK:E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetFinishMode(UInt32 mode) noexcept override{finish_=mode!=0;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetInStreamProcessedSize(UInt64* n) noexcept override{if(!n)return E_POINTER;*n=processed_;return S_OK;}
    HRESULT STDMETHODCALLTYPE Code(ISequentialInStream* input,ISequentialOutStream* output,
        const UInt64* input_size,const UInt64* output_size,ICompressProgressInfo* progress) noexcept override {
        try {
            processed_=0;UInt64 produced=0,read_total=0;
            if(!finish_ && output_size && !*output_size)return S_OK;
            auto buffers=std::make_unique<unsigned char[]>(131072);
            auto* in=buffers.get();auto* out=in+65536;
            Budget budget;ZSTD_DCtx* z=nullptr;bz_stream bz{};bool bz_open=false;
            struct Close {
                ZSTD_DCtx*& z;bz_stream& b;bool& opened;
                ~Close(){if(z)ZSTD_freeDCtx(z);if(opened)BZ2_bzDecompressEnd(&b);}
            } close{z,bz,bz_open};
            if(zstd_){
                z=ZSTD_createDCtx_advanced({Budget::Allocate,Budget::Free,&budget});if(!z)return E_OUTOFMEMORY;
                if(ZSTD_isError(ZSTD_DCtx_setMaxWindowSize(z,static_cast<size_t>(ttpcomm_seven_codec_memory_limit()))))return ttpcomm_seven_codec_memory_error();
            }else{
                bz.bzalloc=Budget::BzipAllocate;bz.bzfree=Budget::Free;bz.opaque=&budget;
                if(BZ2_bzDecompressInit(&bz,0,0)!=BZ_OK)return E_OUTOFMEMORY;bz_open=true;
            }
            size_t at=0,available=0;bool ended=false,eof=false,new_frame=true;
            for(;;){
                if(ttpcomm_seven_canceled())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                if(at==available && !eof){
                    UInt32 got{};UInt32 wanted=65536;
                    if(input_size)wanted=static_cast<UInt32>(std::min<UInt64>(wanted,*input_size-read_total));
                    if(wanted){const auto hr=input->Read(in,wanted,&got);if(FAILED(hr))return hr;}
                    if(got>wanted)return E_FAIL;
                    available=got;at=0;read_total+=got;
                    if(!got){eof=true;if(ended)return (!output_size || produced==*output_size) && (!input_size || processed_==*input_size)?S_OK:S_FALSE;}
                    else if(ended && !zstd_)return S_FALSE;
                }
                const auto prior_processed=processed_;size_t count{};
                if(zstd_){
                    if(new_frame){
                        ZSTD_frameHeader header{};
                        auto need=ZSTD_getFrameHeader(&header,in+at,available-at);
                        if(ZSTD_isError(need))return S_FALSE;
                        while(need){
                            if(eof || need>65536)return S_FALSE;
                            const size_t remaining=available-at;memmove(in,in+at,remaining);at=0;available=remaining;
                            UInt32 got{};const UInt64 room=65536-remaining;
                            const auto wanted=static_cast<UInt32>(input_size?std::min<UInt64>(room,*input_size-read_total):room);
                            if(!wanted)return S_FALSE;
                            const auto hr=input->Read(in+remaining,wanted,&got);if(FAILED(hr))return hr;
                            if(!got || got>wanted)return S_FALSE;available+=got;read_total+=got;
                            need=ZSTD_getFrameHeader(&header,in,available);if(ZSTD_isError(need))return S_FALSE;
                        }
                        // libzstd's direct-decode shortcut may not allocate a
                        // window. Enforce the declared window even on that path.
                        if(header.windowSize>ttpcomm_seven_codec_memory_limit())return ttpcomm_seven_codec_memory_error();
                        new_frame=false;
                    }
                    const size_t before=at;
                    ZSTD_inBuffer i{in,available,at};ZSTD_outBuffer o{out,65536,0};
                    const auto result=ZSTD_decompressStream(z,&o,&i);at=i.pos;count=o.pos;
                    if(ZSTD_isError(result)){
                        if(budget.exceeded || ZSTD_getErrorCode(result)==ZSTD_error_frameParameter_windowTooLarge)return ttpcomm_seven_codec_memory_error();
                        return ZSTD_getErrorCode(result)==ZSTD_error_memory_allocation?E_OUTOFMEMORY:S_FALSE;
                    }
                    processed_+=at-before;ended=result==0;new_frame=ended;
                }else{
                    if(new_frame){
                        while(available-at<4){
                            if(eof)return S_FALSE;
                            const size_t remaining=available-at;memmove(in,in+at,remaining);at=0;available=remaining;
                            const UInt64 room=65536-remaining;UInt32 got{};
                            const auto wanted=static_cast<UInt32>(input_size?std::min<UInt64>(room,*input_size-read_total):room);
                            if(!wanted)return S_FALSE;
                            const auto hr=input->Read(in+remaining,wanted,&got);if(FAILED(hr))return hr;
                            if(!got || got>wanted)return S_FALSE;available+=got;read_total+=got;
                        }
                        if(in[at]!='B' || in[at+1]!='Z' || in[at+2]!='h' || in[at+3]<'1' || in[at+3]>'9')return S_FALSE;
                        if(uint64_t(in[at+3]-'0')*100000>ttpcomm_seven_codec_memory_limit())return ttpcomm_seven_codec_memory_error();
                        new_frame=false;
                    }
                    const size_t before=at;
                    bz.next_in=reinterpret_cast<char*>(in+at);bz.avail_in=static_cast<unsigned>(available-at);
                    bz.next_out=reinterpret_cast<char*>(out);bz.avail_out=65536;
                    const int result=BZ2_bzDecompress(&bz);at=available-bz.avail_in;count=65536-bz.avail_out;
                    if(budget.exceeded)return ttpcomm_seven_codec_memory_error();
                    if(result!=BZ_OK && result!=BZ_STREAM_END)return result==BZ_MEM_ERROR?E_OUTOFMEMORY:S_FALSE;
                    ended=result==BZ_STREAM_END;
                    if(ended && at!=available && finish_)return S_FALSE;
                    processed_+=at-before;
                }
                if(output_size && count>*output_size-produced){if(finish_)return S_FALSE;count=static_cast<size_t>(*output_size-produced);}
                size_t sent=0;
                while(sent<count){UInt32 n{};const auto hr=output->Write(out+sent,static_cast<UInt32>(count-sent),&n);
                    if(FAILED(hr))return hr;if(!n || n>count-sent)return STG_E_WRITEFAULT;sent+=n;}
                produced+=count;
                if(progress){const auto hr=progress->SetRatioInfo(&processed_,&produced);if(FAILED(hr))return hr;}
                if(!finish_ && output_size && produced==*output_size)return S_OK;
                if(ended && at==available && (eof || input_size && read_total==*input_size))
                    return !output_size || produced==*output_size?S_OK:S_FALSE;
                if(prior_processed==processed_ && !count)return S_FALSE;
            }
        }catch(HRESULT hr){return hr;}catch(...){return E_OUTOFMEMORY;}
    }
};
REGISTER_CODEC_CREATE(CreateBzipDecoder,Decoder(false))
REGISTER_CODEC_2(BZip2,CreateBzipDecoder,nullptr,0x40202,"BZip2")
REGISTER_CODEC_CREATE(CreateZstdDecoder,Decoder(true))
REGISTER_CODEC_2(Zstandard,CreateZstdDecoder,nullptr,0x4F71101,"ZSTD")
}
