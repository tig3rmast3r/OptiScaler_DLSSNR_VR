#include "../OptiScaler/dlssnr/NrCommandListIdentity.h"
#include "../OptiScaler/dlssnr/NrSubmissionLedger.h"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <atomic>
#include <cstdio>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static unsigned checks;
static void Check(bool condition, const char* label)
{ ++checks; if (!condition) throw std::runtime_error(label); }
static void Hr(HRESULT result) { Check(SUCCEEDED(result), "D3D12 operation failed"); }
constexpr GUID nativeGuid {0xadec44e2,0x61f0,0x45c3,{0xad,0x9f,0x1b,0x37,0x37,0x92,0x84,0xff}};
class Proxy final : public ID3D12CommandList
{
    std::atomic_ulong references {1};
    ComPtr<ID3D12GraphicsCommandList> list;
    bool malformed;
public:
    Proxy(ID3D12GraphicsCommandList* value, bool broken=false) : list(value), malformed(broken) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
    {
        if (!output) return E_POINTER;
        *output=nullptr;
        if (iid==nativeGuid && !malformed) { *output=list.Get(); list->AddRef(); return S_OK; }
        if (iid==nativeGuid || iid==__uuidof(IUnknown) || iid==__uuidof(ID3D12CommandList) ||
            iid==__uuidof(ID3D12DeviceChild) || iid==__uuidof(ID3D12Object))
        { *output=static_cast<ID3D12CommandList*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const auto value=--references; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* bytes, void* data) override
    { return list->GetPrivateData(guid,bytes,data); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT bytes, const void* data) override
    { return list->SetPrivateData(guid,bytes,data); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override
    { return list->SetPrivateDataInterface(guid,data); }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override { return list->SetName(name); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** output) override { return list->GetDevice(iid,output); }
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return list->GetType(); }
};
int main()
{
    try
    {
        ComPtr<ID3D12Debug> debug; Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory; Hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> warp; Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        ComPtr<ID3D12Device> device; Hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
        ComPtr<ID3D12InfoQueue> info; Hr(device.As(&info));
        ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC desc {};
        Hr(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12CommandAllocator> allocator; Hr(device->CreateCommandAllocator(desc.Type,IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> native; Hr(device->CreateCommandList(0,desc.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&native)));
        ComPtr<ID3D12CommandList> first, second, broken;
        first.Attach(new Proxy(native.Get())); second.Attach(new Proxy(native.Get())); broken.Attach(new Proxy(native.Get(),true));
        auto left=DlssNr::NrNativeCommandList(first.Get());
        auto right=DlssNr::NrNativeCommandList(second.Get());
        Check(left.Get()==native.Get() && right.Get()==native.Get(), "distinct Streamline proxies resolve the exact native list");
        Check(DlssNr::NrNativeCommandList(native.Get()).Get()==native.Get(), "ordinary native list retains identity");
        Check(!DlssNr::NrNativeCommandList(nullptr), "null cannot authorize a receipt");
        Check(!DlssNr::NrNativeCommandList(broken.Get()), "invalid native-object contract cannot use the proxy as a substitute");
        DlssNr::NrSubmissionLedger original, ledger;
        original.Record(0,first.Get());
        Check(original.Detach(native.Get()).empty(), "reproduce original proxy/native receipt mismatch");
        ledger.Record(0,left.Get()); ledger.Record(1,right.Get());
        auto receipts=ledger.Detach(DlssNr::NrNativeCommandList(native.Get()).Get());
        Check(receipts.size()==2, "one actual batch detaches both recorded eyes");
        Check(ledger.Detach(DlssNr::NrNativeCommandList(first.Get()).Get()).empty(), "proxy duplicate cannot detach again");

        D3D12_RESOURCE_DESC buffer {}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width=256;
        buffer.Height=1; buffer.DepthOrArraySize=1; buffer.MipLevels=1; buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES upload {}; upload.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_HEAP_PROPERTIES readback {}; readback.Type=D3D12_HEAP_TYPE_READBACK;
        ComPtr<ID3D12Resource> source, destination;
        Hr(device->CreateCommittedResource(&upload,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&source)));
        Hr(device->CreateCommittedResource(&readback,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&destination)));
        void* data=nullptr; Hr(source->Map(0,nullptr,&data)); *static_cast<unsigned*>(data)=0x5afe1212; source->Unmap(0,nullptr);
        native->CopyBufferRegion(destination.Get(),0,source.Get(),0,4); Hr(native->Close());
        ID3D12CommandList* batch[] {native.Get()}; queue->ExecuteCommandLists(1,batch);
        // An opposite-eye recording after Execute must not be consumed by the
        // older batch's callback, even though both use the same native object.
        auto future=ledger.Record(2,DlssNr::NrNativeCommandList(second.Get()).Get());
        Check(ledger.Complete(receipts[0]) && ledger.Complete(receipts[1]), "only detached eye serials complete after actual Execute");
        Check(!ledger.Complete(receipts[0]), "duplicate callback cannot replace retirement");
        ComPtr<ID3D12Fence> fence; Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
        Hr(queue->Signal(fence.Get(),1));
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); Check(event!=nullptr,"completion event");
        Hr(fence->SetEventOnCompletion(1,event)); auto waited=WaitForSingleObject(event,10000); CloseHandle(event);
        Check(waited==WAIT_OBJECT_0 && DlssNr::NrFenceRetired(fence->GetCompletedValue(),1), "real submitted batch retires on its GPU fence");
        Hr(destination->Map(0,nullptr,&data)); Check(*static_cast<unsigned*>(data)==0x5afe1212,"native batch actually executed the recorded copy"); destination->Unmap(0,nullptr);
        auto next=ledger.Detach(DlssNr::NrNativeCommandList(first.Get()).Get());
        Check(next.size()==1 && next[0].recording==future.recording && ledger.Complete(next[0]),"later eye recording keeps its own serial");
        Check(!DlssNr::NrFenceRetired(UINT64_MAX,1),"device removal is never GPU completion");
        unsigned errors=0;
        for (UINT64 i=0; i<info->GetNumStoredMessages(); ++i)
        {
            SIZE_T bytes=0; Hr(info->GetMessage(i,nullptr,&bytes)); std::vector<unsigned char> storage(bytes);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data()); Hr(info->GetMessage(i,message,&bytes));
            if (message->Severity==D3D12_MESSAGE_SEVERITY_ERROR || message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION) ++errors;
        }
        Check(errors==0,"zero GPU validation errors");
        std::printf("PASS NR native identity: %u checks; proxy/native receipts, eye serials, real WARP submission/copy/fence; GPU errors=%u\n",checks,errors);
        return 0;
    }
    catch (const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
}
