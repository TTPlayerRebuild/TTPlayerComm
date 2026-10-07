#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
bool alphanumeric(unsigned char c) noexcept {return std::isalnum(c)!=0;}
struct Device {
    HANDLE handle=INVALID_HANDLE_VALUE;
    ~Device(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
    void open(unsigned number,bool scsi,DWORD access) noexcept {
        char path[64];std::sprintf(path,scsi?"\\\\.\\Scsi%u:":"\\\\.\\PhysicalDrive%u",scsi?number/2:number);
        handle=CreateFileA(path,access,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    }
    explicit operator bool()const noexcept{return handle!=INVALID_HANDLE_VALUE;}
};
void put32(unsigned char* buffer,unsigned offset,DWORD value) noexcept {std::memcpy(buffer+offset,&value,4);}
DWORD get32(const unsigned char* buffer,unsigned offset) noexcept {DWORD v;std::memcpy(&v,buffer+offset,4);return v;}
bool ataSerial(const unsigned char* identify,char* serial) noexcept {
    char text[21]{};
    for(unsigned i=0;i<20;i+=2){text[i]=static_cast<char>(identify[21+i]);text[i+1]=static_cast<char>(identify[20+i]);}
    for(int i=19;i>0 && text[i]==' ';--i)text[i]=0;
    if(alphanumeric(text[0])||alphanumeric(text[19]))std::strcpy(serial,text);
    return true;
}
bool smartSerial(unsigned disk,char* serial) noexcept {
    Device device;device.open(disk,false,GENERIC_READ|GENERIC_WRITE);if(!device)return false;
    unsigned char version[24]{};DWORD returned=0;
    if(!DeviceIoControl(device.handle,0x74080,nullptr,0,version,sizeof(version),&returned,nullptr))return false;
    if(!version[3])return false;
    unsigned char request[32]{},reply[528]{};
    put32(request,0,512);request[5]=1;request[9]=static_cast<unsigned char>(((disk&1)|0xfa)<<4);
    request[10]=((version[3]>>disk)&0x10)?0xa1:0xec;request[12]=static_cast<unsigned char>(disk);
    if(!DeviceIoControl(device.handle,0x7c088,request,sizeof(request),reply,sizeof(reply),&returned,nullptr))return false;
    if(returned<56)return false;
    return ataSerial(reply+16,serial);
}
bool scsiSerial(unsigned disk,char* serial) noexcept {
    Device device;device.open(disk,true,GENERIC_READ|GENERIC_WRITE);if(!device)return false;
    unsigned char data[557]{};DWORD returned=0;
    put32(data,0,28);std::memcpy(data+4,"SCSIDISK",8);put32(data,12,10000);
    put32(data,16,0x1b0501);put32(data,24,0x211);data[38]=0xec;data[40]=static_cast<unsigned char>(disk&1);
    if(!DeviceIoControl(device.handle,0x4d008,data,60,data,sizeof(data),&returned,nullptr))return false;
    if(returned<100 || !data[98])return false;
    return ataSerial(data+44,serial);
}
unsigned hexDigit(unsigned char c) noexcept {
    if(c>='0'&&c<='9')return c-'0';
    if(c>='A'&&c<='F')return c-'A'+10;
    if(c>='a'&&c<='f')return c-'a'+10;
    return 0;
}
void storageHex(const char* input,size_t length,char* output) noexcept {
    // 60001BC0 decodes pairs in swapped-word order; it is not plain text.
    unsigned written=0;
    for(size_t i=0;i<length && written<1023;i+=4)for(unsigned offset:{2u,0u}) {
        auto digit=[&](size_t at){return at<length?hexDigit(static_cast<unsigned char>(input[at])):0;};
        const unsigned value=digit(i+offset)*16+digit(i+offset+1);
        if(value && written<1023)output[written++]=static_cast<char>(value);
    }
    output[written]=0;
}
bool storageSerial(unsigned disk,char* serial) noexcept {
    Device device;device.open(disk,false,0);if(!device)return false;
    unsigned char query[12]{},data[10000]{};DWORD returned=0;char text[1024]{};
    if(DeviceIoControl(device.handle,0x2d1400,query,sizeof(query),data,sizeof(data),&returned,nullptr) && returned>=28) {
        DWORD offset=get32(data,24);const size_t available=std::min<DWORD>(returned,sizeof(data));
        if(offset<available) {
            const auto* start=reinterpret_cast<const char*>(data+offset);
            storageHex(start,strnlen(start,available-offset),text);
            if(alphanumeric(text[0])||alphanumeric(text[19])){std::strcpy(serial,text);return true;}
        }
    }
    std::memset(data,0,sizeof(data));returned=0;
    if(DeviceIoControl(device.handle,0x2d0c10,nullptr,0,data,sizeof(data),&returned,nullptr) && returned>16) {
        std::memset(text,0,sizeof(text));
        const size_t count=std::min<size_t>(std::min<DWORD>(returned,sizeof(data))-16,1023);
        std::memcpy(text,data+16,strnlen(reinterpret_cast<const char*>(data+16),count));
        if(alphanumeric(text[0])||alphanumeric(text[19])){std::strcpy(serial,text);return true;}
    }
    return false;
}
}
extern "C" int __cdecl ttp_machine_mac(char* output,unsigned capacity) noexcept {
    alignas(IP_ADAPTER_INFO) unsigned char storage[0x2808]{};ULONG bytes=sizeof(storage);
    auto* adapter=reinterpret_cast<IP_ADAPTER_INFO*>(storage);
    if(GetAdaptersInfo(adapter,&bytes)!=ERROR_SUCCESS || !output || capacity<=24)return 0;
    const auto* mac=adapter->Address;
    if(!(mac[0]|mac[1]|mac[2]) || !(mac[3]|mac[4]|mac[5]) || (mac[0]&3)==1)return 0;
    std::sprintf(output,"%02X%02X%02X%02X%02X%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    return 12;
}
extern "C" size_t __cdecl ttp_machine_disk(char* output,size_t capacity) noexcept {
    if(!output && capacity)return 0;
    char serial[1024]{};bool found=false;
    // NT ordering from 60002540. XP and later are the supported platforms.
    for(auto reader:{smartSerial,scsiSerial,storageSerial}) {
        for(unsigned disk=0;disk<32;++disk) {
            serial[0]=0;if(reader(disk,serial)&&serial[0]){found=true;break;}
        }
        if(found)break;
    }
    if(!found)return 0;
    const char* first=serial;while(*first==' ')++first;
    size_t length=0;
    for(const char* p=first;*p;++p) {
        if(!alphanumeric(*p)&&*p!='-'&&*p!='_'){length=0;break;}
        serial[length++]=*p;
    }
    serial[length]=0;
    if(capacity)std::strncpy(output,serial,capacity);
    return std::min(length,capacity);
}
extern "C" int __cdecl ttp_machine_token(char* output,int capacity,unsigned char* flags) noexcept {
    if(!output || !flags || capacity<=6)return 0;
    int used=6;output[used]=0;if(!*flags)*flags=3;
    for(unsigned bit:{1u,2u})if(*flags&bit) {
        char value[1024]{};
        const size_t length=bit==1?ttp_machine_mac(value,sizeof(value)):ttp_machine_disk(value,sizeof(value));
        // The old check omitted the five-byte separator, allowing an overflow.
        if(length && length+5<static_cast<unsigned>(capacity-used)) {
            std::memcpy(output+used,bit==1?"&mac=":"&hds=",5);used+=5;
            std::memcpy(output+used,value,length+1);used+=static_cast<int>(length);
        }else *flags=static_cast<unsigned char>(*flags&~bit);
    }
    std::memcpy(output,"uid=",4);output[4]=static_cast<char>((*flags>>4)+'0');output[5]=static_cast<char>((*flags&15)+'0');
    return used;
}
