#include <zlib.h>
#include <cstddef>
static_assert(sizeof(z_stream)==56, "Original x86 stream ABI");
extern "C" int __cdecl ttp_inflate_init(z_streamp stream, int bits, const char* version, int size) {
    if (!version || version[0]!='1' || size!=56) return Z_VERSION_ERROR;
    if (!stream) return Z_STREAM_ERROR;
    // Original 1.1.4 supports raw/zlib windows 8..15, not gzip/autodetect.
    if ((bits<8 || bits>15) && (bits>-8 || bits<-15)) return Z_STREAM_ERROR;
    return inflateInit2_(stream,bits,ZLIB_VERSION,sizeof(z_stream));
}
extern "C" int __cdecl ttp_inflate(z_streamp stream,int flush) { return inflate(stream,flush); }
extern "C" int __cdecl ttp_inflate_end(z_streamp stream) { return inflateEnd(stream); }
extern "C" int __cdecl ttp_uncompress(Bytef* output,uLongf* size,const Bytef* input,uLong length) {
    if(!size)return Z_STREAM_ERROR;
    // 60018F67 uses a single Z_FINISH call and leaves *size unchanged on error.
    // Modern uncompress() changes both truncated-input errors and length output.
    z_stream stream{};
    stream.next_in=const_cast<Bytef*>(input);stream.avail_in=length;
    stream.next_out=output;stream.avail_out=*size;
    int result=ttp_inflate_init(&stream,15,"1.1.4",sizeof(stream));
    if(result!=Z_OK)return result;
    result=inflate(&stream,Z_FINISH);
    if(result!=Z_STREAM_END) {
        inflateEnd(&stream);
        return result==Z_OK ? Z_BUF_ERROR : result;
    }
    *size=stream.total_out;
    return inflateEnd(&stream);
}
