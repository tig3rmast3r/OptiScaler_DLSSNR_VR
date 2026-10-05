#pragma once
#include "xrfg/ram_timing.hpp"
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <sstream>
#include <map>
#include <array>
struct RamTimingFixture {
    std::filesystem::path root;
    explicit RamTimingFixture(bool enabled, bool pose_profile = false) {
        if(!enabled) return;
        root=std::filesystem::temp_directory_path()/(L"ofxr-ram-gpu-"+
            std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(root);
        {std::ofstream ini(root/L"ofxr_bridge.ini");ini<<"[diagnostics]\nlogging_enabled=0\ntiming_capture=1\npose_cadence_capture="<<(pose_profile?1:0)<<"\n";}
        xrfg::bridge_flight_logger().initialize(root);
    }
    void verify(bool expect_d3d11 = false, bool expect_gpu_delay = false) {
        if(root.empty()) return;
        std::filesystem::path saved;std::wstring error;
        if(!xrfg::save_ram_timing(GetCurrentProcessId(),root,&saved,&error))
            throw std::runtime_error("GPU timing snapshot failed");
        std::ifstream input(saved);
        const std::string text{std::istreambuf_iterator<char>(input),{}};
        for(const auto* event:{"three_slot_present", "timing_worker_run", "timing_slot_wait",
                "timing_director_wait", "three_slot_handoff", "internal_wait_frame",
                "timing_worker_prepare", "timing_synthesis_complete", "timing_aux_complete",
                "timing_worker_build"})
            if(text.find(event)==std::string::npos) throw std::runtime_error(event);
        if(xrfg::bridge_flight_logger().enabled()) throw std::runtime_error("RAM enabled normal logger");
        if(expect_d3d11 && (text.find("timing_publication_complete")==std::string::npos ||
                text.find("d3d11_publish")==std::string::npos))
            throw std::runtime_error("missing D3D11 publication stages");
        if(text.find(",gpu_timing_error,")!=std::string::npos)
            throw std::runtime_error("GPU timestamp failure");
        using Key=std::pair<uint64_t,uint64_t>;
        std::map<Key,std::array<uint64_t,6>> histories, syntheses;
        std::istringstream lines(text); std::string line;
        while(std::getline(lines,line)) {
            if(line.empty() || line[0]=='#') continue;
            std::istringstream fields(line); std::array<std::string,9> f;
            for(auto& v:f) std::getline(fields,v,',');
            if(f[4]!="gpu_history" && f[4]!="gpu_synthesis") continue;
            auto& map=f[4]=="gpu_history"?histories:syntheses;
            const auto point=std::stoul(f[5]);
            if(point>=6) throw std::runtime_error("GPU point out of range");
            map[{std::stoull(f[6]),std::stoull(f[7])}][point]=std::stoull(f[8]);
        }
        LARGE_INTEGER hz{}; QueryPerformanceFrequency(&hz);
        unsigned complete=0; bool delayed=false;
        for(const auto& [key,v]:syntheses) {
            if(!v[0] || !v[4] || !v[5]) continue; // final unharvested slots
            for(unsigned i=1;i<5;++i) if(!v[i] || v[i]<v[i-1])
                throw std::runtime_error("GPU stages not monotonic");
            const auto found=histories.find(key);
            if(found==histories.end() || !found->second[1]) continue;
            const auto& h=found->second;
            if(!h[0] || h[1]<h[0] || !h[2] || h[1]>v[0]+hz.QuadPart/1000)
                throw std::runtime_error("GPU source/synthesis alignment invalid");
            delayed |= v[0]>v[5] && v[0]-v[5]>static_cast<uint64_t>(hz.QuadPart/50);
            ++complete;
        }
        if(complete<2) throw std::runtime_error("missing correlated GPU source/synthesis timings");
        if(expect_gpu_delay && !delayed) throw std::runtime_error("queued delay not visible in GPU timestamp");
    }
    ~RamTimingFixture() {
        if(root.empty()) return;
        xrfg::bridge_flight_logger().shutdown();
        std::error_code error;std::filesystem::remove_all(root,error);
    }
};
