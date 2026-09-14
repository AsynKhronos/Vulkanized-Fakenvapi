#include "al2_proxy.h"

#include "fakenvapi.h"
#include "log.h"

#include <atomic>
#include <cstdint>
#include <mutex>

AMD::AntiLag2DX12::PFNAmdExtD3DCreateInterface AL2Proxy::o_AmdExtD3DCreateInterface = nullptr;
AL2Proxy::PFNAmdDxExtCreate11 AL2Proxy::o_AmdDxExtCreate11 = nullptr;
std::atomic<bool> AL2Proxy::disableAl2Kill{false};

namespace {

std::mutex hook_mutex;
std::atomic<bool> dx12_hooked{false};
std::atomic<bool> dx11_hooked{false};
constexpr std::uintptr_t kDx11AntiLagRequest = 0xbf380ebc5ab4d0a6ull;

std::uint32_t fps_to_interval_us(unsigned int max_fps) noexcept {
    if (max_fps == 0) return 0;
    return static_cast<std::uint32_t>((1'000'000ull + max_fps - 1) / max_fps);
}

SleepMode sleep_mode_from_v1(const AMD::AntiLag2DX12::APIData_v1& data) noexcept {
    SleepMode mode{};
    mode.low_latency_enabled = data.eMode == 1;
    mode.low_latency_boost = false;
    mode.minimum_interval_us = fps_to_interval_us(data.maxFPS);
    mode.use_markers_to_optimize = true;
    return mode;
}

SleepMode sleep_mode_from_v1(const AMD::AntiLag2DX11::APIData_v1& data) noexcept {
    SleepMode mode{};
    mode.low_latency_enabled = data.eMode == 1;
    mode.low_latency_boost = false;
    mode.minimum_interval_us = fps_to_interval_us(data.maxFPS);
    mode.use_markers_to_optimize = true;
    return mode;
}

class D3D12AntiLagInputProxy final : public AMD::AntiLag2DX12::IAmdExtAntiLagApi {
public:
    explicit D3D12AntiLagInputProxy(IUnknown* device) : device_(device) {
        if (device_) device_->AddRef();
    }

    ~D3D12AntiLagInputProxy() {
        if (device_) device_->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IAmdExtAntiLagApi)) {
            *object = static_cast<AMD::AntiLag2DX12::IAmdExtAntiLagApi*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return refs_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT UpdateAntiLagState(VOID* raw_data) override {
        LowLatency* runtime = LowLatencyCtx::get();
        if (!runtime) return E_UNEXPECTED;

        if (!raw_data) {
            const std::uint64_t frame = frame_id_.fetch_add(1, std::memory_order_acq_rel) + 1;

            // AL2 Update(nullptr) is called once per frame immediately before
            // user input is polled.  Emit both anchors: SimulationStart drives
            // backends whose pacing point is simulation start, while
            // InputSample preserves the semantic source marker.
            runtime->FrontendSetMarker(
                policy::InputFrontend::AntiLag2,
                policy::GraphicsApi::D3D12,
                device_,
                MarkerType::SIMULATION_START,
                frame);
            runtime->FrontendSetMarker(
                policy::InputFrontend::AntiLag2,
                policy::GraphicsApi::D3D12,
                device_,
                MarkerType::INPUT_SAMPLE,
                frame);

            return runtime->FrontendSleep(
                       policy::InputFrontend::AntiLag2,
                       policy::GraphicsApi::D3D12,
                       device_,
                       frame)
                ? S_OK
                : E_FAIL;
        }

        const auto* header = static_cast<const unsigned int*>(raw_data);
        const unsigned int size = header[0];
        const unsigned int version = header[1];

        if (version == 1 && size >= sizeof(AMD::AntiLag2DX12::APIData_v1)) {
            const auto& data = *static_cast<const AMD::AntiLag2DX12::APIData_v1*>(raw_data);
            const SleepMode mode = sleep_mode_from_v1(data);
            return runtime->FrontendSetSleepMode(
                       policy::InputFrontend::AntiLag2,
                       policy::GraphicsApi::D3D12,
                       device_,
                       mode)
                ? S_OK
                : E_FAIL;
        }

        if (version == 2 && size >= sizeof(AMD::AntiLag2DX12::APIData_v2)) {
            const auto& data = *static_cast<const AMD::AntiLag2DX12::APIData_v2*>(raw_data);
            std::uint64_t frame = data.iiFrameIdx;
            if (frame == 0) frame = frame_id_.load(std::memory_order_acquire);
            if (frame == 0) frame = 1;

            if (data.flags.signalGetUserInputIdx) {
                runtime->FrontendSetMarker(
                    policy::InputFrontend::AntiLag2,
                    policy::GraphicsApi::D3D12,
                    device_,
                    MarkerType::INPUT_SAMPLE,
                    frame);
            }
            if (data.flags.signalEndOfFrameIdx) {
                runtime->FrontendSetMarker(
                    policy::InputFrontend::AntiLag2,
                    policy::GraphicsApi::D3D12,
                    device_,
                    MarkerType::RENDERSUBMIT_END,
                    frame);
            }
            if (data.flags.signalFgFrameType) {
                runtime->FrontendSetFgType(
                    policy::InputFrontend::AntiLag2,
                    data.flags.isInterpolatedFrame != 0,
                    frame);
            }
            return S_OK;
        }

        // Preserve forward compatibility. Unknown AMD control packets do not
        // get passed to a second pacing implementation; they are acknowledged
        // while the translated backend remains the sole output.
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    std::atomic<std::uint64_t> frame_id_{0};
    IUnknown* device_ = nullptr;
};

class D3D11AntiLagInputProxy final : public AMD::AntiLag2DX11::IAmdDxExtAntiLagApi {
public:
    unsigned int AddRef() override {
        return refs_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    unsigned int Release() override {
        const unsigned int remaining = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT UpdateAntiLagStateDx11(AMD::AntiLag2DX11::APIData_v1* data) override {
        LowLatency* runtime = LowLatencyCtx::get();
        if (!runtime) return E_UNEXPECTED;

        if (!data) {
            const std::uint64_t frame = frame_id_.fetch_add(1, std::memory_order_acq_rel) + 1;
            runtime->FrontendSetMarker(
                policy::InputFrontend::AntiLag2,
                policy::GraphicsApi::D3D11,
                nullptr,
                MarkerType::SIMULATION_START,
                frame);
            runtime->FrontendSetMarker(
                policy::InputFrontend::AntiLag2,
                policy::GraphicsApi::D3D11,
                nullptr,
                MarkerType::INPUT_SAMPLE,
                frame);
            return runtime->FrontendSleep(
                       policy::InputFrontend::AntiLag2,
                       policy::GraphicsApi::D3D11,
                       nullptr,
                       frame)
                ? S_OK
                : E_FAIL;
        }

        if (data->uiVersion == 1 && data->uiSize >= sizeof(*data)) {
            const SleepMode mode = sleep_mode_from_v1(*data);
            return runtime->FrontendSetSleepMode(
                       policy::InputFrontend::AntiLag2,
                       policy::GraphicsApi::D3D11,
                       nullptr,
                       mode)
                ? S_OK
                : E_FAIL;
        }
        return S_OK;
    }

private:
    std::atomic<unsigned int> refs_{1};
    std::atomic<std::uint64_t> frame_id_{0};
};

bool hook_one(PVOID* original, PVOID replacement) {
    if (DetourTransactionBegin() != NO_ERROR) return false;
    DetourUpdateThread(GetCurrentThread());
    if (DetourAttach(original, replacement) != NO_ERROR) {
        DetourTransactionAbort();
        return false;
    }
    return DetourTransactionCommit() == NO_ERROR;
}

} // namespace

HRESULT AL2Proxy::hkAmdExtD3DCreateInterface(IUnknown* pOuter, REFIID riid, void** ppvObject) {
    if (!o_AmdExtD3DCreateInterface) return E_NOINTERFACE;

    if (IsEqualIID(riid, IID_IAmdExtAntiLagApi) &&
        !disableAl2Kill.load(std::memory_order_acquire)) {
        if (!ppvObject) return E_POINTER;
        *ppvObject = static_cast<AMD::AntiLag2DX12::IAmdExtAntiLagApi*>(new D3D12AntiLagInputProxy(pOuter));
        if (auto* runtime = LowLatencyCtx::get()) {
            runtime->FrontendObserveEvidence(
                policy::InputFrontend::AntiLag2, policy::InputEvidenceContext);
        }
        spdlog::info("Anti-Lag 2 DX12 input frontend proxy created");
        return S_OK;
    }

    return o_AmdExtD3DCreateInterface(pOuter, riid, ppvObject);
}

HRESULT AL2Proxy::hkAmdDxExtCreate11(
    ID3D11Device* pDevice,
    AMD::AntiLag2DX11::IAmdDxExtInterface** ppAntiLagApi) {
    (void)pDevice;
    if (!o_AmdDxExtCreate11) return E_NOINTERFACE;

    const bool is_antilag_request =
        ppAntiLagApi && reinterpret_cast<std::uintptr_t>(*ppAntiLagApi) == kDx11AntiLagRequest;

    if (is_antilag_request && !disableAl2Kill.load(std::memory_order_acquire)) {
        *ppAntiLagApi = static_cast<AMD::AntiLag2DX11::IAmdDxExtAntiLagApi*>(new D3D11AntiLagInputProxy());
        if (auto* runtime = LowLatencyCtx::get()) {
            runtime->FrontendObserveEvidence(
                policy::InputFrontend::AntiLag2, policy::InputEvidenceContext);
        }
        spdlog::info("Anti-Lag 2 DX11 input frontend proxy created");
        return S_OK;
    }

    return o_AmdDxExtCreate11(pDevice, ppAntiLagApi);
}

void AL2Proxy::hookAntiLag() {
    std::scoped_lock lock(hook_mutex);

    if (!dx12_hooked.load(std::memory_order_relaxed)) {
        HMODULE module = GetModuleHandleA("amdxc64.dll");
        if (module) {
            o_AmdExtD3DCreateInterface = reinterpret_cast<AMD::AntiLag2DX12::PFNAmdExtD3DCreateInterface>(
                reinterpret_cast<VOID*>(GetProcAddress(module, "AmdExtD3DCreateInterface")));
            if (o_AmdExtD3DCreateInterface &&
                hook_one(reinterpret_cast<PVOID*>(&o_AmdExtD3DCreateInterface),
                         reinterpret_cast<PVOID>(hkAmdExtD3DCreateInterface))) {
                dx12_hooked.store(true, std::memory_order_release);
                spdlog::info("Anti-Lag 2 DX12 input frontend hooked");
            }
        }
    }

    if (!dx11_hooked.load(std::memory_order_relaxed)) {
        HMODULE module = GetModuleHandleA("amdxx64.dll");
        if (module) {
            o_AmdDxExtCreate11 = reinterpret_cast<PFNAmdDxExtCreate11>(
                reinterpret_cast<VOID*>(GetProcAddress(module, "AmdDxExtCreate11")));
            if (o_AmdDxExtCreate11 &&
                hook_one(reinterpret_cast<PVOID*>(&o_AmdDxExtCreate11),
                         reinterpret_cast<PVOID>(hkAmdDxExtCreate11))) {
                dx11_hooked.store(true, std::memory_order_release);
                spdlog::info("Anti-Lag 2 DX11 input frontend hooked");
            }
        }
    }
}

bool AL2Proxy::isDx12Hooked() noexcept {
    return dx12_hooked.load(std::memory_order_acquire);
}

bool AL2Proxy::isDx11Hooked() noexcept {
    return dx11_hooked.load(std::memory_order_acquire);
}
