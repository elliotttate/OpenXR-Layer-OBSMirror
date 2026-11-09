# Vulkan OpenXR applications

The layer mirrors OpenXR applications that render with **Vulkan**
(`XR_KHR_vulkan_enable2` / `XR_KHR_vulkan_enable`), in addition to Direct3D 11
and Direct3D 12.

## How it works

1. **Device setup.** When the application creates its Vulkan instance and
   device through `xrCreateVulkanInstanceKHR` / `xrCreateVulkanDeviceKHR`
   (`XR_KHR_vulkan_enable2`), the layer adds what the copy path needs:
   `VK_KHR_external_memory_win32`, `VK_KHR_external_semaphore_win32` (plus
   their promoted Vulkan 1.0 dependencies) and the `timelineSemaphore`
   feature. If the driver lacks one of them the device is left untouched and
   the session is simply not mirrored. Devices the application created by
   itself (for example with `XR_KHR_vulkan_enable`) are used as they are;
   OpenXR runtimes generally require the same extensions for their own Vulkan
   interop, and the layer skips mirroring cleanly if they are missing.
2. **Session.** `xrCreateSession` accepts `XrGraphicsBindingVulkanKHR`. The
   layer creates a private D3D12 device on the adapter the Vulkan device runs
   on (matched by `VkPhysicalDeviceIDProperties::deviceLUID`), and the mirror's
   D3D11 device on that same adapter.
3. **Shared copy target.** For every mirrorable color swapchain the layer
   creates a shared D3D12 texture and a shared D3D12 fence, and imports them
   into Vulkan (`VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT` memory, and
   a timeline semaphore from `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT`).
   Color swapchains get `XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT` added.
4. **Copy.** In `xrReleaseSwapchainImage`, before the runtime is called, the
   layer records `vkCmdCopyImage` (mip 0, every array layer) on the
   application's queue - which the application must already hold for that
   call - and signals the timeline semaphore. The existing D3D12 mirror path
   then opens the texture by NT handle and GPU-waits on the fence, so the OBS
   plugin, the Control Center and the shared-memory IPC are unchanged.

Vulkan formats are translated to their DXGI equivalents
(`DxgiFormatFromVulkan` in `vkmirror.cpp`); swapchains in other formats, and
multisampled swapchains, are not mirrored and are logged as such.

## Log

`%LOCALAPPDATA%\XR_APILAYER_NOVENDOR_OBSMirror.log` on a working session:

```
Vulkan bridge ready on '<GPU name>' (LUID ...), queue family 0 index 0
Graphics binding: Vulkan
Vulkan bridge: shared copy target 3744x2928 x2 (VkFormat 43 -> DXGI 29) for swapchain ...
Mirror source registered for swapchain ... (D3D12, copy fence: yes)
Mirror state: actively mirroring frames to OBS
```

Failures start with `Vulkan session cannot be mirrored:` or `Vulkan bridge:`
and name the missing piece.

## Build

The Vulkan headers come from the `external/Vulkan-Headers` submodule
(`git submodule update --init`); no Vulkan SDK is needed. Entry points are
resolved at run time from the application's Vulkan instance, so the layer does
not link against `vulkan-1.lib`.
