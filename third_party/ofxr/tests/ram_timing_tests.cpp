#include "xrfg/ram_timing.hpp"
#include "xrfg/d3d12_ram_timing.hpp"
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <vector>
#include <stdexcept>

namespace fs = std::filesystem;
void require(bool b, const char* text) { if(!b) throw std::runtime_error(text); }
std::string read(const fs::path& path) {
    std::ifstream f(path); return {std::istreambuf_iterator<char>(f),{}};
}
int main(int argc, char** argv) {
    if(argc==4 && std::string(argv[1])=="save") {
        fs::path saved; std::wstring error;
        return xrfg::save_ram_timing(std::stoul(argv[2]),argv[3],&saved,&error) ? 0 : 2;
    }
    const auto root=fs::temp_directory_path()/(L"xrfg-ram-v225-"+std::to_wstring(GetCurrentProcessId())+
        L"-"+std::to_wstring(GetTickCount64()));
    try {
        fs::create_directories(root);
        if(argc==2 && std::string(argv[1])=="disabled") {
            { std::ofstream ini(root/L"ofxr_bridge.ini");
              ini<<"[diagnostics]\nlogging_enabled=0\ntiming_capture=0\n"; }
            auto& disabled=xrfg::bridge_flight_logger();
            disabled.shutdown(); disabled.initialize(root);
            require(!disabled.ram_enabled() && !disabled.enabled() && disabled.log_path().empty(),
                "disabled capture created recorder");
            using O=xrfg::BridgeFlightOperation;
            const auto token=disabled.begin(O::application_wait_frame);
            require(!token.sequence && !token.start_counter,"disabled begin acquired timestamp/token");
            disabled.end(token,O::application_wait_frame,0);
            disabled.event(O::three_slot_handoff,0,1,0,0);
            // Null device/queue/lists deliberately prove the disabled helper
            // issues no calibration, allocations, EndQuery, Resolve or readback.
            xrfg::D3D12RamTiming<2> source;
            xrfg::D3D12RamTiming<5> synthesis;
            source.initialize(nullptr,nullptr,O::gpu_history);
            synthesis.initialize(nullptr,nullptr,O::gpu_synthesis);
            source.mark(nullptr,0); source.resolve(nullptr); source.submitted(1,1); source.harvest();
            synthesis.mark(nullptr,0); synthesis.resolve(nullptr); synthesis.submitted(1,1); synthesis.harvest();
            fs::path saved; std::wstring error;
            require(!xrfg::save_ram_timing(GetCurrentProcessId(),root,&saved,&error),"disabled mapping exists");
            for(const auto& file:fs::directory_iterator(root))
                require(file.path().extension()==L".ini","disabled diagnostics wrote a file");
            disabled.shutdown(); fs::remove_all(root);
            std::cout<<"RAM OFF: no mapping, event timestamp, file or GPU query/readback: PASS\n";
            return 0;
        }
        { std::ofstream ini(root/L"ofxr_bridge.ini");
          ini<<"[diagnostics]\nlogging_enabled=1\ntiming_capture=1\npose_cadence_capture=1\n"; }
        xrfg::BridgeFlightLogger logger; logger.initialize(root);
        require(logger.ram_enabled() && !logger.enabled() && logger.log_path().empty(),"RAM must not enable normal diagnostics");
        using O=xrfg::BridgeFlightOperation;
        require(!logger.begin(O::nvidia_gpu_stages).sequence,"unselected event captured");
        auto token=logger.begin(O::application_wait_frame,11,22,33);
        require((token.sequence&xrfg::kRamTimingToken)!=0,"missing RAM begin");
        logger.end(token,O::application_wait_frame,0,44,55,66);
        require(fs::is_empty(root)==false,"fixture missing");
        unsigned files=0; for(auto& p:fs::directory_iterator(root)) {++files; require(p.path().extension()==L".ini","premature disk write");}
        require(files==1,"recorder wrote before save");
        require(xrfg::pause_ram_timing(GetCurrentProcessId(),true),"pause failed");
        require(!logger.begin(O::application_wait_frame).sequence,"pause didn't stop records");
        require(xrfg::pause_ram_timing(GetCurrentProcessId(),false),"resume failed");
        // Verify the enlarged ring retains both ends of this payload.
        for(unsigned n=0;n<80000;++n) logger.event(O::three_slot_handoff,0,n,n-1,215);
        fs::path saved; std::wstring error;
        require(xrfg::save_ram_timing(GetCurrentProcessId(),root,&saved,&error),"save failed");
        const auto csv=read(saved);
        require(csv.find("schema=225")!=std::string::npos &&
            csv.find("three_slot_handoff,0,79999,79998,215")!=std::string::npos,
            "ring payload lost");
        require(csv.find("three_slot_handoff,0,1,0,215")!=std::string::npos,"early ring entry lost");
        // Cross-process reader while multiple writer threads are running.
        std::atomic<bool> stop{};
        std::atomic<std::uint64_t> count{};
        std::atomic<unsigned> registered{}, missing{};
        std::vector<std::jthread> writers;
        // Forty concurrently live owners reproduce V41's 8 mapped + 32 lost
        // threads. Keep startup owners alive while the late producers arrive.
        for(unsigned i=0;i<39;++i) writers.emplace_back([&,i]{
            const auto marker=logger.begin(O::application_wait_frame,9000+i,122,887);
            if(!marker.sequence) ++missing;
            logger.end(marker,O::application_wait_frame,0,9000+i,122,887);
            // Selected AFW identity, optical and compact read telemetry.
            for(const std::uint64_t stage : {89ULL,90ULL,101ULL,122ULL})
                logger.event(O::luke_afw_stage,0,stage,9000+i,887);
            ++registered;
            while(!stop.load()) {
                if(i<7) {
                    auto t=logger.begin(O::timing_worker_run,123,122,0);
                    logger.end(t,O::timing_worker_run,0,123,122,0); ++count;
                } else std::this_thread::yield();
            }
        });
        const auto registration_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(registered.load()!=39 && std::chrono::steady_clock::now()<registration_deadline)
            std::this_thread::yield();
        if(registered.load()!=39 || missing.load()!=0) {
            stop=true; writers.clear();
            require(false,"late producer threads did not receive independent lanes");
        }
        wchar_t exe[32768]{}; GetModuleFileNameW(nullptr,exe,32768);
        std::wstring command=L"\""+std::wstring(exe)+L"\" save "+std::to_wstring(GetCurrentProcessId())+
            L" \""+root.wstring()+L"\"";
        STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
        const bool created=CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)!=FALSE;
        DWORD result=99;
        if(created) {
            if(WaitForSingleObject(pi.hProcess,10000)==WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess,&result);
            CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
        }
        stop=true;writers.clear();
        require(created && result==0 && count>0,"concurrent cross-process capture failed");
        require(logger.begin(O::application_end_frame).sequence!=0,"capture did not resume");
        require(xrfg::pause_ram_timing(GetCurrentProcessId(),true),"second pause");
        require(xrfg::save_ram_timing(GetCurrentProcessId(),root,&saved,&error),"save paused");
        const auto forty_csv=read(saved);
        require(forty_csv.find("unmapped_threads=0") != std::string::npos,
            "producer/worker/director writer lanes were dropped");
        require(forty_csv.find("lane_count=64") != std::string::npos,
            "snapshot used obsolete eight-lane layout");
        for(unsigned i=7;i<39;++i)
            for(const std::uint64_t stage : {89ULL,90ULL,101ULL,122ULL})
                require(forty_csv.find("luke_afw_stage,0,"+std::to_string(stage)+","+
                    std::to_string(9000+i)+",887") != std::string::npos,
                    "late AFW/XR/SR producer stage was lost");
        require(!logger.begin(O::application_wait_frame).sequence,"save changed manual pause state");
        xrfg::pause_ram_timing(GetCurrentProcessId(),false);
        const auto before=std::chrono::steady_clock::now();
        for(unsigned n=0;n<200000;++n) logger.event(O::three_slot_handoff,0,n,0,0);
        const auto us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-before).count();
        std::cout<<"RAM capture record mean (local CPU test, not game validation): "<<us/200000<<" us\n";
        require(xrfg::save_ram_timing(GetCurrentProcessId(),root,&saved,&error),"wrapped ring save failed");
        const auto wrapped_csv=read(saved);
        require(wrapped_csv.find("capacity=262144")!=std::string::npos &&
            wrapped_csv.find("truncated_window=1")!=std::string::npos &&
            wrapped_csv.find("three_slot_handoff,0,199999,0,0")!=std::string::npos,
            "bounded ring did not expose truncation or retain newest event");
        logger.shutdown();
        require(!xrfg::pause_ram_timing(GetCurrentProcessId(),true),"mapping leaked after shutdown");
        // Retain mature cadence/AFW records, compact read and bounded ingress receipts.
        { std::ofstream ini(root/L"ofxr_bridge.ini");
          ini<<"[diagnostics]\nlogging_enabled=0\ntiming_capture=1\npose_cadence_capture=1\n"; }
        logger.initialize(root);
        require(logger.ram_enabled() && !logger.enabled(),"pose profile enabled normal logging");
        constexpr std::array<std::uint64_t,26> selected{28,71,82,89,90,101,102,103,104,122,123,124,125,126,127,128,129,130,131,132,944,945,946,947,948,949};
        constexpr std::array<std::uint64_t,54> unselected{83,84,85,200,208,320,360,400,480,520,105,121,133,139,140,141,145,146,155,156,160,204,212,325,354,397,909,910,911,912,913,914,915,916,917,918,919,920,921,930,931,932,933,934,935,936,937,938,939,940,941,942,943,999};
        for(const auto stage:selected) logger.event(O::luke_afw_stage,-731,stage,812,913);
        for(const auto stage:unselected) logger.event(O::luke_afw_stage,-732,stage,812,913);
        require(xrfg::save_ram_timing(GetCurrentProcessId(),root,&saved,&error),"pose profile save failed");
        const auto pose_csv=read(saved);
        for(const auto stage:selected)
            require(pose_csv.find("luke_afw_stage,-731,"+std::to_string(stage)+",812,913") !=
                std::string::npos,"pose profile filtered compact AFW or cadence telemetry");
        for(const auto stage:unselected)
            require(pose_csv.find("luke_afw_stage,-732,"+std::to_string(stage)+",812,913") ==
                std::string::npos,"pose profile admitted a removed diagnostic stage");
        logger.shutdown();
        require(!xrfg::pause_ram_timing(GetCurrentProcessId(),true),"pose mapping leaked after shutdown");
        fs::remove_all(root);
        std::cout<<"RAM retention, no disk before save, cross-process concurrent snapshot, pause/resume: PASS\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
