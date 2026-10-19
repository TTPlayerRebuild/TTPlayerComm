#include "archive_internal.h"
#include "archive_group.h"
#include <ttpcomm/archive_format.h>
#include <ttpcomm/archive_volumes.h>
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
bool Zip(const wchar_t* path) { return archive_format::Detect(path)==archive_format::Kind::zip; }
bool Rar(const wchar_t* path) { return archive_format::Detect(path)==archive_format::Kind::rar; }
struct OpenControl {
    const TtpCommArchiveStreamOptions& options;
    DWORD progressed=GetTickCount();
    HRESULT result{S_OK};
    HRESULT Poll() {
        if(FAILED(result))return result;
        if(options.canceled && options.canceled(options.context))result=Cancelled();
        else if(GetTickCount()-progressed>=(options.timeout_ms?options.timeout_ms:30000))result=HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        return result;
    }
    void Advance() {
        if(FAILED(Poll()))throw result;
        progressed=GetTickCount();
    }
};
struct FileIdentity {
    std::unique_ptr<archive_volumes::Set> volumes;std::wstring source,path,key;
    std::vector<std::wstring> paths;
    std::vector<std::pair<std::wstring,std::wstring>> mappings;
    void Finish(VolumeContext& context) {
        volumes->ForgetCallbacks();path=volumes->Path();key=volumes->Key();paths=volumes->Paths();mappings=std::move(context.mappings);
    }
    HRESULT Open(const wchar_t* input,VolumeContext& context,OpenControl& control) {
        try {
            auto limits=control.options.limits;limits.size=sizeof(limits);limits.context=&context;
            limits.progress=[](void* p,uint64_t,uint64_t)->BOOL{auto& c=*static_cast<VolumeContext*>(p);return !c.canceled || !c.canceled();};
            volumes=context.Prepare(input,&limits);
            if(FAILED(control.Poll()))return control.result;
            Finish(context);return S_OK;
        }catch(HRESULT hr){return FAILED(control.result)?control.result:hr;}
    }
    HRESULT OpenKnown(const std::wstring& first,const std::vector<std::wstring>& files,
        const std::vector<std::pair<std::wstring,std::wstring>>& known_mappings,
        const std::wstring& expected_key,VolumeContext& context,OpenControl& control) {
        try {
            // Re-resolve missing-volume mappings on the caller thread. A file
            // restored at its expected location, or a newly selected replacement,
            // must invalidate the old group even if every old file still exists.
            for(const auto& mapping:known_mappings) {
                const auto current=context.Resolve(mapping.first);control.Advance();
                if(_wcsicmp(current.c_str(),mapping.second.c_str()))return S_FALSE;
            }
        }catch(HRESULT hr){return FAILED(control.result)?control.result:hr;}
        // Keep only decisions made by the current resolver in this context;
        // later candidates/discovery can reuse them without another UI prompt.
        // File identity failures below require rediscovery, resolver failures
        // above are an explicit caller abort and must not prompt a second time.
        try {
            // This constructor polls once before each known file; reaching the
            // next poll means the preceding identity was successfully read.
            auto checked=std::make_unique<archive_volumes::Set>(first,files,[&control]{control.Advance();return false;});
            control.Advance();
            if(!checked->Reliable() || _wcsicmp(checked->Key().c_str(),expected_key.c_str()))return S_FALSE;
            volumes=std::move(checked);Finish(context);return S_OK;
        }catch(HRESULT hr){
            if(FAILED(control.result))return control.result;
            if(hr==Cancelled() || hr==HRESULT_FROM_WIN32(ERROR_TIMEOUT) || hr==E_OUTOFMEMORY)return hr;
            return S_FALSE;
        }
    }
};
std::atomic<uint64_t> sequence{0};
struct Task final : Sink {
    std::wstring source,path,member,key,password;
    std::vector<std::wstring> paths;
    TtpCommArchiveOptions limits{};
    const uint64_t id=++sequence;
    std::mutex mutex,join_mutex;std::condition_variable changed;
    std::vector<unsigned char> memory;HANDLE file{INVALID_HANDLE_VALUE};
    std::unique_ptr<archive_volumes::Set> identity;
    std::vector<std::pair<std::wstring,std::wstring>> mappings;
    bool identity_reliable{};
    uint64_t total{},produced{};uint32_t users{};
    bool ready{},complete{},unknown{},encrypted{},cacheable{true},retained{},has_password{};HRESULT result{E_PENDING};
    std::atomic<bool> cancel{false};std::thread worker;
    std::atomic<DWORD> activity{GetTickCount()};
    uint64_t work_done{},work_total{}; // producer only
    Task(FileIdentity& input,const wchar_t* name,const TtpCommArchiveOptions& options)
        :source(input.source),path(input.path),member(name),key(input.key),paths(std::move(input.paths)),limits(options) {
        has_password=options.password!=nullptr;if(has_password)password=options.password;
        limits.password=nullptr;limits.progress=nullptr;limits.context=nullptr;
        identity=std::move(input.volumes);identity_reliable=identity->Reliable();mappings=std::move(input.mappings);
        if(!limits.member_limit_bytes)limits.member_limit_bytes=8ULL*1024*1024*1024;
        if(!limits.dictionary_limit_kib)limits.dictionary_limit_kib=256*1024;
        limits.dictionary_limit_kib=std::min(limits.dictionary_limit_kib,512u*1024);
        if(!limits.memory_limit_bytes)limits.memory_limit_bytes=16*1024*1024;
        limits.memory_limit_bytes=std::min(limits.memory_limit_bytes,64u*1024*1024);
        if(!limits.entry_limit)limits.entry_limit=100000;
    }
    ~Task(){cancel=true;changed.notify_all();Join();SecureZeroMemory(password.data(),password.size()*sizeof(wchar_t));if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
    void Join(){std::lock_guard<std::mutex> guard(join_mutex);if(worker.joinable())worker.join();}
    void Start(){worker=std::thread([this]{Run();});}
    void Run() noexcept {
        HRESULT hr=E_FAIL;
        try {
            VolumeContext volumes;volumes.archive=path;volumes.mappings=mappings;
            volumes.canceled=[this]{return cancel.load();};VolumeScope scope(volumes);
            auto options=limits;options.size=sizeof(options);options.password=has_password?password.c_str():nullptr;
            options.progress=[](void* p,uint64_t n,uint64_t total)->BOOL{
                auto& task=*static_cast<Task*>(p);
                if(n!=task.work_done || total!=task.work_total){task.work_done=n;task.work_total=total;task.activity=GetTickCount();}
                return !task.cancel.load();
            };options.context=this;
            hr=Zip(path.c_str()) ? ExtractZip(path.c_str(),member.c_str(),&options,*this)
                : Rar(path.c_str()) ? ExtractRar(path.c_str(),member.c_str(),&options,*this)
                : ExtractExtended(path.c_str(),member.c_str(),&options,*this);
        } catch(const std::bad_alloc&){hr=E_OUTOFMEMORY;}catch(...){hr=E_FAIL;}
        // A completed cache holds bytes, not a deny-write handle on the archive.
        identity.reset();
        {std::lock_guard<std::mutex> lock(mutex);
            if(unknown){total=produced;unknown=false;}
            if(SUCCEEDED(hr) && (!ready || produced!=total))hr=STG_E_READFAULT;
            result=hr;complete=true;}
        changed.notify_all();
    }
    HRESULT Begin(uint64_t length,uint32_t flags) override {
        std::lock_guard<std::mutex> lock(mutex);if(cancel)return Cancelled();
        unknown=(flags&TTPCOMM_ARCHIVE_UNKNOWN_SIZE)!=0;
        encrypted=(flags&TTPCOMM_ARCHIVE_ENCRYPTED)!=0;
        if(!unknown && (length>limits.member_limit_bytes || length>uint64_t(INT64_MAX)))return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        total=length;
        // Preserve the current large CUE image. Only completed small previous
        // members enter the additional LRU slots (see Open below).
        cacheable=identity_reliable;
        retained=retained && cacheable;
        if(!unknown && length<=limits.memory_limit_bytes)memory.reserve(static_cast<size_t>(length));
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
    HRESULT RetryEncrypted() override {
        std::lock_guard<std::mutex> lock(mutex);
        if(cancel)return Cancelled();if(complete || ready && !encrypted)return E_UNEXPECTED;
        SecureZeroMemory(memory.data(),memory.size());memory.clear();
        if(file!=INVALID_HANDLE_VALUE){CloseHandle(file);file=INVALID_HANDLE_VALUE;}
        total=produced=0;ready=false;unknown=false;encrypted=true;
        changed.notify_all();return S_OK;
    }
    HRESULT Write(const void* bytes,uint32_t size) override {
        std::lock_guard<std::mutex> lock(mutex);if(cancel)return Cancelled();
        if(size>limits.member_limit_bytes-produced)return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        if(!unknown && (produced>total || size>total-produced))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
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
    bool MatchesOptions(const wchar_t* name,const TtpCommArchiveOptions& options) const {
        const auto limit=options.member_limit_bytes?options.member_limit_bytes:8ULL*1024*1024*1024;
        const auto dict=std::min(options.dictionary_limit_kib?options.dictionary_limit_kib:256u*1024,512u*1024);
        const auto mem=std::min(options.memory_limit_bytes?options.memory_limit_bytes:16u*1024*1024,64u*1024*1024);
        const auto entries=options.entry_limit?options.entry_limit:100000u;
        return cacheable && !cancel && (!complete || SUCCEEDED(result)) && has_password==(options.password!=nullptr) &&
            (!has_password || password==options.password) &&
            !_wcsicmp(member.c_str(),name) && limits.member_limit_bytes==limit &&
            limits.dictionary_limit_kib==dict && limits.memory_limit_bytes==mem && limits.entry_limit==entries;
    }
    bool KnownGroup(const std::wstring& input,const wchar_t* name,const TtpCommArchiveOptions& options) {
        std::lock_guard<std::mutex> lock(mutex);
        return identity_reliable && !_wcsicmp(source.c_str(),input.c_str()) && MatchesOptions(name,options);
    }
    bool Matches(const FileIdentity& input,const wchar_t* name,const TtpCommArchiveOptions& options) {
        std::lock_guard<std::mutex> lock(mutex);
        return !_wcsicmp(key.c_str(),input.key.c_str()) && MatchesOptions(name,options);
    }
};
std::mutex cache_mutex;
std::vector<std::shared_ptr<Task>> cached;
// Private identity lets status reject unrelated IStreams without RTTI casts.
const auto& StatusId=IID_TtpCommArchiveStreamStatus;
using StatusInterface=TtpCommArchiveStreamStatusInterface;
class Stream final : public IStream,public StatusInterface {
    std::atomic<ULONG> refs_{1};std::shared_ptr<Task> task_;
    TtpCommArchiveStreamOptions options_{};uint64_t position_{};std::mutex cursor_;
    std::atomic<bool> aborted_{false};
    bool Canceled() const {return options_.canceled && options_.canceled(options_.context);}
    HRESULT Wait(std::unique_lock<std::mutex>& lock,uint64_t wanted,bool only_header) {
        DWORD start=GetTickCount();
        const DWORD timeout=options_.timeout_ms?options_.timeout_ms:30000;
        auto produced=task_->produced;
        auto activity=task_->activity.load();
        for(;;) {
            lock.unlock();bool canceled{};try{canceled=Canceled();}catch(...){canceled=true;}lock.lock();
            if(canceled){aborted_=true;return Cancelled();}
            if(task_->complete && FAILED(task_->result))return task_->result;
            if(task_->ready && (!task_->encrypted || task_->complete) && (only_header || (task_->produced>=wanted && (wanted<task_->total || task_->complete))))return S_OK;
            if(task_->ready && task_->complete)return S_OK;
            if(task_->complete)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            const auto next=task_->activity.load();
            if(task_->produced!=produced || next!=activity){produced=task_->produced;activity=next;start=GetTickCount();}
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
        auto n=static_cast<ULONG>(position_<task_->total?std::min<uint64_t>(count,task_->total-position_):0);
        hr=Wait(lock,position_<task_->total?position_+n:task_->total,false);if(FAILED(hr))return hr;
        n=static_cast<ULONG>(position_<task_->total?std::min<uint64_t>(count,task_->total-position_):0);
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
        if(origin==STREAM_SEEK_END && task_->unknown){hr=Wait(lock,UINT64_MAX,false);if(FAILED(hr))return hr;}
        const auto base=origin==STREAM_SEEK_SET?0:origin==STREAM_SEEK_CUR?position_:task_->total;
        if(move.QuadPart<0){const auto distance=uint64_t(-(move.QuadPart+1))+1;if(distance>base)return STG_E_INVALIDFUNCTION;position_=base-distance;}
        else {if(uint64_t(move.QuadPart)>uint64_t(INT64_MAX)-base)return STG_E_INVALIDFUNCTION;position_=base+uint64_t(move.QuadPart);}
        if(out)out->QuadPart=position_;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* out,DWORD flags) override {
        if(!out)return E_POINTER;if(flags!=STATFLAG_DEFAULT && flags!=STATFLAG_NONAME)return STG_E_INVALIDFLAG;
        std::unique_lock<std::mutex> lock(task_->mutex);HRESULT hr=Wait(lock,0,true);if(FAILED(hr))return hr;
        if(task_->unknown){hr=Wait(lock,UINT64_MAX,false);if(FAILED(hr))return hr;}
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
    return Zip(path)?EnumerateZip(path,options,visitor,context):Rar(path)?EnumerateRar(path,options,visitor,context):EnumerateExtended(path,options,visitor,context);
}
HRESULT __cdecl Open(const wchar_t* path,const wchar_t* member,const TtpCommArchiveStreamOptions* input,IStream** out) {
    if(!out)return E_POINTER;*out=nullptr;
    if(!path || !*path || !member || !*member || (input && input->size!=sizeof(*input)))return E_INVALIDARG;
    if(!Zip(path) && !Rar(path) && !IsExtended(path))return TTPCOMM_ARCHIVE_UNSUPPORTED;
    TtpCommArchiveStreamOptions options{};if(input)options=*input;
    if(options.flags&~TTPCOMM_ARCHIVE_CACHE_CURRENT)return E_INVALIDARG;
    if(options.limits.size && options.limits.size!=sizeof(options.limits))return E_INVALIDARG;
    try {
        OpenControl control{options};if(FAILED(control.Poll()))return control.result;
        VolumeContext context;if(active_volumes){context.options=active_volumes->options;context.mappings=active_volumes->mappings;}
        context.canceled=[&control]{return FAILED(control.Poll());};
        context.progressed=[&control]{control.Advance();};
        FileIdentity identity;identity.source=ArchivePath(path);context.archive=identity.source;
        std::wstring name(member);std::replace(name.begin(),name.end(),L'/',L'\\');
        HRESULT hr=S_FALSE;
        if((options.flags&TTPCOMM_ARCHIVE_CACHE_CURRENT) && Rar(path)) {
            // Do not acquire UnRAR's global engine lock for a known unchanged
            // group. Snapshot candidates under the manager lock, then open and
            // validate every actual volume without either engine/cache lock.
            std::vector<std::shared_ptr<Task>> candidates;
            {std::lock_guard<std::mutex> lock(cache_mutex);candidates=cached;}
            for(auto i=candidates.rbegin();i!=candidates.rend();++i) {
                if(!(*i)->KnownGroup(identity.source,name.c_str(),options.limits))continue;
                hr=identity.OpenKnown((*i)->path,(*i)->paths,(*i)->mappings,(*i)->key,context,control);
                if(FAILED(hr))return hr;if(hr==S_OK)break;
            }
        }
        if(hr!=S_OK)hr=identity.Open(path,context,control);
        if(FAILED(hr))return hr;
        std::shared_ptr<Task> task;std::vector<std::shared_ptr<Task>> retired;
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            if(options.flags&TTPCOMM_ARCHIVE_CACHE_CURRENT){
                for(size_t i=0;i<cached.size();++i)if(cached[i]->Matches(identity,name.c_str(),options.limits) && cached[i]->AddUser()){
                    task=cached[i];cached.erase(cached.begin()+i);cached.push_back(task);break;
                }
            }
            if(!task) {
                task=std::make_shared<Task>(identity,name.c_str(),options.limits);
                task->retained=(options.flags&TTPCOMM_ARCHIVE_CACHE_CURRENT)!=0;
                task->AddUser();task->Start();
                if(options.flags&TTPCOMM_ARCHIVE_CACHE_CURRENT){
                    cached.push_back(task);
                }
            }
            if(options.flags&TTPCOMM_ARCHIVE_CACHE_CURRENT){
                for(auto i=cached.begin();i!=cached.end();){bool keep=*i==task;
                    if(!keep){std::lock_guard<std::mutex> item((*i)->mutex);keep=(*i)->complete && SUCCEEDED((*i)->result) && (*i)->cacheable && (*i)->total<=128ULL*1024*1024;}
                    if(keep){++i;continue;}(*i)->Unretain();retired.push_back(*i);i=cached.erase(i);
                }
                while(cached.size()>3){cached.front()->Unretain();retired.push_back(cached.front());cached.erase(cached.begin());}
            }
        }
        retired.clear(); // Do not join while holding the cache manager lock.
        Stream* stream{};
        try{stream=new Stream(task,options);}catch(...){task->DropUser();throw;}
        hr=stream->Initialize();if(FAILED(hr)){stream->Release();return hr;}
        *out=static_cast<IStream*>(stream);return S_OK;
    }catch(HRESULT hr){return hr;}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
}
HRESULT __cdecl Status(IStream* stream,TtpCommArchiveStreamStatus* out) {
    if(!stream || !out)return E_POINTER;StatusInterface* status{};
    HRESULT hr=stream->QueryInterface(StatusId,reinterpret_cast<void**>(&status));if(FAILED(hr))return hr;
    hr=status->Snapshot(out);status->Release();return hr;
}
void __cdecl Clear(){std::vector<std::shared_ptr<Task>> retired;{std::lock_guard<std::mutex> lock(cache_mutex);retired.swap(cached);for(auto& item:retired)item->Unretain();}retired.clear();ClearExtendedCache();}

bool ValidGroup(const TtpCommArchiveVolumeOptions* options){return !options || options->size==sizeof(*options);}
HRESULT __cdecl GroupEnumerate(const wchar_t* path,const TtpCommArchiveOptions* limits,const TtpCommArchiveVolumeOptions* options,TtpCommArchiveVisitor visitor,void* opaque){
    if(!path || !*path || !ValidGroup(options) || !visitor)return E_INVALIDARG;
    try{VolumeContext c;c.archive=path;if(options)c.options=*options;
        c.canceled=[limits]{return limits && limits->progress && !limits->progress(limits->context,0,0);};
        VolumeScope scope(c);return Enumerate(path,limits,visitor,opaque);
    }catch(HRESULT hr){return hr;}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
}
HRESULT __cdecl GroupOpen(const wchar_t* path,const wchar_t* member,const TtpCommArchiveStreamOptions* limits,const TtpCommArchiveVolumeOptions* options,IStream** out){
    if(!ValidGroup(options)){if(out)*out=nullptr;return E_INVALIDARG;}
    try{VolumeContext c;if(options)c.options=*options;VolumeScope scope(c);return Open(path,member,limits,out);}
    catch(const std::bad_alloc&){if(out)*out=nullptr;return E_OUTOFMEMORY;}catch(...){if(out)*out=nullptr;return E_FAIL;}
}
HRESULT __cdecl Volumes(const wchar_t* path,const TtpCommArchiveOptions* limits,const TtpCommArchiveVolumeOptions* options,TtpCommArchiveVolumeVisitor visitor,void* opaque){
    if(!path || !*path || !ValidGroup(options) || !visitor || (limits && limits->size!=sizeof(*limits)))return E_INVALIDARG;
    try{VolumeContext c;if(options)c.options=*options;
        c.canceled=[limits]{return limits && limits->progress && !limits->progress(limits->context,0,0);};
        const auto files=c.Prepare(path,limits);
        for(const auto& p:files->Paths()){if(c.canceled() || !visitor(opaque,p.c_str()))return Cancelled();}
        return S_OK;
    }catch(HRESULT hr){return hr;}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
}
}
BOOL QueryStreams(TtpCommArchiveStreamApi* api) {
    if(!api || api->size<sizeof(*api))return FALSE;
    *api={sizeof(*api),TTPCOMM_ARCHIVE_STREAM_ABI,TTPCOMM_ARCHIVE_GROUP_REQUIRED,0,Enumerate,Open,Status,Clear};return TRUE;
}
BOOL QueryGroups(TtpCommArchiveGroupApi* api){
    if(!api || api->size<sizeof(*api))return FALSE;
    *api={sizeof(*api),TTPCOMM_ARCHIVE_GROUP_ABI,TTPCOMM_ARCHIVE_GROUP_REQUIRED,0,GroupEnumerate,GroupOpen,Volumes,Status,Clear};return TRUE;
}
}
extern "C" BOOL __cdecl ttpcomm_query_archive_stream(uint32_t abi,TtpCommArchiveStreamApi* api) {
    if(abi==TTPCOMM_ARCHIVE_GROUP_ABI)return ttpcomm::archive::QueryGroups(reinterpret_cast<TtpCommArchiveGroupApi*>(api));
    return abi==TTPCOMM_ARCHIVE_STREAM_ABI && ttpcomm::archive::QueryStreams(api);
}
