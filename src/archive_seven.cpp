#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include "CPP/Common/MyInitGuid.h"
#include "CPP/7zip/Archive/7z/7zHandler.h"
#include "CPP/7zip/IPassword.h"
#include "archive_internal.h"
#include "archive_group.h"
#include "archive_seven_hooks.h"
#include <ttpcomm/archive_volumes.h>
#include <algorithm>
#include <atomic>
#include <string>


namespace {
struct Context {
    TtpCommArchiveOptions options{};
    HRESULT error{S_OK}; bool asked{}, encrypted{};
    uint64_t produced{}, length{},input_bytes{},work{}; uint32_t index{};
    ttpcomm::archive::Sink* sink{};
    explicit Context(const TtpCommArchiveOptions* o) {
        if(o)options=*o;
        if(!options.dictionary_limit_kib)options.dictionary_limit_kib=256*1024;
        options.dictionary_limit_kib=std::min(options.dictionary_limit_kib,512u*1024);
        if(!options.memory_limit_bytes)options.memory_limit_bytes=16*1024*1024;
        options.memory_limit_bytes=std::min(options.memory_limit_bytes,64u*1024*1024);
        if(!options.entry_limit)options.entry_limit=100000;
        if(!options.member_limit_bytes)options.member_limit_bytes=8ULL*1024*1024*1024;
    }
    HRESULT Progress() {
        // Work on preceding solid members and packed headers is real activity
        // even before the selected member emits its first byte.
        if(options.progress && !options.progress(options.context,produced+input_bytes+work,0))
            error=HRESULT_FROM_WIN32(ERROR_CANCELLED);
        return error;
    }
};
thread_local Context* active{};
struct Scope {Context* old{active};explicit Scope(Context& c){active=&c;}~Scope(){active=old;}};
HRESULT Last(){return HRESULT_FROM_WIN32(GetLastError());}
class Input final:public IInStream {
    std::atomic<ULONG> refs_{1};ttpcomm::archive_volumes::Set& file_;Context& c_;uint64_t position_{};
public:
    Input(ttpcomm::archive_volumes::Set& f,Context& c):file_(f),c_(c){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** p) noexcept override {
        if(!p)return E_POINTER;*p=nullptr;
        if(id==IID_IUnknown || id==IID_IInStream || id==IID_ISequentialInStream){*p=static_cast<IInStream*>(this);AddRef();return S_OK;}return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override{return ++refs_;}
    ULONG STDMETHODCALLTYPE Release() noexcept override{const auto n=--refs_;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Read(void* p,UInt32 n,UInt32* read) noexcept override {
        if(read)*read=0;if(FAILED(c_.Progress()))return c_.error;DWORD got{};
        try {got=static_cast<DWORD>(std::min<uint64_t>(n,position_<file_.Length()?file_.Length()-position_:0));
            file_.Read(position_,p,got);position_+=got;c_.input_bytes+=got;if(read)*read=got;return S_OK;
        }catch(HRESULT hr){return hr;}catch(...){return E_FAIL;}
    }
    HRESULT STDMETHODCALLTYPE Seek(Int64 move,UInt32 origin,UInt64* pos) noexcept override {
        if(FAILED(c_.Progress()))return c_.error;
        if(origin>STREAM_SEEK_END)return STG_E_INVALIDFUNCTION;
        const auto base=origin==STREAM_SEEK_SET?0:origin==STREAM_SEEK_CUR?position_:file_.Length();
        if(move<0 && uint64_t(-(move+1))+1>base || move>=0 && uint64_t(move)>uint64_t(INT64_MAX)-base)return STG_E_INVALIDFUNCTION;
        position_=move<0?base-(uint64_t(-(move+1))+1):base+uint64_t(move);if(pos)*pos=position_;return S_OK;
    }
};
class Callbacks final:public IArchiveOpenCallback,public IArchiveExtractCallback,
    public ICryptoGetTextPassword,public ISequentialOutStream,public IArchiveExtractCallbackMessage2 {
    std::atomic<ULONG> refs_{1};Context& c_;
public:
    explicit Callbacks(Context& c):c_(c){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** p) noexcept override {
        if(!p)return E_POINTER;*p=nullptr;
        if(id==IID_IUnknown || id==IID_IArchiveOpenCallback)*p=static_cast<IArchiveOpenCallback*>(this);
        else if(id==IID_IArchiveExtractCallback || id==IID_IProgress)*p=static_cast<IArchiveExtractCallback*>(this);
        else if(id==IID_ICryptoGetTextPassword)*p=static_cast<ICryptoGetTextPassword*>(this);
        else if(id==IID_ISequentialOutStream)*p=static_cast<ISequentialOutStream*>(this);
        else if(id==IID_IArchiveExtractCallbackMessage2)*p=static_cast<IArchiveExtractCallbackMessage2*>(this);
        else return E_NOINTERFACE;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override{return ++refs_;}
    ULONG STDMETHODCALLTYPE Release() noexcept override{const auto n=--refs_;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE SetTotal(const UInt64*,const UInt64*) noexcept override{return c_.Progress();}
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64*,const UInt64*) noexcept override{return c_.Progress();}
    HRESULT STDMETHODCALLTYPE SetTotal(UInt64) noexcept override{return c_.Progress();}
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* n) noexcept override{if(n)c_.work=*n;return c_.Progress();}
    HRESULT STDMETHODCALLTYPE PrepareOperation(Int32) noexcept override{return c_.Progress();}
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* password) noexcept override {
        if(!password)return E_POINTER;*password=nullptr;c_.asked=true;
        if(FAILED(c_.Progress()))return c_.error;
        if(!c_.options.password)return c_.error=TTPCOMM_ARCHIVE_PASSWORD;
        *password=SysAllocString(c_.options.password);return *password?S_OK:E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetStream(UInt32 index,ISequentialOutStream** p,Int32 mode) noexcept override {
        if(!p)return E_POINTER;*p=nullptr;
        if(index==c_.index && mode==NArchive::NExtract::NAskMode::kExtract){*p=this;AddRef();}return c_.Progress();
    }
    HRESULT STDMETHODCALLTYPE Write(const void* p,UInt32 n,UInt32* written) noexcept override {
        if(written)*written=0;if(FAILED(c_.Progress()))return c_.error;
        if(n>c_.length-c_.produced)return c_.error=HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        const auto hr=c_.sink->Write(p,n);if(FAILED(hr))return c_.error=hr;
        c_.produced+=n;if(written)*written=n;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 result) noexcept override {
        if(result==NArchive::NExtract::NOperationResult::kOK)return c_.Progress();
        if(result==NArchive::NExtract::NOperationResult::kUnsupportedMethod)c_.error=TTPCOMM_ARCHIVE_UNSUPPORTED;
        else if(c_.asked)c_.error=TTPCOMM_ARCHIVE_BAD_PASSWORD;
        else c_.error=HRESULT_FROM_WIN32(ERROR_CRC);
        return c_.error;
    }
    HRESULT STDMETHODCALLTYPE ReportExtractResult(UInt32,UInt32,Int32 result) noexcept override{return SetOperationResult(result);}
};
bool Name(std::wstring& name) {
    if(name.empty() || name.size()>32767 || name.front()==L'/' || name.front()==L'\\' || name.find_first_of(L":|")!=name.npos)return false;
    std::replace(name.begin(),name.end(),L'/',L'\\');
    for(size_t a=0;a<name.size();) {const auto b=name.find(L'\\',a);const auto n=b==name.npos?name.size()-a:b-a;
        if(name.substr(a,n)==L"..")return false;if(b==name.npos)break;a=b+1;}
    return true;
}
HRESULT Result(HRESULT hr,Context& c) {
    if(FAILED(c.error))return c.error;
    if(hr==E_NOTIMPL)return TTPCOMM_ARCHIVE_UNSUPPORTED;
    if(hr==S_FALSE || hr==E_FAIL || hr==HRESULT_FROM_WIN32(ERROR_INVALID_DATA))
        return c.asked?TTPCOMM_ARCHIVE_BAD_PASSWORD:HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    return hr;
}
}
uint32_t ttpcomm_seven_entry_limit(){return active->options.entry_limit;}
HRESULT ttpcomm_seven_header_limit(uint64_t size){
    return size>active->options.memory_limit_bytes?active->error=E_OUTOFMEMORY:active->Progress();
}
bool ttpcomm_seven_canceled(){return FAILED(active->Progress());}
uint64_t ttpcomm_seven_codec_memory_limit(){return uint64_t(active->options.dictionary_limit_kib)*1024;}
HRESULT ttpcomm_seven_codec_memory_error(){return active->error=TTPCOMM_ARCHIVE_DICTIONARY_LIMIT;}
HRESULT ttpcomm_seven_dictionary(uint64_t method,const unsigned char* p,unsigned n){
    uint64_t bytes=0;
    const auto u32=[](const unsigned char* b){return uint32_t(b[0])|(uint32_t(b[1])<<8)|(uint32_t(b[2])<<16)|(uint32_t(b[3])<<24);};
    if(method==0x30101 && n>=5)bytes=u32(p+1);
    if(method==0x21 && n==1){if(p[0]>40)return TTPCOMM_ARCHIVE_UNSUPPORTED;bytes=p[0]==40?UINT32_MAX:uint64_t(2|(p[0]&1))<<(p[0]/2+11);}
    if(method==0x30401 && n>=5)bytes=u32(p+1);
    if(bytes>uint64_t(active->options.dictionary_limit_kib)*1024)return active->error=TTPCOMM_ARCHIVE_DICTIONARY_LIMIT;
    return active->Progress();
}
namespace ttpcomm::archive {
HRESULT ProcessSeven(const wchar_t* path,const wchar_t* member,const TtpCommArchiveOptions* options,
                     TtpCommArchiveVisitor visitor,void* opaque,Sink* sink) {
    try {
        Context c(options);Scope scope(c);c.sink=sink;
        archive_volumes::Set file(path,[&]{return FAILED(c.Progress());},VolumeResolver());
        CMyComPtr<IInStream> input;input.Attach(new Input(file,c));
        CMyComPtr<IInArchive> archive;archive.Attach(new NArchive::N7z::CHandler());
        // SDK COM objects start with zero references.
        archive->AddRef();
        CMyComPtr<Callbacks> callbacks;callbacks.Attach(new Callbacks(c));
        UInt64 start=0;HRESULT hr=archive->Open(input,&start,callbacks);
        if(hr!=S_OK)return Result(hr,c);
        UInt32 count{};hr=archive->GetNumberOfItems(&count);if(FAILED(hr))return hr;
        if(count>c.options.entry_limit)return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
        std::wstring wanted=member?member:L"";std::replace(wanted.begin(),wanted.end(),L'/',L'\\');
        for(UInt32 i=0;i<count;++i) {
            if(FAILED(c.Progress()))return c.error;
            NWindows::NCOM::CPropVariant value;
            hr=archive->GetProperty(i,kpidPath,&value);if(FAILED(hr))return hr;
            if(value.vt!=VT_BSTR || !value.bstrVal)continue;
            std::wstring name(value.bstrVal,SysStringLen(value.bstrVal));if(!Name(name))continue;
            TtpCommArchiveEntry item{};item.size=sizeof(item);item.index=i;item.name=name.c_str();
            const auto number=[&](PROPID id)->uint64_t{value.Clear();if(FAILED(archive->GetProperty(i,id,&value)))return 0;
                return value.vt==VT_UI8?value.uhVal.QuadPart:value.vt==VT_UI4?value.ulVal:value.vt==VT_BOOL && value.boolVal!=VARIANT_FALSE;};
            item.unpacked_bytes=number(kpidSize);item.packed_bytes=number(kpidPackSize);
            if(number(kpidIsDir))item.flags|=TTPCOMM_ARCHIVE_DIRECTORY;
            if(number(kpidEncrypted))item.flags|=TTPCOMM_ARCHIVE_ENCRYPTED;
            if(file.Split())item.flags|=TTPCOMM_ARCHIVE_SPLIT;
            if(number(kpidIsAnti))item.flags|=TTPCOMM_ARCHIVE_LINK;
            value.Clear();archive->GetProperty(i,kpidSymLink,&value);if(value.vt==VT_BSTR)item.flags|=TTPCOMM_ARCHIVE_LINK;
            if(!sink){if(!visitor(opaque,&item))return HRESULT_FROM_WIN32(ERROR_CANCELLED);continue;}
            if(_wcsicmp(wanted.c_str(),name.c_str()))continue;
            if(item.flags&(TTPCOMM_ARCHIVE_DIRECTORY|TTPCOMM_ARCHIVE_LINK))return TTPCOMM_ARCHIVE_UNSUPPORTED;
            if(item.unpacked_bytes>c.options.member_limit_bytes)return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            c.index=i;c.length=item.unpacked_bytes;c.encrypted=(item.flags&TTPCOMM_ARCHIVE_ENCRYPTED)!=0;
            if(c.encrypted && !c.options.password)return TTPCOMM_ARCHIVE_PASSWORD;
            hr=sink->Begin(c.length,item.flags);if(FAILED(hr))return hr;
            hr=archive->Extract(&i,1,0,callbacks);hr=Result(hr,c);
            if(FAILED(hr))return hr;
            return c.produced==c.length?S_OK:STG_E_READFAULT;
        }
        return sink?HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND):S_OK;
    }catch(HRESULT hr){return hr;}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
}
}
