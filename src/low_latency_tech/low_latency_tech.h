#pragma once

#include <dxgi.h>
#if _MSC_VER
#include <d3d12.h>
#else
#include "../../external/d3d12.h"
#endif

#include "log.h"
#include <fakenvapi_inc.h>
#include <magic_enum.hpp>

#define INVALID_ID 0xFFFFFFFFFFFFFFFF

enum class CallSpot {
    SleepCall = 0,
    InputSample = 1,
    SimulationStart = 2
};

struct SleepParams {
    bool low_latency_enabled;
    bool fullscreen_vrr;
    bool control_panel_vsync_override;
};

struct SleepMode {
    bool low_latency_enabled;
    bool low_latency_boost;
    uint32_t minimum_interval_us; // 0 -> no fps limit
    bool use_markers_to_optimize; // TODO: log this if false
};

enum class MarkerType
{
    SIMULATION_START = 0,
    SIMULATION_END = 1,
    RENDERSUBMIT_START = 2,
    RENDERSUBMIT_END = 3,
    PRESENT_START = 4,
    PRESENT_END = 5,
    INPUT_SAMPLE = 6,
    TRIGGER_FLASH = 7,
    PC_LATENCY_PING = 8,
    OUT_OF_BAND_RENDERSUBMIT_START = 9,
    OUT_OF_BAND_RENDERSUBMIT_END = 10,
    OUT_OF_BAND_PRESENT_START = 11,
    OUT_OF_BAND_PRESENT_END = 12,
};

struct MarkerParams {
    uint64_t frame_id;
    MarkerType marker_type;
};

// R3.8: presentation identity is intentionally separate from render identity.
// A frame-generation stack may emit multiple presentations for one rendered
// frame. Backends may observe this relation, but it never authorizes a second
// pacing wait.
struct PresentationParams {
    uint64_t render_frame_id;
    uint64_t present_frame_id;
    uint32_t generation;
    bool interpolated;
};

class LowLatencyTech {
protected:
    CallSpot current_call_spot = CallSpot::SimulationStart;
    ForceReflex low_latency_override = ForceReflex::InGame;
    bool low_latency_enabled = false;
    bool effective_fg_state = false;
    bool inited_using_context = false;

public:
    LowLatencyTech():
        current_call_spot(CallSpot::SimulationStart), 
        low_latency_override(ForceReflex::InGame), 
        low_latency_enabled(false), 
        effective_fg_state(false),
        inited_using_context(false) {}
    virtual ~LowLatencyTech() {}

    virtual bool init(IUnknown* pDevice) = 0;
    virtual bool init_using_ctx(void* context) = 0;
    virtual void deinit() = 0;

    virtual Mode get_mode() = 0;
    virtual void* get_tech_context() = 0;
    virtual void set_fg_type(bool interpolated, uint64_t frame_id) = 0;
    virtual void set_low_latency_override(ForceReflex low_latency_override) = 0;
    virtual void set_effective_fg_state(bool effective_fg_state) = 0;

    virtual bool is_enabled() = 0;

    virtual void get_sleep_status(SleepParams* sleep_params) = 0;
    virtual void set_sleep_mode(SleepMode* sleep_mode) = 0;
    virtual void sleep() = 0;
    // Hybrid frontends can provide an exact canonical frame ID. Backends that
    // do not consume explicit IDs retain their normal sleep behavior.
    virtual void sleep_with_frame_id(uint64_t frame_id) { (void)frame_id; sleep(); }
    virtual void set_marker(IUnknown* pDevice, MarkerParams* marker_params) = 0;
    virtual void set_async_marker(MarkerParams* marker_params) = 0;
    // Optional D3D12 transport bridge. Backends that care about the concrete
    // command queue can consume it without changing legacy marker semantics.
    virtual void set_async_marker_on_queue(ID3D12CommandQueue* queue, MarkerParams* marker_params) {
        (void)queue;
        set_async_marker(marker_params);
    }
    virtual void observe_d3d12_queue(ID3D12CommandQueue* queue) { (void)queue; }

    // Optional canonical-timeline observer hooks. These are intentionally
    // no-ops for normal executors; cooperative observer backends such as
    // VulkanFlex can consume the canonical HybridFusion timeline without
    // becoming a second pacing owner.
    virtual void observe_canonical_frame_start(uint64_t frame_id) { (void)frame_id; }
    virtual void observe_canonical_marker(const MarkerParams* marker) { (void)marker; }
    virtual void observe_canonical_presentation(const PresentationParams* presentation) {
        (void)presentation;
    }
};
