#pragma once
#ifndef MANGOHUD_WINFG_H
#define MANGOHUD_WINFG_H

#include <cstdint>
#include <string>

// win-fg frame-generation statistics reader.
//
// win-fg (the Vulkan frame-generation implicit layer) publishes per-frame
// counters into a POSIX shared-memory segment named "win-fg-<pid>-stats". This
// reader locates that segment (mirroring the FEX stats reader: it scans Termux
// tmp, /dev/shm, and other candidate dirs) and turns the cumulative counters
// into per-second rates so the HUD can show "real -> real+generated" FPS
// (e.g. "60->120").
namespace winfg {

// Shared-memory block layout (must match win-fg's winfg_stats.hpp).
struct Stats {
    uint32_t magic;           // 0x57464753 "WFGS"
    uint32_t version;
    uint64_t real_frames;     // cumulative REAL presents
    uint64_t gen_frames;      // cumulative GENERATED presents
    uint64_t last_ns;         // CLOCK_MONOTONIC timestamp of last update
    uint32_t enabled;         // 1 if FG active
    uint32_t multiplier;      // configured multiplier
};

constexpr uint32_t kMagic   = 0x57464753u;
constexpr uint32_t kVersion = 1;

// True once we have located and mapped a valid win-fg stats segment.
bool is_available();

// True if win-fg reports frame generation is currently enabled.
bool is_enabled();

// Per-second rates, computed from the cumulative counters since the last
// sampling period (matching MangoHud's fps_sampling_period cadence).
//   real_fps(): guest/app real frame rate.
//   gen_fps():  generated-frame rate (win-fg inserted).
//   total_fps(): real + generated (what actually reaches the display).
double real_fps();
double gen_fps();
double total_fps();

// Called once per HUD frame (from update_hud_info) to re-read the shared
// memory and update the FPS estimates.
void update();

} // namespace winfg

#endif // MANGOHUD_WINFG_H
