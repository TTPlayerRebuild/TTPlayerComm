#include "archive_internal.h"
#include <zlib.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace ttpcomm::archive {
namespace {
struct Failure { HRESULT hr; };
void Require(bool ok,HRESULT hr=HRESULT_FROM_WIN32(ERROR_INVALID_DATA)) { if(!ok) throw Failure{hr}; }
void Check(HRESULT hr) { if(FAILED(hr)) throw Failure{hr}; }
uint16_t U16(const unsigned char* p) { return uint16_t(p[0]|(uint16_t(p[1])<<8)); }
uint32_t U32(const unsigned char* p) { return uint32_t(U16(p))|(uint32_t(U16(p+2))<<16); }
struct File {
    HANDLE handle{INVALID_HANDLE_VALUE}; uint64_t length{};
    explicit File(const wchar_t* path) {
        handle=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE) throw Failure{HRESULT_FROM_WIN32(GetLastError())};
        LARGE_INTEGER size{};
        if(!GetFileSizeEx(handle,&size) || size.QuadPart<0) {
            const DWORD e=GetLastError(); CloseHandle(handle);handle=INVALID_HANDLE_VALUE;
            throw Failure{HRESULT_FROM_WIN32(e ? e : ERROR_INVALID_DATA)};
        }
        length=uint64_t(size.QuadPart);
    }
    ~File() { if(handle!=INVALID_HANDLE_VALUE) CloseHandle(handle); }
    void Read(uint64_t at,void* data,DWORD count) {
        Require(at<=length && count<=length-at);
        LARGE_INTEGER offset{};offset.QuadPart=static_cast<LONGLONG>(at);DWORD read{};
        if(!SetFilePointerEx(handle,offset,nullptr,FILE_BEGIN) || !ReadFile(handle,data,count,&read,nullptr))
            throw Failure{HRESULT_FROM_WIN32(GetLastError())};
        Require(read==count,HRESULT_FROM_WIN32(ERROR_HANDLE_EOF));
    }
};
struct Entry {
    std::string raw; std::wstring name;
    uint32_t packed{},length{},crc{},offset{};
    uint16_t flags{},method{};
    bool directory{};
};
void Progress(const TtpCommArchiveOptions* options,uint64_t done,uint64_t total) {
    if(options && options->progress && !options->progress(options->context,done,total))
        throw Failure{HRESULT_FROM_WIN32(ERROR_CANCELLED)};
}
// 0041EF26/0041F5BA: OEM -> Unicode, then match Unicode names. Never fold
// individual bytes: DBCS trail bytes can look like ASCII uppercase letters.
std::wstring Name(const std::string& raw) {
    Require(raw.find('\0')==std::string::npos);
    const int n=MultiByteToWideChar(CP_OEMCP,0,raw.data(),static_cast<int>(raw.size()),nullptr,0);
    Require(n>0);std::wstring name(n,L'\0');
    Require(MultiByteToWideChar(CP_OEMCP,0,raw.data(),static_cast<int>(raw.size()),name.data(),n)==n);
    std::replace(name.begin(),name.end(),L'/',L'\\');return name;
}
std::vector<Entry> Directory(File& file,const TtpCommArchiveOptions* options) {
    Progress(options,0,0);Require(file.length>=22);
    const DWORD tail_size=static_cast<DWORD>(std::min<uint64_t>(file.length,65535+22));
    std::vector<unsigned char> tail(tail_size);file.Read(file.length-tail_size,tail.data(),tail_size);
    size_t e=tail.size()-22;
    for(;;) {
        if(U32(tail.data()+e)==0x06054b50 && e+22+U16(tail.data()+e+20)==tail.size()) break;
        Require(e!=0);--e;
    }
    const auto* end=tail.data()+e;
    Require(U16(end+4)==0 && U16(end+6)==0 && U16(end+8)==U16(end+10),TTPCOMM_ARCHIVE_UNSUPPORTED);
    const uint32_t count=U16(end+10),bytes=U32(end+12),offset=U32(end+16);
    Require(count!=0xffff && bytes!=0xffffffff && offset!=0xffffffff,TTPCOMM_ARCHIVE_UNSUPPORTED);
    Require(!options || !options->entry_limit || count<=options->entry_limit,HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES));
    const uint64_t end_at=file.length-tail_size+e;
    Require(offset<=end_at && bytes<=end_at-offset);
    uint64_t at=offset;const uint64_t limit=uint64_t(offset)+bytes;
    std::vector<Entry> result;result.reserve(count);
    for(uint32_t i=0;i<count;++i) {
        Progress(options,0,0);unsigned char header[46];
        Require(at<=limit && sizeof(header)<=limit-at);file.Read(at,header,sizeof(header));
        Require(U32(header)==0x02014b50);
        const uint32_t names=U16(header+28),extra=U16(header+30),comment=U16(header+32);
        Require(names && uint64_t(46)+names+extra+comment<=limit-at);
        Entry entry;entry.flags=U16(header+8);entry.method=U16(header+10);
        entry.crc=U32(header+16);entry.packed=U32(header+20);entry.length=U32(header+24);entry.offset=U32(header+42);
        Require(entry.packed!=0xffffffff && entry.length!=0xffffffff && entry.offset!=0xffffffff && U16(header+34)==0,TTPCOMM_ARCHIVE_UNSUPPORTED);
        entry.raw.resize(names);file.Read(at+46,entry.raw.data(),names);entry.name=Name(entry.raw);
        entry.directory=entry.name.back()==L'\\';result.push_back(std::move(entry));
        at+=46+names+extra+comment;
    }
    Require(at==limit);return result;
}
void Extract(File& file,const Entry& entry,const TtpCommArchiveOptions* options,Sink& sink) {
    Require(!entry.directory,TTPCOMM_ARCHIVE_UNSUPPORTED);
    Require(!(entry.flags&1),TTPCOMM_ARCHIVE_PASSWORD);
    Require(entry.method==0 || entry.method==8,TTPCOMM_ARCHIVE_UNSUPPORTED);
    const uint64_t maximum=options && options->member_limit_bytes ? options->member_limit_bytes : 8ULL*1024*1024*1024;
    Require(entry.length<=maximum,HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
    unsigned char local[30];file.Read(entry.offset,local,sizeof(local));
    Require(U32(local)==0x04034b50 && U16(local+8)==entry.method && ((U16(local+6)^entry.flags)&0x809)==0);
    const uint32_t names=U16(local+26),extra=U16(local+28);
    Require(names==entry.raw.size());std::string name(names,'\0');file.Read(uint64_t(entry.offset)+30,name.data(),names);
    Require(name==entry.raw);
    if(!(entry.flags&8)) Require(U32(local+14)==entry.crc && U32(local+18)==entry.packed && U32(local+22)==entry.length);
    const uint64_t payload=uint64_t(entry.offset)+30+names+extra;
    Require(payload<=file.length && entry.packed<=file.length-payload);
    Check(sink.Begin(entry.length));uint64_t produced=0,consumed=0;uLong crc=crc32(0,nullptr,0);
    unsigned char input[16384],output[16384];
    const auto write=[&](const void* data,uint32_t count) {
        Require(produced<=entry.length && count<=entry.length-produced);
        Check(sink.Write(data,count));crc=crc32(crc,static_cast<const Bytef*>(data),count);produced+=count;
        Progress(options,produced,entry.length);
    };
    if(entry.method==0) {
        Require(entry.packed==entry.length);
        while(consumed<entry.packed) {
            Progress(options,produced,entry.length);
            const DWORD count=static_cast<DWORD>(std::min<uint64_t>(sizeof(input),entry.packed-consumed));
            file.Read(payload+consumed,input,count);write(input,count);consumed+=count;
        }
    } else {
        struct Inflater { z_stream z{};bool initialized{};~Inflater(){if(initialized)inflateEnd(&z);} } inflater;
        Require(inflateInit2(&inflater.z,-15)==Z_OK,E_OUTOFMEMORY);inflater.initialized=true;
        int result=Z_OK;
        while(result!=Z_STREAM_END) {
            Progress(options,produced,entry.length);
            if(!inflater.z.avail_in && consumed<entry.packed) {
                const DWORD count=static_cast<DWORD>(std::min<uint64_t>(sizeof(input),entry.packed-consumed));
                file.Read(payload+consumed,input,count);consumed+=count;
                inflater.z.next_in=input;inflater.z.avail_in=count;
            }
            inflater.z.next_out=output;inflater.z.avail_out=sizeof(output);
            const auto before=inflater.z.avail_in;result=inflate(&inflater.z,Z_NO_FLUSH);
            Require(result==Z_OK || result==Z_STREAM_END);
            const uint32_t count=sizeof(output)-inflater.z.avail_out;write(output,count);
            // Music 0041F185 requires the stream terminator, unlike skin F481.
            Require(result==Z_STREAM_END || count || before!=inflater.z.avail_in);
        }
        Require(consumed-inflater.z.avail_in==entry.packed);
    }
    Require(produced==entry.length);
    Require(static_cast<uint32_t>(crc)==entry.crc,HRESULT_FROM_WIN32(ERROR_CRC));
}
}
HRESULT EnumerateZip(const wchar_t* path,const TtpCommArchiveOptions* options,TtpCommArchiveVisitor visitor,void* context) {
    if(!path || !*path || !visitor || (options && options->size!=sizeof(*options))) return E_INVALIDARG;
    try {
        File file(path);const auto entries=Directory(file,options);uint32_t index=0;
        for(const auto& item:entries) {
            TtpCommArchiveEntry entry{};entry.size=sizeof(entry);entry.index=index++;
            entry.name=item.name.c_str();entry.unpacked_bytes=item.length;entry.packed_bytes=item.packed;
            if(item.directory)entry.flags|=TTPCOMM_ARCHIVE_DIRECTORY;
            if(item.flags&1)entry.flags|=TTPCOMM_ARCHIVE_ENCRYPTED;
            Progress(options,0,0);
            if(!visitor(context,&entry)) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        }
        return S_OK;
    } catch(const Failure& f){return f.hr;} catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
      catch(...){return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);}
}
HRESULT ExtractZip(const wchar_t* path,const wchar_t* member,const TtpCommArchiveOptions* options,Sink& sink) {
    if(!path || !*path || !member || !*member || (options && options->size!=sizeof(*options))) return E_INVALIDARG;
    try {
        File file(path);const auto entries=Directory(file,options);
        std::wstring name(member);std::replace(name.begin(),name.end(),L'/',L'\\');
        for(const auto& entry:entries) if(!_wcsicmp(entry.name.c_str(),name.c_str())) {
            Extract(file,entry,options,sink);return S_OK;
        }
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    } catch(const Failure& f){return f.hr;} catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
      catch(...){return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);}
}
}
