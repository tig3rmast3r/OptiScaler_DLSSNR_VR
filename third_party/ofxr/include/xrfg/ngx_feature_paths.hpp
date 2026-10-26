#pragma once
#include <windows.h>
#include <nvsdk_ngx_defs.h>
#include <string>
#include <utility>
#include <vector>

namespace xrfg {
inline std::wstring ngx_application_directory() {
    std::wstring path(32768,L'\0');
    const auto size=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(!size||size>=path.size())return {};
    path.resize(size);
    const auto separator=path.find_last_of(L"\\/");
    return separator==std::wstring::npos?std::wstring{}:path.substr(0,separator);
}

// NGX keeps the first initialization's search list for the process. Private
// OFXR initialization must include the game's models even before game NGX
// startup. Keep an existing caller's paths and logging/callback policy intact.
class NgxFeaturePaths {
    NVSDK_NGX_FeatureCommonInfo info_{};
    std::wstring application_,layer_;
    std::vector<const wchar_t*> paths_;
    bool changed_{};
    void append(const std::wstring& path) {
        if(path.empty())return;
        for(const auto* existing:paths_)
            if(existing&&!_wcsicmp(existing,path.c_str()))return;
        paths_.push_back(path.c_str());changed_=true;
    }
public:
    explicit NgxFeaturePaths(const NVSDK_NGX_FeatureCommonInfo* caller,
        std::wstring layer,std::wstring application=ngx_application_directory())
        :application_(std::move(application)),layer_(std::move(layer)) {
        if(caller)info_=*caller;
        if(caller&&caller->PathListInfo.Path)
            paths_.assign(caller->PathListInfo.Path,
                caller->PathListInfo.Path+caller->PathListInfo.Length);
        append(application_);append(layer_);
        info_.PathListInfo.Path=paths_.empty()?nullptr:paths_.data();
        info_.PathListInfo.Length=static_cast<unsigned int>(paths_.size());
    }
    NgxFeaturePaths(const NgxFeaturePaths&)=delete;
    NgxFeaturePaths& operator=(const NgxFeaturePaths&)=delete;
    const NVSDK_NGX_FeatureCommonInfo* get() const noexcept {return &info_;}
    bool changed() const noexcept {return changed_;}
};
}
