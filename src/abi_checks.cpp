#include "ttpcomm_layouts.h"
#include "ttpcomm_objects.h"
#include <id3tag.h>
#include <zlib.h>
#include <ttpcomm/runtime_api.h>
static_assert(sizeof(id3_tag)==sizeof(TtpId3TagLayout));
static_assert(offsetof(id3_tag,nframes)==offsetof(TtpId3TagLayout,frame_count));
static_assert(offsetof(id3_tag,frames)==offsetof(TtpId3TagLayout,frames));
static_assert(sizeof(id3_frame)==sizeof(TtpId3FrameLayout));
static_assert(offsetof(id3_frame,fields)==offsetof(TtpId3FrameLayout,fields));
static_assert(sizeof(id3_field)==16);
static_assert(sizeof(id3_ucs4_t)==4 && sizeof(id3_utf16_t)==2);
static_assert(sizeof(z_stream)==56);
static_assert(sizeof(TtpCommPcmFormat)==32);
static_assert(sizeof(TtpCommInflateResult)==8);
static_assert(sizeof(TtpCommTagFrame)==24);
static_assert(sizeof(TtpCommPicture)==36);
static_assert(sizeof(TtpCommDecoded)==12);
static_assert(sizeof(TtpCommRuntimeApi)==80);
static_assert(offsetof(TtpCommRuntimeApi,inflate_buffer)==16);
static_assert(offsetof(TtpCommRuntimeApi,tag_decoded_destroy)==76);
