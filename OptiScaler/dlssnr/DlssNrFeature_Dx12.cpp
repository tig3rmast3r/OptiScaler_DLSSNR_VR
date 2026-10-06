#include "pch.h"

#include <set>

#include "DlssNr.h"
#include "NrBeforeSrContract.h"
#include "NrRetirementCadence.h"
#include "NrHistoryContract.h"
#include "NrGuideSnapshot.h"
#include "NrForwarderContract.h"


#include "DlssNr_Codec.h"
#include "DlssNr_Capture.h"
#include "DlssNr_Proxy.h"

#include <shaders/dlssnr/DlssNr_Dx12.h>

#include <Config.h>
#include <State.h>
#include <Util.h>

#include <proxies/NVNGX_Proxy.h>
#include <diagnostics/DredMarkers.h>
#include <inputs/NVNGX_DLSS.h>

#include <mutex>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <unordered_set>

namespace
{
class DredScope
{
  public:
    DredScope(ID3D12GraphicsCommandList* commandList, const wchar_t* label)
        : _commandList(commandList), _label(label)
    {
        DredMarkers::Begin(_commandList, label);
    }

    ~DredScope()
    {
        DredMarkers::End(_commandList, _label);
    }

    DredScope(const DredScope&) = delete;
    DredScope& operator=(const DredScope&) = delete;

  private:
    ID3D12GraphicsCommandList* _commandList = nullptr;
    const wchar_t* _label = nullptr;
};

// NGX result codes, by name.
//
// A user's log recently read "init 0x-452FFFFF", which is an int formatted as hex and is
// undiagnosable by anyone. It was 0xBAD00001, FeatureNotSupported -- a complete answer, printed as
// noise. Names cost nothing and turn a bug report into a diagnosis.
const char* NgxResultName(unsigned int r)
{
    switch (r)
    {
    case 0x1: return "Success";
    case 0xBAD00001: return "FAIL_FeatureNotSupported";
    case 0xBAD00002: return "FAIL_PlatformError";
    case 0xBAD00003: return "FAIL_FeatureAlreadyExists";
    case 0xBAD00004: return "FAIL_FeatureNotFound";
    case 0xBAD00005: return "FAIL_InvalidParameter";
    case 0xBAD00006: return "FAIL_ScratchBufferTooSmall";
    case 0xBAD00007: return "FAIL_NotInitialized";
    case 0xBAD00008: return "FAIL_UnsupportedInputFormat";
    case 0xBAD00009: return "FAIL_RWFlagMissing";
    case 0xBAD0000A: return "FAIL_MissingInput";
    case 0xBAD0000B: return "FAIL_UnableToInitializeFeature";
    case 0xBAD0000C: return "FAIL_OutOfDate";
    case 0xBAD0000D: return "FAIL_OutOfGPUMemory";
    case 0xBAD0000E: return "FAIL_UnsupportedFormat";
    case 0xBAD0000F: return "FAIL_UnableToWriteToAppDataPath";
    case 0xBAD00010: return "FAIL_UnsupportedParameter";
    case 0xBAD00011: return "FAIL_Denied";
    case 0xBAD00012: return "FAIL_NotImplemented";
    default: return "unknown";
    }
}

// Does the driver's own nvngx.dll dispatch Neural Rendering?
//
// The trick is that correct parameters are not needed to find out, because the KIND of failure is
// the answer. A dispatcher that has never heard of feature 18 rejects it before looking at anything:
//
//   FeatureNotFound / FeatureNotSupported / NotImplemented -- the driver does not route it, and the
//       forwarder is necessary rather than merely tolerated.
//   MissingInput / InvalidParameter / UnsupportedParameter -- the driver DOES route it. It reached
//       the feature, which then complained about the arguments. That is the win: it means the whole
//       forwarder, and the per-game copy of the model, can go.
//   Success -- better still, though not expected from an empty parameter block.
//
// Once per session, and only when asked for.
void ProbeProxyDispatch(ID3D12GraphicsCommandList* cmdList)
{
    static bool done = false;

    if (done)
        return;

    done = true;

    if (!NVNGXProxy::IsDx12Inited())
    {
        LOG_INFO("DLSS-NR proxy probe: the driver's nvngx is not initialised here, nothing to ask");
        return;
    }

    const auto allocate = NVNGXProxy::D3D12_AllocateParameters();
    const auto destroy = NVNGXProxy::D3D12_DestroyParameters();
    const auto create = NVNGXProxy::D3D12_CreateFeature();
    const auto release = NVNGXProxy::D3D12_ReleaseFeature();

    if (allocate == nullptr || create == nullptr)
    {
        LOG_INFO("DLSS-NR proxy probe: the driver's nvngx does not export what the probe needs");
        return;
    }

    NVSDK_NGX_Parameter* params = nullptr;

    if (allocate(&params) != NVSDK_NGX_Result_Success || params == nullptr)
    {
        LOG_INFO("DLSS-NR proxy probe: could not allocate a parameter block");
        return;
    }

    // Feature 18, and a feature that certainly does not exist, asked the same way.
    //
    // A single result cannot answer this. "UnableToInitializeFeature" for 18 looks like the
    // dispatcher having found the feature and failed to start it on an empty parameter block -- but
    // it might equally be what this dispatcher says about anything it cannot set up. The control
    // settles it: if a nonsense id comes back differently, the difference is knowledge of feature
    // 18. If both come back the same, the first result meant nothing.
    NVSDK_NGX_Handle* handle = nullptr;
    const auto result = (unsigned int) create(cmdList, (NVSDK_NGX_Feature) 18, params, &handle);

    if (handle != nullptr && release != nullptr)
        release(handle);

    NVSDK_NGX_Handle* controlHandle = nullptr;
    const auto control =
        (unsigned int) create(cmdList, (NVSDK_NGX_Feature) 200, params, &controlHandle);

    if (controlHandle != nullptr && release != nullptr)
        release(controlHandle);

    LOG_INFO("DLSS-NR proxy probe: feature 18 -> 0x{:X} ({}), control feature 200 -> 0x{:X} ({})",
             result, NgxResultName(result), control, NgxResultName(control));

    const bool rejectedOutright =
        result == 0xBAD00004 || result == 0xBAD00001 || result == 0xBAD00012;

    if (result == control)
        LOG_INFO("DLSS-NR proxy probe: both answers identical, so this says nothing about feature 18 "
                 "-- the driver treats it exactly as it treats a feature that does not exist");
    else if (rejectedOutright)
        LOG_INFO("DLSS-NR proxy probe: feature 18 is rejected outright -- the driver does not route "
                 "it and the forwarder is required");
    else
        LOG_INFO("DLSS-NR proxy probe: feature 18 answers differently from a nonexistent one, so the "
                 "driver knows it -- the forwarder and the per-game model copy could both go");

    if (destroy != nullptr)
        destroy(params);
}

// Everything the model is reached through. The snippet refuses callers whose module path does not
// contain "nvngx.dll", so the calls are made from a small library named for exactly that reason and
// shipped beside OptiScaler; see nvngx.dll_dlssnr.dll.
using PFN_NrCreate = void*(__cdecl*) (const wchar_t*, const wchar_t*, ID3D12Device*,
                                      ID3D12GraphicsCommandList*, void*, unsigned int, unsigned int, int,
                                      float, int, float, float, float, int, int);
using PFN_NrEvaluate = int(__cdecl*) (ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*,
                                      ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, unsigned int,
                                      unsigned int, unsigned int, unsigned int, int, int, float, int,
                                      float, float, float, int, float, float);
using PFN_NrRelease = void(__cdecl*) (void*);
using PFN_NrSetExtras = void(__cdecl*) (void*, float, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
                                        unsigned int, unsigned int, unsigned int, unsigned int);
using PFN_NrSetContract = void(__cdecl*) (void*, unsigned int, unsigned int, unsigned int,
                                          unsigned int, unsigned int, int, unsigned int);
using PFN_NrSetFloatSlot = void(__cdecl*) (int);
using PFN_NrProbeFloat = void(__cdecl*) (void*, const char*, float, int);

// The caller gate is process-wide. Mutable NGX call parameters live per source handle below.
struct NrRuntime
{
    HMODULE forwarder = nullptr;
    bool forwarderReady = false;
    PFN_NrCreate create = nullptr;
    PFN_NrEvaluate evaluate = nullptr;
    PFN_NrRelease release = nullptr;
    PFN_NrSetExtras setExtras = nullptr;
    PFN_NrSetContract setContract = nullptr;
    PFN_NrSetFloatSlot setFloatSlot = nullptr;
    PFN_NrProbeFloat probeFloat = nullptr;
    bool floatSlotKnown = false;
    int* lastInit = nullptr;
    int* lastCreate = nullptr;

    // Once the runtime itself fails there is no recovering it mid-session, and retrying every frame
    // turns a failure into a crash. Per-handle feature state lives separately below.
    bool failed = false;
    const char* reason = "";
};

// One independent model feature and scratch set per source DLSS handle. The signed feature keeps
// temporal history in its handle, so sharing one of these between two VR eyes makes every evaluate
// reproject the other eye's previous image.
struct NrPerHandle
{
    void* feature = nullptr;

    // Creation state stays owned by the feature for its complete lifetime.
    NVSDK_NGX_Parameter* params = nullptr;

    // NGX evaluate parameters are mutable call state. Per-handle ownership fixed cross-eye
    // mutation, but still reused one table while several command lists from the same eye were in
    // flight. Command-list identity alone is NOT a retirement proof (Reset can use another
    // allocator). Reuse is protected by the source handle's exact submission receipt and fence.
    std::unordered_map<ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*> evaluateParams;

    // The model cannot read and write one resource, so the frame is staged through these.
    ID3D12Resource* colorCopy = nullptr;
    ID3D12Resource* output = nullptr;

    // The frame as the upscaler wrote it. The resolve adds the model's edit to this rather than
    // reconstructing it by inverting the tone curve, which is what turned every light in the frame into
    // a string of coloured cells.
    ID3D12Resource* hdrCopy = nullptr;

    // Pre-SR Color is not necessarily UAV-capable. Resolve there and copy back only the active rect.
    ID3D12Resource* resolveCopy = nullptr;

    // The frame shrunk for the model, when it is working below full resolution.
    ID3D12Resource* colorSmall = nullptr;

    unsigned int workWidth = 0;
    unsigned int workHeight = 0;

    // Private per-handle guide snapshots, typed and typeless alike. Never hand
    // mutable game motion/depth surfaces directly to the NR backend.
    ID3D12Resource* depthClone = nullptr;
    ID3D12Resource* motionClone = nullptr;

    unsigned int width = 0;
    unsigned int height = 0;
    bool reset = true;
    bool sourceReset = false;
    unsigned long long sourceResetRequests = 0, lastSourceResetReport = 0;

    // Dimensions of the guides as the upscaler handed them over, kept for the present path, which runs
    // long after that call has returned.
    unsigned int guideWidth = 0;
    unsigned int guideHeight = 0;

    // How the game encodes its guides, as the game itself reports it. Captured with the guides, since
    // the finished-frame path runs long after the upscaler's call has returned.
    bool guideDepthInverted = false;
    float guideMvScaleX = 1.0f;
    float guideMvScaleY = 1.0f;

    // The values the live feature was created with, and when a difference from them was first seen.
    unsigned int builtPreset = 0;
    float builtIntensity = 0.0f;
    unsigned int builtStyle = 0;
    float builtLocalStructure = 0.0f;
    float builtLocalTone = 0.0f;
    float builtSkinStructure = 0.0f;
    bool builtAutoMask = false;
    unsigned long long settledAt = 0;

};

NrRuntime g_runtime;
int g_capabilityFloatSlot = -1;
std::unordered_map<unsigned int, NrPerHandle> g_nrByHandle;
std::unique_ptr<DlssNr_Dx12> g_compose;

void CaptureSourceHistoryReset(NrPerHandle& nr, unsigned int sourceHandleId,
                               NVSDK_NGX_Parameter* params)
{
    int sourceReset = 0;
    const bool available = params->Get(NVSDK_NGX_Parameter_Reset, &sourceReset) ==
                           NVSDK_NGX_Result_Success;
    nr.sourceReset = available && sourceReset != 0;
    nr.reset = DlssNr::NrResetWithSource(nr.reset, available, sourceReset);
    const auto* cfg = Config::Instance();
    if (!nr.sourceReset || !(cfg->LogToFile.value_or_default() ||
                            cfg->LogToConsole.value_or_default() || cfg->LogToNGX.value_or_default()))
        return;
    ++nr.sourceResetRequests;
    const auto now = GetTickCount64();
    if (nr.lastSourceResetReport == 0 || now - nr.lastSourceResetReport >= 1000)
    {
        LOG_INFO("V23228 NR source history reset handle {} source_requests {} pending {}",
                 sourceHandleId, nr.sourceResetRequests, nr.reset);
        nr.lastSourceResetReport = now;
    }
}

// Retire exact submissions before reusing each feature's parameters and scratch.
// DRED stops inside NR, but does not prove the opaque vendor backend's root cause.
// Different feature histories must not add cross-eye CPU waits: V23226 prolonged
// startup instability and its backend-global retirement policy was rejected.
struct NrHandleFlight
{
    bool awaitingSubmission = false;
    ID3D12GraphicsCommandList* recordedList = nullptr;
    unsigned long long recording = 0;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    unsigned long long fenceValue = 0;
};

struct NrQueueFence
{
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    unsigned long long nextValue = 0;
};

std::mutex g_nrFlightMutex;
std::unordered_map<unsigned int, NrHandleFlight> g_nrFlights;
DlssNr::NrSubmissionLedger g_nrSubmissionLedger;
std::unordered_map<ID3D12CommandQueue*, NrQueueFence> g_nrQueueFences;
std::atomic<unsigned long long> g_nrRetirementSkips { 0 };
std::atomic<bool> g_nrRetirementSignalFailed { false };

void MarkNrRecorded(unsigned int sourceHandleId, ID3D12GraphicsCommandList* commandList)
{
    // Luke Ross submits the native NGX command list outside the queue path observed by
    // OptiScaler. V30004 waited forever for a receipt that never arrives; keep the
    // V23291 native-NGX lifetime contract for that ingress only.
    if (commandList == nullptr || LukeRossDlssIngress::Detected())
        return;

    std::lock_guard<std::mutex> lock(g_nrFlightMutex);
    NrHandleFlight& flight = g_nrFlights[sourceHandleId];
    flight.awaitingSubmission = true;
    flight.recordedList = commandList;
    flight.recording = g_nrSubmissionLedger.Record(sourceHandleId, commandList).recording;
    if (NrFlightTrace::Enabled()) {
        wchar_t label[160] {};
        swprintf_s(label, L"NR_RECORD handle=%u recording=%llu", sourceHandleId, flight.recording);
        NrFlightTrace::Write("NR_RECORD", commandList, label);
    }
}

struct NrRetirementObservation
{
    bool enabled = false, admitted = false, waited = false, pending = false;
    unsigned int handle;
    unsigned long long started = 0;
    explicit NrRetirementObservation(unsigned int source) : handle(source)
    {
        const auto* cfg = Config::Instance();
        enabled = cfg->LogToFile.value_or_default() || cfg->LogToConsole.value_or_default() ||
                  cfg->LogToNGX.value_or_default();
        if (enabled) started = GetTickCount64();
    }
    ~NrRetirementObservation()
    {
        if (!enabled) return;
        // Evaluate owns g_nrMutex. At most one summary per second per handle;
        // no per-frame disk trace, and no timing/counter work with logging off.
        struct Window { unsigned long long started = 0; DlssNr::NrRetirementCadence stats; };
        static std::unordered_map<unsigned int, Window> windows;
        auto& window = windows[handle];
        if (window.started == 0) window.started = started;
        const auto now = GetTickCount64();
        window.stats.Observe(now - started, admitted, waited, pending);
        if (now - window.started < 1000) return;
        LOG_INFO("V23227 NR cadence handle {} calls {} waits {} skips {} pending {} cpu_total_ms {} cpu_max_ms {} scope=feature",
                 handle, window.stats.calls, window.stats.waits, window.stats.skipped,
                 window.stats.pending, window.stats.totalMs, window.stats.maximumMs);
        window.stats = {};
        window.started = now;
    }
};

bool RetirePreviousNrEvaluate(unsigned int sourceHandleId)
{
    NrRetirementObservation observation(sourceHandleId);
    if (LukeRossDlssIngress::Detected())
        return observation.admitted = true;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    unsigned long long fenceValue = 0;

    {
        std::lock_guard<std::mutex> lock(g_nrFlightMutex);
        const auto found = g_nrFlights.find(sourceHandleId);
        if (found == g_nrFlights.end())
            return observation.admitted = true;

        if (found->second.awaitingSubmission)
        {
            observation.pending = true;
            const auto skipped = ++g_nrRetirementSkips;
            if (skipped == 1 || (skipped % 120) == 0)
                LOG_WARN("V23216 DLSS-NR skipped handle {}: its previous command list has not been submitted yet (skip {})",
                         sourceHandleId, skipped);
            return false;
        }

        fence = found->second.fence;
        fenceValue = found->second.fenceValue;
    }

    const auto completed = fence == nullptr ? 0 : fence->GetCompletedValue();
    if (NrFlightTrace::Enabled()) {
        wchar_t retirementLabel[200] {};
        swprintf_s(retirementLabel, L"NR_REUSE handle=%u fence=%p required=%llu completed=%llu",
                   sourceHandleId, fence.Get(), fenceValue, completed);
        NrFlightTrace::Write("NR_REUSE", nullptr, retirementLabel);
    }
    if (completed == UINT64_MAX)
        return false;
    if (fence == nullptr || fenceValue == 0 || DlssNr::NrFenceRetired(completed, fenceValue))
        return observation.admitted = true;

    // Same-eye evaluates are normally about two game frames apart, so the fence is already complete.
    // Give a temporarily deep queue a small chance to drain; if it does not, skip this NR evaluate
    // instead of either stalling the render thread or reusing in-flight NVIDIA state.
    observation.waited = true;
    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (eventHandle != nullptr)
    {
        const HRESULT setResult = fence->SetEventOnCompletion(fenceValue, eventHandle);
        if (SUCCEEDED(setResult))
        {
            const DWORD waitResult = WaitForSingleObject(eventHandle, 8);
            CloseHandle(eventHandle);
            if (waitResult == WAIT_OBJECT_0 && DlssNr::NrFenceRetired(fence->GetCompletedValue(), fenceValue))
                return observation.admitted = true;
        }
        else
        {
            CloseHandle(eventHandle);
        }
    }

    const auto skipped = ++g_nrRetirementSkips;
    if (skipped == 1 || (skipped % 120) == 0)
        LOG_WARN("V23216 DLSS-NR skipped handle {}: previous GPU evaluate has not retired (skip {})",
                 sourceHandleId, skipped);
    return false;
}

// Writes matched before/after frames on request, so comparisons stop depending on video.
capture::FrameCapture g_capture;

// One capture happens on its own each session, so there is always a fresh sample without anyone having
// to remember to ask. Started after the scene has had a moment to settle: the first frames after a
// feature is built carry its reset, and are not representative of anything.
constexpr unsigned long long kAutoCaptureAfterFrames = 180;
bool g_autoCaptureDone = false;

// Cleared once per run, so a session's captures are its own and nothing accumulates across launches.
void ClearCaptureDirectory()
{
    static bool cleared = false;

    if (cleared)
        return;

    cleared = true;

    std::error_code ec;
    const auto dir = Util::DllPath().remove_filename() / "dlssnr-capture";

    if (std::filesystem::exists(dir, ec))
    {
        std::filesystem::remove_all(dir, ec);

        if (ec)
            LOG_WARN("DLSS-NR could not clear {}: {}", dir.string(), ec.message());
    }
}

unsigned long long g_frames = 0;

// A capture requested from outside the game: when the render path has no fence of its own, the write
// waits until this frame count, by which point the GPU is certainly past the copies.
unsigned long long g_captureWriteAtFrame = 0;

// Dropping a file named dlssnr-capture.trigger beside OptiScaler requests a capture, so a session can
// be asked for one from outside the game -- no alt-tab, no menu. Checked once a second, effectively.
void CheckCaptureTrigger()
{
    if (!NrFlightTrace::Enabled()) return;
    if ((g_frames % 60) != 0)
        return;

    std::error_code ec;
    const auto trigger = Util::DllPath().remove_filename() / "dlssnr-capture.trigger";

    if (std::filesystem::exists(trigger, ec))
    {
        std::filesystem::remove(trigger, ec);
        DlssNr::RequestCapture(capture::kMaxFrames);
        LOG_INFO("DLSS-NR capture requested by trigger file");
    }
}

// The encoded mean is aimed here. Mid-grey rather than anything brighter: the model has to see both the
// shadow detail it might lift and the highlights it must not blow out.
constexpr float kTargetEncodedMean = 0.45f;

// How fast the derived value follows the scene. Readings arrive a few times a second, and an exposure
// that lunges at every cut is worse than one that arrives a moment late.
constexpr float kWhitePointBlend = 0.25f;

// Recomputes the white point from a measured mean. Inverting the encode for the white point that puts
// that mean at the target gives wp = mean * (1 - t^g) / t^g.
float WhitePointForMean(float meanLuma)
{
    const float encoded = powf(kTargetEncodedMean, 2.2f);
    const float ratio = encoded / (1.0f - encoded);
    const float wp = meanLuma / ratio;
    // A black frame between scenes would otherwise drive this to zero and divide the next frame by it.
    return wp < 0.01f ? 0.01f : (wp > 10000.0f ? 10000.0f : wp);
}

std::filesystem::path g_dllDir;

// Loads the forwarder that owns the calls into the snippet.
bool EnsureForwarder()
{
    if (g_runtime.forwarder != nullptr)
        return g_runtime.forwarderReady;

    if (g_dllDir.empty())
        g_dllDir = Util::DllPath().remove_filename();

    // Beside OptiScaler first, then beside the executable: someone dropping this into a game folder may
    // reasonably put it in either place.
    auto found = Util::FindFilePath(g_dllDir, "nvngx.dll_dlssnr.dll");

    if (!found.has_value())
        found = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx.dll_dlssnr.dll");

    if (!found.has_value())
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll not found beside OptiScaler ({}) or the game executable",
                  g_dllDir.string());
        g_runtime.reason = "nvngx.dll_dlssnr.dll is missing";
        return false;
    }

    // FindFilePath hands back the file itself, not the directory holding it.
    const auto path = found.value();
    g_runtime.forwarder = LoadLibraryW(path.wstring().c_str());

    if (g_runtime.forwarder == nullptr)
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll found at {} but would not load, error {}", path.string(),
                  GetLastError());
        g_runtime.reason = "nvngx.dll_dlssnr.dll would not load";
        return false;
    }

    g_runtime.create = (PFN_NrCreate) GetProcAddress(g_runtime.forwarder, "dlssnr_call_create");
    g_runtime.evaluate =
        (PFN_NrEvaluate) GetProcAddress(g_runtime.forwarder, "dlssnr_call_evaluate");
    g_runtime.release = (PFN_NrRelease) GetProcAddress(g_runtime.forwarder, "dlssnr_call_release");
    g_runtime.setExtras =
        (PFN_NrSetExtras) GetProcAddress(g_runtime.forwarder, "dlssnr_call_set_extras");
    g_runtime.setContract =
        (PFN_NrSetContract) GetProcAddress(g_runtime.forwarder, "dlssnr_call_set_contract");
    g_runtime.setFloatSlot =
        (PFN_NrSetFloatSlot) GetProcAddress(g_runtime.forwarder, "dlssnr_call_set_float_slot");
    g_runtime.probeFloat =
        (PFN_NrProbeFloat) GetProcAddress(g_runtime.forwarder, "dlssnr_call_probe_float");
    g_runtime.lastInit = (int*) GetProcAddress(g_runtime.forwarder, "dlssnr_call_last_init");
    g_runtime.lastCreate = (int*) GetProcAddress(g_runtime.forwarder, "dlssnr_call_last_create");

    const unsigned exportMask = (g_runtime.create ? 1u : 0u) |
        (g_runtime.evaluate ? 2u : 0u) | (g_runtime.release ? 4u : 0u) |
        (g_runtime.setExtras ? 8u : 0u) | (g_runtime.setContract ? 16u : 0u) |
        (g_runtime.setFloatSlot ? 32u : 0u) | (g_runtime.probeFloat ? 64u : 0u);
    std::array<wchar_t, 32768> loadedPath{};
    const auto loadedLength = GetModuleFileNameW(g_runtime.forwarder,
        loadedPath.data(), static_cast<DWORD>(loadedPath.size()));
    LOG_INFO("V33012 NR forwarder requested={} loaded={} exports=0x{:X} required=0x{:X}",
        path.string(), loadedLength && loadedLength < loadedPath.size()
            ? wstring_to_string(std::wstring(loadedPath.data(), loadedLength)) : "unknown",
        exportMask, nr_forwarder::required_exports);
    g_runtime.forwarderReady = nr_forwarder::complete(exportMask);
    if (!g_runtime.forwarderReady)
    {
        LOG_ERROR("V33012 NR forwarder ABI mismatch: missing export mask=0x{:X}; use the matched checkpoint forwarder",
            nr_forwarder::required_exports & ~exportMask);
        g_runtime.reason = "the forwarder is missing its exports";
        return false;
    }

    LOG_INFO("DLSS-NR forwarder loaded from {}", path.string());
    return true;
}

// The model needs the driver core's own capability block: it carries the snippet and preset callbacks a
// feature expects at create time, which a freshly allocated block does not have.
void DiscoverFloatSlot(NVSDK_NGX_Parameter* params);

bool EnsureHandleParams(NrPerHandle& nr, ID3D12Device* device)
{
    if (nr.params != nullptr)
        return true;

    if (!NVNGXProxy::IsDx12Inited() && !NVNGXProxy::InitDx12(device))
    {
        g_runtime.reason = "the NGX core would not initialise";
        return false;
    }

    if (NVNGXProxy::D3D12_GetCapabilityParameters() == nullptr ||
        NVNGXProxy::D3D12_DestroyParameters() == nullptr)
    {
        g_runtime.reason = "the NGX core cannot own per-handle capability parameters";
        return false;
    }

    if (NVNGXProxy::D3D12_GetCapabilityParameters()(&nr.params) != NVSDK_NGX_Result_Success ||
        nr.params == nullptr)
    {
        nr.params = nullptr;
        g_runtime.reason = "the NGX core refused its capability parameters";
        return false;
    }

    // Before anything is written to it, work out where this block keeps floats.
    DiscoverFloatSlot(nr.params);
    if (g_capabilityFloatSlot < 0)
    {
        NVNGXProxy::D3D12_DestroyParameters()(nr.params);
        nr.params = nullptr;
        g_runtime.reason = "cannot safely write the per-handle NR float parameters";
        return false;
    }
    return true;
}

NVSDK_NGX_Parameter* EvaluateParamsForCommandList(
    NrPerHandle& nr,
    ID3D12GraphicsCommandList* commandList,
    ID3D12Device* device)
{
    if (commandList == nullptr || device == nullptr)
        return nullptr;

    const auto found = nr.evaluateParams.find(commandList);
    if (found != nr.evaluateParams.end())
        return found->second;

    if (!NVNGXProxy::IsDx12Inited() && !NVNGXProxy::InitDx12(device))
    {
        g_runtime.reason = "the NGX core would not initialise";
        return nullptr;
    }

    if (NVNGXProxy::D3D12_GetCapabilityParameters() == nullptr ||
        NVNGXProxy::D3D12_DestroyParameters() == nullptr)
    {
        g_runtime.reason = "the NGX core cannot own per-command-list evaluate parameters";
        return nullptr;
    }

    NVSDK_NGX_Parameter* params = nullptr;
    if (NVNGXProxy::D3D12_GetCapabilityParameters()(&params) != NVSDK_NGX_Result_Success ||
        params == nullptr)
    {
        g_runtime.reason = "the NGX core refused its per-command-list evaluate parameters";
        return nullptr;
    }

    DiscoverFloatSlot(params);
    if (g_capabilityFloatSlot < 0)
    {
        NVNGXProxy::D3D12_DestroyParameters()(params);
        g_runtime.reason = "cannot safely write the per-command-list NR float parameters";
        return nullptr;
    }

    nr.evaluateParams.emplace(commandList, params);
    LOG_INFO("V23216 DLSS-NR acquired params {:X} for command list {:X}",
             (size_t) params, (size_t) commandList);
    return params;
}

// Works out which vtable slot this parameter block keeps floats in, by writing a known value through
// each candidate and asking for it back through the header's typed getter. Only a slot that returns the
// value it was given is accepted.
//
// Slot 1 is where the public header declares the float overload, so it is tried first and wins wherever
// that assumption holds. It does not hold for the driver's own block: every float written there reads
// back as FAIL_UnsupportedParameter while every uint lands, which is why intensity, local structure,
// local tone and skin structure never did anything.
void DiscoverFloatSlot(NVSDK_NGX_Parameter* params)
{
    if (g_runtime.floatSlotKnown || params == nullptr || g_runtime.probeFloat == nullptr ||
        g_runtime.setFloatSlot == nullptr)
        return;

    g_runtime.floatSlotKnown = true;

    static const char* kProbeKey = "DLSSNR.OptiScalerFloatProbe";
    static const int kCandidates[] = { 1, 2, 5, 6, 7, 4, 3, 0 };
    const float expected = 0.375f; // exact in binary, so the round trip is exact or it is wrong

    for (int slot : kCandidates)
    {
        float readBack = 0.0f;
        g_runtime.probeFloat(params, kProbeKey, expected, slot);

        if (params->Get(kProbeKey, &readBack) == NVSDK_NGX_Result_Success && readBack == expected)
        {
            g_runtime.setFloatSlot(slot);
            g_capabilityFloatSlot = slot;
            LOG_INFO("DLSS-NR float parameters go through vtable slot {}", slot);
            return;
        }
    }

    LOG_ERROR("DLSS-NR could not find the float setter: intensity, local structure, local tone and skin "
              "structure will have no effect. The uint parameters still apply.");
}

// Switching inject points changes the surface format underneath the scratch set: the finished frame
// works in the swapchain's format, the pre-frame-generation path in the upscaler's. A stale set either
// clamps linear HDR into an 8-bit texture -- wrong brightness until something forces a rebuild -- or
// hands CopyResource mismatched formats, which fails silently and makes the whole pass appear to do
// nothing. So the set is torn down whenever the format it was built for is not the format needed now.
// Retired model features and surfaces are parked and freed a comfortable number of evaluates later.
// Releasing them immediately was the device hang: with frame generation the GPU runs several frames
// behind, this work rides the game's own queue that no module fence covers, and an NGX feature or
// scratch texture freed under in-flight work kills the device.
struct NrRetired
{
    void* feature = nullptr;
    ID3D12Resource* resource = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    int framesLeft = 32;
};

std::vector<NrRetired> g_nrRetired;

void ParkNrFeature(void*& feature)
{
    if (feature == nullptr)
        return;

    NrRetired r;
    r.feature = feature;
    feature = nullptr;
    g_nrRetired.push_back(r);
}

void ParkNrResource(ID3D12Resource*& res)
{
    if (res == nullptr)
        return;

    NrRetired r;
    r.resource = res;
    res = nullptr;
    g_nrRetired.push_back(r);
}

void ParkNrParams(NVSDK_NGX_Parameter*& params)
{
    if (params == nullptr)
        return;

    NrRetired r;
    r.params = params;
    params = nullptr;
    g_nrRetired.push_back(r);
}

void ParkNrEvaluateParams(NrPerHandle& nr)
{
    for (auto& [unusedCommandList, params] : nr.evaluateParams)
    {
        (void) unusedCommandList;
        ParkNrParams(params);
    }
    nr.evaluateParams.clear();
}

void TickNrRetired()
{
    for (size_t i = 0; i < g_nrRetired.size();)
    {
        if (--g_nrRetired[i].framesLeft > 0)
        {
            ++i;
            continue;
        }

        if (g_nrRetired[i].feature != nullptr && g_runtime.release != nullptr)
            g_runtime.release(g_nrRetired[i].feature);

        if (g_nrRetired[i].resource != nullptr)
            g_nrRetired[i].resource->Release();

        if (g_nrRetired[i].params != nullptr &&
            NVNGXProxy::D3D12_DestroyParameters() != nullptr)
            NVNGXProxy::D3D12_DestroyParameters()(g_nrRetired[i].params);

        g_nrRetired.erase(g_nrRetired.begin() + i);
    }
}

void ReleaseSurfacesIfFormatChanged(NrPerHandle& nr, DXGI_FORMAT needed)
{
    if (nr.output == nullptr || nr.output->GetDesc().Format == needed)
        return;

    LOG_INFO("DLSS-NR rebuilding surfaces: format {} -> {} (inject point changed)",
             (int) nr.output->GetDesc().Format, (int) needed);

    ParkNrFeature(nr.feature);
    ParkNrParams(nr.params);
    ParkNrEvaluateParams(nr);

    for (ID3D12Resource** r :
         { &nr.output, &nr.colorCopy, &nr.hdrCopy, &nr.colorSmall, &nr.resolveCopy })
        ParkNrResource(*r);

    nr.reset = true;
}

ID3D12Resource* CreateScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width,
                              unsigned int height)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // The model writes its result, so the destination has to be a UAV.
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ID3D12Resource* res = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res));
    return res;
}

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to, const wchar_t* role)
{
    if (from == to)
        return;

    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    DredMarkers::BeforeBarrier(cmdList, role, res, from, to);
    cmdList->ResourceBarrier(1, &b);
    NrFlightTrace::BarrierAfter(cmdList, role, res, from, to);
}

ID3D12Resource* PrivateNrGuide(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
                             ID3D12Resource* source, ID3D12Resource** snapshot, bool& reset)
{
    const HRESULT result = DlssNr::SnapshotNrGuide(device, cmdList, source, snapshot, Barrier,
        [&](ID3D12Resource*& replaced) { ParkNrResource(replaced); reset = true; });
    if (FAILED(result)) {
        NrFlightTrace::Write("NR_GUIDE_FAILED", cmdList, L"V23235 private NR guide", source,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE, result);
        return nullptr;
    }
    NrFlightTrace::Write("NR_GUIDE_PRIVATE", cmdList, L"V23235 private NR guide", *snapshot);
    return *snapshot;
}

// The upscaler's own names differ between super resolution and ray reconstruction, and only one set is
// present on any given block.
// Whether a surface can physically hold linear HDR.
//
// Only a float format can: linear light is open-ended and runs far past 1.0, which a normalised
// integer surface cannot represent. An 8-bit UNORM frame is finished, display-referred output, and
// so is a 10-bit one -- HDR10 is PQ-encoded, which is display-referred too.
//
// The game's IsHDR flag is a statement of intent that is not always true, and believing it over a
// format that cannot hold linear light means encoding an already-encoded frame a second time.
bool FormatCanHoldLinearHdr(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return true;
    default:
        return false;
    }
}

ID3D12Resource* GetResource(NVSDK_NGX_Parameter* params, const char* a, const char* b)
{
    ID3D12Resource* res = nullptr;

    if (params->Get(a, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    res = nullptr;

    if (params->Get(b, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    // The same key again, as a plain pointer.
    //
    // NVSDK_NGX_Parameter has a typed setter per resource kind and an untyped one, and on a real NGX
    // parameter block those are separate slots: what goes in through Set(name, void*) does not come
    // back out of Get(name, ID3D12Resource**). A game running its own D3D12 upscaler sets these
    // typed, so the typed read above is enough and always was.
    //
    // Both of OptiScaler's bridges write them untyped. IFeature_Dx11wDx12 and IFeature_VkwDx12 turn
    // the game's D3D11 textures or Vulkan images into D3D12 resources and hand them over with
    // Set(name, (void*) resource) -- so the typed read came back null a few lines after the resource
    // had been written, and the pass quietly did nothing. That is the whole reason this never ran in
    // a DirectX 11 or Vulkan game.
    void* untyped = nullptr;

    if (params->Get(a, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr)
        return static_cast<ID3D12Resource*>(untyped);

    untyped = nullptr;

    if (params->Get(b, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr)
        return static_cast<ID3D12Resource*>(untyped);

    return nullptr;
}

// A change has to hold still before it is acted on: a slider being dragged reports a new value every
// frame, and each one would otherwise mean a new model.
constexpr unsigned long long kSettleFrames = 30;

// The extras the official integration sets: global tone (read at create) and the interface inputs.
// Written before every create and evaluate, nulls included, so nothing stale ever sits in the block.
void SetExtras(NVSDK_NGX_Parameter* params, const Config& cfg, ID3D12Resource* ui,
               ID3D12Resource* backbuffer, unsigned int uiWidth, unsigned int uiHeight,
               unsigned int bbWidth, unsigned int bbHeight)
{
    if (g_runtime.setExtras == nullptr || params == nullptr)
        return;

    // Global tone is written at the model's own default: the control that exposed it changed nothing
    // that could be seen, and the block persists, so a value still has to be put there.
    g_runtime.setExtras(params, 1.0f, ui, ui, backbuffer, uiWidth, uiHeight, bbWidth, bbHeight);
}

bool TuningMatchesFeature(const NrPerHandle& nr, const Config& cfg)
{
    return nr.builtPreset == cfg.DlssNrPreset.value_or_default() &&
           nr.builtIntensity == cfg.DlssNrIntensity.value_or_default() &&
           nr.builtStyle == cfg.DlssNrStyle.value_or_default() &&
           nr.builtLocalStructure == cfg.DlssNrLocalStructure.value_or_default() &&
           nr.builtLocalTone == cfg.DlssNrLocalTone.value_or_default() &&
           nr.builtSkinStructure == cfg.DlssNrSkinStructure.value_or_default() &&
           nr.builtAutoMask == cfg.DlssNrAutoMask.value_or_default();
}

void RecordBuiltTuning(NrPerHandle& nr, const Config& cfg)
{
    nr.builtPreset = cfg.DlssNrPreset.value_or_default();
    nr.builtIntensity = cfg.DlssNrIntensity.value_or_default();
    nr.builtStyle = cfg.DlssNrStyle.value_or_default();
    nr.builtLocalStructure = cfg.DlssNrLocalStructure.value_or_default();
    nr.builtLocalTone = cfg.DlssNrLocalTone.value_or_default();
    nr.builtSkinStructure = cfg.DlssNrSkinStructure.value_or_default();
    nr.builtAutoMask = cfg.DlssNrAutoMask.value_or_default();
}

} // namespace

namespace DlssNr
{
// Guards the module's state. Every caller is now on the game's render thread, so this is no longer
// holding two threads apart -- but the D3D11-on-D3D12 bridge enters from its own call site, and the
// cost is a CPU-side lock on a path that already records command lists.
std::mutex g_nrMutex;

void RetryAfterFailure()
{
    g_runtime.failed = false;
    g_runtime.reason = "";

    for (auto& [handleId, nr] : g_nrByHandle)
        nr.reset = true;
}

// Every way out of the pass before it does anything is silent on purpose -- an evaluate that carries
// no depth is normal and would otherwise print every frame forever. That silence is fine until the
// pass does nothing at all and the log has no opinion about why.
//
// So each distinct reason is reported once. Once, not once per frame.
void ReportSkipOnce(const char* reason)
{
    static std::set<std::string> seen;

    if (seen.insert(reason).second)
        LOG_INFO("DLSS-NR did not run: {}", reason);
}

std::vector<NrSubmissionReceipt> TakeCommandListSubmissionReceipts(
    unsigned int commandListCount, ID3D12CommandList* const* commandLists)
{
    std::vector<NrSubmissionReceipt> receipts;
    if (commandLists == nullptr)
        return receipts;
    std::lock_guard<std::mutex> lock(g_nrFlightMutex);
    for (unsigned int i = 0; i < commandListCount; ++i)
    {
        auto detached = g_nrSubmissionLedger.Detach(commandLists[i]);
        receipts.insert(receipts.end(), detached.begin(), detached.end());
    }
    for (const auto& receipt : receipts)
    {
        if (!NrFlightTrace::Enabled()) break;
        wchar_t label[160] {};
        swprintf_s(label, L"NR_DETACH handle=%u recording=%llu", receipt.handle, receipt.recording);
        NrFlightTrace::Write("NR_DETACH", static_cast<ID3D12GraphicsCommandList*>(receipt.list), label);
    }
    return receipts;
}

void CompleteCommandListSubmission(ID3D12CommandQueue* queue,
                                   const std::vector<NrSubmissionReceipt>& receipts)
{
    if (queue == nullptr || receipts.empty())
        return;
    std::lock_guard<std::mutex> lock(g_nrFlightMutex);

    NrQueueFence& queueFence = g_nrQueueFences[queue];
    if (queueFence.fence == nullptr)
    {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(device.ReleaseAndGetAddressOf()))) || device == nullptr ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                       IID_PPV_ARGS(queueFence.fence.ReleaseAndGetAddressOf()))))
        {
            g_nrRetirementSignalFailed.store(true, std::memory_order_release);
            LOG_ERROR("V23216 DLSS-NR could not create its submission-retirement fence");
            return;
        }
        queueFence.queue = queue;
        LOG_INFO("V23216 DLSS-NR retirement fence attached to game queue {:X}", (size_t) queue);
    }

    const unsigned long long fenceValue = ++queueFence.nextValue;
    const HRESULT signalResult = queue->Signal(queueFence.fence.Get(), fenceValue);
    if (FAILED(signalResult))
    {
        g_nrRetirementSignalFailed.store(true, std::memory_order_release);
        LOG_ERROR("V23216 DLSS-NR could not signal its submission-retirement fence: 0x{:X}",
                  (unsigned int) signalResult);
        return;
    }

    for (const auto& receipt : receipts)
    {
        const auto found = g_nrFlights.find(receipt.handle);
        if (found == g_nrFlights.end() || found->second.recording != receipt.recording ||
            !g_nrSubmissionLedger.Complete(receipt))
            continue;

        found->second.awaitingSubmission = false;
        found->second.recordedList = nullptr;
        found->second.fence = queueFence.fence;
        found->second.fenceValue = fenceValue;
        if (NrFlightTrace::Enabled()) {
            wchar_t label[220] {};
            swprintf_s(label, L"NR_SUBMIT handle=%u recording=%llu queue=%p fence=%p value=%llu completed=%llu",
                       receipt.handle, receipt.recording, queue, queueFence.fence.Get(), fenceValue,
                       queueFence.fence->GetCompletedValue());
            NrFlightTrace::Write("NR_SUBMIT", static_cast<ID3D12GraphicsCommandList*>(receipt.list), label);
        }
    }
}

void EvaluateBeforeUpscale(unsigned int sourceHandleId, ID3D12GraphicsCommandList* cmdList,
                          NVSDK_NGX_Parameter* params, ID3D12CommandQueue* timingQueue)
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);
    const Config& cfg = *Config::Instance();

    if (!cfg.DlssNrEnabled.value_or_default())
    {
        ReportSkipOnce("it is switched off");
        return;
    }

    if (g_runtime.failed || cmdList == nullptr || params == nullptr)
    {
        ReportSkipOnce(g_runtime.failed ? "it already failed this session"
                                        : "no command list or no parameter block");
        return;
    }

    NrPerHandle& nr = g_nrByHandle[sourceHandleId];
    // Read the game's reset before validation, retirement or NR parameter writes.
    // Missing resources / unsubmitted work must not lose a one-frame reset pulse.
    CaptureSourceHistoryReset(nr, sourceHandleId, params);
    ID3D12Resource* target = GetResource(params, NVSDK_NGX_Parameter_Color, "DLSSD.Color");
    ID3D12Resource* srOutput = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

    // Without all three there is nothing to run on. This is not a failure -- some evaluates legitimately
    // carry none of it -- so it stays quiet and tries again next frame.
    if (target == nullptr || depth == nullptr || motion == nullptr)
    {
        ReportSkipOnce(target == nullptr    ? "the parameters carried no pre-SR color texture"
                       : depth == nullptr   ? "the parameters carried no depth"
                                            : "the parameters carried no motion vectors");
        return;
    }

    ID3D12Device* device = nullptr;

    if (FAILED(target->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        ReportSkipOnce("the output texture belongs to no D3D12 device");
        return;
    }

    // The source handle is the temporal identity. Witcher VR evaluates two same-sized DLSS features
    // in strict left/right alternation; keying only on dimensions made both eyes share one NR history.
    if (g_nrRetirementSignalFailed.load(std::memory_order_acquire))
    {
        g_runtime.failed = true;
        g_runtime.reason = "the NR submission-retirement fence failed";
        LOG_ERROR("DLSS-NR unavailable: {}", g_runtime.reason);
        device->Release();
        return;
    }

    if (!RetirePreviousNrEvaluate(sourceHandleId))
    {
        nr.reset = true;
        device->Release();
        return;
    }

    const D3D12_RESOURCE_DESC desc = target->GetDesc();
    const auto allocationWidth = (unsigned int) desc.Width;
    const auto allocationHeight = desc.Height;
    const bool targetSupportsUav = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;

    // V23030's authoritative render subrect now determines NR color AND guide extent. The allocation
    // may be padded to display size. Never infer the model size from that allocation or a partial pair.
    unsigned int guideWidth = 0;
    unsigned int guideHeight = 0;

    const bool hasRenderSubrect =
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &guideWidth) ==
            NVSDK_NGX_Result_Success &&
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &guideHeight) ==
            NVSDK_NGX_Result_Success &&
        guideWidth != 0 && guideHeight != 0;

    RenderExtent generic {};
    if (!hasRenderSubrect)
    {
        params->Get(NVSDK_NGX_Parameter_Width, &generic.width);
        params->Get(NVSDK_NGX_Parameter_Height, &generic.height);
    }

    const auto active = BeforeSrExtent({guideWidth, guideHeight}, hasRenderSubrect, generic,
                                       {allocationWidth, allocationHeight});
    const auto width = active.width;
    const auto height = active.height;
    guideWidth = width;
    guideHeight = height;

    // This trial handles separate 2D, zero-origin inputs (including padded allocations). Do not
    // silently read another eye/region for layouts that need a separate offset-aware integration.
    bool unsupportedOffset = false;
    for (const char* key : { NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
                            NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
                            NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
                            NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
                            NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
                            NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y })
    {
        unsigned int offset = 0;
        if (params->Get(key, &offset) == NVSDK_NGX_Result_Success && offset != 0)
            unsupportedOffset = true;
    }
    const auto depthDesc = depth->GetDesc();
    const auto motionDesc = motion->GetDesc();
    if (unsupportedOffset || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 ||
        (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0 ||
        depthDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        motionDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        depthDesc.DepthOrArraySize != 1 || motionDesc.DepthOrArraySize != 1 ||
        depthDesc.SampleDesc.Count != 1 || motionDesc.SampleDesc.Count != 1 ||
        !FitsBeforeSr(active, desc.Width, desc.Height) ||
        !FitsBeforeSr(active, depthDesc.Width, depthDesc.Height) ||
        !FitsBeforeSr(active, motionDesc.Width, motionDesc.Height))
    {
        ReportSkipOnce("V23070 pre-SR requires valid zero-origin, separate 2D color/depth/motion inputs");
        device->Release();
        return;
    }

    // The game states its depth convention in the flags it created its own feature with, so there is no
    // reason to assume one.
    unsigned int createFlags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &createFlags);
    const bool gameSaysInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    const bool gameSaysHdr = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;
    // PR #6 uses the SR output's color-space contract; keep this decision independent of inject point.
    const bool isHdrBuffer = gameSaysHdr &&
        FormatCanHoldLinearHdr(srOutput != nullptr ? srOutput->GetDesc().Format : desc.Format);

    nr.guideDepthInverted = gameSaysInverted;

    // And it states how its motion vectors are encoded. Inventing a resolution ratio here meant handing
    // the model vectors it could not interpret.
    float mvScaleX = 1.0f;
    float mvScaleY = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX) != NVSDK_NGX_Result_Success)
        mvScaleX = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY) != NVSDK_NGX_Result_Success)
        mvScaleY = 1.0f;

    // V23030's motion encoding is preserved. WorkingScale is applied exactly once at evaluate.
    nr.guideMvScaleX = mvScaleX;
    nr.guideMvScaleY = mvScaleY;

    static bool reportedGuides = false;

    if (!reportedGuides)
    {
        reportedGuides = true;
        LOG_INFO("V23070 DLSS-NR BEFORE SR: depth {}, game motion vector scale {} x {}",
                 nr.guideDepthInverted ? "inverted" : "not inverted", nr.guideMvScaleX,
                 nr.guideMvScaleY);
    }

    if (cfg.DlssNrProxyProbe.value_or_default())
        ProbeProxyDispatch(cmdList);

    if (!EnsureForwarder())
    {
        g_runtime.failed = true;
        LOG_ERROR("DLSS-NR unavailable: {}", g_runtime.reason);
        device->Release();
        return;
    }

    const auto outputDesc = srOutput != nullptr ? srOutput->GetDesc() : desc;
    // WorkingScale is now relative to the active render extent, not the display extent.
    float workScale = cfg.DlssNrWorkingScale.value_or_default();
    workScale = workScale < 0.25f ? 0.25f : (workScale > 1.0f ? 1.0f : workScale);
    const auto workWidth = (unsigned int) (width * workScale + 0.5f);
    const auto workHeight = (unsigned int) (height * workScale + 0.5f);
    const bool reduced = workWidth != width || workHeight != height;

    ReleaseSurfacesIfFormatChanged(nr, desc.Format);

    const bool resolutionChanged = nr.width != width || nr.height != height ||
                                   nr.workWidth != workWidth || nr.workHeight != workHeight;

    // The model reads its tuning once, while the feature is built, so a changed setting only takes
    // effect when the feature is rebuilt. TuningMatchesFeature was written to notice that and then
    // never called, which is why every one of these controls appeared to do nothing until something
    // else -- a resolution change -- happened to force a rebuild by accident.
    const bool tuningChanged = !TuningMatchesFeature(nr, cfg);

    if (nr.feature != nullptr && (resolutionChanged || tuningChanged))
    {
        // Parked rather than released: with frame generation the GPU can still be several frames
        // deep in work that references all of it.
        ParkNrFeature(nr.feature);
        ParkNrParams(nr.params);
        ParkNrEvaluateParams(nr);

        // Only a resolution change invalidates the scratch textures. Tuning does not, and throwing
        // them away for it would mean a reallocation every time a slider moves.
        if (resolutionChanged)
        {
            ParkNrResource(nr.output);
            ParkNrResource(nr.colorCopy);
            ParkNrResource(nr.hdrCopy);
            ParkNrResource(nr.colorSmall);
            ParkNrResource(nr.resolveCopy);
        }
    }

    if (nr.output == nullptr)
    {
        nr.output = CreateScratch(device, desc.Format, workWidth, workHeight);
        nr.colorCopy = CreateScratch(device, desc.Format, width, height);
        nr.hdrCopy = CreateScratch(device, desc.Format, width, height);
        nr.workWidth = workWidth;
        nr.workHeight = workHeight;
    }

    if (reduced && nr.colorSmall == nullptr)
        nr.colorSmall = CreateScratch(device, desc.Format, workWidth, workHeight);

    if (!targetSupportsUav && nr.resolveCopy == nullptr)
        nr.resolveCopy = CreateScratch(device, desc.Format, width, height);

    if (nr.output == nullptr || nr.colorCopy == nullptr || nr.hdrCopy == nullptr ||
        (reduced && nr.colorSmall == nullptr) || (!targetSupportsUav && nr.resolveCopy == nullptr))
    {
        g_runtime.failed = true;
        g_runtime.reason = "V23070 pre-SR scratch allocation failed";
        LOG_ERROR("DLSS-NR unavailable: {}", g_runtime.reason);
        device->Release();
        return;
    }

    if (!EnsureHandleParams(nr, device))
    {
        g_runtime.failed = true;
        LOG_ERROR("DLSS-NR unavailable: {}", g_runtime.reason);
        device->Release();
        return;
    }

    if (nr.feature == nullptr && nr.output != nullptr && nr.colorCopy != nullptr &&
        nr.hdrCopy != nullptr)
    {
        auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

        if (!snippet.has_value())
            snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

        if (!snippet.has_value())
        {
            g_runtime.failed = true;
            g_runtime.reason = "nvngx_dlssnr.dll was not found beside OptiScaler or the game";
            LOG_ERROR("DLSS-NR unavailable: {}", g_runtime.reason);
            device->Release();
            return;
        }

        // The stable ReShade route invokes NR natively: model input and output have the same
        // dimensions, even when OptiScaler has first reduced both with WorkingScale. UI correction
        // is disabled because this path supplies no separate UI layer, matching the tested add-on.
        constexpr unsigned int uiCorrection = 0;
        g_runtime.setContract(nr.params, workWidth, workHeight, workWidth, workHeight, createFlags,
                              isHdrBuffer ? 1 : 0, uiCorrection);
        SetExtras(nr.params, cfg, nullptr, nullptr, 0, 0, 0, 0);
        nr.feature = g_runtime.create(
            snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(), device,
            cmdList, nr.params, workWidth, workHeight,
            (int) cfg.DlssNrPreset.value_or_default(), cfg.DlssNrIntensity.value_or_default(),
            (int) cfg.DlssNrStyle.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
            cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
            cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, (int) uiCorrection);

        if (nr.feature == nullptr)
        {
            g_runtime.failed = true;
            g_runtime.reason = "the model would not initialise";
            const auto initResult =
                (unsigned int) (g_runtime.lastInit != nullptr ? *g_runtime.lastInit : 0);
            const auto createResult =
                (unsigned int) (g_runtime.lastCreate != nullptr ? *g_runtime.lastCreate : 0);

            // Cast before formatting. These are ints, and "0x{:X}" on a negative int prints
            // 0x-452FFFFF, which no one can decode back to 0xBAD00001.
            LOG_ERROR("DLSS-NR create failed: init 0x{:X} ({}), create 0x{:X} ({})", initResult,
                      NgxResultName(initResult), createResult, NgxResultName(createResult));
            device->Release();
            return;
        }

        nr.width = width;
        nr.height = height;
        nr.reset = true;
        RecordBuiltTuning(nr, cfg);
        LOG_INFO("V23070 DLSS-NR BEFORE SR handle {} running at {}x{}, guides {}x{} (preset {}, intensity {}, style {})",
                 sourceHandleId, width, height, guideWidth, guideHeight, nr.builtPreset,
                 nr.builtIntensity, nr.builtStyle);
        LOG_INFO("V23216 pre-SR allocation {}x{}, SR output {}x{}, copy resolve {}, create params {:X}, game params {:X}",
                 desc.Width, desc.Height, outputDesc.Width, outputDesc.Height, !targetSupportsUav,
                 (size_t) nr.params, (size_t) params);
        LOG_INFO("DLSS-NR contract: input/output {}x{}, native scale 1, upscaling off, create flags "
                 "0x{:X}, HDR {}, pre-exposure 1, exposure scale 1, UI correction off",
                 workWidth, workHeight, createFlags, isHdrBuffer ? "on" : "off");

        // Creating and evaluating a feature in the same command list is the dice-roll that hung the
        // GPU (every crash died on a creation frame). The creation goes through the game's own submit
        // first; the first evaluate happens next frame. One frame without the model is invisible.
        MarkNrRecorded(sourceHandleId, cmdList);
        device->Release();
        return;
    }

    if (nr.feature == nullptr)
    {
        device->Release();
        return;
    }

    // SR's input arrives shader-readable and must leave in that same state.
    // Whether the buffer the upscaler just wrote is linear HDR or an already tone-mapped picture is not
    // something to assume: the game says so, in the flags it created its own DLSS feature with. Running
    // the colour transform over a frame that has already been through a tonemapper is pure damage, and
    // skipping it on one that has not leaves the model reading ordinary values as enormously bright.
    static bool reportedHdr = false;

    if (!reportedHdr)
    {
        reportedHdr = true;
        LOG_INFO("DLSS-NR: the game's DLSS buffer is {} (create flags 0x{:X}), so the colour transform is {}",
                 isHdrBuffer ? "linear HDR" : "already tone-mapped", createFlags,
                 isHdrBuffer ? "on" : "off");
    }

    if (g_compose == nullptr)
        g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", device);

    const bool haveCodec = g_compose->IsInit();

    if (!haveCodec)
    {
        g_runtime.failed = true;
        g_runtime.reason = "the colour codec would not compile";
        LOG_ERROR("DLSS-NR unavailable: {}", g_runtime.reason);
        device->Release();
        return;
    }

    // What the upscaler produces is linear HDR with an open-ended range; the model was trained on
    // finished, sRGB-encoded frames. The white point is what maps one to the other, and it is a property
    // of the game's exposure rather than a number worth asking anyone to guess: measured means of 0.065,
    // 1.8 and 185 have all been seen in this one game.
    ++g_frames;
    TickNrRetired();
    CheckCaptureTrigger();

    if (g_captureWriteAtFrame != 0 && g_frames >= g_captureWriteAtFrame)
    {
        g_captureWriteAtFrame = 0;
        const auto captureDir = Util::DllPath().remove_filename() / "dlssnr-capture";
        const auto written = g_capture.write(captureDir);

        if (!written.empty())
            LOG_INFO("DLSS-NR wrote matched before/after frames to {}", written);
    }

    // Paper white, and nothing else. The frame is divided by this and encoded, and the soft knee
    // above 0.75 takes whatever is left over.
    //
    // It used to be divided by a white point measured from the frame -- around 3 in Cyberpunk -- which
    // was right for the old composition, where the encode had to be inverted and highlights therefore
    // had to survive it. Under the composition this now uses it is actively wrong twice over: the
    // model is handed a picture three times darker than it should see, and the highlight branch is
    // defeated. That branch hands back `originalLuma - proxyLuma`, the headroom the proxy could not
    // represent -- it exists precisely because the proxy is meant to clip. Normalising the highlights
    // away first leaves it nothing to give back.
    const float whitePoint = cfg.DlssNrWhitePointScale.value_or_default();

    DlssNrConstants encodeParams {};
    encodeParams.Mode = DlssNrMode_Encode;
    // A frame that is already display-referred is handed over untouched: the encode becomes a copy and
    // the resolve adds the model's edit back at full scale.
    encodeParams.Passthrough = isHdrBuffer ? 0u : 1u;
    encodeParams.WhitePoint = whitePoint;
    // Match only takes effect once a fit exists; until then the table is empty and the shader would
    // read a curve of zeros, so it falls back to the plain proxy.
    encodeParams.Width = width;
    encodeParams.Height = height;

    {
        DredScope stage(cmdList, L"OptiScaler V23216 DLSS-NR pre-SR encode");
        g_compose->Dispatch(cmdList, encodeParams, target, nullptr, nullptr, nullptr, nullptr,
                            nr.colorCopy, nr.hdrCopy);
    }

    // The transitions double as the wait for the encode's writes.
    Barrier(cmdList, nr.colorCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"pre-SR-encode-color-ready");
    Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"pre-SR-encode-hdr-ready");

    // Below full resolution the model is shown a filtered shrink of the proxy; the edit it returns is
    // enlarged during the resolve while the frame underneath stays full size and untouched.
    ID3D12Resource* modelInput = nr.colorCopy;

    if (reduced && nr.colorSmall != nullptr)
    {
        DlssNrConstants down {};
        down.Mode = DlssNrMode_Downsample;
        down.Width = workWidth;
        down.Height = workHeight;
        {
            DredScope stage(cmdList, L"OptiScaler V23216 DLSS-NR pre-SR downsample");
            g_compose->Dispatch(cmdList, down, modelInput, nullptr, nullptr, nullptr, nullptr,
                                nr.colorSmall, nullptr);
        }
        Barrier(cmdList, nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"pre-SR-downsample-ready");
        modelInput = nr.colorSmall;
    }

    ID3D12Resource* depthIn = PrivateNrGuide(device, cmdList, depth, &nr.depthClone, nr.reset);
    ID3D12Resource* motionIn = PrivateNrGuide(device, cmdList, motion, &nr.motionClone, nr.reset);

    if (depthIn == nullptr || motionIn == nullptr)
    {
        g_runtime.failed = true;
        g_runtime.reason = "the game's depth or motion vectors could not be made readable";
        LOG_ERROR("DLSS-NR unavailable: {}", g_runtime.reason);
        device->Release();
        return;
    }

    // The vectors were scaled to full-frame pixels; the image the model reprojects is the working size.
    const float mvToWork = width != 0 ? (float) workWidth / (float) width : 1.0f;

    NVSDK_NGX_Parameter* const evaluateParams =
        EvaluateParamsForCommandList(nr, cmdList, device);
    if (evaluateParams == nullptr)
    {
        g_runtime.failed = true;
        LOG_ERROR("DLSS-NR unavailable: {}", g_runtime.reason);
        device->Release();
        return;
    }

    g_runtime.setContract(evaluateParams, workWidth, workHeight, workWidth, workHeight, createFlags,
                          isHdrBuffer ? 1 : 0, 0);
    SetExtras(evaluateParams, cfg, nullptr, nullptr, 0, 0, 0, 0);

    // The proxy path, when asked for. Same inputs, same model -- the difference is who calls it.
    //
    // Nothing falls back automatically. A silent fallback would mean never finding out the proxy
    // path was broken: the picture would look right either way, because the forwarder would be
    // quietly doing the work.
    if (cfg.DlssNrUseProxy.value_or_default())
    {
        const unsigned int proxyResult = DlssNr::Proxy::Run(
            cmdList, device, modelInput, depthIn, motionIn, nr.output, workWidth, workHeight,
            guideWidth, guideHeight, nr.guideDepthInverted, nr.reset,
            nr.guideMvScaleX * mvToWork, nr.guideMvScaleY * mvToWork);

        nr.reset = false;

        if (proxyResult == NVSDK_NGX_Result_Success)
            MarkNrRecorded(sourceHandleId, cmdList);

        if (proxyResult != 1)
        {
            g_runtime.failed = true;
            g_runtime.reason = "the proxy path could not run the model";
            LOG_ERROR("DLSS-NR (proxy): evaluate returned 0x{:X} ({}), disabling for this session",
                      proxyResult, NgxResultName(proxyResult));
        }

        device->Release();
        return;
    }

    int result = 0;
    {
        if (NrFlightTrace::Enabled()) {
        wchar_t modelReceipt[256] {};
        swprintf_s(modelReceipt, L"NR_MODEL handle=%u feature=%p params=%p color=%p depth=%p motion=%p output=%p reset=%u source=%u",
                   sourceHandleId, nr.feature, evaluateParams, modelInput, depthIn, motionIn, nr.output,
                   nr.reset ? 1u : 0u, nr.sourceReset ? 1u : 0u);
        NrFlightTrace::Write("NR_MODEL", cmdList, modelReceipt);
        }
        DredScope stage(cmdList, L"OptiScaler V23216 DLSS-NR pre-SR model");
        result = g_runtime.evaluate(
            cmdList, nr.feature, evaluateParams, modelInput, depthIn, motionIn, nr.output,
            workWidth, workHeight, guideWidth, guideHeight, nr.guideDepthInverted ? 1 : 0,
            nr.reset ? 1 : 0, cfg.DlssNrIntensity.value_or_default(),
            (int) cfg.DlssNrStyle.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
            cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
            cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, nr.guideMvScaleX * mvToWork,
            nr.guideMvScaleY * mvToWork);
    }

    if (result == NVSDK_NGX_Result_Success)
        MarkNrRecorded(sourceHandleId, cmdList);

    nr.reset = false;

    // Once, a few seconds in, so it lands after the values have been written at least once.
    static bool tuningReported = false;

    if (!tuningReported && g_frames > 240)
    {
        tuningReported = true;

        // At INFO, because whether the model actually took a value is the only way to tell a
        // control that does nothing from one that is not being written.
        auto report = [evaluateParams](const char* name, float wrote)
        {
            float value = 0.0f;
            const NVSDK_NGX_Result r = evaluateParams->Get(name, &value);
            LOG_INFO("DLSS-NR readback {} -> {} (we wrote {}, result 0x{:X})", name, value, wrote,
                     (uint32_t) r);
        };

        const Config& rcfg = *Config::Instance();
        report("DLSSNR.Intensity", rcfg.DlssNrIntensity.value_or_default());
        report("DLSSNR.LocalStructureStrength", rcfg.DlssNrLocalStructure.value_or_default());
        report("DLSSNR.LocalToneStrength", rcfg.DlssNrLocalTone.value_or_default());
        report("DLSSNR.SkinStructureStrength", rcfg.DlssNrSkinStructure.value_or_default());

        unsigned int style = 0;
        const NVSDK_NGX_Result styleResult =
            evaluateParams->Get("DLSSNR.Style", &style);
        LOG_DEBUG("DLSS-NR readback DLSSNR.Style -> {} (result 0x{:X})", style, (uint32_t) styleResult);

        // The preset is the last control whose arrival has never been checked, and three of them look
        // identical in play. Either it is not landing or the presets really are alike.
        unsigned int preset = 0;
        const NVSDK_NGX_Result presetResult =
            evaluateParams->Get("DLSSNR.Hint.Render.Preset", &preset);
        LOG_DEBUG("DLSS-NR readback DLSSNR.Hint.Render.Preset -> {} (result 0x{:X}, we wrote {})", preset,
                 (uint32_t) presetResult, cfg.DlssNrPreset.value_or_default());

        LOG_DEBUG("DLSS-NR wrote intensity {}, local structure {}, local tone {}, skin {}, style {}",
                 cfg.DlssNrIntensity.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
                 cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
                 cfg.DlssNrStyle.value_or_default());
    }

    if (result == NVSDK_NGX_Result_Success)
    {
        // Resolve takes the difference between what the model returned and what it was shown, and adds
        // that back to the frame. At strength zero the result is what the upscaler produced, exactly, and
        // anything the model left alone is untouched rather than round-tripped through the curve.
        DlssNrConstants resolveParams {};
        resolveParams.Mode = DlssNrMode_Resolve;
        resolveParams.WhitePoint = whitePoint;
        resolveParams.Width = width;
        resolveParams.Height = height;
        resolveParams.TransferStrength = cfg.DlssNrTransferStrength.value_or_default();
        resolveParams.ColourStrength = cfg.DlssNrColourStrength.value_or_default();
        resolveParams.DebugView = cfg.DlssNrDebugView.value_or_default();
        resolveParams.MaxRatio = cfg.DlssNrMaxRatio.value_or_default();
        resolveParams.Passthrough = isHdrBuffer ? 0u : 1u;
        resolveParams.CompareMode = cfg.DlssNrCompare.value_or_default();
        resolveParams.CompareSplit = cfg.DlssNrCompareSplit.value_or_default();
        resolveParams.CompareZoom = std::max(1.0f, cfg.DlssNrCompareZoom.value_or_default());
        resolveParams.CompareSwap = cfg.DlssNrCompareSwap.value_or_default() ? 1u : 0u;

        Barrier(cmdList, nr.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"pre-SR-model-output-ready");

        ID3D12Resource* resolveTarget = targetSupportsUav ? target : nr.resolveCopy;
        if (targetSupportsUav)
            Barrier(cmdList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"pre-SR-resolve-target-write");
        {
            DredScope stage(cmdList, L"OptiScaler V23216 DLSS-NR pre-SR resolve");
            g_compose->Dispatch(cmdList, resolveParams, modelInput, nr.output, nr.hdrCopy, motionIn,
                                nullptr, resolveTarget, nullptr);
        }
        Barrier(cmdList, nr.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"pre-SR-model-output-restore");

        if (targetSupportsUav)
        {
            Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"pre-SR-resolve-target-ready");
        }
        else
        {
            Barrier(cmdList, nr.resolveCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE, L"pre-SR-resolve-copy-source");
            Barrier(cmdList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST, L"pre-SR-resolve-copy-target");
            D3D12_TEXTURE_COPY_LOCATION src {};
            src.pResource = nr.resolveCopy;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION dst {};
            dst.pResource = target;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            const D3D12_BOX rect {0, 0, 0, width, height, 1};
            cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, &rect);
            Barrier(cmdList, target, D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"pre-SR-resolve-copy-target-restore");
            Barrier(cmdList, nr.resolveCopy, D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"pre-SR-resolve-copy-source-restore");
        }

        // Captures compare the pre-SR proxy with the NR-edited render input, NOT the final SR output.
        // The edited Color has already been returned to shader-read state. The write happens a few
        // frames later, once the GPU is certainly past these copies -- this path has no fence of its
        // own.
        if (g_capture.isActive())
        {
            g_capture.record(cmdList, device, nr.colorCopy,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, target,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            if (g_capture.readyToWrite() && g_captureWriteAtFrame == 0)
                g_captureWriteAtFrame = g_frames + 8;
        }
    }
    else
    {
        g_runtime.failed = true;
        g_runtime.reason = "the model refused to run";
        LOG_ERROR("DLSS-NR evaluate returned 0x{:X} ({}), disabling for this session", (uint32_t) result,
                  NgxResultName((unsigned int) result));
    }

    Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"pre-SR-hdr-restore");

    // Put any guide clones back where the next frame's copy expects to find them.
    if (nr.depthClone != nullptr)
        Barrier(cmdList, nr.depthClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST, L"pre-SR-depth-clone-restore");

    if (nr.motionClone != nullptr)
        Barrier(cmdList, nr.motionClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST, L"pre-SR-motion-clone-restore");

    if (reduced && nr.colorSmall != nullptr)
        Barrier(cmdList, nr.colorSmall, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"pre-SR-downsample-restore");

    // Leave the staging copy as the next frame expects to find it.
    Barrier(cmdList, nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"pre-SR-color-restore");

    device->Release();
}

void EvaluateAfterUpscale(unsigned int sourceHandleId, ID3D12GraphicsCommandList* cmdList,
                          NVSDK_NGX_Parameter* params, ID3D12CommandQueue* timingQueue)
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);
    const Config& cfg = *Config::Instance();

    if (!cfg.DlssNrEnabled.value_or_default())
    {
        ReportSkipOnce("it is switched off");
        return;
    }

    if (g_runtime.failed || cmdList == nullptr || params == nullptr)
    {
        ReportSkipOnce(g_runtime.failed ? "it already failed this session"
                                        : "no command list or no parameter block");
        return;
    }

    NrPerHandle& nr = g_nrByHandle[sourceHandleId];
    CaptureSourceHistoryReset(nr, sourceHandleId, params);
    ID3D12Resource* target = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

    // Without all three there is nothing to run on. This is not a failure -- some evaluates legitimately
    // carry none of it -- so it stays quiet and tries again next frame.
    if (target == nullptr || depth == nullptr || motion == nullptr)
    {
        ReportSkipOnce(target == nullptr    ? "the parameters carried no output texture"
                       : depth == nullptr   ? "the parameters carried no depth"
                                            : "the parameters carried no motion vectors");
        return;
    }

    ID3D12Device* device = nullptr;

    if (FAILED(target->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        ReportSkipOnce("the output texture belongs to no D3D12 device");
        return;
    }

    // The source handle is the temporal identity. Witcher VR evaluates two same-sized DLSS features
    // in strict left/right alternation; keying only on dimensions made both eyes share one NR history.
    if (g_nrRetirementSignalFailed.load(std::memory_order_acquire))
    {
        g_runtime.failed = true;
        g_runtime.reason = "the NR submission-retirement fence failed";
        LOG_ERROR("DLSS-NR unavailable: {}", g_runtime.reason);
        device->Release();
        return;
    }

    if (!RetirePreviousNrEvaluate(sourceHandleId))
    {
        nr.reset = true;
        device->Release();
        return;
    }

    const D3D12_RESOURCE_DESC desc = target->GetDesc();
    const auto width = (unsigned int) desc.Width;
    const auto height = desc.Height;

    // Depth and motion vectors are the upscaler's inputs and so are at render resolution, while colour
    // and output are at display resolution. The authoritative guide extent is the immutable DLSS render
    // subrect. Generic Width/Height live in the shared NGX parameter block and the NR forwarder rewrites
    // them to its native output contract, so reading those first made the second eye inherit 3072x3264
    // instead of Witcher VR's actual 1536x1632 guide extent.
    unsigned int guideWidth = 0;
    unsigned int guideHeight = 0;

    const bool hasRenderSubrect =
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &guideWidth) ==
            NVSDK_NGX_Result_Success &&
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &guideHeight) ==
            NVSDK_NGX_Result_Success &&
        guideWidth != 0 && guideHeight != 0;

    if (!hasRenderSubrect)
    {
        guideWidth = 0;
        guideHeight = 0;
        params->Get(NVSDK_NGX_Parameter_Width, &guideWidth);
        params->Get(NVSDK_NGX_Parameter_Height, &guideHeight);
    }

    if (guideWidth == 0 || guideHeight == 0)
    {
        guideWidth = width;
        guideHeight = height;
    }

    // The game states its depth convention in the flags it created its own feature with, so there is no
    // reason to assume one.
    unsigned int createFlags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &createFlags);
    const bool gameSaysInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    const bool gameSaysHdr = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;
    const bool isHdrBuffer = gameSaysHdr && FormatCanHoldLinearHdr(desc.Format);

    nr.guideDepthInverted = gameSaysInverted;

    // And it states how its motion vectors are encoded. Inventing a resolution ratio here meant handing
    // the model vectors it could not interpret.
    float mvScaleX = 1.0f;
    float mvScaleY = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX) != NVSDK_NGX_Result_Success)
        mvScaleX = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY) != NVSDK_NGX_Result_Success)
        mvScaleY = 1.0f;

    // Two factors, and both are needed. The game's own scale turns its vectors into render pixels --
    // Cyberpunk reports 1920 x 1080, so its vectors are normalised. The upscale ratio then carries
    // render pixels onto a display-resolution image. They coincide only at native resolution, which is
    // exactly where this was first tested.
    const float upscaleX = guideWidth != 0 ? (float) width / (float) guideWidth : 1.0f;
    const float upscaleY = guideHeight != 0 ? (float) height / (float) guideHeight : 1.0f;
    // The game's own encoding, times the resolution ratio.
    // The game's own encoding, and nothing else.
    //
    // Multiplying by the upscale ratio was reasoning, not measurement, and it was only ever exercised
    // at native resolution where the ratio is 1 and the mistake is invisible. Every resource already
    // carries its own subrect -- MVecSubrectWidth and Height say the motion texture is render sized --
    // so the model is told the size twice and scales for it twice. Vectors come out half again too
    // long and the model warps its history past where the surface actually went, which smears the
    // frame along the direction of motion.
    nr.guideMvScaleX = mvScaleX;
    nr.guideMvScaleY = mvScaleY;
    (void) upscaleX;
    (void) upscaleY;

    static bool reportedGuides = false;

    if (!reportedGuides)
    {
        reportedGuides = true;
        LOG_INFO("DLSS-NR guides: depth {}, motion vector scale {} x {} (the game says {} x {}, times "
                 "the {}x{} upscale ratio)",
                 nr.guideDepthInverted ? "inverted" : "not inverted", nr.guideMvScaleX,
                 nr.guideMvScaleY, mvScaleX, mvScaleY, upscaleX, upscaleY);
    }

    if (cfg.DlssNrProxyProbe.value_or_default())
        ProbeProxyDispatch(cmdList);

    if (!EnsureForwarder())
    {
        g_runtime.failed = true;
        LOG_ERROR("DLSS-NR unavailable: {}", g_runtime.reason);
        device->Release();
        return;
    }

    // What the model works at. The frame and its edit stay full resolution; only the model's input and
    // answer shrink, and the resolve enlarges the answer while compositing.
    float workScale = cfg.DlssNrWorkingScale.value_or_default();
    workScale = workScale < 0.25f ? 0.25f : (workScale > 1.0f ? 1.0f : workScale);
    const auto workWidth = (unsigned int) (width * workScale + 0.5f);
    const auto workHeight = (unsigned int) (height * workScale + 0.5f);
    const bool reduced = workWidth != width || workHeight != height;

    ReleaseSurfacesIfFormatChanged(nr, desc.Format);

    const bool resolutionChanged = nr.width != width || nr.height != height ||
                                   nr.workWidth != workWidth || nr.workHeight != workHeight;

    // The model reads its tuning once, while the feature is built, so a changed setting only takes
    // effect when the feature is rebuilt. TuningMatchesFeature was written to notice that and then
    // never called, which is why every one of these controls appeared to do nothing until something
    // else -- a resolution change -- happened to force a rebuild by accident.
    const bool tuningChanged = !TuningMatchesFeature(nr, cfg);

    if (nr.feature != nullptr && (resolutionChanged || tuningChanged))
    {
        // Parked rather than released: with frame generation the GPU can still be several frames
        // deep in work that references all of it.
        ParkNrFeature(nr.feature);
        ParkNrParams(nr.params);
        ParkNrEvaluateParams(nr);

        // Only a resolution change invalidates the scratch textures. Tuning does not, and throwing
        // them away for it would mean a reallocation every time a slider moves.
        if (resolutionChanged)
        {
            ParkNrResource(nr.output);
            ParkNrResource(nr.colorCopy);
            ParkNrResource(nr.hdrCopy);
            ParkNrResource(nr.colorSmall);
        }
    }

    if (nr.output == nullptr)
    {
        nr.output = CreateScratch(device, desc.Format, workWidth, workHeight);
        nr.colorCopy = CreateScratch(device, desc.Format, width, height);
        nr.hdrCopy = CreateScratch(device, desc.Format, width, height);
        nr.workWidth = workWidth;
        nr.workHeight = workHeight;
    }

    if (reduced && nr.colorSmall == nullptr)
        nr.colorSmall = CreateScratch(device, desc.Format, workWidth, workHeight);

    if (nr.output != nullptr && nr.colorCopy != nullptr && nr.hdrCopy != nullptr &&
        !EnsureHandleParams(nr, device))
    {
        g_runtime.failed = true;
        LOG_ERROR("DLSS-NR unavailable: {}", g_runtime.reason);
        device->Release();
        return;
    }

    if (nr.feature == nullptr && nr.output != nullptr && nr.colorCopy != nullptr &&
        nr.hdrCopy != nullptr)
    {
        auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

        if (!snippet.has_value())
            snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

        if (!snippet.has_value())
        {
            g_runtime.failed = true;
            g_runtime.reason = "nvngx_dlssnr.dll was not found beside OptiScaler or the game";
            LOG_ERROR("DLSS-NR unavailable: {}", g_runtime.reason);
            device->Release();
            return;
        }

        // The stable ReShade route invokes NR natively: model input and output have the same
        // dimensions, even when OptiScaler has first reduced both with WorkingScale. UI correction
        // is disabled because this path supplies no separate UI layer, matching the tested add-on.
        constexpr unsigned int uiCorrection = 0;
        g_runtime.setContract(nr.params, workWidth, workHeight, workWidth, workHeight, createFlags,
                              isHdrBuffer ? 1 : 0, uiCorrection);
        SetExtras(nr.params, cfg, nullptr, nullptr, 0, 0, 0, 0);
        nr.feature = g_runtime.create(
            snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(), device,
            cmdList, nr.params, workWidth, workHeight,
            (int) cfg.DlssNrPreset.value_or_default(), cfg.DlssNrIntensity.value_or_default(),
            (int) cfg.DlssNrStyle.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
            cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
            cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, (int) uiCorrection);

        if (nr.feature == nullptr)
        {
            g_runtime.failed = true;
            g_runtime.reason = "the model would not initialise";
            const auto initResult =
                (unsigned int) (g_runtime.lastInit != nullptr ? *g_runtime.lastInit : 0);
            const auto createResult =
                (unsigned int) (g_runtime.lastCreate != nullptr ? *g_runtime.lastCreate : 0);

            // Cast before formatting. These are ints, and "0x{:X}" on a negative int prints
            // 0x-452FFFFF, which no one can decode back to 0xBAD00001.
            LOG_ERROR("DLSS-NR create failed: init 0x{:X} ({}), create 0x{:X} ({})", initResult,
                      NgxResultName(initResult), createResult, NgxResultName(createResult));
            device->Release();
            return;
        }

        nr.width = width;
        nr.height = height;
        nr.reset = true;
        RecordBuiltTuning(nr, cfg);
        LOG_INFO("V23070 DLSS-NR AFTER SR handle {} running at {}x{}, guides {}x{} (preset {}, intensity {}, style {})",
                 sourceHandleId, width, height, guideWidth, guideHeight, nr.builtPreset,
                 nr.builtIntensity, nr.builtStyle);
        LOG_INFO("DLSS-NR contract: input/output {}x{}, native scale 1, upscaling off, create flags "
                 "0x{:X}, HDR {}, pre-exposure 1, exposure scale 1, UI correction off",
                 workWidth, workHeight, createFlags, isHdrBuffer ? "on" : "off");

        // Creating and evaluating a feature in the same command list is the dice-roll that hung the
        // GPU (every crash died on a creation frame). The creation goes through the game's own submit
        // first; the first evaluate happens next frame. One frame without the model is invisible.
        MarkNrRecorded(sourceHandleId, cmdList);
        device->Release();
        return;
    }

    if (nr.feature == nullptr)
    {
        device->Release();
        return;
    }

    // The upscaler has just written this, so it is a UAV. The model needs it readable.
    // Whether the buffer the upscaler just wrote is linear HDR or an already tone-mapped picture is not
    // something to assume: the game says so, in the flags it created its own DLSS feature with. Running
    // the colour transform over a frame that has already been through a tonemapper is pure damage, and
    // skipping it on one that has not leaves the model reading ordinary values as enormously bright.
    static bool reportedHdr = false;

    if (!reportedHdr)
    {
        reportedHdr = true;
        LOG_INFO("DLSS-NR: the game's DLSS buffer is {} (create flags 0x{:X}), so the colour transform is {}",
                 isHdrBuffer ? "linear HDR" : "already tone-mapped", createFlags,
                 isHdrBuffer ? "on" : "off");
    }

    if (g_compose == nullptr)
        g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", device);

    const bool haveCodec = g_compose->IsInit();

    if (!haveCodec)
    {
        g_runtime.failed = true;
        g_runtime.reason = "the colour codec would not compile";
        LOG_ERROR("DLSS-NR unavailable: {}", g_runtime.reason);
        device->Release();
        return;
    }

    // What the upscaler produces is linear HDR with an open-ended range; the model was trained on
    // finished, sRGB-encoded frames. The white point is what maps one to the other, and it is a property
    // of the game's exposure rather than a number worth asking anyone to guess: measured means of 0.065,
    // 1.8 and 185 have all been seen in this one game.
    ++g_frames;
    TickNrRetired();
    CheckCaptureTrigger();

    if (g_captureWriteAtFrame != 0 && g_frames >= g_captureWriteAtFrame)
    {
        g_captureWriteAtFrame = 0;
        const auto captureDir = Util::DllPath().remove_filename() / "dlssnr-capture";
        const auto written = g_capture.write(captureDir);

        if (!written.empty())
            LOG_INFO("DLSS-NR wrote matched before/after frames to {}", written);
    }

    // Paper white, and nothing else. The frame is divided by this and encoded, and the soft knee
    // above 0.75 takes whatever is left over.
    //
    // It used to be divided by a white point measured from the frame -- around 3 in Cyberpunk -- which
    // was right for the old composition, where the encode had to be inverted and highlights therefore
    // had to survive it. Under the composition this now uses it is actively wrong twice over: the
    // model is handed a picture three times darker than it should see, and the highlight branch is
    // defeated. That branch hands back `originalLuma - proxyLuma`, the headroom the proxy could not
    // represent -- it exists precisely because the proxy is meant to clip. Normalising the highlights
    // away first leaves it nothing to give back.
    const float whitePoint = cfg.DlssNrWhitePointScale.value_or_default();

    DlssNrConstants encodeParams {};
    encodeParams.Mode = DlssNrMode_Encode;
    // A frame that is already display-referred is handed over untouched: the encode becomes a copy and
    // the resolve adds the model's edit back at full scale.
    encodeParams.Passthrough = isHdrBuffer ? 0u : 1u;
    encodeParams.WhitePoint = whitePoint;
    // Match only takes effect once a fit exists; until then the table is empty and the shader would
    // read a curve of zeros, so it falls back to the plain proxy.
    encodeParams.Width = width;
    encodeParams.Height = height;

    Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"post-SR-encode-source-ready");
    {
        DredScope stage(cmdList, L"OptiScaler V23216 DLSS-NR post-SR encode");
        g_compose->Dispatch(cmdList, encodeParams, target, nullptr, nullptr, nullptr, nullptr,
                            nr.colorCopy, nr.hdrCopy);
    }

    Barrier(cmdList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"post-SR-encode-source-restore");
    // The transitions double as the wait for the encode's writes.
    Barrier(cmdList, nr.colorCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"post-SR-encode-color-ready");
    Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"post-SR-encode-hdr-ready");

    // Below full resolution the model is shown a filtered shrink of the proxy; the edit it returns is
    // enlarged during the resolve while the frame underneath stays full size and untouched.
    ID3D12Resource* modelInput = nr.colorCopy;

    if (reduced && nr.colorSmall != nullptr)
    {
        DlssNrConstants down {};
        down.Mode = DlssNrMode_Downsample;
        down.Width = workWidth;
        down.Height = workHeight;
        {
            DredScope stage(cmdList, L"OptiScaler V23216 DLSS-NR post-SR downsample");
            g_compose->Dispatch(cmdList, down, modelInput, nullptr, nullptr, nullptr, nullptr,
                                nr.colorSmall, nullptr);
        }
        Barrier(cmdList, nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"post-SR-downsample-ready");
        modelInput = nr.colorSmall;
    }

    ID3D12Resource* depthIn = PrivateNrGuide(device, cmdList, depth, &nr.depthClone, nr.reset);
    ID3D12Resource* motionIn = PrivateNrGuide(device, cmdList, motion, &nr.motionClone, nr.reset);

    if (depthIn == nullptr || motionIn == nullptr)
    {
        g_runtime.failed = true;
        g_runtime.reason = "the game's depth or motion vectors could not be made readable";
        LOG_ERROR("DLSS-NR unavailable: {}", g_runtime.reason);
        device->Release();
        return;
    }

    // The vectors were scaled to full-frame pixels; the image the model reprojects is the working size.
    const float mvToWork = width != 0 ? (float) workWidth / (float) width : 1.0f;

    NVSDK_NGX_Parameter* const evaluateParams =
        EvaluateParamsForCommandList(nr, cmdList, device);
    if (evaluateParams == nullptr)
    {
        g_runtime.failed = true;
        LOG_ERROR("DLSS-NR unavailable: {}", g_runtime.reason);
        device->Release();
        return;
    }

    g_runtime.setContract(evaluateParams, workWidth, workHeight, workWidth, workHeight, createFlags,
                          isHdrBuffer ? 1 : 0, 0);
    SetExtras(evaluateParams, cfg, nullptr, nullptr, 0, 0, 0, 0);

    // The proxy path, when asked for. Same inputs, same model -- the difference is who calls it.
    //
    // Nothing falls back automatically. A silent fallback would mean never finding out the proxy
    // path was broken: the picture would look right either way, because the forwarder would be
    // quietly doing the work.
    if (cfg.DlssNrUseProxy.value_or_default())
    {
        const unsigned int proxyResult = DlssNr::Proxy::Run(
            cmdList, device, modelInput, depthIn, motionIn, nr.output, workWidth, workHeight,
            guideWidth, guideHeight, nr.guideDepthInverted, nr.reset,
            nr.guideMvScaleX * mvToWork, nr.guideMvScaleY * mvToWork);

        nr.reset = false;

        if (proxyResult == NVSDK_NGX_Result_Success)
            MarkNrRecorded(sourceHandleId, cmdList);

        if (proxyResult != 1)
        {
            g_runtime.failed = true;
            g_runtime.reason = "the proxy path could not run the model";
            LOG_ERROR("DLSS-NR (proxy): evaluate returned 0x{:X} ({}), disabling for this session",
                      proxyResult, NgxResultName(proxyResult));
        }

        device->Release();
        return;
    }

    int result = 0;
    {
        if (NrFlightTrace::Enabled()) {
        wchar_t modelReceipt[256] {};
        swprintf_s(modelReceipt, L"NR_MODEL handle=%u feature=%p params=%p color=%p depth=%p motion=%p output=%p reset=%u source=%u",
                   sourceHandleId, nr.feature, evaluateParams, modelInput, depthIn, motionIn, nr.output,
                   nr.reset ? 1u : 0u, nr.sourceReset ? 1u : 0u);
        NrFlightTrace::Write("NR_MODEL", cmdList, modelReceipt);
        }
        DredScope stage(cmdList, L"OptiScaler V23216 DLSS-NR post-SR model");
        result = g_runtime.evaluate(
            cmdList, nr.feature, evaluateParams, modelInput, depthIn, motionIn, nr.output,
            workWidth, workHeight, guideWidth, guideHeight, nr.guideDepthInverted ? 1 : 0,
            nr.reset ? 1 : 0, cfg.DlssNrIntensity.value_or_default(),
            (int) cfg.DlssNrStyle.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
            cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
            cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, nr.guideMvScaleX * mvToWork,
            nr.guideMvScaleY * mvToWork);
    }

    if (result == NVSDK_NGX_Result_Success)
        MarkNrRecorded(sourceHandleId, cmdList);

    nr.reset = false;

    // Once, a few seconds in, so it lands after the values have been written at least once.
    static bool tuningReported = false;

    if (!tuningReported && g_frames > 240)
    {
        tuningReported = true;

        // At INFO, because whether the model actually took a value is the only way to tell a
        // control that does nothing from one that is not being written.
        auto report = [evaluateParams](const char* name, float wrote)
        {
            float value = 0.0f;
            const NVSDK_NGX_Result r = evaluateParams->Get(name, &value);
            LOG_INFO("DLSS-NR readback {} -> {} (we wrote {}, result 0x{:X})", name, value, wrote,
                     (uint32_t) r);
        };

        const Config& rcfg = *Config::Instance();
        report("DLSSNR.Intensity", rcfg.DlssNrIntensity.value_or_default());
        report("DLSSNR.LocalStructureStrength", rcfg.DlssNrLocalStructure.value_or_default());
        report("DLSSNR.LocalToneStrength", rcfg.DlssNrLocalTone.value_or_default());
        report("DLSSNR.SkinStructureStrength", rcfg.DlssNrSkinStructure.value_or_default());

        unsigned int style = 0;
        const NVSDK_NGX_Result styleResult =
            evaluateParams->Get("DLSSNR.Style", &style);
        LOG_DEBUG("DLSS-NR readback DLSSNR.Style -> {} (result 0x{:X})", style, (uint32_t) styleResult);

        // The preset is the last control whose arrival has never been checked, and three of them look
        // identical in play. Either it is not landing or the presets really are alike.
        unsigned int preset = 0;
        const NVSDK_NGX_Result presetResult =
            evaluateParams->Get("DLSSNR.Hint.Render.Preset", &preset);
        LOG_DEBUG("DLSS-NR readback DLSSNR.Hint.Render.Preset -> {} (result 0x{:X}, we wrote {})", preset,
                 (uint32_t) presetResult, cfg.DlssNrPreset.value_or_default());

        LOG_DEBUG("DLSS-NR wrote intensity {}, local structure {}, local tone {}, skin {}, style {}",
                 cfg.DlssNrIntensity.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
                 cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
                 cfg.DlssNrStyle.value_or_default());
    }

    if (result == NVSDK_NGX_Result_Success)
    {
        // Resolve takes the difference between what the model returned and what it was shown, and adds
        // that back to the frame. At strength zero the result is what the upscaler produced, exactly, and
        // anything the model left alone is untouched rather than round-tripped through the curve.
        DlssNrConstants resolveParams {};
        resolveParams.Mode = DlssNrMode_Resolve;
        resolveParams.WhitePoint = whitePoint;
        resolveParams.Width = width;
        resolveParams.Height = height;
        resolveParams.TransferStrength = cfg.DlssNrTransferStrength.value_or_default();
        resolveParams.ColourStrength = cfg.DlssNrColourStrength.value_or_default();
        resolveParams.DebugView = cfg.DlssNrDebugView.value_or_default();
        resolveParams.MaxRatio = cfg.DlssNrMaxRatio.value_or_default();
        resolveParams.Passthrough = isHdrBuffer ? 0u : 1u;
        resolveParams.CompareMode = cfg.DlssNrCompare.value_or_default();
        resolveParams.CompareSplit = cfg.DlssNrCompareSplit.value_or_default();
        resolveParams.CompareZoom = std::max(1.0f, cfg.DlssNrCompareZoom.value_or_default());
        resolveParams.CompareSwap = cfg.DlssNrCompareSwap.value_or_default() ? 1u : 0u;

        Barrier(cmdList, nr.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"post-SR-model-output-ready");
        {
            DredScope stage(cmdList, L"OptiScaler V23216 DLSS-NR post-SR resolve");
            g_compose->Dispatch(cmdList, resolveParams, modelInput, nr.output, nr.hdrCopy, motionIn,
                                nullptr, target, nullptr);
        }
        Barrier(cmdList, nr.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"post-SR-model-output-restore");

        // On-demand capture works in this path too: the staging copy still holds the frame as the
        // upscaler produced it, and the edited frame is the output itself. The write happens a few
        // frames later, once the GPU is certainly past these copies -- this path has no fence of its
        // own.
        if (g_capture.isActive())
        {
            g_capture.record(cmdList, device, nr.colorCopy,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, target,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            if (g_capture.readyToWrite() && g_captureWriteAtFrame == 0)
                g_captureWriteAtFrame = g_frames + 8;
        }
    }
    else
    {
        g_runtime.failed = true;
        g_runtime.reason = "the model refused to run";
        LOG_ERROR("DLSS-NR evaluate returned 0x{:X} ({}), disabling for this session", (uint32_t) result,
                  NgxResultName((unsigned int) result));
    }

    Barrier(cmdList, nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"post-SR-hdr-restore");

    // Put any guide clones back where the next frame's copy expects to find them.
    if (nr.depthClone != nullptr)
        Barrier(cmdList, nr.depthClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST, L"post-SR-depth-clone-restore");

    if (nr.motionClone != nullptr)
        Barrier(cmdList, nr.motionClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST, L"post-SR-motion-clone-restore");

    if (reduced && nr.colorSmall != nullptr)
        Barrier(cmdList, nr.colorSmall, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"post-SR-downsample-restore");

    // Leave the staging copy as the next frame expects to find it.
    Barrier(cmdList, nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"post-SR-color-restore");

    device->Release();
}


bool IsRunning()
{
    if (g_runtime.failed)
        return false;

    return std::any_of(g_nrByHandle.begin(), g_nrByHandle.end(),
                       [](const auto& entry) { return entry.second.feature != nullptr; });
}

const char* FailureReason() { return g_runtime.failed ? g_runtime.reason : ""; }

// V23072 deliberately records no timestamp queries for Neural Rendering. The
// old three-slot query ring had no retirement fence and became invalid once
// OFXR kept more than three game command lists in flight.
std::optional<double> LastGpuTime() { return std::nullopt; }

void RequestCapture(unsigned int frames)
{
    if (!NrFlightTrace::Enabled()) return;
    ClearCaptureDirectory();
    g_capture.request(frames);
}

bool CaptureInProgress() { return g_capture.isActive(); }

void ReleaseForHandle(unsigned int sourceHandleId)
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);
    const auto it = g_nrByHandle.find(sourceHandleId);

    if (it == g_nrByHandle.end())
        return;

    NrPerHandle& nr = it->second;
    ParkNrFeature(nr.feature);
    ParkNrParams(nr.params);
    ParkNrEvaluateParams(nr);

    for (ID3D12Resource** r : { &nr.output, &nr.colorCopy, &nr.hdrCopy, &nr.colorSmall, &nr.resolveCopy,
                                &nr.depthClone, &nr.motionClone })
        ParkNrResource(*r);

    g_nrByHandle.erase(it);
    LOG_INFO("DLSS-NR retired state for source handle {}", sourceHandleId);
}

void Shutdown()
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);

    for (auto& r : g_nrRetired)
    {
        if (r.feature != nullptr && g_runtime.release != nullptr)
            g_runtime.release(r.feature);

        if (r.resource != nullptr)
            r.resource->Release();

        if (r.params != nullptr && NVNGXProxy::D3D12_DestroyParameters() != nullptr)
            NVNGXProxy::D3D12_DestroyParameters()(r.params);
    }

    g_nrRetired.clear();

    for (auto& [handleId, nr] : g_nrByHandle)
    {
        if (nr.feature != nullptr && g_runtime.release != nullptr)
            g_runtime.release(nr.feature);

        if (nr.params != nullptr && NVNGXProxy::D3D12_DestroyParameters() != nullptr)
            NVNGXProxy::D3D12_DestroyParameters()(nr.params);

        if (NVNGXProxy::D3D12_DestroyParameters() != nullptr)
        {
            for (auto& [unusedCommandList, params] : nr.evaluateParams)
            {
                (void) unusedCommandList;
                if (params != nullptr)
                    NVNGXProxy::D3D12_DestroyParameters()(params);
            }
        }

        for (ID3D12Resource* r : { nr.output, nr.colorCopy, nr.hdrCopy, nr.colorSmall, nr.resolveCopy,
                                   nr.depthClone, nr.motionClone })
        {
            if (r != nullptr)
                r->Release();
        }
    }

    g_nrByHandle.clear();

    {
        std::lock_guard<std::mutex> flightLock(g_nrFlightMutex);
        g_nrSubmissionLedger.Clear();
        g_nrFlights.clear();
        g_nrQueueFences.clear();
    }
    g_nrRetirementSkips.store(0, std::memory_order_release);
    g_nrRetirementSignalFailed.store(false, std::memory_order_release);

    g_capture.release();
    g_compose.reset();
}
} // namespace DlssNr

