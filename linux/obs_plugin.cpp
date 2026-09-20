#include <obs-module.h>
#include <util/platform.h>
#include "frame_ipc.h"

#include <algorithm>
#include <mutex>
#include <new>

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("jebot-git")

namespace {
    struct Capture {
        obs_source_t* source;
        std::mutex mutex;
        uint32_t pid = 0;
        int eye = 0;
        std::string path;
        linux_mirror::Frame frame;
        uint64_t sequence = 0, lastFrame = 0, lastScan = 0;
    };
    void update(void* data, obs_data_t* settings) {
        auto& capture = *static_cast<Capture*>(data);
        std::lock_guard<std::mutex> lock(capture.mutex);
        capture.eye = std::clamp<int>(obs_data_get_int(settings, "eye"), 0, 2);
        capture.pid = static_cast<uint32_t>(std::max<int64_t>(0, obs_data_get_int(settings, "pid")));
        capture.sequence = 0;
        capture.path.clear();
        capture.lastScan = 0;
    }
    void* create(obs_data_t* settings, obs_source_t* source) {
        auto* capture = new (std::nothrow) Capture;
        if (!capture)
            return nullptr;
        capture->source = source;
        update(capture, settings);
        obs_source_set_async_unbuffered(source, true);
        return capture;
    }
    void tick(void* data, float) {
        auto& capture = *static_cast<Capture*>(data);
        std::lock_guard<std::mutex> lock(capture.mutex);
        const uint64_t now = os_gettime_ns();
        try {
            if (now - capture.lastScan > 1000000000ull) {
                const auto path = linux_mirror::newestFrame(capture.pid);
                if (path != capture.path) {
                    capture.path = path;
                    capture.sequence = 0;
                }
                capture.lastScan = now;
            }
            if (!capture.path.empty() && linux_mirror::readFrame(capture.path, capture.frame, capture.sequence)) {
                const auto& header = capture.frame.header;
                capture.sequence = header.sequence;
                capture.lastFrame = now;
                obs_source_frame output{};
                output.width = header.width;
                uint32_t offset = 0;
                if (capture.eye == 0 || !header.rightWidth)
                    output.width = header.leftWidth;
                else if (capture.eye == 1) {
                    offset = header.leftWidth;
                    output.width = header.rightWidth;
                }
                output.height = header.height;
                output.format = VIDEO_FORMAT_RGBA;
                output.data[0] = capture.frame.pixels.data() + offset * 4;
                output.linesize[0] = header.stride;
                output.timestamp = now;
                output.full_range = true;
                obs_source_output_video(capture.source, &output);
            } else if (capture.lastFrame && now - capture.lastFrame > 3000000000ull) {
                obs_source_output_video(capture.source, nullptr);
                capture.lastFrame = 0;
            }
        } catch (...) {
            // A disappearing producer or allocation failure must not escape
            // through OBS's C callbacks. Retry discovery on the next tick.
            capture.path.clear();
            capture.lastScan = 0;
        }
    }
    obs_properties_t* properties(void*) {
        auto* result = obs_properties_create();
        auto* eye = obs_properties_add_list(result, "eye", "Eye", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
        obs_property_list_add_int(eye, "Left", 0);
        obs_property_list_add_int(eye, "Right", 1);
        obs_property_list_add_int(eye, "Side by side", 2);
        obs_properties_add_int(result, "pid", "Application process ID (0 = automatic)", 0, INT32_MAX, 1);
        return result;
    }
} // namespace

bool obs_module_load(void) {
    static_assert(LIBOBS_API_MAJOR_VER >= 32, "Build with the supported OBS 32 SDK or newer");
    if ((obs_get_version() >> 24) < 32) {
        blog(LOG_ERROR, "[OpenXR OBSMirror] OBS Studio 32 or newer is required; found %s. Update OBS to load the mirror source.",
             obs_get_version_string());
        return false;
    }
    obs_source_info info{};
    info.id = "openxr_vulkan_mirror_linux";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
    info.get_name = [](void*) { return "OpenXR Vulkan Mirror"; };
    info.create = create;
    info.destroy = [](void* data) { delete static_cast<Capture*>(data); };
    info.update = update;
    info.video_tick = tick;
    info.get_properties = properties;
    info.get_defaults = [](obs_data_t* settings) {
        obs_data_set_default_int(settings, "eye", 0);
        obs_data_set_default_int(settings, "pid", 0);
    };
    obs_register_source(&info);
    blog(LOG_INFO, "[OpenXR OBSMirror] Linux Vulkan plugin 0.4.0-beta.2 loaded");
    return true;
}
