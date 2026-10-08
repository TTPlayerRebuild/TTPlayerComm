#include "runtime_internal.h"
#include <ttpcomm/tags_zlib.h>
#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>
#include <new>

namespace ttpcomm::runtime {
namespace {
int __cdecl Inflate(uint32_t mode,uint32_t flags,const void* input,uint32_t size,
    void* output,uint32_t capacity,TtpCommInflateResult* result) noexcept {
    if(!result) return TTPCOMM_INVALID;
    *result={};
    if(mode>TTPCOMM_INFLATE_ZLIB || (flags&~TTPCOMM_INFLATE_ALLOW_TRAILING) ||
        (!input && size) || (!output && capacity)) return TTPCOMM_INVALID;
    z_stream stream{};stream.next_in=static_cast<Bytef*>(const_cast<void*>(input));stream.avail_in=size;
    unsigned char spare{};
    stream.next_out=capacity?static_cast<Bytef*>(output):&spare;stream.avail_out=capacity?capacity:1;
    const int init=inflateInit2(&stream,mode==TTPCOMM_INFLATE_RAW?-MAX_WBITS:MAX_WBITS);
    if(init!=Z_OK) return init==Z_MEM_ERROR?TTPCOMM_NOMEM:TTPCOMM_DATA;
    const int status=inflate(&stream,Z_FINISH);
    *result={stream.total_in,stream.total_out};
    const bool full_input=stream.total_in==size || (flags&TTPCOMM_INFLATE_ALLOW_TRAILING);
    const bool full_output=stream.total_out<=capacity;
    const bool exhausted=!stream.avail_out;
    inflateEnd(&stream);
    if(!full_output || (status==Z_BUF_ERROR && exhausted)) return TTPCOMM_CAPACITY;
    if(status==Z_MEM_ERROR) return TTPCOMM_NOMEM;
    return status==Z_STREAM_END && full_input?TTPCOMM_OK:TTPCOMM_DATA;
}
uint32_t __cdecl Crc(uint32_t seed,const void* data,uint32_t size) noexcept {
    // A zero-size input is an empty continuation, even if the buffer is null.
    return size && data ? crc32(seed,static_cast<const Bytef*>(data),size) : seed;
}
uint32_t __cdecl Terminator(const void* input,uint32_t size,uint32_t encoding) noexcept {
    if(size && !input) return size;
    return static_cast<uint32_t>(tags::FindTerminator({static_cast<const unsigned char*>(input),size},encoding));
}
int __cdecl Unsync(const void* input,uint32_t size,void* output,uint32_t capacity,uint32_t* produced) noexcept {
    if(!produced) return TTPCOMM_INVALID;
    *produced=0;if((size && !input) || (capacity && !output)) return TTPCOMM_INVALID;
    const auto* bytes=static_cast<const unsigned char*>(input);
    uint32_t count=0;
    for(uint32_t i=0;i<size;++i) {++count;if(bytes[i]==0xff && i<size-1 && bytes[i+1]==0) ++i;}
    if(count>capacity) return TTPCOMM_CAPACITY;
    auto* target=static_cast<unsigned char*>(output);
    for(uint32_t i=0,j=0;i<size;++i) {target[j++]=bytes[i];if(bytes[i]==0xff && i<size-1 && bytes[i+1]==0) ++i;}
    *produced=count;return TTPCOMM_OK;
}
int __cdecl FrameStart(const void* input,uint32_t size,uint32_t major,uint32_t flags,uint32_t* offset) noexcept {
    if(!offset || (size && !input)) return TTPCOMM_INVALID;
    const auto start=tags::FrameStart({static_cast<const unsigned char*>(input),size},major,flags);
    if(!start) return TTPCOMM_DATA;
    *offset=static_cast<uint32_t>(*start);return TTPCOMM_OK;
}
int __cdecl NextFrame(const void* input,uint32_t size,uint32_t major,uint32_t* offset,TtpCommTagFrame* output) noexcept {
    if(!offset || !output || (size && !input)) return TTPCOMM_INVALID;
    const auto* bytes=static_cast<const unsigned char*>(input);
    size_t next=*offset;tags::Frame frame;
    const auto status=tags::NextFrame({bytes,size},major,next,frame);
    if(status==tags::FrameStatus::end) return TTPCOMM_END;
    if(status!=tags::FrameStatus::frame) return TTPCOMM_DATA;
    *output={};output->offset=*offset;output->raw_size=static_cast<uint32_t>(frame.raw.size());
    output->payload_offset=static_cast<uint32_t>(frame.payload.data()-bytes);
    output->payload_size=static_cast<uint32_t>(frame.payload.size());output->flags=frame.flags;
    std::memcpy(output->identifier,frame.identifier.data(),frame.identifier.size());
    *offset=static_cast<uint32_t>(next);return TTPCOMM_OK;
}
int __cdecl Picture(const void* input,uint32_t size,uint32_t major,TtpCommPicture* output) noexcept {
    if(!output || (size && !input)) return TTPCOMM_INVALID;
    const auto* bytes=static_cast<const unsigned char*>(input);
    const auto picture=major?tags::Id3Picture({bytes,size},major):tags::FlacPicture({bytes,size});
    if(!picture) return TTPCOMM_DATA;
    *output={picture->type,picture->width,picture->height,picture->depth,
        static_cast<uint32_t>(picture->data.data()-bytes),static_cast<uint32_t>(picture->data.size()),0,0,0};
    if(major==2) {
        const char* names[]{"image/jpeg","image/png","image/bmp","image/gif"};
        for(unsigned i=0;i<4;++i) if(picture->mime==names[i]) {output->legacy_mime=i+1;return TTPCOMM_OK;}
    }
    output->mime_offset=static_cast<uint32_t>(reinterpret_cast<const unsigned char*>(picture->mime.data())-bytes);
    output->mime_size=static_cast<uint32_t>(picture->mime.size());return TTPCOMM_OK;
}
int __cdecl TagDecode(const void* input,uint32_t size,uint32_t major,uint32_t flags,uint32_t unsync,uint64_t* budget,TtpCommDecoded* output) noexcept {
    if(!output) return TTPCOMM_INVALID;
    *output={};
    if(!budget || (size && !input) || major<2 || major>4 || flags>255 || unsync>1) return TTPCOMM_INVALID;
    try {
        std::vector<unsigned char> storage;auto remaining=*budget;
        const auto view=tags::DecodePayload({static_cast<const unsigned char*>(input),size},static_cast<unsigned char>(major),
            static_cast<unsigned char>(flags),unsync!=0,storage,remaining);
        if(!view) return TTPCOMM_DATA;
        if(storage.capacity()) {
            auto owner=std::make_unique<std::vector<unsigned char>>(std::move(storage));
            *output={owner->data(),static_cast<uint32_t>(view->size()),owner.get()};owner.release();
        } else *output={view->data(),static_cast<uint32_t>(view->size()),nullptr};
        *budget=remaining;return TTPCOMM_OK;
    } catch(const std::bad_alloc&) {return TTPCOMM_NOMEM;}
    catch(...) {return TTPCOMM_INVALID;}
}
void __cdecl TagDestroy(void* object) noexcept {delete static_cast<std::vector<unsigned char>*>(object);}
}
}
extern "C" int __cdecl ttpcomm_query_runtime(uint32_t version,uint32_t required,TtpCommRuntimeApi* api) noexcept {
    using namespace ttpcomm::runtime;
    if(!api || api->size<sizeof(*api) || version!=TTPCOMM_RUNTIME_ABI || (required&~TTPCOMM_RUNTIME_REQUIRED)) return 0;
    const TtpCommRuntimeApi table{sizeof(table),TTPCOMM_RUNTIME_ABI,TTPCOMM_RUNTIME_REQUIRED,0,
        Inflate,Crc,PcmDecode,PcmEncode,PcmGain,QuantizerCreate,QuantizerDestroy,QuantizerEncode,QuantizerReset,
        Terminator,Unsync,FrameStart,NextFrame,Picture,TagDecode,TagDestroy};
    *api=table;return 1;
}
