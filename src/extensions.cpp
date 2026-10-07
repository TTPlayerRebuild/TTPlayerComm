#include <ttpcomm/extensions.h>

extern "C" int __cdecl ttpcomm_query_extension(uint32_t version,
                                              TtpCommExtensionInfo* info) noexcept {
    if (!info || version != TTPCOMM_EXTENSION_ABI || info->size < sizeof(*info))
        return 0;
    const TtpCommExtensionInfo result{sizeof(*info), TTPCOMM_EXTENSION_ABI,
        0x00050700u, TTPCOMM_CAP_PARALLEL_REPLAYGAIN};
    *info = result;
    return 1;
}
