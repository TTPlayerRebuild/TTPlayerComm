#include "ttpcomm_layouts.h"
#include "ttpcomm_objects.h"
#include <id3tag.h>
#include <zlib.h>
#include <ttpcomm/runtime_api.h>
#include <ttpcomm/audio_api.h>
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
static_assert(sizeof(TtpCommAudioEncoderConfig)==44);
static_assert(sizeof(TtpCommAudioEncodeResult)==24);
static_assert(sizeof(TtpCommAudioEffectsConfig)==112);
static_assert(sizeof(TtpCommAudioEffectsInfo)==32);
static_assert(sizeof(TtpCommAudioProcessResult)==16);
static_assert(sizeof(TtpCommAudioApi)==56);
static_assert(offsetof(TtpCommAudioApi,encoder_create)==16);
static_assert(offsetof(TtpCommAudioApi,effects_reset)==52);
