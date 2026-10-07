#pragma once
#include <stddef.h>
#include <stdint.h>

/* Fixed x86 layouts observed by original hosts. Kept separate from any backend
 * header so changing a dependency cannot silently change the published ABI. */
typedef struct TtpDecoderIo {
    const unsigned char* input;
    uint32_t input_bytes;
    uint32_t consumed_bytes;
    double* output;
    uint32_t output_capacity_bytes;
    uint32_t produced_bytes;
} TtpDecoderIo;

typedef struct TtpId3TagLayout {
    uint32_t refcount,version;
    int32_t flags,extended_flags,restrictions,options;
    uint32_t frame_count;
    void** frames;
    uint32_t padded_size;
} TtpId3TagLayout;

typedef struct TtpId3FrameLayout {
    char id[5];
    const char* description;
    uint32_t refcount;
    int32_t flags,group_id,encryption_method;
    unsigned char* encoded;
    uint32_t encoded_length,decoded_length,field_count;
    void* fields;
} TtpId3FrameLayout;

#ifdef __cplusplus
static_assert(sizeof(void*)==4,"ttpcomm's original interface is x86 only");
static_assert(sizeof(TtpDecoderIo)==24);
static_assert(sizeof(TtpId3TagLayout)==36);
static_assert(offsetof(TtpId3TagLayout,frame_count)==0x18);
static_assert(offsetof(TtpId3TagLayout,frames)==0x1c);
static_assert(sizeof(TtpId3FrameLayout)==48);
static_assert(offsetof(TtpId3FrameLayout,fields)==0x2c);
#endif
