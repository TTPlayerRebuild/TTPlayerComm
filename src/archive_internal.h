#pragma once
#include <ttpcomm/archive_api.h>
namespace ttpcomm::archive {
struct Sink {
    virtual ~Sink() = default;
    virtual HRESULT Begin(uint64_t length, uint32_t flags = 0) = 0;
    virtual HRESULT Write(const void* data, uint32_t length) = 0;
};
HRESULT EnumerateRar(const wchar_t*, const TtpCommArchiveOptions*, TtpCommArchiveVisitor, void*);
HRESULT ExtractRar(const wchar_t*, const wchar_t*, const TtpCommArchiveOptions*, Sink&);
HRESULT EnumerateZip(const wchar_t*, const TtpCommArchiveOptions*, TtpCommArchiveVisitor, void*);
HRESULT ExtractZip(const wchar_t*, const wchar_t*, const TtpCommArchiveOptions*, Sink&);
BOOL QueryStreams(TtpCommArchiveStreamApi*);
}
