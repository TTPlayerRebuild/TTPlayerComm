#include "archive_group.h"
#include <algorithm>

namespace ttpcomm::archive {
thread_local VolumeContext* active_volumes{};
namespace {
std::wstring Full(const std::wstring& path){
    archive_volumes::Check(!path.empty() && path.find(L'|')==std::wstring::npos,E_INVALIDARG);
    const DWORD n=GetFullPathNameW(path.c_str(),0,nullptr,nullptr);
    archive_volumes::Check(n && n<=32768,HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME));
    std::vector<wchar_t> full(n);archive_volumes::Check(GetFullPathNameW(path.c_str(),n,full.data(),nullptr)!=0);
    return full.data();
}
}
std::wstring VolumeContext::Resolve(const std::wstring& expected){
    if(canceled && canceled())throw HRESULT_FROM_WIN32(ERROR_CANCELLED);
    const auto name=Full(expected);
    // A real volume at its expected location takes precedence over old session
    // mappings. Its identity is validated afresh before a cache can be reused.
    const auto attr=GetFileAttributesW(name.c_str());
    if(attr!=INVALID_FILE_ATTRIBUTES && !(attr&FILE_ATTRIBUTE_DIRECTORY))return name;
    for(const auto& pair:mappings)if(!_wcsicmp(pair.first.c_str(),name.c_str()))return pair.second;
    if(!options.resolve)throw TTPCOMM_ARCHIVE_MISSING_VOLUME;
    std::vector<wchar_t> out(32768);
    const auto hr=options.resolve(options.context,archive.c_str(),name.c_str(),out.data(),static_cast<uint32_t>(out.size()));
    if(FAILED(hr))throw hr;
    archive_volumes::Check(hr==S_OK,TTPCOMM_ARCHIVE_MISSING_VOLUME);
    archive_volumes::Check(wmemchr(out.data(),0,out.size()) && out[0],E_INVALIDARG);
    auto actual=Full(out.data());
    archive_volumes::File check(actual);
    archive_volumes::Check(mappings.size()<4096,TTPCOMM_ARCHIVE_UNSUPPORTED);
    mappings.emplace_back(name,actual);return actual;
}
void VolumeContext::TrackRar(const wchar_t* actual){
    if(canceled && canceled())throw HRESULT_FROM_WIN32(ERROR_CANCELLED);
    const auto name=Full(actual);
    if(!rar_paths.empty() && !_wcsicmp(rar_paths.back().c_str(),name.c_str()))return;
    for(const auto& p:rar_paths)archive_volumes::Check(_wcsicmp(p.c_str(),name.c_str())!=0,HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    archive_volumes::Check(rar_paths.size()<4096,TTPCOMM_ARCHIVE_UNSUPPORTED);
    rar_files.push_back(std::make_unique<archive_volumes::File>(name));rar_paths.push_back(name);
}
std::unique_ptr<archive_volumes::Set> VolumeContext::Prepare(const wchar_t* path,const TtpCommArchiveOptions* limits){
    archive=Full(archive_format::Canonical(path));
    if(archive_format::Detect(path)==archive_format::Kind::rar){
        VolumeScope scope(*this);const auto hr=DiscoverRarVolumes(path,limits);if(FAILED(hr))throw hr;
        auto result=std::make_unique<archive_volumes::Set>(rar_paths.front(),rar_paths,canceled);
        rar_files.clear();return result;
    }
    return std::make_unique<archive_volumes::Set>(path,canceled,[this](const auto& p){return Resolve(p);});
}
}
