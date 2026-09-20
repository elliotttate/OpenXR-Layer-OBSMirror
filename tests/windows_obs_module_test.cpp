#include <obs-module.h>
#include <cstring>
#include <iostream>

// Load the real release DLL into each supported libobs runtime. Module loading
// alone is insufficient: OBS can reject obs_source_info and still load the DLL.
int main(int argc, char** argv) {
    if (argc != 3 || !obs_startup("en-US", nullptr, nullptr))
        return 1;
    obs_module_t* module = nullptr;
    bool passed = obs_open_module(&module, argv[1], argv[2]) == MODULE_SUCCESS && obs_init_module(module);
    struct ExpectedSource {
        const char* id;
        const char* name;
        const char* property;
    };
    for (const auto& expected : {
             ExpectedSource{"openxrmirror_capture", "VR Mirror Capture (Auto: OpenXR / SteamVR)", "captureeye"},
             ExpectedSource{"openvrmirror_capture", "OpenVR / SteamVR Mirror Capture", "openvr_eye"}}) {
        bool registered = false;
        const char* id = nullptr;
        for (size_t i = 0; obs_enum_input_types(i, &id); ++i)
            registered |= std::strcmp(id, expected.id) == 0;
        const char* name = obs_source_get_display_name(expected.id);
        auto* defaults = obs_get_source_defaults(expected.id);
        auto* properties = obs_get_source_properties(expected.id);
        bool sourcePassed = registered && name && std::strcmp(name, expected.name) == 0 && defaults && properties &&
                            obs_properties_get(properties, expected.property);
        auto* source = sourcePassed ? obs_source_create(expected.id, "Mirror registration test", defaults, nullptr) : nullptr;
        sourcePassed = sourcePassed && source && obs_source_get_width(source) > 0 && obs_source_get_height(source) > 0;
        if (source)
            obs_source_release(source);
        if (properties)
            obs_properties_destroy(properties);
        if (defaults)
            obs_data_release(defaults);
        std::cout << expected.id << (sourcePassed ? ": PASS\n" : ": FAIL\n");
        passed = passed && sourcePassed;
    }
    obs_shutdown();
    return passed ? 0 : 1;
}
