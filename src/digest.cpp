#include <cstdint>
#include <cstring>

namespace {
using U32 = std::uint32_t;
constexpr U32 constants[]{
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
constexpr unsigned shifts[]{7,12,17,22,5,9,14,20,4,11,16,23,6,10,15,21};
void block(U32 (&h)[4], const unsigned char* bytes) {
    U32 words[16];
    std::memcpy(words, bytes, sizeof(words)); // fixed little-endian x86 ABI
    U32 a=h[0], b=h[1], c=h[2], d=h[3];
    for (unsigned i=0; i<64; ++i) {
        U32 f; unsigned index;
        if (i<16) { f=(b&c)|(~b&d); index=i; }
        else if (i<32) { f=(d&b)|(~d&c); index=(5*i+1)&15; }
        else if (i<48) { f=b^c^d; index=(3*i+5)&15; }
        else { f=c^(b|~d); index=(7*i)&15; }
        U32 v=a+f+constants[i]+words[index];
        unsigned shift=shifts[(i/16)*4+i%4];
        a=d; d=c; c=b; b+=(v<<shift)|(v>>(32-shift));
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
}
}
extern "C" void __cdecl ttp_digest_raw(const void* input, unsigned size, void* output) {
    if (!output || (!input && size)) return;
    U32 h[]{0x67452301,0xefcdab89,0x98badcfe,0x10325476};
    const auto* bytes=static_cast<const unsigned char*>(input);
    unsigned remaining=size;
    while (remaining>=64) { block(h,bytes); bytes+=64; remaining-=64; }
    unsigned char tail[128]{};
    if (remaining) std::memcpy(tail,bytes,remaining);
    tail[remaining]=0x80;
    unsigned tailSize=remaining<56 ? 64 : 128;
    std::uint64_t bits=std::uint64_t(size)*8;
    std::memcpy(tail+tailSize-8,&bits,8);
    block(h,tail);
    if (tailSize==128) block(h,tail+64);
    std::memcpy(output,h,16);
}
extern "C" char* __cdecl ttp_digest_hex(const char* input, unsigned size, char* output, int capacity) {
    if (!input || !output || capacity<=32) return nullptr;
    if (size==0xffffffff) size=static_cast<unsigned>(std::strlen(input));
    unsigned char hash[16];
    ttp_digest_raw(input,size,hash);
    constexpr char digits[]="0123456789ABCDEF";
    for (unsigned i=0;i<16;++i) { output[2*i]=digits[hash[i]>>4]; output[2*i+1]=digits[hash[i]&15]; }
    output[32]=0;
    return output;
}
