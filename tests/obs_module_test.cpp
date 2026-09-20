#include <obs-module.h>
#include <cstring>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2 || !obs_startup("en-US", nullptr, nullptr))
        return 1;
    obs_module_t* module = nullptr;
    bool passed = obs_open_module(&module, argv[1], ".") == MODULE_SUCCESS && obs_init_module(module);
    const char* id = "openxr_vulkan_mirror_linux";
    passed = passed && std::strcmp(obs_source_get_display_name(id), "OpenXR Vulkan Mirror") == 0;
    auto* defaults = obs_get_source_defaults(id);
    auto* properties = obs_get_source_properties(id);
    passed = passed && defaults && properties && obs_properties_get(properties, "eye") &&
             obs_properties_get(properties, "pid");
    auto* source = passed ? obs_source_create(id, "OpenXR test", defaults, nullptr) : nullptr;
    passed = passed && source;
    if (source)
        obs_source_release(source);
    if (properties)
        obs_properties_destroy(properties);
    if (defaults)
        obs_data_release(defaults);
    obs_shutdown();
    std::cout << (passed ? "OBS module loading and source registration passed\n" : "OBS module test failed\n");
    return passed ? 0 : 1;
}
