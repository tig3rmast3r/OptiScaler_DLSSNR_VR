// Exercises the actual optimized shipping DLL, using its own link map to call
// internal functions and hold its real discovery mutex. No test export or
// alternate implementation is added to the shipped runtime.
#include <Windows.h>
#include <mutex>
#include <cstdio>
#include "shipping_addresses.h"
HMODULE host;
HMODULE nvapi;
HANDLE ready, releaseWorker;
extern "C" __declspec(dllexport) void RunNvapiLoaderTest() {
    const auto load=reinterpret_cast<HMODULE(*)()>(reinterpret_cast<char*>(host)+loadRva);
    const auto query=reinterpret_cast<void*(__stdcall*)(unsigned)>(reinterpret_cast<char*>(host)+queryRva);
    nvapi=load();
    query(0x100); // Unknown interface must still return through the native path.
}
DWORD WINAPI holdDiscovery(void*) {
    auto& mutex=*reinterpret_cast<std::mutex*>(reinterpret_cast<char*>(host)+mutexRva);
    std::scoped_lock lock(mutex);
    SetEvent(ready);
    return WaitForSingleObject(releaseWorker,15000)==WAIT_OBJECT_0 ? 0 : 1;
}
int main() {
    host=LoadLibraryW(L"version.dll");
    if (!host) { std::printf("host load error %lu\n",GetLastError()); return 2; }
    ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    releaseWorker=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE worker=CreateThread(nullptr,0,holdDiscovery,nullptr,0,nullptr);
    if (WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0) return 3;
    std::puts("Actual shipping GPU cache mutex held; entering real loader callback");
    std::fflush(stdout);
    HMODULE fixture=LoadLibraryW(L"loader_fixture.dll");
    if (!fixture) return 4;
    SetEvent(releaseWorker);
    if (WaitForSingleObject(worker,2000)!=WAIT_OBJECT_0) return 5;
    if (!nvapi) return 6;
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nvapi,path,MAX_PATH);
    std::wprintf(L"PASS: actual DLL load/query returned with discovery mutex held; NVAPI=%ls\n",path);
    FreeLibrary(fixture);
    CloseHandle(worker); CloseHandle(ready); CloseHandle(releaseWorker);
    return 0;
}
