# Vulkan capture (Windows)

For native Linux builds, see [Linux capture](LINUX.md). The remainder of this
page describes the Windows compositor path.

The OpenXR layer accepts both `XR_KHR_vulkan_enable` and
`XR_KHR_vulkan_enable2`. Their graphics bindings and swapchain image types are
aliases, so both use the same backend. The existing OBS plugin and Control
Center preview consume the same D3D11 mirror textures as before.

## Supported images

| Vulkan format | D3D11 compositor source |
| --- | --- |
| `VK_FORMAT_R8G8B8A8_UNORM` | `DXGI_FORMAT_R8G8B8A8_UNORM` |
| `VK_FORMAT_R8G8B8A8_SRGB` | `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB` |
| `VK_FORMAT_B8G8R8A8_UNORM` | `DXGI_FORMAT_B8G8R8A8_UNORM` |
| `VK_FORMAT_B8G8R8A8_SRGB` | `DXGI_FORMAT_B8G8R8A8_UNORM_SRGB` |
| `VK_FORMAT_A2B10G10R10_UNORM_PACK32` | `DXGI_FORMAT_R10G10B10A2_UNORM` |

Color swapchains can be ordinary 2D textures or texture arrays. Capture copies
mip zero of every array slice. Multisampled images resolve to a single-sample
image before readback. Existing eye selection, side-by-side output, cropping,
quad overlays, and smoothing use the common compositor.

Depth, protected content, cube maps, and other color formats (including float
HDR and `A2R10G10B10`) are not captured. They still pass through to the runtime.
The layer requests `XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT` for supported images;
if the runtime rejects this added usage, it retries the application's original
request and disables capture for that swapchain. Unsupported images and setup
failures are reported in the layer log.

## Transfer and lifetime

The backend requires only Vulkan 1.0 operations and no extra Vulkan instance
or device extensions. It loads `vulkan-1.dll` only for Vulkan sessions, so D3D
applications do not need the Vulkan loader installed.

Before forwarding `xrReleaseSwapchainImage`, the layer submits a copy on the
queue supplied in the session graphics binding. It uses the oldest acquired
image, and only after a successful `xrWaitSwapchainImage` (a timeout is not
sufficient). Barriers order application writes before the copy and restore
`VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` before the runtime uses the image.
This follows OpenXR's [Vulkan image state rules](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_vulkan_enable-swapchain-image-state.html)
and [queue synchronization requirements](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_vulkan_enable-concurrency.html).

A fence protects one reusable host-visible buffer per captured swapchain.
Capture polls the fence without waiting, invalidates mapped memory after GPU
completion, and uploads the bytes onto the compositor's D3D11 device. If the
previous capture is still pending, the new capture is skipped. The last
completed texture stays available for OBS. Pending GPU copies are drained
before destroying a swapchain or session. Application Vulkan objects are
borrowed and never destroyed by the layer.

The first image of each supported swapchain is cached even if OBS is not yet
listening. This allows a static overlay, which can only be acquired once, to
appear when capture starts later. Subsequent copies are made while a consumer
is active. A non-static overlay changed while no consumer was active refreshes
on its next release after capture resumes. The layer never tries to read
runtime-owned images again from `xrEndFrame`.

## Performance and adapter selection

This is a host-readback bridge, not external-memory GPU sharing. It transfers
four bytes per pixel per array slice from Vulkan to host memory and then to
D3D11. It can increase memory bandwidth usage and CPU upload time, and capture
can lag the application's current pose by one or more frames under load.
High resolutions and MSAA increase the cost. GPU-only interoperability is not
implemented in this backend.

The D3D11 compositor uses the default DXGI adapter for Vulkan sessions. Because
the input crosses host memory, it does not have to use the Vulkan adapter, but
OBS must still use the same adapter as the D3D11 compositor to open its shared
output textures. Only the first supported live session is mirrored; additional
concurrent sessions pass through without replacing its graphics binding.

## Build and automated checks

The Windows layer requires Visual Studio 2022, the repository's submodules and
NuGet packages, and Vulkan SDK headers via `VULKAN_SDK`. See the main README
for the native build commands. Both Win32 and x64 project configurations include
the backend and dynamically load the matching Vulkan loader.

The transfer tests also run on Linux with Vulkan headers, a loader, and a
Vulkan driver (Mesa lavapipe is sufficient):

```sh
cmake -S tests -B /tmp/obs-vulkan-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/obs-vulkan-tests
ctest --test-dir /tmp/obs-vulkan-tests --output-on-failure -V
```

Tests enable `VK_LAYER_KHRONOS_validation` when installed; their output states
whether validation was available. Set `VK_LAYER_VALIDATE_SYNC=1` to include
synchronization validation. To isolate the test from desktop capture/overlay
layers, use `VK_LOADER_LAYERS_DISABLE=~implicit~`. `VK_DRIVER_FILES` can select
the lavapipe ICD. On Windows, use a normal build directory and add `-C Debug`
to `ctest` when using a multi-configuration generator.

Tests check exact pixel values/channel ordering for all supported formats,
odd dimensions, stereo arrays, mip-zero selection, repeated image reuse,
MSAA resolves where supported, pending-readback backpressure, invalid formats
and dimensions, allocation failure, and destruction with an in-flight copy.
These tests cover the Vulkan transfer backend; they do not replace a native
Windows layer build or headset/OBS interoperability tests.

## Windows smoke test

1. Build and install the layer, enable Vulkan validation and the OpenXR API
   validation layer, and start a Vulkan OpenXR application using each of the
   two Vulkan enable extensions. Check for `Graphics binding: Vulkan` and
   `Mirroring Vulkan swapchain` in the layer log.
2. Verify left, right, and side-by-side captures, orientation, crop offsets,
   sRGB colors, array slices, and quad overlays in OBS and Control Center.
   Start OBS after the application to check static overlays.
3. Exercise single-sample and MSAA swapchains, multiple acquired images,
   a wait timeout followed by success, and a count-only/repeated image
   enumeration. Confirm there are no image-layout, synchronization, or
   object-lifetime validation errors.
4. Stop/restart the session, destroy swapchains with captures pending, and
   switch between Vulkan and D3D applications. Test unsupported formats and
   a runtime rejecting transfer-source usage: the headset must still work.
5. Check D3D11 and D3D12 captures for regressions and compare application and
   recording frame times with Vulkan capture enabled and disabled.
