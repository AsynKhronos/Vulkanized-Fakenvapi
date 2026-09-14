#include "config.h"

#include "log.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <cwctype>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>

namespace {

std::wstring trim_lower(std::wstring value) {
    const auto not_space = [](wchar_t ch) { return !std::iswspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return std::towlower(ch); });
    return value;
}

policy::LoggingLevel parse_logging(std::wstring value, policy::LoggingLevel fallback) {
    value = trim_lower(std::move(value));
    if (value == L"off" || value == L"0") return policy::LoggingLevel::Off;
    if (value == L"info" || value == L"1") return policy::LoggingLevel::Info;
    if (value == L"trace" || value == L"2" || value == L"debug") return policy::LoggingLevel::Trace;
    return fallback;
}

policy::InputFrontend parse_input(std::wstring value, policy::InputFrontend fallback) {
    value = trim_lower(std::move(value));
    if (value == L"auto") return policy::InputFrontend::Auto;
    if (value == L"reflex" || value == L"nvapi_reflex") return policy::InputFrontend::Reflex;
    if (value == L"vk_nv_low_latency2" || value == L"vulkan_nv_low_latency2") return policy::InputFrontend::VkNvLowLatency2;
    if (value == L"xell") return policy::InputFrontend::XeLL;
    if (value == L"antilag2" || value == L"anti_lag2") return policy::InputFrontend::AntiLag2;
    return fallback;
}

policy::Backend parse_backend(std::wstring value, policy::Backend fallback) {
    value = trim_lower(std::move(value));
    if (value == L"auto") return policy::Backend::Auto;
    if (value == L"antilag2" || value == L"anti_lag2") return policy::Backend::AntiLag2;
    if (value == L"xell") return policy::Backend::XeLL;
    if (value == L"amd_anti_lag" || value == L"antilag_vk" || value == L"vk_amd_anti_lag") return policy::Backend::AmdAntiLagVk;
    if (value == L"latencyflex" || value == L"latency_flex") return policy::Backend::LatencyFlex;
    if (value == L"native_reflex" || value == L"reflex") return policy::Backend::NativeReflex;
    if (value == L"vulkanflex" || value == L"vulkan_flex" || value == L"vkflex") return policy::Backend::VulkanFlex;
    return fallback;
}

policy::AutoBool parse_auto_bool(std::wstring value, policy::AutoBool fallback) {
    value = trim_lower(std::move(value));
    if (value == L"auto") return policy::AutoBool::Auto;
    if (value == L"1" || value == L"true" || value == L"yes" || value == L"on" || value == L"enabled") return policy::AutoBool::Enabled;
    if (value == L"0" || value == L"false" || value == L"no" || value == L"off" || value == L"disabled") return policy::AutoBool::Disabled;
    return fallback;
}

policy::VulkanSpoofing parse_spoofing(std::wstring value, policy::VulkanSpoofing fallback) {
    value = trim_lower(std::move(value));
    if (value == L"off" || value == L"disabled") return policy::VulkanSpoofing::Off;
    if (value == L"selective" || value == L"auto") return policy::VulkanSpoofing::Selective;
    if (value == L"aggressive" || value == L"all") return policy::VulkanSpoofing::Aggressive;
    return fallback;
}

std::vector<std::wstring> split_csv(std::wstring value) {
    std::vector<std::wstring> result;
    std::wstringstream stream(std::move(value));
    std::wstring item;
    while (std::getline(stream, item, L',')) {
        item = trim_lower(std::move(item));
        if (!item.empty()) result.push_back(std::move(item));
    }
    return result;
}

policy::InputPriority parse_input_priority(const std::wstring& value, const policy::InputPriority& fallback) {
    policy::InputPriority result;
    for (const auto& token : split_csv(value)) {
        const auto frontend = parse_input(token, policy::InputFrontend::Auto);
        if (frontend != policy::InputFrontend::Auto && !result.contains(frontend)) {
            result.push_back(frontend);
        }
    }
    return result.size == 0 ? fallback : result;
}

policy::BackendOrder parse_backend_order(const std::wstring& value, const policy::BackendOrder& fallback) {
    policy::BackendOrder result;
    for (const auto& token : split_csv(value)) {
        const auto backend = parse_backend(token, policy::Backend::Auto);
        if (backend != policy::Backend::Auto && !result.contains(backend)) {
            result.push_back(backend);
        }
    }
    return result.size == 0 ? fallback : result;
}

} // namespace

Config& Config::get() {
    static Config instance;
    return instance;
}

Config::Config() {
    published_snapshots_.reserve(16);
    auto initial = std::make_unique<const policy::RuntimePolicySnapshot>();
    const auto* initial_ptr = initial.get();
    published_snapshots_.push_back(std::move(initial));
    active_snapshot_.store(initial_ptr, std::memory_order_release);
}

void Config::get_ini_path(wchar_t* path) {
    HMODULE module = GetModuleHandleW(nullptr);
    GetModuleFileNameW(module, path, MAX_PATH);
    wchar_t* last_slash = wcsrchr(path, L'\\');
    if (last_slash != nullptr) *last_slash = L'\0';
    wcscat(path, L"\\fakenvapi.ini");
}

std::wstring Config::get_string(const wchar_t* section, const wchar_t* key, const wchar_t* fallback) const {
    wchar_t buffer[1024]{};
    GetPrivateProfileStringW(section, key, fallback, buffer, static_cast<DWORD>(std::size(buffer)), path_);
    return buffer;
}

bool Config::has_key(const wchar_t* section, const wchar_t* key) const {
    static constexpr wchar_t sentinel[] = L"{VULKANIZED_KEY_MISSING}";
    return get_string(section, key, sentinel) != sentinel;
}

int Config::get_int(const wchar_t* section, const wchar_t* key, int fallback) const {
    return GetPrivateProfileIntW(section, key, fallback, path_);
}

bool Config::get_bool(const wchar_t* section, const wchar_t* key, bool fallback) const {
    return get_int(section, key, fallback ? 1 : 0) != 0;
}

policy::RuntimePolicySnapshot Config::load_snapshot(std::uint64_t generation) const {
    policy::RuntimePolicySnapshot result;
    result.generation = generation;

    // Logging supports both the new named setting and the original booleans.
    if (has_key(L"general", L"logging")) {
        result.general.logging = parse_logging(get_string(L"general", L"logging"), result.general.logging);
    } else {
        const bool enable_logs = get_bool(L"fakenvapi", L"enable_logs", true);
        const bool trace_logs = get_bool(L"fakenvapi", L"enable_trace_logs", false);
        result.general.logging = !enable_logs ? policy::LoggingLevel::Off
                                             : (trace_logs ? policy::LoggingLevel::Trace : policy::LoggingLevel::Info);
    }
    result.general.allow_fallback = get_bool(L"general", L"allow_fallback", result.general.allow_fallback);
    result.general.runtime_switching = get_bool(L"general", L"runtime_switching", result.general.runtime_switching);

    if (has_key(L"input", L"mode")) {
        result.input.mode = parse_input(get_string(L"input", L"mode"), result.input.mode);
    }
    if (has_key(L"input", L"priority")) {
        result.input.priority = parse_input_priority(get_string(L"input", L"priority"), result.input.priority);
    }
    result.input.minimum_quality = static_cast<std::uint8_t>(std::clamp(
        get_int(L"input", L"minimum_quality", result.input.minimum_quality), 0, 100));

    const bool has_d3d11_output = has_key(L"output", L"d3d11");
    const bool has_d3d12_output = has_key(L"output", L"d3d12");
    const bool has_vulkan_output = has_key(L"output", L"vulkan");
    if (has_d3d11_output) result.output.d3d11 = parse_backend(get_string(L"output", L"d3d11"), result.output.d3d11);
    if (has_d3d12_output) result.output.d3d12 = parse_backend(get_string(L"output", L"d3d12"), result.output.d3d12);
    if (has_vulkan_output) result.output.vulkan = parse_backend(get_string(L"output", L"vulkan"), result.output.vulkan);

    if (has_key(L"backend_order", L"d3d11")) {
        result.backend_order.d3d11 = parse_backend_order(get_string(L"backend_order", L"d3d11"), result.backend_order.d3d11);
    }
    if (has_key(L"backend_order", L"d3d12")) {
        result.backend_order.d3d12 = parse_backend_order(get_string(L"backend_order", L"d3d12"), result.backend_order.d3d12);
    }
    if (has_key(L"backend_order", L"vulkan")) {
        result.backend_order.vulkan = parse_backend_order(get_string(L"backend_order", L"vulkan"), result.backend_order.vulkan);
    }

    result.hybrid.xell_fusion = get_bool(L"hybrid", L"xell_fusion", result.hybrid.xell_fusion);
    result.hybrid.vulkan_fusion = get_bool(L"hybrid", L"vulkan_fusion", result.hybrid.vulkan_fusion);
    result.hybrid.startup_locked = get_bool(L"hybrid", L"startup_locked", result.hybrid.startup_locked);
    result.hybrid.startup_observations = static_cast<std::uint16_t>(std::clamp(
        get_int(L"hybrid", L"startup_observations", result.hybrid.startup_observations), 8, 4096));
    result.hybrid.aspect_adaptive = get_bool(L"hybrid", L"aspect_adaptive", result.hybrid.aspect_adaptive);
    result.hybrid.fill_missing_signals = get_bool(
        L"hybrid", L"fill_missing_signals", result.hybrid.fill_missing_signals);
    result.hybrid.secondary_minimum_quality = static_cast<std::uint8_t>(std::clamp(
        get_int(L"hybrid", L"secondary_minimum_quality", result.hybrid.secondary_minimum_quality), 0, 100));
    result.hybrid.aspect_minimum_quality = static_cast<std::uint8_t>(std::clamp(
        get_int(L"hybrid", L"aspect_minimum_quality", result.hybrid.aspect_minimum_quality), 0, 100));
    result.hybrid.aspect_switch_margin = static_cast<std::uint8_t>(std::clamp(
        get_int(L"hybrid", L"aspect_switch_margin", result.hybrid.aspect_switch_margin), 0, 32));

    result.vulkan.spoofing = parse_spoofing(get_string(L"vulkan", L"spoofing", L"selective"), result.vulkan.spoofing);
    result.vulkan.expose_nv_low_latency2 = parse_auto_bool(get_string(L"vulkan", L"expose_nv_low_latency2", L"auto"), result.vulkan.expose_nv_low_latency2);
    result.vulkan.expose_amd_anti_lag = parse_auto_bool(get_string(L"vulkan", L"expose_amd_anti_lag", L"auto"), result.vulkan.expose_amd_anti_lag);
    result.vulkan.prefer_native_extensions = get_bool(L"vulkan", L"prefer_native_extensions", result.vulkan.prefer_native_extensions);

    result.vulkanflex.enabled = get_bool(L"vulkanflex", L"enabled", result.vulkanflex.enabled);
    result.vulkanflex.core_pacing = get_bool(L"vulkanflex", L"core_pacing", result.vulkanflex.core_pacing);
    result.vulkanflex.only_native_vulkan = get_bool(L"vulkanflex", L"only_native_vulkan", result.vulkanflex.only_native_vulkan);
    result.vulkanflex.vkd3d_bridge = get_bool(L"vulkanflex", L"vkd3d_bridge", result.vulkanflex.vkd3d_bridge);
    result.vulkanflex.dxvk_bridge = get_bool(L"vulkanflex", L"dxvk_bridge", result.vulkanflex.dxvk_bridge);
    result.vulkanflex.dxvk_execution = parse_auto_bool(
        get_string(L"vulkanflex", L"dxvk_execution", L"auto"), result.vulkanflex.dxvk_execution);
    result.vulkanflex.present_precision = parse_auto_bool(
        get_string(L"vulkanflex", L"present_precision", L"auto"), result.vulkanflex.present_precision);
    result.vulkanflex.queue_pressure = parse_auto_bool(
        get_string(L"vulkanflex", L"queue_pressure", L"auto"), result.vulkanflex.queue_pressure);
    result.vulkanflex.queue_target_presents = static_cast<std::uint8_t>(std::clamp(
        get_int(L"vulkanflex", L"queue_target_presents", static_cast<int>(result.vulkanflex.queue_target_presents)), 1, 4));
    result.vulkanflex.queue_max_delay_us = static_cast<std::uint32_t>(std::clamp(
        get_int(L"vulkanflex", L"queue_max_delay_us", static_cast<int>(result.vulkanflex.queue_max_delay_us)), 0, 10000));
    result.vulkanflex.structural_stall = parse_auto_bool(
        get_string(L"vulkanflex", L"structural_stall", L"auto"), result.vulkanflex.structural_stall);
    result.vulkanflex.pipeline_threshold_us = static_cast<std::uint32_t>(std::clamp(
        get_int(L"vulkanflex", L"pipeline_threshold_us", static_cast<int>(result.vulkanflex.pipeline_threshold_us)), 250, 100000));
    result.vulkanflex.stall_max_compensation_us = static_cast<std::uint32_t>(std::clamp(
        get_int(L"vulkanflex", L"stall_max_compensation_us", static_cast<int>(result.vulkanflex.stall_max_compensation_us)), 0, 250000));
    result.vulkanflex.minimum_interval_us = static_cast<std::uint32_t>(std::clamp(
        get_int(L"vulkanflex", L"minimum_interval_us", static_cast<int>(result.vulkanflex.minimum_interval_us)), 0, 1000000));
    result.vulkanflex.max_sleep_us = static_cast<std::uint32_t>(std::clamp(
        get_int(L"vulkanflex", L"max_sleep_us", static_cast<int>(result.vulkanflex.max_sleep_us)), 1000, 1000000));

    result.openxr.enabled = get_bool(L"openxr", L"enabled", result.openxr.enabled);
    result.openxr.timeline = get_bool(L"openxr", L"timeline", result.openxr.timeline);
    result.openxr.clock_sync = get_bool(L"openxr", L"clock_sync", result.openxr.clock_sync);

    result.audio.enabled = get_bool(L"audio", L"enabled", result.audio.enabled);
    result.audio.wasapi_observer = get_bool(L"audio", L"wasapi_observer", result.audio.wasapi_observer);
    result.audio.clock = get_bool(L"audio", L"clock", result.audio.clock);
    result.audio.active_probe = get_bool(L"audio", L"active_probe", result.audio.active_probe);
    result.audio.queue_model = get_bool(L"audio", L"queue_model", result.audio.queue_model);
    result.audio.clock_probe_interval_ms = static_cast<std::uint32_t>(std::clamp(
        get_int(L"audio", L"clock_probe_interval_ms", static_cast<int>(result.audio.clock_probe_interval_ms)), 50, 5000));
    result.audio.adaptive_period = parse_auto_bool(
        get_string(L"audio", L"adaptive_period", L"auto"), result.audio.adaptive_period);
    result.audio.period_target_us = static_cast<std::uint32_t>(std::clamp(
        get_int(L"audio", L"period_target_us", static_cast<int>(result.audio.period_target_us)), 500, 50000));
    result.audio.period_max_reduction_percent = static_cast<std::uint8_t>(std::clamp(
        get_int(L"audio", L"period_max_reduction_percent", static_cast<int>(result.audio.period_max_reduction_percent)), 10, 90));

    result.overlay.startup_status = get_bool(L"overlay", L"startup_status", result.overlay.startup_status);
    result.overlay.duration_ms = static_cast<std::uint32_t>(std::clamp(
        get_int(L"overlay", L"duration_ms", static_cast<int>(result.overlay.duration_ms)), 250, 30000));

    result.legacy.force_latencyflex = get_bool(L"fakenvapi", L"force_latencyflex", false);
    result.legacy.force_reflex = static_cast<std::uint8_t>(std::clamp(get_int(L"fakenvapi", L"force_reflex", 0), 0, 2));
    result.legacy.latencyflex_mode = static_cast<std::uint8_t>(std::clamp(get_int(L"fakenvapi", L"latencyflex_mode", 0), 0, 2));
    result.legacy.save_pcl_to_file = get_bool(L"fakenvapi", L"save_pcl_to_file", false);
    result.legacy.legacy_keys_seen =
        has_key(L"fakenvapi", L"force_latencyflex") ||
        has_key(L"fakenvapi", L"force_reflex") ||
        has_key(L"fakenvapi", L"latencyflex_mode") ||
        has_key(L"fakenvapi", L"enable_logs") ||
        has_key(L"fakenvapi", L"enable_trace_logs");

    // Exact 1.x behavior: force_latencyflex wins only where the user did not
    // explicitly choose a new 0.2 output policy.
    if (result.legacy.force_latencyflex) {
        if (!has_d3d11_output) result.output.d3d11 = policy::Backend::LatencyFlex;
        if (!has_d3d12_output) result.output.d3d12 = policy::Backend::LatencyFlex;
        if (!has_vulkan_output) result.output.vulkan = policy::Backend::LatencyFlex;

        // In a pure 1.x config, force_latencyflex was a hard force rather than
        // a preference. Preserve that unless the user explicitly opted into
        // the new fallback policy.
        if (!has_key(L"general", L"allow_fallback") &&
            !has_d3d11_output && !has_d3d12_output && !has_vulkan_output) {
            result.general.allow_fallback = false;
        }
    }

    result.routing_signature = policy::compute_routing_signature(result);
    return result;
}

void Config::publish_snapshot(policy::RuntimePolicySnapshot snapshot) {
    auto published = std::make_unique<const policy::RuntimePolicySnapshot>(std::move(snapshot));
    const auto* ptr = published.get();
    published_snapshots_.push_back(std::move(published));
    active_snapshot_.store(ptr, std::memory_order_release);

    if (logging_ready_.load(std::memory_order_acquire)) {
        apply_logging_policy(*ptr);
        log_snapshot(*ptr);
    }
}

void Config::update_config() {
    std::scoped_lock lock(update_mutex_);
    const std::uint64_t next_generation = snapshot().generation + 1;
    publish_snapshot(load_snapshot(next_generation));
}

void Config::apply_logging_policy(const policy::RuntimePolicySnapshot& value) const {
    switch (value.general.logging) {
        case policy::LoggingLevel::Off:
            spdlog::set_level(spdlog::level::off);
            break;
        case policy::LoggingLevel::Info:
            spdlog::set_level(spdlog::level::info);
            spdlog::flush_on(spdlog::level::info);
            break;
        case policy::LoggingLevel::Trace:
            spdlog::set_level(spdlog::level::trace);
            spdlog::flush_on(spdlog::level::trace);
            break;
    }
}

void Config::log_snapshot(const policy::RuntimePolicySnapshot& value) const {
    spdlog::info("Policy generation: {}", value.generation);
    spdlog::info("Policy logging: {}", policy::to_string(value.general.logging));
    spdlog::info("Policy input mode: {}, minimum_quality: {}", policy::to_string(value.input.mode), value.input.minimum_quality);
    spdlog::info("Policy output d3d11: {}, d3d12: {}, vulkan: {}",
                 policy::to_string(value.output.d3d11),
                 policy::to_string(value.output.d3d12),
                 policy::to_string(value.output.vulkan));
    spdlog::info("Hybrid startup fusion: xell={}, vulkan={}, startup_locked={}, startup_observations={}, aspect_minimum_quality={}",
                 value.hybrid.xell_fusion ? "enabled" : "disabled",
                 value.hybrid.vulkan_fusion ? "enabled" : "disabled",
                 value.hybrid.startup_locked ? "enabled" : "disabled",
                 value.hybrid.startup_observations,
                 value.hybrid.aspect_minimum_quality);
    spdlog::info("VulkanFlex: enabled={}, core_pacing={}, native_only={}, vkd3d_bridge={}, dxvk_bridge={}, dxvk_execution={}, present_precision={}, queue_pressure={}, queue_target={}, queue_max_delay_us={}, structural_stall={}, pipeline_threshold_us={}, stall_max_compensation_us={}, minimum_interval_us={}, max_sleep_us={}",
                 value.vulkanflex.enabled, value.vulkanflex.core_pacing,
                 value.vulkanflex.only_native_vulkan, value.vulkanflex.vkd3d_bridge,
                 value.vulkanflex.dxvk_bridge,
                 value.vulkanflex.dxvk_execution == policy::AutoBool::Auto ? "auto" :
                     (value.vulkanflex.dxvk_execution == policy::AutoBool::Enabled ? "enabled" : "disabled"),
                 value.vulkanflex.present_precision == policy::AutoBool::Auto ? "auto" :
                     (value.vulkanflex.present_precision == policy::AutoBool::Enabled ? "enabled" : "disabled"),
                 value.vulkanflex.queue_pressure == policy::AutoBool::Auto ? "auto" :
                     (value.vulkanflex.queue_pressure == policy::AutoBool::Enabled ? "enabled" : "disabled"),
                 value.vulkanflex.queue_target_presents, value.vulkanflex.queue_max_delay_us,
                 value.vulkanflex.structural_stall == policy::AutoBool::Auto ? "auto" :
                     (value.vulkanflex.structural_stall == policy::AutoBool::Enabled ? "enabled" : "disabled"),
                 value.vulkanflex.pipeline_threshold_us, value.vulkanflex.stall_max_compensation_us,
                 value.vulkanflex.minimum_interval_us, value.vulkanflex.max_sleep_us);
    spdlog::info("XRFlex: enabled={}, timeline={}, clock_sync={}, host_clock=qpc, timing_owner=openxr-runtime, observer_only=true",
                 value.openxr.enabled, value.openxr.timeline, value.openxr.clock_sync);
    spdlog::info("AudioFlex: enabled={}, wasapi_observer={}, clock={}, active_probe={}, queue_model={}, clock_probe_interval_ms={}, adaptive_period={}, period_target_us={}, period_max_reduction_percent={}, mutation=init-time-opt-in",
                 value.audio.enabled, value.audio.wasapi_observer, value.audio.clock,
                 value.audio.active_probe, value.audio.queue_model, value.audio.clock_probe_interval_ms,
                 value.audio.adaptive_period == policy::AutoBool::Auto ? "auto" :
                     (value.audio.adaptive_period == policy::AutoBool::Enabled ? "enabled" : "disabled"),
                 value.audio.period_target_us, value.audio.period_max_reduction_percent);
    if (value.hybrid.aspect_adaptive) {
        spdlog::debug("Hybrid legacy key aspect_adaptive is accepted for compatibility; runtime aspect switching is disabled");
    }
    if (value.legacy.legacy_keys_seen) {
        spdlog::warn("Legacy [fakenvapi] routing keys are deprecated; migrate to [general]/[input]/[output]/[backend_order].");
    }
}

FILETIME Config::get_last_write_time(const wchar_t* file_path) const {
    WIN32_FILE_ATTRIBUTE_DATA file_info{};
    if (GetFileAttributesExW(file_path, GetFileExInfoStandard, &file_info)) return file_info.ftLastWriteTime;
    return FILETIME{0, 0};
}

void Config::monitor_config_file() {
    FILETIME last_write_time = get_last_write_time(path_);

    wchar_t directory[MAX_PATH]{};
    wcsncpy_s(directory, path_, _TRUNCATE);
    wchar_t* last_slash = wcsrchr(directory, L'\\');
    if (last_slash != nullptr) *last_slash = L'\0';

    HANDLE change_handle = FindFirstChangeNotificationW(directory, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (change_handle == INVALID_HANDLE_VALUE) {
        if (logging_ready_.load(std::memory_order_acquire)) spdlog::error("Unable to set up config file change notification.");
        return;
    }

    while (!stop_monitoring_.load(std::memory_order_acquire)) {
        const DWORD wait_status = WaitForSingleObject(change_handle, 500);
        if (wait_status == WAIT_TIMEOUT) continue;
        if (wait_status != WAIT_OBJECT_0) break;

        const FILETIME current_write_time = get_last_write_time(path_);
        if (CompareFileTime(&last_write_time, &current_write_time) != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            update_config();
            last_write_time = current_write_time;
        }

        if (!FindNextChangeNotification(change_handle)) break;
    }

    FindCloseChangeNotification(change_handle);
}

void Config::init_config() {
    get_ini_path(path_);
    stop_monitoring_.store(false, std::memory_order_release);
    update_config();
    std::thread(&Config::monitor_config_file, this).detach();
}

void Config::set_logging_ready() {
    logging_ready_.store(true, std::memory_order_release);
    apply_logging_policy(snapshot());
    log_snapshot(snapshot());
}

void Config::kill_config_monitoring() {
    const bool first_stop = !stop_monitoring_.exchange(true, std::memory_order_acq_rel);
    if (first_stop && logging_ready_.load(std::memory_order_acquire)) {
        spdlog::info("Stopping config monitoring thread");
    }
    if (first_stop) {
        logging_ready_.store(false, std::memory_order_release);
    }
}

const policy::RuntimePolicySnapshot& Config::snapshot() const noexcept {
    return *active_snapshot_.load(std::memory_order_acquire);
}

bool Config::get_enable_logs() const noexcept {
    return snapshot().general.logging != policy::LoggingLevel::Off;
}

bool Config::get_enable_trace_logs() const noexcept {
    return snapshot().general.logging == policy::LoggingLevel::Trace;
}

bool Config::get_force_latencyflex() const noexcept {
    return snapshot().legacy.force_latencyflex;
}

LFXMode Config::get_latencyflex_mode() const noexcept {
    return static_cast<LFXMode>(snapshot().legacy.latencyflex_mode);
}

ForceReflex Config::get_force_reflex() const noexcept {
    return static_cast<ForceReflex>(snapshot().legacy.force_reflex);
}

bool Config::get_save_pcl_to_file() const noexcept {
    return snapshot().legacy.save_pcl_to_file;
}
