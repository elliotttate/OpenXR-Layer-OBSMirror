#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace obs_mirror_ipc {

    inline constexpr wchar_t kSharedMemoryName[] = L"OpenXROBSMirrorSurface";

    inline constexpr std::uint32_t kMirrorTextureCount = 3;

    // Size of the original MirrorSurfaceData layout. Binaries that predate the
    // diagnostics block map only this prefix, so its layout is frozen forever.
    inline constexpr std::size_t kLegacySurfaceSize = 64;

    // Stamped by a writer once its half of MirrorDiagnostics is populated.
    // Pagefile-backed sections are zero-initialized, so a reader seeing zero
    // knows the other binary predates diagnostics (or has not connected yet).
    inline constexpr std::uint32_t kDiagnosticsMagic = 0x4D52584F; // "OXRM"
    inline constexpr std::uint32_t kDiagnosticsVersion = 1;

    // Diagnostic-only state appended behind the frozen 64-byte surface block.
    // Each process writes only its own section and reads the others', so no
    // cross-process synchronization is needed beyond the heartbeat atomics.
    // Nothing here may influence rendering behavior — readers must work with
    // all fields zero so mixed old/new layer+plugin installs keep capturing.
    //
    // LAYOUT CONTRACT: the Control Center's MirrorPreviewService.cs reads this
    // block through raw offsets (it has no C++ headers). The static_asserts at
    // the bottom of this file pin every offset it uses — extend this struct by
    // appending fields only, and keep the asserts and the C# constants in sync.
    struct MirrorDiagnostics {
        // ---- Written by the OpenXR layer (game process) ----
        std::uint32_t layerMagic{0};
        std::uint32_t layerDiagVersion{0};
        std::uint32_t layerPid{0};
        std::uint32_t layerAdapterLuidLow{0};
        std::int32_t layerAdapterLuidHigh{0};
        // Ticks every xrEndFrame while a session is alive, even when mirroring
        // is idle; lets OBS distinguish "game exited" from "game alive but not
        // feeding frames".
        std::atomic<std::uint32_t> layerHeartbeat{0};
        // Description of the published mirror texture ring (0 until the first
        // publish).
        std::uint32_t mirrorWidth{0};
        std::uint32_t mirrorHeight{0};
        std::uint32_t mirrorFormat{0};
        char layerVersionString[32]{};
        char applicationName[64]{};

        // ---- Written by the OBS plugin (magic cleared again on teardown) ----
        std::uint32_t pluginMagic{0};
        std::uint32_t pluginDiagVersion{0};
        std::uint32_t pluginPid{0};
        std::uint32_t pluginAdapterLuidLow{0};
        std::int32_t pluginAdapterLuidHigh{0};
        char pluginVersionString[32]{};

        // ---- Written by the Control Center mirror preview (magic cleared on
        // teardown). The preview ticks the same frameNumber heartbeat as the
        // OBS plugin, so this is how the layer tells the consumers apart. ----
        std::uint32_t previewMagic{0};
        std::uint32_t previewPid{0};
    };

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
        // Published last whenever the layer replaces the shared texture ring.
        // Handles can be numerically reused by Windows across application
        // processes, so OBS must not rely on handle equality alone to detect a
        // new capture session.
        std::atomic<std::uint32_t> surfaceGeneration{0};

        // Explicit tail padding so the diagnostics block starts exactly at the
        // legacy prefix boundary older binaries map.
        std::uint32_t reservedLegacyPad{0};

        MirrorDiagnostics diagnostics;

        void reset() {
            for (auto& handle : sharedHandle)
                handle = 0;
            surfaceGeneration.fetch_add(1, std::memory_order_release);
        }
    };

    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "Cross-process signalling requires lock-free 32-bit atomics");
    static_assert(offsetof(MirrorSurfaceData, diagnostics) == kLegacySurfaceSize,
                  "The legacy 64-byte prefix must stay layout-identical so older layer/plugin builds interoperate");
    // These offsets (plus kLegacySurfaceSize) are mirrored as constants in
    // MirrorPreviewService.cs; a failing assert means that file needs updating.
    static_assert(offsetof(MirrorDiagnostics, layerPid) == 8 &&
                      offsetof(MirrorDiagnostics, layerAdapterLuidLow) == 12 &&
                      offsetof(MirrorDiagnostics, layerAdapterLuidHigh) == 16 &&
                      offsetof(MirrorDiagnostics, layerHeartbeat) == 20 &&
                      offsetof(MirrorDiagnostics, applicationName) == 68 &&
                      offsetof(MirrorDiagnostics, previewMagic) == 184 &&
                      offsetof(MirrorDiagnostics, previewPid) == 188,
                  "MirrorDiagnostics layout changed - update MirrorPreviewService.cs to match");
    // Named pagefile-backed sections are page-granular, so as long as the whole
    // struct fits in one page every old/new layer+plugin pairing can map its
    // own view size against the same section.
    static_assert(sizeof(MirrorSurfaceData) <= 4096,
                  "MirrorSurfaceData must fit one page so mixed-version mappings always succeed");

} // namespace obs_mirror_ipc
