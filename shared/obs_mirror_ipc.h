#pragma once

#include <atomic>
#include <cstdint>

namespace obs_mirror_ipc {

    inline constexpr wchar_t kSharedMemoryName[] = L"OpenXROBSMirrorSurface";

    inline constexpr std::uint32_t kMirrorTextureCount = 3;

    // Lives in a named shared-memory section written concurrently by the OpenXR
    // layer (inside the game) and the OBS plugin. The counters below are the
    // cross-process signals, so they must be lock-free atomics; the float
    // parameters are single-writer (OBS) and are read whole each frame.
    struct MirrorSurfaceData {
        // Written by the layer: index of the frame most recently copied into
        // sharedHandle[lastProcessedIndex % kMirrorTextureCount].
        std::atomic<std::uint32_t> lastProcessedIndex{0};
        // Written by the OBS plugin every tick as a heartbeat.
        std::atomic<std::uint32_t> frameNumber{0};
        // Written by the OBS plugin: left = 0, right = 1, both = 2.
        std::atomic<std::uint32_t> eyeIndex{0};
        float overlap = 50.0f;
        float blend = 10.0f;
        float blendPos = 10.0f;
        // Camera smoothing (recording only): 0 = off, 100 = maximum smoothing.
        float smoothing = 0.0f;
        // Tan-space crop percentage the smoother may pan within (0-25).
        float smoothCrop = 8.0f;
        std::uint64_t sharedHandle[kMirrorTextureCount] = {};

        void reset() {
            for (auto& handle : sharedHandle)
                handle = 0;
        }
    };

    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "Cross-process signalling requires lock-free 32-bit atomics");
    static_assert(sizeof(MirrorSurfaceData) == 56, "The OpenXR layer and OBS plugin must use the same IPC layout");

} // namespace obs_mirror_ipc
