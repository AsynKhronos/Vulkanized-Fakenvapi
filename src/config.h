#pragma once

#include "runtime_policy.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

enum class ForceReflex : std::uint8_t {
    InGame = 0,
    ForceDisable = 1,
    ForceEnable = 2,
};

enum class LFXMode : std::uint8_t {
    Conservative = 0,
    Aggressive = 1,
    ReflexIDs = 2,
};

class Config {
public:
    static Config& get();

    void init_config();
    void kill_config_monitoring();
    void set_logging_ready();

    // Hot-path access: one acquire-load of a raw pointer. Published snapshots
    // are owned by Config for the remainder of the process lifetime.
    [[nodiscard]] const policy::RuntimePolicySnapshot& snapshot() const noexcept;

    // Legacy accessors retained during the 0.2 compatibility window.
    [[nodiscard]] bool get_enable_logs() const noexcept;
    [[nodiscard]] bool get_enable_trace_logs() const noexcept;
    [[nodiscard]] bool get_force_latencyflex() const noexcept;
    [[nodiscard]] LFXMode get_latencyflex_mode() const noexcept;
    [[nodiscard]] ForceReflex get_force_reflex() const noexcept;
    [[nodiscard]] bool get_save_pcl_to_file() const noexcept;

private:
    Config();

    wchar_t path_[MAX_PATH]{};
    std::mutex update_mutex_;
    std::atomic<bool> stop_monitoring_{false};
    std::atomic<bool> logging_ready_{false};
    std::atomic<const policy::RuntimePolicySnapshot*> active_snapshot_{nullptr};

    // The pointees never move. Keeping old immutable snapshots alive means
    // hot-path readers need no refcount, lock or lifetime handshake.
    std::vector<std::unique_ptr<const policy::RuntimePolicySnapshot>> published_snapshots_;

    static void get_ini_path(wchar_t* path);
    [[nodiscard]] std::wstring get_string(const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"") const;
    [[nodiscard]] bool has_key(const wchar_t* section, const wchar_t* key) const;
    [[nodiscard]] int get_int(const wchar_t* section, const wchar_t* key, int fallback) const;
    [[nodiscard]] bool get_bool(const wchar_t* section, const wchar_t* key, bool fallback) const;

    [[nodiscard]] policy::RuntimePolicySnapshot load_snapshot(std::uint64_t generation) const;
    void publish_snapshot(policy::RuntimePolicySnapshot snapshot);
    void update_config();
    void apply_logging_policy(const policy::RuntimePolicySnapshot& snapshot) const;
    void log_snapshot(const policy::RuntimePolicySnapshot& snapshot) const;

    [[nodiscard]] FILETIME get_last_write_time(const wchar_t* file_path) const;
    void monitor_config_file();
};
