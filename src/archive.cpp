#include <ttpcomm/archive_api.h>
#include <dll.hpp>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
constexpr uint64_t MiB = 1024ULL * 1024;
HRESULT LastError() { const DWORD e=GetLastError(); return HRESULT_FROM_WIN32(e ? e : ERROR_READ_FAULT); }
HRESULT Cancelled() { return HRESULT_FROM_WIN32(ERROR_CANCELLED); }
HRESULT RarResult(int code) {
    switch(code) {
    case ERAR_SUCCESS: return S_OK;
    case ERAR_END_ARCHIVE: return S_FALSE;
    case ERAR_NO_MEMORY: return E_OUTOFMEMORY;
    case ERAR_BAD_DATA: return HRESULT_FROM_WIN32(ERROR_CRC);
    case ERAR_BAD_ARCHIVE: return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    case ERAR_UNKNOWN_FORMAT: return TTPCOMM_ARCHIVE_UNSUPPORTED;
    case ERAR_MISSING_PASSWORD: return TTPCOMM_ARCHIVE_PASSWORD;
    case ERAR_BAD_PASSWORD: return TTPCOMM_ARCHIVE_BAD_PASSWORD;
    case ERAR_LARGE_DICT: return TTPCOMM_ARCHIVE_DICTIONARY_LIMIT;
    case ERAR_EOPEN: return HRESULT_FROM_WIN32(ERROR_OPEN_FAILED);
    case ERAR_EREFERENCE: return TTPCOMM_ARCHIVE_UNSUPPORTED;
    default: return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
    }
}

struct Storage {
    std::vector<unsigned char> memory;
    HANDLE file{INVALID_HANDLE_VALUE};
    uint64_t length{};
    std::mutex mutex;
    ~Storage() { if(file!=INVALID_HANDLE_VALUE) CloseHandle(file); }
    HRESULT Prepare(uint64_t size, uint32_t memory_limit) {
        if(size<=memory_limit) { memory.reserve(static_cast<size_t>(size)); return S_OK; }
        wchar_t temp[MAX_PATH+1]{};
        const DWORD n=GetTempPathW(MAX_PATH,temp);
        if(!n || n>=MAX_PATH) return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
        GUID id{};
        HRESULT hr=CoCreateGuid(&id); if(FAILED(hr)) return hr;
        wchar_t guid[40]{}; StringFromGUID2(id,guid,40);
        const std::wstring path=std::wstring(temp)+L"ttpcomm-member-"+guid+L".tmp";
        file=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        return file==INVALID_HANDLE_VALUE ? LastError() : S_OK;
    }
    HRESULT Append(const void* bytes, uint32_t count) {
        if(file==INVALID_HANDLE_VALUE) {
            const auto* p=static_cast<const unsigned char*>(bytes);
            memory.insert(memory.end(),p,p+count);
        } else {
            DWORD written{};
            if(!WriteFile(file,bytes,count,&written,nullptr)) return LastError();
            if(written!=count) return STG_E_WRITEFAULT;
        }
        length+=count; return S_OK;
    }
};

class MemberStream final : public IStream {
    std::atomic<ULONG> references_{1};
    std::shared_ptr<Storage> storage_;
    uint64_t position_{};
public:
    explicit MemberStream(std::shared_ptr<Storage> storage, uint64_t position=0)
        : storage_(std::move(storage)),position_(position) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out) return E_POINTER; *out=nullptr;
        if(id!=IID_IUnknown && id!=IID_ISequentialStream && id!=IID_IStream) return E_NOINTERFACE;
        *out=static_cast<IStream*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG left=--references_; if(!left) delete this; return left; }
    HRESULT STDMETHODCALLTYPE Read(void* buffer,ULONG count,ULONG* read) override {
        if(read) *read=0; if(!buffer && count) return STG_E_INVALIDPOINTER;
        std::lock_guard<std::mutex> lock(storage_->mutex);
        const ULONG available=position_<storage_->length ? static_cast<ULONG>(
            std::min<uint64_t>(count,storage_->length-position_)) : 0;
        if(available) {
            if(storage_->file==INVALID_HANDLE_VALUE) {
                memcpy(buffer,storage_->memory.data()+static_cast<size_t>(position_),available);
            } else {
                LARGE_INTEGER offset{}; offset.QuadPart=static_cast<LONGLONG>(position_);
                DWORD actual{};
                if(!SetFilePointerEx(storage_->file,offset,nullptr,FILE_BEGIN) ||
                   !ReadFile(storage_->file,buffer,available,&actual,nullptr)) return LastError();
                if(actual!=available) return STG_E_READFAULT;
            }
        }
        position_+=available; if(read) *read=available;
        return available==count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move,DWORD origin,ULARGE_INTEGER* result) override {
        std::lock_guard<std::mutex> lock(storage_->mutex);
        const uint64_t base=origin==STREAM_SEEK_SET ? 0 : origin==STREAM_SEEK_CUR ? position_ : storage_->length;
        if(origin>STREAM_SEEK_END) return STG_E_INVALIDFUNCTION;
        if(move.QuadPart<0) {
            const uint64_t distance=uint64_t(-(move.QuadPart+1))+1;
            if(distance>base) return STG_E_INVALIDFUNCTION;
            position_=base-distance;
        } else {
            if(uint64_t(move.QuadPart)>uint64_t(INT64_MAX)-base) return STG_E_INVALIDFUNCTION;
            position_=base+uint64_t(move.QuadPart);
        }
        if(result) result->QuadPart=position_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*,ULONG,ULONG* n) override { if(n) *n=0; return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* target,ULARGE_INTEGER count,ULARGE_INTEGER* read,ULARGE_INTEGER* written) override {
        if(read) read->QuadPart=0; if(written) written->QuadPart=0;
        if(!target) return STG_E_INVALIDPOINTER;
        unsigned char buffer[16384]; uint64_t remaining=count.QuadPart;
        while(remaining) {
            ULONG n{},w{};
            HRESULT hr=Read(buffer,static_cast<ULONG>(std::min<uint64_t>(remaining,sizeof(buffer))),&n);
            if(FAILED(hr)) return hr;
            if(read) read->QuadPart+=n;
            if(n) { HRESULT whr=target->Write(buffer,n,&w); if(written) written->QuadPart+=w;
                if(FAILED(whr)) return whr; if(w!=n) return STG_E_WRITEFAULT; }
            if(hr==S_FALSE) return S_FALSE;
            remaining-=n;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER,ULARGE_INTEGER,DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER,ULARGE_INTEGER,DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat,DWORD flags) override {
        if(!stat) return STG_E_INVALIDPOINTER;
        if(flags!=STATFLAG_DEFAULT && flags!=STATFLAG_NONAME) return STG_E_INVALIDFLAG;
        *stat={}; stat->type=STGTY_STREAM; stat->cbSize.QuadPart=storage_->length;
        stat->grfMode=STGM_READ; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** out) override {
        if(!out) return E_POINTER; *out=nullptr;
        try { std::lock_guard<std::mutex> lock(storage_->mutex);
            *out=new MemberStream(storage_,position_); return S_OK; }
        catch(...) { return E_OUTOFMEMORY; }
    }
};

struct Operation {
    TtpCommArchiveOptions options{};
    Storage* output{};
    uint64_t expected{};
    HRESULT error{S_OK};
    explicit Operation(const TtpCommArchiveOptions* input) {
        if(input) options=*input;
        if(!options.dictionary_limit_kib) options.dictionary_limit_kib=256*1024;
        options.dictionary_limit_kib=std::min(options.dictionary_limit_kib,512u*1024);
        if(!options.member_limit_bytes) options.member_limit_bytes=8*1024*MiB;
        if(!options.memory_limit_bytes) options.memory_limit_bytes=16*static_cast<uint32_t>(MiB);
        options.memory_limit_bytes=std::min(options.memory_limit_bytes,64*static_cast<uint32_t>(MiB));
        if(!options.entry_limit) options.entry_limit=100000;
    }
    bool Progress() {
        if(FAILED(error)) return false;
        if(options.progress && !options.progress(options.context,output ? output->length : 0,expected)) error=Cancelled();
        return SUCCEEDED(error);
    }
    static int CALLBACK Callback(UINT message,LPARAM user,LPARAM p1,LPARAM p2) {
        auto& op=*reinterpret_cast<Operation*>(user);
        try {
            if(!op.Progress()) return -1;
            if(message==UCM_PROCESSDATA && op.output) {
                const uint32_t count=static_cast<uint32_t>(p2);
                if(count>op.expected-op.output->length) op.error=HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                else op.error=op.output->Append(reinterpret_cast<const void*>(p1),count);
                return op.Progress() ? 1 : -1;
            }
            if(message==UCM_NEEDPASSWORDW) {
                if(!op.options.password || !*op.options.password) op.error=TTPCOMM_ARCHIVE_PASSWORD;
                else if(wcslen(op.options.password)>=static_cast<size_t>(p2)) op.error=TTPCOMM_ARCHIVE_BAD_PASSWORD;
                else { wcscpy_s(reinterpret_cast<wchar_t*>(p1),static_cast<size_t>(p2),op.options.password); return 1; }
                return -1;
            }
            if(message==UCM_NEEDPASSWORD) { op.error=TTPCOMM_ARCHIVE_PASSWORD; return -1; }
            if((message==UCM_CHANGEVOLUME || message==UCM_CHANGEVOLUMEW) && p2==RAR_VOL_ASK) {
                op.error=TTPCOMM_ARCHIVE_MISSING_VOLUME; return -1;
            }
            if(message==UCM_LARGEDICT) { op.error=TTPCOMM_ARCHIVE_DICTIONARY_LIMIT; return -1; }
            return 1;
        } catch(...) { op.error=E_OUTOFMEMORY; return -1; }
    }
    HRESULT Result(int result) const { return FAILED(error) ? error : RarResult(result); }
};

// Upstream has a process-global ErrorHandler. Serialize calls, including Close,
// while allowing waiting operations to cancel. No lock is held by returned streams.
std::mutex engine_mutex;
bool Acquire(std::unique_lock<std::mutex>& lock,Operation& op) {
    while(!lock.try_lock()) { if(!op.Progress()) return false; Sleep(10); }
    return op.Progress();
}
struct RarHandle {
    HANDLE value{};
    ~RarHandle() { if(value) RARCloseArchive(value); }
    HRESULT Open(const wchar_t* path,unsigned mode,Operation& op) {
        RAROpenArchiveDataEx data{};
        data.ArcNameW=const_cast<wchar_t*>(path); data.OpenMode=mode;
        data.Callback=Operation::Callback; data.UserData=reinterpret_cast<LPARAM>(&op);
        value=RAROpenArchiveEx(&data);
        return value ? S_OK : data.OpenResult ? op.Result(data.OpenResult) : E_UNEXPECTED;
    }
};
bool SafeMember(const std::wstring& name) {
    if(name.empty() || name.front()==L'\\' || name.front()==L'/' || name.find(L':')!=std::wstring::npos) return false;
    size_t pos=0;
    while(pos<name.size()) {
        const size_t end=name.find_first_of(L"\\/",pos);
        const auto part=name.substr(pos,end==std::wstring::npos ? end : end-pos);
        if(part==L".." || part==L".") return false;
        if(end==std::wstring::npos) break;
        pos=end+1;
    }
    return true;
}
struct Header {
    RARHeaderDataEx data{};
    std::vector<wchar_t> name=std::vector<wchar_t>(32768);
    int Read(HANDLE archive) {
        data={}; data.FileNameEx=name.data(); data.FileNameExSize=static_cast<unsigned>(name.size());
        return RARReadHeaderEx(archive,&data);
    }
    uint64_t Size() const { return (uint64_t(data.UnpSizeHigh)<<32)|data.UnpSize; }
    std::wstring Name() const { std::wstring result(name.data()); std::replace(result.begin(),result.end(),L'/',L'\\'); return result; }
};

HRESULT __cdecl Enumerate(const wchar_t* path,const TtpCommArchiveOptions* options,TtpCommArchiveVisitor visitor,void* context) {
    if(!path || !*path || !visitor || (options && options->size!=sizeof(*options))) return E_INVALIDARG;
    try {
        Operation op(options); std::unique_lock<std::mutex> lock(engine_mutex,std::defer_lock);
        if(!Acquire(lock,op)) return op.error;
        RarHandle archive; HRESULT hr=archive.Open(path,RAR_OM_LIST,op); if(FAILED(hr)) return hr;
        Header header; uint32_t index=0;
        for(;;) {
            if(!op.Progress()) return op.error;
            const int code=header.Read(archive.value);
            if(code==ERAR_END_ARCHIVE) return op.Result(ERAR_SUCCESS);
            if(code) return op.Result(code);
            if(index>=op.options.entry_limit) return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
            const auto name=header.Name();
            TtpCommArchiveEntry entry{}; entry.size=sizeof(entry); entry.index=index++;
            entry.name=name.c_str(); entry.unpacked_bytes=header.Size();
            entry.packed_bytes=(uint64_t(header.data.PackSizeHigh)<<32)|header.data.PackSize;
            entry.dictionary_kib=header.data.DictSize;
            if(header.data.Flags&RHDF_DIRECTORY) entry.flags|=TTPCOMM_ARCHIVE_DIRECTORY;
            if(header.data.Flags&RHDF_ENCRYPTED) entry.flags|=TTPCOMM_ARCHIVE_ENCRYPTED;
            if(header.data.Flags&RHDF_SOLID) entry.flags|=TTPCOMM_ARCHIVE_SOLID;
            if(header.data.Flags&(RHDF_SPLITBEFORE|RHDF_SPLITAFTER)) entry.flags|=TTPCOMM_ARCHIVE_SPLIT;
            if(header.data.RedirType || !SafeMember(name)) entry.flags|=TTPCOMM_ARCHIVE_LINK;
            if(!visitor(context,&entry)) return Cancelled();
            hr=op.Result(RARProcessFileW(archive.value,RAR_SKIP,nullptr,nullptr)); if(FAILED(hr)) return hr;
        }
    } catch(const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch(...) { return HRESULT_FROM_WIN32(ERROR_INVALID_DATA); }
}

HRESULT __cdecl OpenMember(const wchar_t* path,const wchar_t* member,const TtpCommArchiveOptions* options,IStream** out) {
    if(!out) return E_POINTER; *out=nullptr;
    if(!path || !*path || !member || !*member || (options && options->size!=sizeof(*options))) return E_INVALIDARG;
    try {
        std::wstring requested(member); std::replace(requested.begin(),requested.end(),L'/',L'\\');
        if(!SafeMember(requested)) return E_INVALIDARG;
        Operation op(options); std::unique_lock<std::mutex> lock(engine_mutex,std::defer_lock);
        if(!Acquire(lock,op)) return op.error;
        RarHandle archive; HRESULT hr=archive.Open(path,RAR_OM_EXTRACT,op); if(FAILED(hr)) return hr;
        Header header; uint32_t index=0;
        for(;;) {
            if(!op.Progress()) return op.error;
            const int code=header.Read(archive.value);
            if(code==ERAR_END_ARCHIVE) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            if(code) return op.Result(code);
            if(index++>=op.options.entry_limit) return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
            // Solid predecessors can allocate a dictionary even for RAR_SKIP.
            if(header.data.DictSize>op.options.dictionary_limit_kib) return TTPCOMM_ARCHIVE_DICTIONARY_LIMIT;
            if(_wcsicmp(requested.c_str(),header.Name().c_str())==0) {
                if(header.data.RedirType || (header.data.Flags&RHDF_DIRECTORY)) return TTPCOMM_ARCHIVE_UNSUPPORTED;
                op.expected=header.Size();
                if(op.expected>op.options.member_limit_bytes || op.expected>uint64_t(INT64_MAX)) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
                auto storage=std::make_shared<Storage>();
                hr=storage->Prepare(op.expected,op.options.memory_limit_bytes); if(FAILED(hr)) return hr;
                op.output=storage.get();
                hr=op.Result(RARProcessFileW(archive.value,RAR_TEST,nullptr,nullptr));
                op.output=nullptr;
                if(FAILED(hr)) return hr;
                if(storage->length!=op.expected) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                *out=new MemberStream(std::move(storage)); return S_OK;
            }
            hr=op.Result(RARProcessFileW(archive.value,RAR_SKIP,nullptr,nullptr)); if(FAILED(hr)) return hr;
        }
    } catch(const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch(...) { return HRESULT_FROM_WIN32(ERROR_INVALID_DATA); }
}
}

extern "C" BOOL __cdecl ttpcomm_query_archive(uint32_t abi,TtpCommArchiveApi* api) {
    if(!api || api->size<sizeof(*api) || abi!=TTPCOMM_ARCHIVE_ABI) return FALSE;
    *api={sizeof(*api),TTPCOMM_ARCHIVE_ABI,TTPCOMM_ARCHIVE_REQUIRED,0,Enumerate,OpenMember};
    return TRUE;
}
