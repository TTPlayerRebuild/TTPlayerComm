#include "archive_internal.h"
#include "archive_group.h"
#include <zlib.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include "archive_zip_crypto.h"
#include <ttpcomm/archive_volumes.h>

namespace ttpcomm::archive {
namespace {
struct Failure { HRESULT hr; };
void Require(bool ok,HRESULT hr=HRESULT_FROM_WIN32(ERROR_INVALID_DATA)) { if(!ok) throw Failure{hr}; }
void Check(HRESULT hr) { if(FAILED(hr)) throw Failure{hr}; }
uint16_t U16(const unsigned char* p) { return uint16_t(p[0]|(uint16_t(p[1])<<8)); }
uint32_t U32(const unsigned char* p) { return uint32_t(U16(p))|(uint32_t(U16(p+2))<<16); }
uint64_t U64(const unsigned char* p) { return uint64_t(U32(p))|(uint64_t(U32(p+4))<<32); }
struct File {
    archive_volumes::Set volumes; uint64_t length{};
    explicit File(const wchar_t* path,const TtpCommArchiveOptions* options):volumes(path,[options]{
        return options && options->progress && !options->progress(options->context,0,0);
    },VolumeResolver()),length(volumes.Length()){}
    void Read(uint64_t at,void* data,DWORD count) {volumes.Read(at,data,count);}
};
struct Entry {
    std::string raw; std::wstring name;
    uint64_t packed{},length{},offset{};
    uint32_t crc{},disk{};
    uint16_t flags{},method{},time{},aes_version{},aes_method{};
    unsigned aes_strength{};
    bool directory{};
};
void Progress(const TtpCommArchiveOptions* options,uint64_t done,uint64_t total) {
    if(options && options->progress && !options->progress(options->context,done,total))
        throw Failure{HRESULT_FROM_WIN32(ERROR_CANCELLED)};
}
// 0041EF26/0041F5BA: OEM -> Unicode, then match Unicode names. Never fold
// individual bytes: DBCS trail bytes can look like ASCII uppercase letters.
std::wstring Name(const std::string& raw, bool utf8=false) {
    Require(raw.find('\0')==std::string::npos);
    const UINT page=utf8?CP_UTF8:CP_OEMCP;const DWORD flags=utf8?MB_ERR_INVALID_CHARS:0;
    const int n=MultiByteToWideChar(page,flags,raw.data(),static_cast<int>(raw.size()),nullptr,0);
    Require(n>0);std::wstring name(n,L'\0');
    Require(MultiByteToWideChar(page,flags,raw.data(),static_cast<int>(raw.size()),name.data(),n)==n);
    std::replace(name.begin(),name.end(),L'/',L'\\');return name;
}
// Extra fields are length-delimited; Unicode Path is authoritative only when
// its CRC matches the original filename. Legacy unmarked names stay OEM.
void Extra(Entry& entry,const std::vector<unsigned char>& bytes,bool central) {
    const bool need_length=entry.length==UINT32_MAX,need_packed=entry.packed==UINT32_MAX;
    const bool need_offset=central && entry.offset==UINT32_MAX;
    const bool need_disk=central && entry.disk==UINT16_MAX;
    bool zip64=!(need_length || need_packed || need_offset || need_disk);
    for(size_t p=0;p<bytes.size();) {
        Require(bytes.size()-p>=4);const auto id=U16(bytes.data()+p),n=U16(bytes.data()+p+2);p+=4;
        Require(n<=bytes.size()-p);const auto* data=bytes.data()+p;
        if(id==0x9901) {
            Require(n==7 && data[2]=='A' && data[3]=='E' && data[4]>=1 && data[4]<=3,TTPCOMM_ARCHIVE_UNSUPPORTED);
            entry.aes_version=U16(data);entry.aes_strength=data[4];entry.aes_method=U16(data+5);
            Require(entry.aes_version==1 || entry.aes_version==2,TTPCOMM_ARCHIVE_UNSUPPORTED);
        } else if(id==1 && !zip64) {
            size_t cursor=0;
            const auto read=[&](){Require(n-cursor>=8);const auto value=U64(data+cursor);cursor+=8;return value;};
            if(need_length)entry.length=read();if(need_packed)entry.packed=read();if(need_offset)entry.offset=read();
            if(need_disk){Require(n-cursor>=4);entry.disk=U32(data+cursor);}
            zip64=true;
        } else if(central && id==0x7075 && !(entry.flags&0x800) && n>=5 && data[0]==1 &&
            U32(data+1)==uint32_t(crc32(0,reinterpret_cast<const Bytef*>(entry.raw.data()),static_cast<uInt>(entry.raw.size())))) {
            entry.name=Name(std::string(reinterpret_cast<const char*>(data+5),n-5),true);
        }
        p+=n;
    }
    Require(zip64);
}
std::vector<Entry> Directory(File& file,const TtpCommArchiveOptions* options) {
    Progress(options,0,0);Require(file.length>=22);
    const DWORD tail_size=static_cast<DWORD>(std::min<uint64_t>(file.length,65535+22));
    std::vector<unsigned char> tail(tail_size);file.Read(file.length-tail_size,tail.data(),tail_size);
    size_t e=tail.size()-22;
    for(;;) {
        if(U32(tail.data()+e)==0x06054b50 && e+22+U16(tail.data()+e+20)==tail.size()) break;
        Require(e!=0,file.volumes.Split()?TTPCOMM_ARCHIVE_MISSING_VOLUME:HRESULT_FROM_WIN32(ERROR_INVALID_DATA));--e;
    }
    const auto* end=tail.data()+e;
    uint32_t disk=U16(end+4),central_disk=U16(end+6);
    Require(U16(end+8)<=U16(end+10));
    if(file.volumes.Disks()==1)Require(U16(end+8)==U16(end+10));
    uint64_t count=U16(end+10),bytes=U32(end+12),offset=U32(end+16);
    const uint64_t end_at=file.length-tail_size+e;
    uint64_t directory_end=end_at;
    if(count==0xffff || bytes==0xffffffff || offset==0xffffffff || disk==0xffff || central_disk==0xffff) {
        Require(end_at>=20);unsigned char locator[20];file.Read(end_at-20,locator,sizeof(locator));
        Require(U32(locator)==0x07064b50);
        Require(U32(locator+16)==file.volumes.Disks(),TTPCOMM_ARCHIVE_MISSING_VOLUME);
        directory_end=file.volumes.DiskOffset(U32(locator+4),U64(locator+8));Require(directory_end<=end_at-20 && end_at-20-directory_end>=56);
        unsigned char record[56];file.Read(directory_end,record,sizeof(record));
        Require(U32(record)==0x06064b50 && U64(record+4)>=44 && U64(record+4)<=end_at-20-directory_end-12);
        disk=U32(record+16);central_disk=U32(record+20);Require(U64(record+24)<=U64(record+32));
        if(file.volumes.Disks()==1)Require(U64(record+24)==U64(record+32));
        count=U64(record+32);bytes=U64(record+40);offset=U64(record+48);
    }
    Require(disk+1==file.volumes.Disks() && central_disk<=disk,TTPCOMM_ARCHIVE_MISSING_VOLUME);
    offset=file.volumes.DiskOffset(central_disk,offset);
    const auto entries=options && options->entry_limit?options->entry_limit:100000u;
    Require(count<=entries,HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES));
    Require(offset<=directory_end && bytes<=directory_end-offset && count<=bytes/46);
    uint64_t at=offset;const uint64_t limit=uint64_t(offset)+bytes;
    uint64_t metadata=count*sizeof(Entry);Require(metadata<=64u*1024*1024,E_OUTOFMEMORY);
    std::vector<Entry> result;result.reserve(static_cast<size_t>(count));
    for(uint32_t i=0;i<count;++i) {
        Progress(options,0,0);unsigned char header[46];
        Require(at<=limit && sizeof(header)<=limit-at);file.Read(at,header,sizeof(header));
        Require(U32(header)==0x02014b50);
        const uint32_t names=U16(header+28),extra=U16(header+30),comment=U16(header+32);
        Require(names && uint64_t(46)+names+extra+comment<=limit-at);
        Entry entry;entry.flags=U16(header+8);entry.method=U16(header+10);entry.time=U16(header+12);
        entry.crc=U32(header+16);entry.packed=U32(header+20);entry.length=U32(header+24);entry.offset=U32(header+42);
        entry.disk=U16(header+34);
        metadata+=names;Require(metadata<=64u*1024*1024,E_OUTOFMEMORY);
        entry.raw.resize(names);file.Read(at+46,entry.raw.data(),names);entry.name=Name(entry.raw,(entry.flags&0x800)!=0);
        std::vector<unsigned char> fields(extra);file.Read(at+46+names,fields.data(),extra);Extra(entry,fields,true);
        entry.offset=file.volumes.DiskOffset(entry.disk,entry.offset);
        metadata+=uint64_t(entry.name.size())*sizeof(wchar_t);Require(metadata<=64u*1024*1024,E_OUTOFMEMORY);
        entry.directory=entry.name.back()==L'\\';result.push_back(std::move(entry));
        at+=46+names+extra+comment;
    }
    Require(at==limit);return result;
}
void Extract(File& file,const Entry& entry,const TtpCommArchiveOptions* options,Sink& sink,UINT password_page=CP_UTF8) {
    Require(!entry.directory,TTPCOMM_ARCHIVE_UNSUPPORTED);
    const bool encrypted=(entry.flags&1)!=0,aes=entry.method==99;
    const HRESULT invalid=encrypted?TTPCOMM_ARCHIVE_BAD_PASSWORD:HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    Require(!(entry.flags&0x2040),TTPCOMM_ARCHIVE_UNSUPPORTED); // PKWARE strong encryption / masked headers
    Require(!aes || (encrypted && entry.aes_version),TTPCOMM_ARCHIVE_UNSUPPORTED);
    const auto method=aes?entry.aes_method:entry.method;
    Require(method==0 || method==8,TTPCOMM_ARCHIVE_UNSUPPORTED);
    Require(!encrypted || (options && options->password),TTPCOMM_ARCHIVE_PASSWORD);
    const uint64_t maximum=options && options->member_limit_bytes ? options->member_limit_bytes : 8ULL*1024*1024*1024;
    Require(entry.length<=maximum,HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
    unsigned char local[30];file.Read(entry.offset,local,sizeof(local));
    Require(U32(local)==0x04034b50 && U16(local+8)==entry.method && ((U16(local+6)^entry.flags)&0x809)==0);
    const uint32_t names=U16(local+26),extra=U16(local+28);
    Require(names==entry.raw.size());std::string name(names,'\0');file.Read(uint64_t(entry.offset)+30,name.data(),names);
    Require(name==entry.raw);
    if(!(entry.flags&8)) {
        Entry sizes;sizes.length=U32(local+22);sizes.packed=U32(local+18);
        std::vector<unsigned char> fields(extra);file.Read(entry.offset+30+names,fields.data(),extra);Extra(sizes,fields,false);
        Require(U32(local+14)==entry.crc && sizes.packed==entry.packed && sizes.length==entry.length);
    }
    uint64_t payload=uint64_t(entry.offset)+30+names+extra,packed=entry.packed;
    Require(payload<=file.length && entry.packed<=file.length-payload);
    ZipCrypto crypto;unsigned char auth[10]{};
    if(encrypted) {
        if(aes) {
            const unsigned salt=4+entry.aes_strength*4;Require(packed>=salt+12);
            unsigned char header[18];file.Read(payload,header,salt+2);
            Require(crypto.Aes(options->password,password_page,entry.aes_strength,header,salt,header+salt),TTPCOMM_ARCHIVE_BAD_PASSWORD);
            file.Read(payload+packed-10,auth,10);payload+=salt+2;packed-=salt+12;
        } else {
            Require(packed>=12);unsigned char header[12];file.Read(payload,header,12);
            crypto.Traditional(options->password,password_page);crypto.Decode(header,12);
            Require(header[11]==static_cast<unsigned char>((entry.flags&8)?entry.time>>8:entry.crc>>24),TTPCOMM_ARCHIVE_BAD_PASSWORD);
            payload+=12;packed-=12;
        }
    }
    Check(sink.Begin(entry.length,(encrypted?TTPCOMM_ARCHIVE_ENCRYPTED:0)|(file.volumes.Split()?TTPCOMM_ARCHIVE_SPLIT:0)));uint64_t produced=0,consumed=0;uLong crc=crc32(0,nullptr,0);
    unsigned char input[16384],output[16384];
    const auto read=[&](uint64_t offset,DWORD count){file.Read(payload+offset,input,count);if(encrypted)crypto.Decode(input,count);};
    const auto write=[&](const void* data,uint32_t count) {
        Require(produced<=entry.length && count<=entry.length-produced,invalid);
        Check(sink.Write(data,count));crc=crc32(crc,static_cast<const Bytef*>(data),count);produced+=count;
        Progress(options,produced,entry.length);
    };
    if(method==0) {
        Require(packed==entry.length,invalid);
        while(consumed<packed) {
            Progress(options,produced,entry.length);
            const DWORD count=static_cast<DWORD>(std::min<uint64_t>(sizeof(input),packed-consumed));
            read(consumed,count);write(input,count);consumed+=count;
        }
    } else {
        struct Inflater { z_stream z{};bool initialized{};~Inflater(){if(initialized)inflateEnd(&z);} } inflater;
        Require(inflateInit2(&inflater.z,-15)==Z_OK,E_OUTOFMEMORY);inflater.initialized=true;
        int result=Z_OK;
        while(result!=Z_STREAM_END) {
            Progress(options,produced,entry.length);
            if(!inflater.z.avail_in && consumed<packed) {
                const DWORD count=static_cast<DWORD>(std::min<uint64_t>(sizeof(input),packed-consumed));
                read(consumed,count);consumed+=count;
                inflater.z.next_in=input;inflater.z.avail_in=count;
            }
            inflater.z.next_out=output;inflater.z.avail_out=sizeof(output);
            const auto before=inflater.z.avail_in;result=inflate(&inflater.z,Z_NO_FLUSH);
            Require(result==Z_OK || result==Z_STREAM_END,encrypted?TTPCOMM_ARCHIVE_BAD_PASSWORD:HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            const uint32_t count=sizeof(output)-inflater.z.avail_out;write(output,count);
            // Music 0041F185 requires the stream terminator, unlike skin F481.
            Require(result==Z_STREAM_END || count || before!=inflater.z.avail_in,invalid);
        }
        Require(consumed-inflater.z.avail_in==packed,invalid);
    }
    Require(produced==entry.length,invalid);
    if(aes)Require(crypto.Verify(auth),TTPCOMM_ARCHIVE_BAD_PASSWORD);
    if(!aes || entry.aes_version==1)Require(static_cast<uint32_t>(crc)==entry.crc,encrypted?TTPCOMM_ARCHIVE_BAD_PASSWORD:HRESULT_FROM_WIN32(ERROR_CRC));
}
}
HRESULT EnumerateZip(const wchar_t* path,const TtpCommArchiveOptions* options,TtpCommArchiveVisitor visitor,void* context) {
    if(!path || !*path || !visitor || (options && options->size!=sizeof(*options))) return E_INVALIDARG;
    try {
        File file(path,options);const auto entries=Directory(file,options);uint32_t index=0;
        for(const auto& item:entries) {
            TtpCommArchiveEntry entry{};entry.size=sizeof(entry);entry.index=index++;
            entry.name=item.name.c_str();entry.unpacked_bytes=item.length;entry.packed_bytes=item.packed;
            if(item.directory)entry.flags|=TTPCOMM_ARCHIVE_DIRECTORY;
            if(item.flags&1)entry.flags|=TTPCOMM_ARCHIVE_ENCRYPTED;
            if(file.volumes.Split())entry.flags|=TTPCOMM_ARCHIVE_SPLIT;
            Progress(options,0,0);
            if(!visitor(context,&entry)) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        }
        return S_OK;
    } catch(HRESULT hr){return hr;} catch(const Failure& f){return f.hr;} catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
      catch(...){return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);}
}
HRESULT ExtractZip(const wchar_t* path,const wchar_t* member,const TtpCommArchiveOptions* options,Sink& sink) {
    if(!path || !*path || !member || !*member || (options && options->size!=sizeof(*options))) return E_INVALIDARG;
    try {
        File file(path,options);const auto entries=Directory(file,options);
        std::wstring name(member);std::replace(name.begin(),name.end(),L'/',L'\\');
        for(const auto& entry:entries) if(!_wcsicmp(entry.name.c_str(),name.c_str())) {
            std::vector<UINT> pages{CP_UTF8};
            if((entry.flags&1) && options && options->password &&
                std::any_of(options->password,options->password+wcslen(options->password),[](wchar_t c){return c>127;})) {
                for(UINT page:{GetACP(),GetOEMCP()})if(std::find(pages.begin(),pages.end(),page)==pages.end())pages.push_back(page);
            }
            for(size_t index=0;index<pages.size();++index){
                HRESULT hr=S_OK;
                try{Extract(file,entry,options,sink,pages[index]);return S_OK;}
                catch(const Failure& failure){hr=failure.hr;}catch(HRESULT error){hr=error;}
                if(hr!=TTPCOMM_ARCHIVE_BAD_PASSWORD || index+1==pages.size())return hr;
                // No consumer can see candidate plaintext: encrypted opens wait
                // for the final CRC/authentication, including every retry.
                Check(sink.RetryEncrypted());Progress(options,0,entry.length);
            }
        }
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    } catch(HRESULT hr){return hr;} catch(const Failure& f){return f.hr;} catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
      catch(...){return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);}
}
}
