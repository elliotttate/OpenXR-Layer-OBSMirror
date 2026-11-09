# The list of OpenXR functions our layer will override.
override_functions = [
    "xrCreateSession",
    "xrDestroySession",
    "xrCreateSwapchain",
    "xrDestroySwapchain",
    "xrEnumerateSwapchainImages",
    "xrAcquireSwapchainImage",
    "xrReleaseSwapchainImage",
    "xrEnumerateViewConfigurationViews",
    "xrLocateViews",
    "xrBeginFrame",
    "xrEndFrame",
    "xrCreateReferenceSpace",
    "xrDestroySpace",
    "xrGetVisibilityMaskKHR",
    "xrCreateVulkanInstanceKHR",
    "xrCreateVulkanDeviceKHR"
]

# The list of OpenXR functions our layer will use from the runtime.
# Might repeat entries from override_functions above.
requested_functions = [
    "xrGetInstanceProperties",
    "xrGetSystemProperties",
    "xrGetSystem",
    "xrEnumerateViewConfigurationViews",
    "xrLocateSpace",
]

# The list of OpenXR extensions our layer will either override or use.
extensions = ["XR_KHR_visibility_mask", "XR_KHR_vulkan_enable2"]
