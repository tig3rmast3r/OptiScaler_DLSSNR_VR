#include "Streamline_NativeInterface.h"
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
        if (IsEqualIID(iid, streamline_native::native_interface_guid)) {
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
} // namespace

int main() {
    null_and_failure_cases();
    native_and_proxy_cases();
    repeated_and_nested_cases();
    std::cout << "Streamline native borrowing: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
