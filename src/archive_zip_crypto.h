#pragma once
#include <windows.h>
#include <wincrypt.h>
#include <array>
#include <string>
#include <cstring>

namespace ttpcomm::archive {
// WinZip AE-1/AE-2: PBKDF2-HMAC-SHA1 (1000), AES CTR, ciphertext HMAC.
// CryptoAPI's software provider is available on XP SP3. No CNG import.
class ZipCrypto {
    HCRYPTPROV provider_{}; HCRYPTKEY aes_{}; HCRYPTHASH auth_{};
    std::array<unsigned char,64> outer_{};
    unsigned char counter_[16]{},block_[16]{};unsigned used_{16};
    uint32_t keys_[3]{0x12345678,0x23456789,0x34567890};
    static uint32_t Crc(uint32_t crc,unsigned char c) {
        crc^=c;for(unsigned i=0;i<8;++i)crc=(crc>>1)^(0xedb88320u&uint32_t(-int(crc&1)));return crc;
    }
    void Update(unsigned char c){keys_[0]=Crc(keys_[0],c);keys_[1]=(keys_[1]+(keys_[0]&255))*134775813u+1;keys_[2]=Crc(keys_[2],static_cast<unsigned char>(keys_[1]>>24));}
    void Check(BOOL okay){if(!okay)throw HRESULT_FROM_WIN32(GetLastError());}
    void Hash(const unsigned char* a,DWORD n,const unsigned char* b,DWORD m,unsigned char* out) {
        HCRYPTHASH h{};Check(CryptCreateHash(provider_,CALG_SHA1,0,0,&h));
        struct End{HCRYPTHASH h;~End(){CryptDestroyHash(h);}}end{h};
        Check(CryptHashData(h,a,n,0));if(m)Check(CryptHashData(h,b,m,0));DWORD size=20;Check(CryptGetHashParam(h,HP_HASHVAL,out,&size,0));
    }
    void Hmac(const unsigned char* key,size_t size,const unsigned char* data,size_t n,unsigned char* out) {
        unsigned char k[64]{},inner[64],outer[64],digest[20];
        if(size>64)Hash(key,static_cast<DWORD>(size),nullptr,0,k);else memcpy(k,key,size);
        for(unsigned i=0;i<64;++i){inner[i]=k[i]^0x36;outer[i]=k[i]^0x5c;}
        Hash(inner,64,data,static_cast<DWORD>(n),digest);Hash(outer,64,digest,20,out);
        SecureZeroMemory(k,sizeof(k));SecureZeroMemory(inner,sizeof(inner));SecureZeroMemory(outer,sizeof(outer));
    }
public:
    bool aes{};
    ZipCrypto()=default;ZipCrypto(const ZipCrypto&)=delete;
    ~ZipCrypto(){if(auth_)CryptDestroyHash(auth_);if(aes_)CryptDestroyKey(aes_);if(provider_)CryptReleaseContext(provider_,0);
        SecureZeroMemory(keys_,sizeof(keys_));SecureZeroMemory(block_,sizeof(block_));SecureZeroMemory(outer_.data(),outer_.size());}
    static std::string Encode(const wchar_t* text,UINT page){
        BOOL used_default=FALSE;const bool utf=page==CP_UTF8;
        const DWORD flags=utf?0:WC_NO_BEST_FIT_CHARS;
        const int n=WideCharToMultiByte(page,flags,text,-1,nullptr,0,nullptr,utf?nullptr:&used_default);
        if(n<=0)throw HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        std::string out(n,'\0');WideCharToMultiByte(page,flags,text,-1,out.data(),n,nullptr,utf?nullptr:&used_default);out.pop_back();
        if(used_default){SecureZeroMemory(out.data(),out.size());throw TTPCOMM_ARCHIVE_BAD_PASSWORD;}return out;
    }
    void Traditional(const wchar_t* password,UINT page){auto text=Encode(password,page);for(unsigned char c:text)Update(c);SecureZeroMemory(text.data(),text.size());}
    bool Aes(const wchar_t* password,UINT page,unsigned strength,const unsigned char* salt,unsigned salt_size,const unsigned char* verify) {
        Check(CryptAcquireContextW(&provider_,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT));
        const unsigned key_size=8+8*strength,total=key_size*2+2;
        auto text=Encode(password,page);unsigned char derived[80]{},u[20],part[20],salt_block[20]{};
        memcpy(salt_block,salt,salt_size);
        for(unsigned offset=0,index=1;offset<total;offset+=20,++index){
            salt_block[salt_size+3]=static_cast<unsigned char>(index);
            Hmac(reinterpret_cast<const unsigned char*>(text.data()),text.size(),salt_block,salt_size+4,u);memcpy(part,u,20);
            for(unsigned round=1;round<1000;++round){Hmac(reinterpret_cast<const unsigned char*>(text.data()),text.size(),u,20,u);for(unsigned j=0;j<20;++j)part[j]^=u[j];}
            memcpy(derived+offset,part,std::min(20u,total-offset));
        }
        SecureZeroMemory(text.data(),text.size());
        const bool matches=derived[2*key_size]==verify[0] && derived[2*key_size+1]==verify[1];
        if(matches){
            struct KeyBlob{BLOBHEADER header;DWORD size;unsigned char key[32];}key{};
            key.header={PLAINTEXTKEYBLOB,CUR_BLOB_VERSION,0,static_cast<ALG_ID>(key_size==16?CALG_AES_128:key_size==24?CALG_AES_192:CALG_AES_256)};
            key.size=key_size;memcpy(key.key,derived,key_size);
            Check(CryptImportKey(provider_,reinterpret_cast<BYTE*>(&key),sizeof(BLOBHEADER)+4+key_size,0,0,&aes_));
            SecureZeroMemory(&key,sizeof(key));DWORD mode=CRYPT_MODE_ECB;Check(CryptSetKeyParam(aes_,KP_MODE,reinterpret_cast<BYTE*>(&mode),0));
            unsigned char inner[64];
            for(unsigned i=0;i<64;++i){const unsigned char k=i<key_size?derived[key_size+i]:0;inner[i]=k^0x36;outer_[i]=k^0x5c;}
            Check(CryptCreateHash(provider_,CALG_SHA1,0,0,&auth_));Check(CryptHashData(auth_,inner,64,0));SecureZeroMemory(inner,sizeof(inner));aes=true;
        }
        SecureZeroMemory(derived,sizeof(derived));SecureZeroMemory(u,sizeof(u));SecureZeroMemory(part,sizeof(part));return matches;
    }
    void Decode(unsigned char* bytes,DWORD count){
        if(aes){
            Check(CryptHashData(auth_,bytes,count,0));
            for(DWORD i=0;i<count;++i){
                if(used_==16){for(unsigned j=0;j<16;++j)if(++counter_[j])break;memcpy(block_,counter_,16);DWORD n=16;Check(CryptEncrypt(aes_,0,FALSE,0,block_,&n,16));used_=0;}
                bytes[i]^=block_[used_++];
            }
        }else for(DWORD i=0;i<count;++i){const uint32_t t=keys_[2]|2;bytes[i]^=static_cast<unsigned char>((t*(t^1))>>8);Update(bytes[i]);}
    }
    bool Verify(const unsigned char* expected){unsigned char inner[20],digest[20];DWORD n=20;Check(CryptGetHashParam(auth_,HP_HASHVAL,inner,&n,0));Hash(outer_.data(),64,inner,20,digest);
        unsigned diff=0;for(unsigned i=0;i<10;++i)diff|=digest[i]^expected[i];SecureZeroMemory(inner,sizeof(inner));SecureZeroMemory(digest,sizeof(digest));return !diff;}
};
}
