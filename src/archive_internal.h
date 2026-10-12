#pragma once
#include <ttpcomm/archive_api.h>
namespace ttpcomm::archive {
struct Sink {
    virtual ~Sink() = default;
    virtual HRESULT Begin(uint64_t length, uint32_t flags = 0) = 0;
    virtual HRESULT Write(const void* data, uint32_t length) = 0;
    // Only before an encrypted stream is published. A legacy ZIP password
    // encoding candidate may pass its short verifier but fail final integrity.
    virtual HRESULT RetryEncrypted() {return E_NOTIMPL;}
};
HRESULT EnumerateRar(const wchar_t*, const TtpCommArchiveOptions*, TtpCommArchiveVisitor, void*);
HRESULT ExtractRar(const wchar_t*, const wchar_t*, const TtpCommArchiveOptions*, Sink&);
HRESULT EnumerateZip(const wchar_t*, const TtpCommArchiveOptions*, TtpCommArchiveVisitor, void*);
HRESULT ExtractZip(const wchar_t*, const wchar_t*, const TtpCommArchiveOptions*, Sink&);
bool IsExtended(const wchar_t*);
void ClearExtendedCache();
HRESULT EnumerateExtended(const wchar_t*, const TtpCommArchiveOptions*, TtpCommArchiveVisitor, void*);
HRESULT ExtractExtended(const wchar_t*, const wchar_t*, const TtpCommArchiveOptions*, Sink&);
HRESULT ProcessSeven(const wchar_t*, const wchar_t*, const TtpCommArchiveOptions*, TtpCommArchiveVisitor, void*, Sink*);
BOOL QueryStreams(TtpCommArchiveStreamApi*);
}
