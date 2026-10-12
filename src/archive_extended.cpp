#include "archive_internal.h"
#include <ttpcomm/archive_format.h>
#include <ttpcomm/archive_volumes.h>
#define LIBARCHIVE_STATIC
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
struct Context {
    TtpCommArchiveOptions limits{};
    HANDLE file{INVALID_HANDLE_VALUE};
    uint64_t file_size{}, position{}, gzip_bytes{}, gzip_limit{};
    size_t allocated{};
    HRESULT error{S_OK};
    unsigned char buffer[65536];
    explicit Context(const TtpCommArchiveOptions* options) {
        if(options)limits=*options;
        if(!limits.dictionary_limit_kib)limits.dictionary_limit_kib=256*1024;
        limits.dictionary_limit_kib=std::min(limits.dictionary_limit_kib,512u*1024);
        if(!limits.member_limit_bytes)limits.member_limit_bytes=8ULL*1024*1024*1024;
        if(!limits.memory_limit_bytes)limits.memory_limit_bytes=16*1024*1024;
        limits.memory_limit_bytes=std::min(limits.memory_limit_bytes,64u*1024*1024);
        if(!limits.entry_limit)limits.entry_limit=100000;
    }
    ~Context(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
    bool Canceled() {
        if(limits.progress && !limits.progress(limits.context,position,file_size))
            error=HRESULT_FROM_WIN32(ERROR_CANCELLED);
        return FAILED(error);
    }
};
thread_local Context* active{};
struct Scope {Context* previous{active};explicit Scope(Context& c){active=&c;}~Scope(){active=previous;}};
union Allocation {size_t size;std::max_align_t alignment;};
}

// These private C hooks bound libarchive's 7z headers/BCJ2 buffers separately
// from its decoder dictionary and the service's member storage. All allocations
// and releases occur on this operation's thread, before Scope is destroyed.
extern "C" uint64_t ttpcomm_archive_dictionary_limit(){return uint64_t(active->limits.dictionary_limit_kib)*1024;}
extern "C" size_t ttpcomm_archive_entry_limit(){return active->limits.entry_limit;}
extern "C" int ttpcomm_archive_canceled(){return active->Canceled();}
extern "C" int ttpcomm_archive_gzip_account(size_t n) {
    if(n>active->gzip_limit-active->gzip_bytes){
        active->error=HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);return 0;
    }
    active->gzip_bytes+=n;return !active->Canceled();
}
extern "C" void* ttpcomm_7z_malloc(size_t n) {
    const auto limit=active->limits.memory_limit_bytes;
    if(n>limit || active->allocated>limit-n){active->error=E_OUTOFMEMORY;return nullptr;}
    auto* p=static_cast<Allocation*>(std::malloc(sizeof(Allocation)+n));
    if(!p)return nullptr;p->size=n;active->allocated+=n;return p+1;
}
extern "C" void ttpcomm_7z_free(void* p) {
    if(p){auto* h=static_cast<Allocation*>(p)-1;active->allocated-=h->size;std::free(h);}
}
extern "C" void* ttpcomm_7z_calloc(size_t n,size_t size) {
    if(size && n>SIZE_MAX/size){active->error=E_OUTOFMEMORY;return nullptr;}
    auto* p=ttpcomm_7z_malloc(n*size);if(p)std::memset(p,0,n*size);return p;
}
extern "C" void* ttpcomm_7z_realloc(void* p,size_t n) {
    if(!p)return ttpcomm_7z_malloc(n);
    auto* h=static_cast<Allocation*>(p)-1;const size_t old=h->size;
    const auto limit=active->limits.memory_limit_bytes;
    if(n>limit || active->allocated-old>limit-n){active->error=E_OUTOFMEMORY;return nullptr;}
    auto* result=static_cast<Allocation*>(std::realloc(h,sizeof(Allocation)+n));
    if(!result)return nullptr;result->size=n;active->allocated=active->allocated-old+n;return result+1;
}
extern "C" int ttpcomm_archive_verify_gzip(struct archive*);

namespace ttpcomm::archive {
namespace {

// Directory enumeration already traverses/validates a tar.gz stream. Retain
// small CUE members encountered in that pass, not a second copy of the album.
// Publish only after the complete archive/gzip integrity check succeeds.
struct CachedText {std::wstring name;std::vector<unsigned char> bytes;uint32_t index{};};
struct CachedEntry {std::wstring name;TtpCommArchiveEntry info;};
struct TextCache {
    std::wstring path;BY_HANDLE_FILE_INFORMATION stamp{};uint64_t change_time{};
    std::vector<CachedText> members;
    std::vector<CachedEntry> directory;
    size_t bytes{},metadata{};uint64_t expanded{};uint32_t count{};bool complete_index{true};
};
std::mutex text_cache_mutex;
std::shared_ptr<const TextCache> text_cache;
bool SameStamp(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber && a.nFileIndexHigh==b.nFileIndexHigh && a.nFileIndexLow==b.nFileIndexLow &&
        a.nFileSizeHigh==b.nFileSizeHigh && a.nFileSizeLow==b.nFileSizeLow &&
        a.ftLastWriteTime.dwHighDateTime==b.ftLastWriteTime.dwHighDateTime && a.ftLastWriteTime.dwLowDateTime==b.ftLastWriteTime.dwLowDateTime;
}
std::wstring AbsoluteName(const wchar_t* path) {
    const auto n=GetFullPathNameW(path,0,nullptr,nullptr);if(!n)return {};
    std::wstring out(n,L'\0');const auto written=GetFullPathNameW(path,n,out.data(),nullptr);
    if(!written || written>=n)return {};out.resize(written);return out;
}
enum class Kind {none,tar,gzip,targzip,seven};
Kind Format(const wchar_t* path) {
    switch(archive_format::Detect(path)) {
    case archive_format::Kind::zip:case archive_format::Kind::rar:return Kind::none;
    case archive_format::Kind::seven:return Kind::seven;
    case archive_format::Kind::tar:return Kind::tar;
    case archive_format::Kind::gzip:return Kind::gzip;
    case archive_format::Kind::targzip:return Kind::targzip;
    default:break;
    }
    std::wstring s(path?path:L"");std::transform(s.begin(),s.end(),s.begin(),towlower);
    const auto ends=[&](const wchar_t* suffix){const size_t n=wcslen(suffix);return s.size()>=n && s.compare(s.size()-n,n,suffix)==0;};
    if(ends(L".tar.gz") || ends(L".tgz"))return Kind::targzip;
    if(ends(L".tar"))return Kind::tar;if(ends(L".gz"))return Kind::gzip;
    if(ends(L".7z"))return Kind::seven;return Kind::none;
}
HRESULT LastError(){const DWORD e=GetLastError();return HRESULT_FROM_WIN32(e?e:ERROR_READ_FAULT);}
HRESULT Failure(struct ::archive* a,Context& c) {
    if(FAILED(c.error))return c.error;
    std::string s=archive_error_string(a)?archive_error_string(a):"";
    std::transform(s.begin(),s.end(),s.begin(),[](unsigned char x){return char(std::tolower(x));});
    if(s.find("dictionary limit")!=std::string::npos)return TTPCOMM_ARCHIVE_DICTIONARY_LIMIT;
    if(s.find("encrypt")!=std::string::npos || s.find("passphrase")!=std::string::npos || s.find("crypto codec")!=std::string::npos)
        return c.limits.password && *c.limits.password?TTPCOMM_ARCHIVE_UNSUPPORTED:TTPCOMM_ARCHIVE_PASSWORD;
    if(s.find("unsupported")!=std::string::npos)return TTPCOMM_ARCHIVE_UNSUPPORTED;
    if(s.find("crc")!=std::string::npos || s.find("checksum")!=std::string::npos)return HRESULT_FROM_WIN32(ERROR_CRC);
    return archive_errno(a)==ENOMEM?E_OUTOFMEMORY:HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
}
la_ssize_t Read(struct ::archive* a,void* opaque,const void** out) {
    auto& c=*static_cast<Context*>(opaque);*out=c.buffer;
    if(c.Canceled()){archive_set_error(a,ECANCELED,"Archive canceled");return -1;}
    DWORD got{};if(!ReadFile(c.file,c.buffer,sizeof(c.buffer),&got,nullptr)){
        c.error=LastError();archive_set_error(a,EIO,"Archive read failed");return -1;}
    c.position+=got;return got;
}
la_int64_t Seek(struct ::archive* a,void* opaque,la_int64_t distance,int origin) {
    auto& c=*static_cast<Context*>(opaque);
    if(c.Canceled()){archive_set_error(a,ECANCELED,"Archive canceled");return -1;}
    LARGE_INTEGER move{},result{};move.QuadPart=distance;
    if(!SetFilePointerEx(c.file,move,&result,origin)){
        c.error=LastError();archive_set_error(a,EIO,"Archive seek failed");return -1;}
    c.position=result.QuadPart;return result.QuadPart;
}
bool Name(struct archive_entry* entry,std::wstring& name) {
    if(const auto* wide=archive_entry_pathname_w(entry))name=wide;
    else {
        const char* s=archive_entry_pathname_utf8(entry);if(!s)return false;
        const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,nullptr,0);if(n<=1)return false;
        name.resize(n);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,name.data(),n);name.resize(n-1);
    }
    if(name.empty() || name.size()>32767 || name[0]==L'/' || name[0]==L'\\' || name.find_first_of(L":|")!=std::wstring::npos)return false;
    std::replace(name.begin(),name.end(),L'/',L'\\');
    std::wstring result;size_t begin=0;
    while(begin<name.size()){
        size_t end=name.find(L'\\',begin);if(end==std::wstring::npos)end=name.size();
        const auto part=name.substr(begin,end-begin);if(part==L"..")return false;
        if(!part.empty() && part!=L"."){if(!result.empty())result+=L'\\';result+=part;}
        begin=end+1;
    }
    name=std::move(result);return !name.empty();
}
HRESULT Process(const wchar_t* path,const wchar_t* member,const TtpCommArchiveOptions* options,
                TtpCommArchiveVisitor visitor,void* opaque,Sink* sink) {
    if(!path || !*path || (options && options->size && options->size!=sizeof(*options)))return E_INVALIDARG;
    const auto kind=Format(path);if(kind==Kind::none)return TTPCOMM_ARCHIVE_UNSUPPORTED;
    if(kind==Kind::seven)return ProcessSeven(path,member,options,visitor,opaque,sink);
    try {
        Context c(options);Scope scope(c);
        // A small CUE member may live in a large album. Its 16 MiB text limit
        // must not accidentally cap the entire TAR stream checked for gzip CRC.
        c.gzip_limit=(kind==Kind::targzip || kind==Kind::gzip)?std::max(c.limits.member_limit_bytes,8ULL*1024*1024*1024):c.limits.member_limit_bytes;
        c.file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(c.file==INVALID_HANDLE_VALUE)return LastError();
        LARGE_INTEGER size{};if(!GetFileSizeEx(c.file,&size))return LastError();c.file_size=size.QuadPart;
        if(c.Canceled())return c.error;
        const auto full=AbsoluteName(path);BY_HANDLE_FILE_INFORMATION stamp{};
        const auto change_time=archive_volumes::ChangeTime(c.file);
        const bool stamped=change_time && !full.empty() && GetFileInformationByHandle(c.file,&stamp);
        if(stamped) {
            std::shared_ptr<const TextCache> cached;
            {std::lock_guard<std::mutex> lock(text_cache_mutex);cached=text_cache;}
            if(cached && cached->change_time==change_time && !_wcsicmp(cached->path.c_str(),full.c_str()) && SameStamp(cached->stamp,stamp)) {
                if(visitor && cached->complete_index && cached->metadata<=c.limits.memory_limit_bytes) {
                    if(cached->count>c.limits.entry_limit)return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
                    if(cached->expanded>c.gzip_limit)return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
                    for(const auto& item:cached->directory) {
                        if(c.Canceled())return c.error;
                        auto info=item.info;info.name=item.name.c_str();
                        if(!visitor(opaque,&info))return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                    }
                    return S_OK;
                }
                if(sink)for(const auto& text:cached->members)if(!_wcsicmp(text.name.c_str(),member)) {
                    if(text.index>=c.limits.entry_limit)return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
                    if(text.bytes.size()>c.limits.member_limit_bytes || cached->expanded>c.gzip_limit)return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
                    auto hr=sink->Begin(text.bytes.size());if(FAILED(hr))return hr;
                    for(size_t at=0;at<text.bytes.size();) {
                        if(c.Canceled())return c.error;
                        const auto n=static_cast<uint32_t>(std::min<size_t>(65536,text.bytes.size()-at));
                        hr=sink->Write(text.bytes.data()+at,n);if(FAILED(hr))return hr;at+=n;
                    }
                    return S_OK;
                }
            }
        }
        auto pending=std::make_shared<TextCache>();pending->path=full;pending->stamp=stamp;pending->change_time=change_time;
        const size_t cache_limit=std::min(c.limits.memory_limit_bytes,16u*1024*1024);
        std::unique_ptr<struct ::archive,decltype(&archive_read_free)> a(archive_read_new(),archive_read_free);
        if(!a)return E_OUTOFMEMORY;
        if(kind==Kind::seven)archive_read_support_format_7zip(a.get());
        else if(kind==Kind::gzip){archive_read_support_format_raw(a.get());archive_read_support_format_tar(a.get());archive_read_set_format_option(a.get(),"tar","hdrcharset","UTF-8");}
        else {
            archive_read_support_format_tar(a.get());
            // GNU/ustar have no encoding marker. Modern archives use UTF-8;
            // do not let the worker's C locale silently reinterpret these names.
            archive_read_set_format_option(a.get(),"tar","hdrcharset","UTF-8");
        }
        if(kind==Kind::gzip || kind==Kind::targzip)archive_read_support_filter_gzip(a.get());
        archive_read_set_seek_callback(a.get(),Seek);
        if(archive_read_open(a.get(),&c,nullptr,Read,nullptr)!=ARCHIVE_OK)return Failure(a.get(),c);
        if((kind==Kind::gzip || kind==Kind::targzip) && archive_filter_code(a.get(),0)!=ARCHIVE_FILTER_GZIP)
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        std::wstring raw_name=path;const auto slash=raw_name.find_last_of(L"/\\");
        if(slash!=std::wstring::npos)raw_name.erase(0,slash+1);
        const bool gzip_suffix=raw_name.size()>3 && !_wcsicmp(raw_name.c_str()+raw_name.size()-3,L".gz");
        if(gzip_suffix)raw_name.resize(raw_name.size()-3);
        else {const auto dot=raw_name.find_last_of(L'.');if(dot!=raw_name.npos)raw_name.resize(dot);}
        uint32_t index=0;bool found=false;struct archive_entry* entry{};
        for(;;){
            if(c.Canceled())return c.error;
            const int status=archive_read_next_header(a.get(),&entry);
            if(status==ARCHIVE_EOF)break;
            if(status!=ARCHIVE_OK)return Failure(a.get(),c);
            if(index>=c.limits.entry_limit)return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
            TtpCommArchiveEntry item{};item.size=sizeof(item);item.index=index++;
            const auto type=archive_entry_filetype(entry);
            if(type==AE_IFDIR)item.flags|=TTPCOMM_ARCHIVE_DIRECTORY;
            if(type!=AE_IFREG && type!=AE_IFDIR || archive_entry_hardlink(entry) || archive_entry_symlink(entry))item.flags|=TTPCOMM_ARCHIVE_LINK;
            if(archive_entry_sparse_count(entry)>0)item.flags|=TTPCOMM_ARCHIVE_SPARSE;
            if(archive_entry_is_encrypted(entry)>0)item.flags|=TTPCOMM_ARCHIVE_ENCRYPTED;
            std::wstring name;
            unsigned char prefix[512]{};size_t prefix_size=0;
            if(::archive_format(a.get())==ARCHIVE_FORMAT_RAW){
                const auto n=archive_read_data(a.get(),prefix,sizeof(prefix));if(n<0)return Failure(a.get(),c);prefix_size=static_cast<size_t>(n);
                name=raw_name.empty()?L"data":raw_name;
                // A header-identified gzip may have no usable inner filename.
                // Preserve conventional foo.wav.gz identities, otherwise give
                // its single virtual member the detected audio suffix.
                if(name.find_last_of(L'.')==name.npos){
                    const wchar_t* suffix=L".bin";
                    if(n>=12 && !memcmp(prefix,"RIFF",4) && !memcmp(prefix+8,"WAVE",4))suffix=L".wav";
                    else if(n>=4 && !memcmp(prefix,"fLaC",4))suffix=L".flac";
                    else if(n>=3 && !memcmp(prefix,"ID3",3) || n>=2 && prefix[0]==0xff && (prefix[1]&0xe0)==0xe0)suffix=L".mp3";
                    else if(n>=4 && !memcmp(prefix,"OggS",4))suffix=L".ogg";
                    else if(n>=8 && !memcmp(prefix+4,"ftyp",4))suffix=L".m4a";
                    name+=suffix;
                }
            }else if(!Name(entry,name))continue;
            item.name=name.c_str();
            if(archive_entry_size_is_set(entry)){
                const auto n=archive_entry_size(entry);if(n<0)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                item.unpacked_bytes=n;
            }else{item.flags|=TTPCOMM_ARCHIVE_UNKNOWN_SIZE;item.unpacked_bytes=UINT64_MAX;}
            if(visitor && !visitor(opaque,&item))return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            if(visitor && stamped && pending->complete_index) {
                const auto bytes=sizeof(CachedEntry)+(name.size()+1)*sizeof(wchar_t);
                if(bytes>cache_limit-pending->metadata){pending->complete_index=false;pending->directory.clear();pending->metadata=0;}
                else {auto info=item;info.name=nullptr;pending->directory.push_back({name,info});pending->metadata+=bytes;}
            }
            const auto dot=name.rfind(L'.');const auto extension=dot==name.npos?L"":name.c_str()+dot;
            bool auxiliary=false;
            for(const auto* suffix:{L".cue",L".lrc",L".txt",L".png",L".jpg",L".jpeg",L".bmp",L".gif"})
                if(!_wcsicmp(extension,suffix)){auxiliary=true;break;}
            if(visitor && stamped && item.flags==0 && auxiliary &&
                pending->members.size()<128 && item.unpacked_bytes<=cache_limit-pending->bytes) {
                CachedText text;text.name=name;text.index=item.index;text.bytes.resize(static_cast<size_t>(item.unpacked_bytes));
                size_t at=0;
                while(at<text.bytes.size()) {
                    if(c.Canceled())return c.error;
                    const auto got=archive_read_data(a.get(),text.bytes.data()+at,std::min<size_t>(65536,text.bytes.size()-at));
                    if(got<0)return Failure(a.get(),c);if(!got)return STG_E_READFAULT;at+=static_cast<size_t>(got);
                }
                unsigned char extra{};const auto tail=archive_read_data(a.get(),&extra,1);
                if(tail<0)return Failure(a.get(),c);if(tail)return STG_E_READFAULT;
                pending->bytes+=text.bytes.size();pending->members.push_back(std::move(text));
            }
            if(sink && !_wcsicmp(name.c_str(),member)){
                if(item.flags&TTPCOMM_ARCHIVE_ENCRYPTED)return TTPCOMM_ARCHIVE_PASSWORD;
                if(item.flags&(TTPCOMM_ARCHIVE_LINK|TTPCOMM_ARCHIVE_DIRECTORY|TTPCOMM_ARCHIVE_SPARSE))return TTPCOMM_ARCHIVE_UNSUPPORTED;
                HRESULT hr=sink->Begin(item.unpacked_bytes,item.flags);if(FAILED(hr))return hr;
                unsigned char bytes[65536];uint64_t produced=0;
                if(prefix_size){hr=sink->Write(prefix,static_cast<uint32_t>(prefix_size));if(FAILED(hr))return hr;produced=prefix_size;}
                for(;;){
                    if(c.Canceled())return c.error;
                    const auto n=archive_read_data(a.get(),bytes,sizeof(bytes));
                    if(n<0)return Failure(a.get(),c);if(!n)break;
                    if(uint64_t(n)>c.limits.member_limit_bytes-produced)return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
                    produced+=n;hr=sink->Write(bytes,static_cast<uint32_t>(n));if(FAILED(hr))return hr;
                }
                if(!(item.flags&TTPCOMM_ARCHIVE_UNKNOWN_SIZE) && produced!=item.unpacked_bytes)return STG_E_READFAULT;
                found=true;break;
            }
        }
        // In particular, TAR's two zero blocks do not verify the gzip footer.
        if(ttpcomm_archive_verify_gzip(a.get())!=ARCHIVE_OK)return Failure(a.get(),c);
        if(visitor && stamped){
            pending->expanded=c.gzip_bytes;pending->count=index;
            std::lock_guard<std::mutex> lock(text_cache_mutex);text_cache=std::move(pending);
        }
        return sink && !found?HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND):S_OK;
    }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
}
}
bool IsExtended(const wchar_t* path){return Format(path)!=Kind::none;}
void ClearExtendedCache(){std::lock_guard<std::mutex> lock(text_cache_mutex);text_cache.reset();}
HRESULT EnumerateExtended(const wchar_t* p,const TtpCommArchiveOptions* o,TtpCommArchiveVisitor v,void* c){
    return v?Process(p,nullptr,o,v,c,nullptr):E_INVALIDARG;
}
HRESULT ExtractExtended(const wchar_t* p,const wchar_t* m,const TtpCommArchiveOptions* o,Sink& s){return Process(p,m,o,nullptr,nullptr,&s);}
}
