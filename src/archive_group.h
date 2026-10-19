#pragma once
#include "archive_internal.h"
#include <ttpcomm/archive_volumes.h>

namespace ttpcomm::archive {
std::wstring ArchivePath(const wchar_t*);
struct VolumeContext {
    std::wstring archive;
    TtpCommArchiveVolumeOptions options{};
    std::function<bool()> canceled;
    std::function<void()> progressed;
    std::vector<std::pair<std::wstring,std::wstring>> mappings;
    std::vector<std::unique_ptr<archive_volumes::File>> rar_files;
    std::vector<std::wstring> rar_paths;
    std::wstring Resolve(const std::wstring& expected);
    void TrackRar(const wchar_t* actual);
    std::unique_ptr<archive_volumes::Set> Prepare(const wchar_t*,const TtpCommArchiveOptions*);
};
extern thread_local VolumeContext* active_volumes;
struct VolumeScope {
    VolumeContext* previous;
    explicit VolumeScope(VolumeContext& context):previous(active_volumes){active_volumes=&context;}
    ~VolumeScope(){active_volumes=previous;}
};
inline std::wstring ResolveVolume(const std::wstring& path){return active_volumes?active_volumes->Resolve(path):path;}
inline std::function<std::wstring(const std::wstring&)> VolumeResolver(){
    return active_volumes?std::function<std::wstring(const std::wstring&)>(ResolveVolume):nullptr;
}
HRESULT DiscoverRarVolumes(const wchar_t*,const TtpCommArchiveOptions*);
BOOL QueryGroups(TtpCommArchiveGroupApi*);
}
