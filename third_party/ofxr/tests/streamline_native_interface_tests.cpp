#include "Streamline_NativeInterface.h"
#include "D3D_NativeInterface.h"
#include "D3D_DeviceOwner.h"
#include <cstring>
#include <cstdint>
#include <iostream>
#include <type_traits>

namespace {
int checks{}, failures{};
void check(bool value, const char* label) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}

// Stack-owned COM stand-ins: reaching zero is an error in these borrowing tests.
// A proxy explicitly owns its native base, independently of temporary QI refs.
class Probe final : public IUnknown {
    ULONG refs_{1};
    Probe* owned_{};
    bool nested_{};
public:
    HRESULT native_result{E_NOINTERFACE};
    bool reshade{};
    Probe* target{};
    bool success_null{}, failure_nonnull{}, nested_query{}, nested_ok{true};
    unsigned adds{}, releases{}, queries{}, zero_releases{};
    ~Probe() { if (owned_) owned_->Release(); }
    ULONG refs() const { return refs_; }
    void own(Probe& native) {
        owned_ = &native;
        target = &native;
        native.AddRef();
        native_result = S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (IsEqualIID(iid, reshade ? d3d_native::reshade_unwrapped_guid :
                streamline_native::native_interface_guid)) {
            ++queries;
            if (nested_query && !nested_) {
                nested_ = true;
                IUnknown* inner{};
                const auto before = target ? target->refs() : refs();
                nested_ok = nested_ok && streamline_native::get_borrowed(this, &inner) &&
                    inner == static_cast<IUnknown*>(target ? target : this) &&
                    (target ? target->refs() : refs()) == before;
                nested_ = false;
            }
            if (SUCCEEDED(native_result) && !success_null) {
                auto* native = target ? target : this;
                native->AddRef();
                *output = static_cast<IUnknown*>(native);
            } else if (FAILED(native_result) && failure_nonnull) {
                // An invalid failure output is not an owned COM reference.
                *output = static_cast<IUnknown*>(target ? target : this);
            }
            return native_result;
        }
        if (IsEqualIID(iid, __uuidof(IUnknown))) {
            AddRef(); *output = static_cast<IUnknown*>(this); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { ++adds; return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ++releases;
        if (!refs_) { ++zero_releases; return 0; }
        const auto result = --refs_;
        if (!result) ++zero_releases;
        return result;
    }
};

static_assert(std::is_same_v<decltype(&streamline_native::get_borrowed),
    bool (*)(IUnknown*, IUnknown**, HRESULT*) noexcept>);

void null_and_failure_cases() {
    Probe native;
    IUnknown* output = &native;
    check(!streamline_native::get_borrowed(nullptr, &output) && !output, "null input clears output");
    check(!streamline_native::get_borrowed(&native, nullptr), "null output rejected");
    check(native.queries == 0 && native.releases == 0, "null cases call no COM API");
    native.native_result = E_FAIL;
    output = &native;
    check(!streamline_native::get_borrowed(&native, &output) && !output, "failed QI clears output");
    check(native.refs() == 1 && native.releases == 0, "failed QI does not release caller owner");
    native.failure_nonnull = true;
    check(!streamline_native::get_borrowed(&native, &output) && !output && native.releases == 0,
        "failed nonnull QI output grants no ownership");
    native.failure_nonnull = false;
    native.native_result = S_OK;
    native.success_null = true;
    check(!streamline_native::get_borrowed(&native, &output) && !output && native.refs() == 1,
        "success-null QI rejected without release");
    HRESULT status{};
    check(!streamline_native::get_borrowed(nullptr, &output, &status) && status == E_POINTER,
        "optional status identifies no-query null input");
    native.native_result = E_FAIL;
    native.success_null = false;
    const auto before = native.queries;
    check(!streamline_native::get_borrowed(&native, &output, &status) && status == E_FAIL &&
        native.queries == before + 1, "diagnostic status records exactly the existing failed QI");
    native.native_result = E_NOINTERFACE;
    check(streamline_native::get_borrowed(&native, &output, &status) && output == &native &&
        status == E_NOINTERFACE, "usable borrowed fallback differs from successful QI");
    native.native_result = S_OK;
    const auto refs = native.refs();
    check(streamline_native::get_borrowed(&native, &output, &status) && status == S_OK &&
        output == &native && native.refs() == refs, "status output preserves balanced successful QI");
    native.success_null = true;
    check(!streamline_native::get_borrowed(&native, &output, &status) && status == S_OK && !output,
        "malformed success-null preserves actual HRESULT without granting usable pointer");
}

void native_and_proxy_cases() {
    Probe native;
    IUnknown* output{};
    check(streamline_native::get_borrowed(&native, &output) && output == &native,
        "E_NOINTERFACE falls back to borrowed input");
    check(native.refs() == 1 && native.adds == 0 && native.releases == 0,
        "fallback has zero reference churn");
    native.native_result = S_OK;
    check(streamline_native::get_borrowed(&native, &output) && output == &native,
        "successful same-pointer QI returns borrowed input");
    check(native.refs() == 1 && native.adds == 1 && native.releases == 1,
        "same-pointer QI balances exactly one owned reference");
    {
        Probe proxy;
        proxy.own(native);
        const auto baseline = native.refs();
        const auto adds = native.adds, releases = native.releases;
        check(streamline_native::get_borrowed(&proxy, &output) && output == &native,
            "proxy QI returns distinct native base");
        check(native.refs() == baseline && native.adds == adds + 1 && native.releases == releases + 1,
            "proxy QI owned reference balanced exactly once");
        check(proxy.refs() == 1 && proxy.releases == 0 && native.refs() == 2,
            "input owner keeps borrowed base alive");
    }
    check(native.refs() == 1 && !native.zero_releases, "proxy destruction releases only its own base reference");
}

void repeated_and_nested_cases() {
    Probe native, proxy;
    proxy.own(native);
    const auto baseline = native.refs();
    const auto adds = native.adds, releases = native.releases;
    for (unsigned i = 0; i < 20000; ++i) {
        IUnknown* output{};
        check(streamline_native::get_borrowed(&proxy, &output) && output == &native &&
            native.refs() == baseline && proxy.refs() == 1, "repeated proxy lookup remains borrowed");
    }
    check(native.adds == adds + 20000 && native.releases == releases + 20000,
        "20000 lookups have zero net COM references");
    proxy.nested_query = true;
    const auto nested_adds = native.adds, nested_releases = native.releases;
    for (unsigned i = 0; i < 20000; ++i) {
        IUnknown* output{};
        check(streamline_native::get_borrowed(&proxy, &output) && output == &native && proxy.nested_ok &&
            native.refs() == baseline, "nested same-input lookups preserve baseline");
    }
    check(native.adds == nested_adds + 40000 && native.releases == nested_releases + 40000,
        "20000 nested lookups balance outer and inner QI exactly");
    check(proxy.releases == 0 && !proxy.zero_releases && !native.zero_releases,
        "no caller-owned object is released by borrowing");
}
void d3d_wrapper_chains() {
    Probe native, inner, outer;
    inner.own(native);
    inner.reshade = true;
    outer.own(inner);
    IUnknown* output{};
    HRESULT receipt{};
    const auto native_refs = native.refs(), inner_refs = inner.refs();
    for (unsigned i = 0; i < 20000; ++i) {
        check(d3d_native::get_borrowed(&outer, &output, &receipt) && output == &native &&
            receipt == S_OK && native.refs() == native_refs && inner.refs() == inner_refs,
            "Streamline over ReShade reaches native with balanced references");
    }
    outer.reshade = true;
    inner.reshade = false;
    check(d3d_native::get_borrowed(&outer, &output) && output == &native,
        "ReShade over Streamline reaches the same native identity");
    native.native_result = E_FAIL;
    check(!d3d_native::get_borrowed(&outer, &output, &receipt) && !output && receipt == E_FAIL,
        "inner query failure rejects, never falls back to wrapper");
    native.native_result = E_NOINTERFACE;
    check(d3d_native::get_borrowed(&native, &output, &receipt) && output == &native &&
        receipt == E_NOINTERFACE, "native unchanged when both contracts unsupported");
    native.native_result = S_OK;
    check(d3d_native::get_borrowed(&native, &output, &receipt) && output == &native &&
        receipt == S_OK && native.refs() == native_refs,
        "successful native self-query terminates with balanced ownership");
    native.native_result = E_NOINTERFACE;
    inner.target = &outer;
    check(!d3d_native::get_borrowed(&outer, &output, &receipt) && !output &&
        receipt == E_UNEXPECTED && outer.refs() == 1 && inner.refs() == inner_refs,
        "cyclic wrappers rejected with balanced QI refs");
    inner.target = &native;
    Probe chain[9];
    for (unsigned i = 0; i < 8; ++i) chain[i].target = &chain[i + 1];
    for (unsigned i = 0; i < 8; ++i) chain[i].native_result = S_OK;
    check(!d3d_native::get_borrowed(&chain[0], &output, &receipt) && !output &&
        receipt == E_UNEXPECTED, "wrapper depth is bounded without guessing identity");
    for (const auto& node : chain) check(node.refs() == 1, "bounded chain QI balanced");
    check(!d3d_native::get_borrowed(nullptr, &output, &receipt) && !output && receipt == E_POINTER,
        "D3D null input rejected");
    inner.success_null = true;
    check(!d3d_native::get_borrowed(&outer, &output) && !output,
        "malformed inner success-null rejected");
}
struct PrivateStore {
    std::array<unsigned char, 128> bytes{};
    UINT size{};
};
class DeviceProbe final : public IUnknown {
    ULONG refs_{1};
public:
    PrivateStore* store;
    bool deny_set{}, bad_read{}, short_read{}, absent_clear_error{};
    unsigned writes{}, reads{};
    LONG adapter{42};
    explicit DeviceProbe(PrivateStore& backing) : store(&backing) {}
    ULONG refs() const { return refs_; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        AddRef(); *out = static_cast<IUnknown*>(this); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
    LUID GetAdapterLuid() { return {static_cast<DWORD>(adapter), 0}; }
    HRESULT GetPrivateData(REFGUID guid, UINT* size, void* output) {
        ++reads;
        if (guid != d3d_owner::receipt_guid || !store->size) return DXGI_ERROR_NOT_FOUND;
        if (!size) return E_POINTER;
        const auto capacity = *size;
        *size = store->size;
        if (!output) return S_OK;
        if (capacity < store->size) return DXGI_ERROR_MORE_DATA;
        std::memcpy(output, store->bytes.data(), store->size);
        if (bad_read) static_cast<unsigned char*>(output)[0] ^= 1;
        if (short_read) --*size;
        return S_OK;
    }
    HRESULT SetPrivateData(REFGUID guid, UINT size, const void* input) {
        ++writes;
        if (deny_set) return E_ACCESSDENIED;
        if (!size && !store->size && absent_clear_error) return DXGI_ERROR_NOT_FOUND;
        if (guid != d3d_owner::receipt_guid || size > store->bytes.size()) return E_INVALIDARG;
        if (size && !input) return E_POINTER;
        if (size) std::memcpy(store->bytes.data(), input, size);
        store->size = size;
        return S_OK;
    }
};
void device_backing_receipts() {
    PrivateStore shared, separate;
    DeviceProbe queue(shared), alias(shared), other(separate);
    {
        d3d_owner::Cache<DeviceProbe> cache;
        d3d_owner::Receipt receipt;
        check(cache.same(&queue, &queue, &receipt) && receipt.canonical &&
            !queue.writes && !queue.reads, "canonical device fast path does not touch private data");
        check(cache.same(&queue, &alias, &receipt) && receipt.fresh && !receipt.canonical &&
            receipt.mask == 63 && !shared.size, "distinct wrappers require both round trips and cleanup");
        const auto writes = queue.writes + alias.writes, reads = queue.reads + alias.reads;
        for (unsigned i = 0; i < 20000; ++i)
            check(cache.same(&alias, &queue, &receipt) && !receipt.fresh &&
                queue.writes + alias.writes == writes && queue.reads + alias.reads == reads,
                "cached reverse-order owner has zero repeated private-data calls");
        check(!cache.same(&queue, &other, &receipt) && receipt.fresh && !receipt.verified &&
            !(receipt.mask & 1) && !shared.size && !separate.size,
            "different devices on same adapter rejected and temporary state cleaned");
        other.adapter = 77;
        check(!cache.same(&queue, &other), "different adapter not accepted");
        other.adapter = 42;
        check(queue.refs() == 3 && alias.refs() == 2 && other.refs() == 2,
            "cached pairs pin canonical identities against pointer reuse");
    }
    check(queue.refs() == 1 && alias.refs() == 1 && other.refs() == 1,
        "cache releases exactly its identity references");
    {
        d3d_owner::Cache<DeviceProbe> cache;
        alias.absent_clear_error = true;
        check(cache.same(&queue, &alias), "already-cleared alias may report NOT_FOUND after successful cleanup");
        alias.absent_clear_error = false;
    }
    {
        d3d_owner::Cache<DeviceProbe> cache;
        alias.deny_set = true;
        check(!cache.same(&queue, &alias), "forward-only shared state is insufficient");
        alias.deny_set = false;
    }
    {
        d3d_owner::Cache<DeviceProbe> cache;
        alias.bad_read = true;
        check(!cache.same(&queue, &alias) && !shared.size, "corrupt proof bytes rejected and removed");
        alias.bad_read = false;
    }
    {
        d3d_owner::Cache<DeviceProbe> cache;
        alias.short_read = true;
        check(!cache.same(&queue, &alias) && !shared.size, "short proof rejected and removed");
        alias.short_read = false;
    }
    {
        d3d_owner::Cache<DeviceProbe> cache;
        shared.size = 1; shared.bytes[0] = 0x91;
        const auto writes = queue.writes + alias.writes;
        check(!cache.same(&queue, &alias) && shared.size == 1 && shared.bytes[0] == 0x91 &&
            queue.writes + alias.writes == writes, "existing private value is never overwritten or deleted");
        shared.size = 0;
    }
    {
        PrivateStore store;
        DeviceProbe nodes[]{DeviceProbe(store), DeviceProbe(store), DeviceProbe(store),
            DeviceProbe(store), DeviceProbe(store), DeviceProbe(store), DeviceProbe(store),
            DeviceProbe(store), DeviceProbe(store), DeviceProbe(store)};
        d3d_owner::Cache<DeviceProbe> cache;
        for (unsigned i = 1; i <= 8; ++i) check(cache.same(&nodes[0], &nodes[i]), "eight exact proofs admitted");
        const auto reads = nodes[0].reads;
        check(!cache.same(&nodes[0], &nodes[9]) && nodes[0].reads == reads,
            "cache capacity rejects without unbounded probes or retained refs");
    }
}
class DeviceFacade final : public IUnknown {
    ULONG refs_{1};
    Microsoft::WRL::ComPtr<ID3D12Device> native_;
public:
    explicit DeviceFacade(ID3D12Device* device) : native_(device) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        AddRef(); *out = static_cast<IUnknown*>(this); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
    LUID GetAdapterLuid() { return native_->GetAdapterLuid(); }
    HRESULT GetPrivateData(REFGUID guid, UINT* size, void* data) {
        return native_->GetPrivateData(guid, size, data);
    }
    HRESULT SetPrivateData(REFGUID guid, UINT size, const void* data) {
        return native_->SetPrivateData(guid, size, data);
    }
};
void real_device_backing() {
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    const auto result = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(device.GetAddressOf()));
    check(SUCCEEDED(result), "real D3D12 device available for private-data ownership test");
    if (FAILED(result)) return;
    DeviceFacade queue(device.Get()), list(device.Get());
    d3d_owner::Cache<DeviceFacade> cache;
    d3d_owner::Receipt receipt;
    check(cache.same(&queue, &list, &receipt) && receipt.mask == 63 && receipt.fresh,
        "real GPU device backing shared through distinct COM facades is verified");
    UINT size{};
    check(device->GetPrivateData(d3d_owner::receipt_guid, &size, nullptr) == DXGI_ERROR_NOT_FOUND,
        "real GPU ownership challenge leaves no private data behind");
    check(cache.same(&list, &queue, &receipt) && !receipt.fresh,
        "real GPU proof cached in reverse order");
}
} // namespace

int main() {
    null_and_failure_cases();
    native_and_proxy_cases();
    repeated_and_nested_cases();
    d3d_wrapper_chains();
    device_backing_receipts();
    real_device_backing();
    std::cout << "Streamline native borrowing: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
