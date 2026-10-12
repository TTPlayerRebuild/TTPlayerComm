#include "archive_internal.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ttpcomm::archive {
namespace {
HRESULT Cancelled() { return HRESULT_FROM_WIN32(ERROR_CANCELLED); }
HRESULT LastError() { const auto e=GetLastError();return HRESULT_FROM_WIN32(e ? e : ERROR_READ_FAULT); }
bool Zip(const wchar_t* path) { const auto dot=wcsrchr(path,L'.');return dot && !_wcsicmp(dot,L".zip"); }
bool Rar(const wchar_t* path) { const auto dot=wcsrchr(path,L'.');return dot && !_wcsicmp(dot,L".rar"); }
struct FileIdentity {
    HANDLE handle{INVALID_HANDLE_VALUE};std::wstring path,key;
    ~FileIdentity(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
    HRESULT Open(const wchar_t* input) {
        const DWORD n=GetFullPathNameW(input,0,nullptr,nullptr);if(!n)return LastError();
        std::vector<wchar_t> buffer(n);if(!GetFullPathNameW(input,n,buffer.data(),nullptr))return LastError();
        path=buffer.data();
        handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE)return LastError();
        BY_HANDLE_FILE_INFORMATION i{};if(!GetFileInformationByHandle(handle,&i))return LastError();
        key=path;
        for(DWORD v:{i.dwVolumeSerialNumber,i.nFileIndexHigh,i.nFileIndexLow,i.nFileSizeHigh,i.nFileSizeLow,
                     i.ftLastWriteTime.dwHighDateTime,i.ftLastWriteTime.dwLowDateTime})key+=L"|"+std::to_wstring(v);
        return S_OK;
    }
};
std::atomic<uint64_t> sequence{0};
struct Task final : Sink {
    std::wstring path,member,key,password;
    TtpCommArchiveOptions limits{};
    const uint64_t id=++sequence;
    std::mutex mutex,join_mutex;std::condition_variable changed;
    std::vector<unsigned char> memory;HANDLE file{INVALID_HANDLE_VALUE},identity{INVALID_HANDLE_VALUE};
    uint64_t total{},produced{};uint32_t users{};
    bool ready{},complete{},cacheable{true},retained{};HRESULT result{E_PENDING};
    std::atomic<bool> cancel{false};std::thread worker;
    Task(FileIdentity& input,const wchar_t* name,const TtpCommArchiveOptions& options)
        :path(input.path),member(name),key(input.key),limits(options) {
        if(options.password)password=options.password;
        limits.password=nullptr;limits.progress=nullptr;limits.context=nullptr;
        identity=input.handle;input.handle=INVALID_HANDLE_VALUE;
        if(!limits.member_limit_bytes)limits.member_limit_bytes=8ULL*1024*1024*1024;
        if(!limits.dictionary_limit_kib)limits.dictionary_limit_kib=256*1024;
        limits.dictionary_limit_kib=std::min(limits.dictionary_limit_kib,512u*1024);
        if(!limits.memory_limit_bytes)limits.memory_limit_bytes=16*1024*1024;
        limits.memory_limit_bytes=std::min(limits.memory_limit_bytes,64u*1024*1024);
        if(!limits.entry_limit)limits.entry_limit=100000;
    }
    ~Task(){cancel=true;changed.notify_all();Join();if(identity!=INVALID_HANDLE_VALUE)CloseHandle(identity);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
    void Join(){std::lock_guard<std::mutex> guard(join_mutex);if(worker.joinable())worker.join();}
    void Start(){worker=std::thread([this]{Run();});}
    void Run() noexcept {
        HRESULT hr=E_FAIL;
        try {
            auto options=limits;options.size=sizeof(options);options.password=password.empty()?nullptr:password.c_str();
            options.progress=[](void* p,uint64_t,uint64_t)->BOOL{return !static_cast<Task*>(p)->cancel.load();};options.context=this;
            hr=Zip(path.c_str()) ? ExtractZip(path.c_str(),member.c_str(),&options,*this)
                                : ExtractRar(path.c_str(),member.c_str(),&options,*this);
        } catch(const std::bad_alloc&){hr=E_OUTOFMEMORY;}catch(...){hr=E_FAIL;}
        // A completed cache holds bytes, not a deny-write handle on the archive.
        if(identity!=INVALID_HANDLE_VALUE){CloseHandle(identity);identity=INVALID_HANDLE_VALUE;}
        {std::lock_guard<std::mutex> lock(mutex);result=hr;complete=true;}
        changed.notify_all();
    }
    HRESULT Begin(uint64_t length,uint32_t flags) override {
        std::lock_guard<std::mutex> lock(mutex);if(cancel)return Cancelled();
        if(length>limits.member_limit_bytes || length>uint64_t(INT64_MAX))return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        total=length;cacheable=!(flags&TTPCOMM_ARCHIVE_SPLIT); // Other volumes have independent identities.
        retained=retained && cacheable;
        if(length<=limits.memory_limit_bytes)memory.reserve(static_cast<size_t>(length));
        else {
            wchar_t temp[MAX_PATH+1]{},guid[40]{};GUID id{};
            const auto n=GetTempPathW(MAX_PATH,temp);if(!n || n>=MAX_PATH)return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
            HRESULT hr=CoCreateGuid(&id);if(FAILED(hr))return hr;StringFromGUID2(id,guid,40);
            const std::wstring name=std::wstring(temp)+L"ttpcomm-member-"+guid+L".tmp";
            file=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,
                             FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
            if(file==INVALID_HANDLE_VALUE)return LastError();
        }
        ready=true;changed.notify_all();return S_OK;
    }
    HRESULT Write(const void* bytes,uint32_t size) override {
        std::lock_guard<std::mutex> lock(mutex);if(cancel)return Cancelled();
        if(produced>total || size>total-produced)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        if(file==INVALID_HANDLE_VALUE) {
            const auto* p=static_cast<const unsigned char*>(bytes);memory.insert(memory.end(),p,p+size);
        } else {
            LARGE_INTEGER pos{};pos.QuadPart=static_cast<LONGLONG>(produced);DWORD written{};
            if(!SetFilePointerEx(file,pos,nullptr,FILE_BEGIN) || !WriteFile(file,bytes,size,&written,nullptr))return LastError();
            if(written!=size)return STG_E_WRITEFAULT;
        }
        produced+=size;changed.notify_all();return S_OK;
    }
    bool AddUser() {
        std::lock_guard<std::mutex> lock(mutex);
        if(cancel || (complete && FAILED(result)))return false;
        ++users;return true;
    }
    void DropUser(bool requested=false) {
        bool join{};
        {std::lock_guard<std::mutex> lock(mutex);
            if(!--users){if(!complete && (requested || !retained))cancel=true;join=complete || cancel;}}
        if(join){changed.notify_all();Join();}
    }
    void Unretain() {
        std::lock_guard<std::mutex> lock(mutex);retained=false;
        if(!users && !complete)cancel=true;
        changed.notify_all();
    }
    bool Matches(const FileIdentity& input,const wchar_t* name,const TtpCommArchiveOptions& options) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto limit=options.member_limit_bytes?options.member_limit_bytes:8ULL*1024*1024*1024;
        const auto dict=std::min(options.dictionary_limit_kib?options.dictionary_limit_kib:256u*1024,512u*1024);
        const auto mem=std::min(options.memory_limit_bytes?options.memory_limit_bytes:16u*1024*1024,64u*1024*1024);
        const auto entries=options.entry_limit?options.entry_limit:100000u;
        return cacheable && !cancel && (!complete || SUCCEEDED(result)) && password.empty() &&
            (!options.password || !*options.password) && !_wcsicmp(key.c_str(),input.key.c_str()) &&
            !_wcsicmp(member.c_str(),name) && limits.member_limit_bytes==limit &&
            limits.dictionary_limit_kib==dict && limits.memory_limit_bytes==mem && limits.entry_limit==entries;
    }
};
std::mutex cache_mutex;
std::shared_ptr<Task> current;
// Private identity lets status reject unrelated IStreams without RTTI casts.
const IID StatusId={0x4d4a30fe,0x8b93,0x44d7,{0xa6,0x72,0x73,0x4d,0x6f,0x4b,0xc5,0x36}};
struct StatusInterface : IUnknown { virtual HRESULT STDMETHODCALLTYPE Snapshot(TtpCommArchiveStreamStatus*)=0; };
class Stream final : public IStream,public StatusInterface {
    std::atomic<ULONG> refs_{1};std::shared_ptr<Task> task_;
    TtpCommArchiveStreamOptions options_{};uint64_t position_{};std::mutex cursor_;
    std::atomic<bool> aborted_{false};
    bool Canceled() const {return options_.canceled && options_.canceled(options_.context);}
    HRESULT Wait(std::unique_lock<std::mutex>& lock,uint64_t wanted,bool only_header) {
        const DWORD start=GetTickCount(),timeout=options_.timeout_ms?options_.timeout_ms:30000;
        for(;;) {
            lock.unlock();bool canceled{};try{canceled=Canceled();}catch(...){canceled=true;}lock.lock();
            if(canceled){aborted_=true;return Cancelled();}
            if(task_->complete && FAILED(task_->result))return task_->result;
            if(task_->ready && (only_header || (task_->produced>=wanted && (wanted<task_->total || task_->complete))))return S_OK;
            if(task_->complete)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            if(GetTickCount()-start>=timeout){aborted_=true;return HRESULT_FROM_WIN32(ERROR_TIMEOUT);}
            task_->changed.wait_for(lock,std::chrono::milliseconds(25));
        }
    }
public:
    Stream(std::shared_ptr<Task> task,const TtpCommArchiveStreamOptions& options,uint64_t position=0)
        :task_(std::move(task)),options_(options),position_(position) {}
    ~Stream(){bool stop=aborted_;try{stop=stop || Canceled();}catch(...){stop=true;}task_->DropUser(stop);}
    HRESULT Initialize(){std::unique_lock<std::mutex> lock(task_->mutex);return Wait(lock,0,true);}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(id==IID_IUnknown || id==IID_IStream || id==IID_ISequentialStream)*out=static_cast<IStream*>(this);
        else if(id==StatusId)*out=static_cast<StatusInterface*>(this);else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs_;}
    ULONG STDMETHODCALLTYPE Release() override{const auto n=--refs_;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Read(void* data,ULONG count,ULONG* read) override {
        if(read)*read=0;if(!data && count)return STG_E_INVALIDPOINTER;
        std::lock_guard<std::mutex> cursor(cursor_);std::unique_lock<std::mutex> lock(task_->mutex);
        HRESULT hr=Wait(lock,0,true);if(FAILED(hr))return hr;
        if(!count)return S_OK;
        const auto n=static_cast<ULONG>(position_<task_->total?std::min<uint64_t>(count,task_->total-position_):0);
        hr=Wait(lock,position_<task_->total?position_+n:task_->total,false);if(FAILED(hr))return hr;
        if(n) {
            if(task_->file==INVALID_HANDLE_VALUE)memcpy(data,task_->memory.data()+static_cast<size_t>(position_),n);
            else {LARGE_INTEGER pos{};pos.QuadPart=static_cast<LONGLONG>(position_);DWORD got{};
                if(!SetFilePointerEx(task_->file,pos,nullptr,FILE_BEGIN) || !ReadFile(task_->file,data,n,&got,nullptr))return LastError();
                if(got!=n)return STG_E_READFAULT;}
        }
        position_+=n;if(read)*read=n;return n==count?S_OK:S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move,DWORD origin,ULARGE_INTEGER* out) override {
        std::lock_guard<std::mutex> cursor(cursor_);std::unique_lock<std::mutex> lock(task_->mutex);
        HRESULT hr=Wait(lock,0,true);if(FAILED(hr))return hr;
        if(origin>STREAM_SEEK_END)return STG_E_INVALIDFUNCTION;
        const auto base=origin==STREAM_SEEK_SET?0:origin==STREAM_SEEK_CUR?position_:task_->total;
        if(move.QuadPart<0){const auto distance=uint64_t(-(move.QuadPart+1))+1;if(distance>base)return STG_E_INVALIDFUNCTION;position_=base-distance;}
        else {if(uint64_t(move.QuadPart)>uint64_t(INT64_MAX)-base)return STG_E_INVALIDFUNCTION;position_=base+uint64_t(move.QuadPart);}
        if(out)out->QuadPart=position_;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* out,DWORD flags) override {
        if(!out)return E_POINTER;if(flags!=STATFLAG_DEFAULT && flags!=STATFLAG_NONAME)return STG_E_INVALIDFLAG;
        std::unique_lock<std::mutex> lock(task_->mutex);const HRESULT hr=Wait(lock,0,true);if(FAILED(hr))return hr;
        *out={};out->type=STGTY_STREAM;out->cbSize.QuadPart=task_->total;out->grfMode=STGM_READ;
        if(flags==STATFLAG_DEFAULT){const auto name=task_->path+L"|"+task_->member;
            out->pwcsName=static_cast<wchar_t*>(CoTaskMemAlloc((name.size()+1)*sizeof(wchar_t)));
            if(!out->pwcsName)return E_OUTOFMEMORY;memcpy(out->pwcsName,name.c_str(),(name.size()+1)*sizeof(wchar_t));}
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** out) override {
        if(!out)return E_POINTER;*out=nullptr;std::lock_guard<std::mutex> cursor(cursor_);
        if(!task_->AddUser())return Cancelled();
        try{*out=new Stream(task_,options_,position_);return S_OK;}catch(...){task_->DropUser();return E_OUTOFMEMORY;}
    }
    HRESULT STDMETHODCALLTYPE Snapshot(TtpCommArchiveStreamStatus* out) override {
        if(!out || out->size<sizeof(*out))return E_INVALIDARG;std::lock_guard<std::mutex> lock(task_->mutex);
        *out={sizeof(*out),task_->complete?1u:0u,task_->produced,task_->total,task_->id,task_->result};return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*,ULONG,ULONG* n) override{if(n)*n=0;return STG_E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override{return STG_E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override{return S_OK;}
    HRESULT STDMETHODCALLTYPE Revert() override{return STG_E_INVALIDFUNCTION;}
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER,ULARGE_INTEGER,DWORD) override{return STG_E_INVALIDFUNCTION;}
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER,ULARGE_INTEGER,DWORD) override{return STG_E_INVALIDFUNCTION;}
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* target,ULARGE_INTEGER size,ULARGE_INTEGER* read,ULARGE_INTEGER* written) override {
        if(read)read->QuadPart=0;if(written)written->QuadPart=0;if(!target)return E_POINTER;
        unsigned char bytes[16384];uint64_t remaining=size.QuadPart;
        while(remaining){ULONG n{},w{};const HRESULT hr=Read(bytes,static_cast<ULONG>(std::min<uint64_t>(remaining,sizeof(bytes))),&n);
            if(FAILED(hr))return hr;if(read)read->QuadPart+=n;
            if(n){const HRESULT whr=target->Write(bytes,n,&w);if(written)written->QuadPart+=w;if(FAILED(whr))return whr;if(w!=n)return STG_E_WRITEFAULT;}
            if(hr==S_FALSE)return S_FALSE;remaining-=n;}
        return S_OK;
    }
};
HRESULT __cdecl Enumerate(const wchar_t* path,const TtpCommArchiveOptions* options,TtpCommArchiveVisitor visitor,void* context) {
    if(!path)return E_INVALIDARG;
    return Zip(path)?EnumerateZip(path,options,visitor,context):Rar(path)?EnumerateRar(path,options,visitor,context):TTPCOMM_ARCHIVE_UNSUPPORTED;
}
HRESULT __cdecl Open(const wchar_t* path,const wchar_t* member,const TtpCommArchiveStreamOptions* input,IStream** out) {
    if(!out)return E_POINTER;*out=nullptr;
    if(!path || !*path || !member || !*member || (input && input->size!=sizeof(*input)))return E_INVALIDARG;
    if(!Zip(path) && !Rar(path))return TTPCOMM_ARCHIVE_UNSUPPORTED;
    TtpCommArchiveStreamOptions options{};if(input)options=*input;
    if(options.flags&~TTPCOMM_ARCHIVE_CACHE_CURRENT)return E_INVALIDARG;
    if(options.limits.size && options.limits.size!=sizeof(options.limits))return E_INVALIDARG;
    try {
        if(options.canceled && options.canceled(options.context))return Cancelled();
        FileIdentity identity;HRESULT hr=identity.Open(path);if(FAILED(hr))return hr;
        std::wstring name(member);std::replace(name.begin(),name.end(),L'/',L'\\');
        std::shared_ptr<Task> task,retired;
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            if((options.flags&TTPCOMM_ARCHIVE_CACHE_CURRENT) && current && current->Matches(identity,name.c_str(),options.limits) && current->AddUser())task=current;
            if(!task) {
                task=std::make_shared<Task>(identity,name.c_str(),options.limits);
                task->retained=(options.flags&TTPCOMM_ARCHIVE_CACHE_CURRENT)!=0;
                task->AddUser();task->Start();
                if(options.flags&TTPCOMM_ARCHIVE_CACHE_CURRENT){retired=std::move(current);if(retired)retired->Unretain();current=task;}
            }
        }
        retired.reset(); // Do not join while holding the cache manager lock.
        Stream* stream{};
        try{stream=new Stream(task,options);}catch(...){task->DropUser();throw;}
        hr=stream->Initialize();if(FAILED(hr)){stream->Release();return hr;}
        *out=static_cast<IStream*>(stream);return S_OK;
    }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
}
HRESULT __cdecl Status(IStream* stream,TtpCommArchiveStreamStatus* out) {
    if(!stream || !out)return E_POINTER;StatusInterface* status{};
    HRESULT hr=stream->QueryInterface(StatusId,reinterpret_cast<void**>(&status));if(FAILED(hr))return hr;
    hr=status->Snapshot(out);status->Release();return hr;
}
void __cdecl Clear(){std::shared_ptr<Task> retired;{std::lock_guard<std::mutex> lock(cache_mutex);retired=std::move(current);if(retired)retired->Unretain();}retired.reset();}
}
BOOL QueryStreams(TtpCommArchiveStreamApi* api) {
    if(!api || api->size<sizeof(*api))return FALSE;
    *api={sizeof(*api),TTPCOMM_ARCHIVE_STREAM_ABI,TTPCOMM_ARCHIVE_STREAM_REQUIRED,0,Enumerate,Open,Status,Clear};return TRUE;
}
}
extern "C" BOOL __cdecl ttpcomm_query_archive_stream(uint32_t abi,TtpCommArchiveStreamApi* api) {
    return abi==TTPCOMM_ARCHIVE_STREAM_ABI && ttpcomm::archive::QueryStreams(api);
}
