# Linux Vulkan capture (experimental)

The project ships a native Linux OpenXR API layer and an OBS input plugin. It
supports native Linux Vulkan applications using either OpenXR Vulkan binding.
It is not a Wine/Proton layer: Windows games running under Proton need a
Windows OpenXR layer inside their Wine prefix, and the Windows D3D11 transport
does not connect to this Linux OBS source.

## Install

Use a native OBS package (OBS 32; older versions are unsupported), a Vulkan driver, and a configured
OpenXR runtime such as Monado, WiVRn, or SteamVR. Extract the Linux x86_64
release archive, close OBS and the OpenXR application, then run:

```sh
./Install-Linux.sh
```

Installation is per-user; do not use sudo. The script installs the implicit
layer manifest below `$XDG_DATA_HOME/openxr/1/api_layers/implicit.d` (default
`~/.local/share`) and the OBS plugin below
`$XDG_CONFIG_HOME/obs-studio/plugins` (default `~/.config`). Restart OBS, add
the **OpenXR Vulkan Mirror** source, and start your OpenXR application.

The source offers left, right, and side-by-side eyes. **Application process ID**
defaults to zero, which chooses the most recently active capture. Specify a
PID to select one application when several are running. Use OBS's standard
transform/crop controls and filters to frame the recording.

Run `./Uninstall-Linux.sh` to remove this installation. To disable the layer for
one application without uninstalling, launch it with
`DISABLE_XR_APILAYER_NOVENDOR_OBSMirror=1`.

Flatpak/sandboxed OBS and Steam applications may not see the same binaries or
frame transport directory. This initial release targets native packages;
sandbox integration is not included.

## Features and differences from Windows

- Captures the first valid projection layer, including image-rect crop offsets,
  stereo texture arrays, single-view submissions, and MSAA resolves.
- Supports RGBA8/BGRA8 UNORM and sRGB, and A2B10G10R10 UNORM (converted to RGBA8
  for OBS). Copies mip zero; excludes depth, cube maps, protected images, and
  unsupported formats.
- Uses the shared Vulkan readback backend. It restores OpenXR image layouts,
  respects acquisition/wait/release ordering, and drains pending copies before
  resource destruction.
- Frames cross a private per-user local file transport with nonblocking file
  locks. The source clears after three seconds without a new frame. Frames are
  capped at 128 MiB. On allocation, disk-space, or lock failures, capture skips
  the frame while the application's OpenXR calls continue.
- The initial Linux plugin does **not** include the Windows Control Center,
  OpenVR fallback, quad-layer composition, camera smoothing, or recording
  overscan. The Windows packages retain those existing features.

Readback and CPU copies consume bandwidth, can add recording latency, and
are not GPU-only sharing. The Linux layer currently captures every released
supported image, even when OBS is closed. Uninstall or disable the layer when
you do not need capture. Live headset testing is still required for each
runtime/application combination; a successful build and automated tests do
not establish runtime compatibility.

The transport directory defaults to
`$XDG_RUNTIME_DIR/openxr-obsmirror-UID`, or `/tmp/openxr-obsmirror-UID` when
`XDG_RUNTIME_DIR` is unset. It must be owned by the user with mode 0700.
`OPENXR_OBS_MIRROR_DIR` can select another private directory if set identically
for OBS and the OpenXR application. Files use mode 0600 and are deleted at
normal session shutdown. After a crashed application, stale `.frame` files
may be removed manually when it is no longer running.

## Build and test

On Ubuntu 24.04:

```sh
sudo apt-get install build-essential cmake libvulkan-dev mesa-vulkan-drivers \
  vulkan-validationlayers libsimde-dev xvfb
# Download the official OBS 32.2.2 Ubuntu package from obsproject/obs-studio:
sudo apt-get install ./OBS-Studio-32.2.2-Ubuntu-24.04-x86_64.deb
sudo ldconfig
git submodule update --init external/OpenXR-SDK
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DOBS_INCLUDE_DIR=/usr/local/include/obs \
  -DOBS_LIBRARY=/usr/local/lib/x86_64-linux-gnu/libobs.so
cmake --build build --parallel
VK_LAYER_VALIDATE_SYNC=1 VK_LOADER_LAYERS_DISABLE='~implicit~' \
  xvfb-run -a ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/package"
```

The Linux release is built on Ubuntu 24.04 x86_64 (glibc 2.39). Older systems
should build from source. OBS headers/library can be selected with
`OBS_INCLUDE_DIR` and `OBS_LIBRARY`; OpenXR headers with `OPENXR_INCLUDE_DIR`.

Automated tests exercise the Vulkan backend, OpenXR loader negotiation and
interception against a mock runtime using real Vulkan images, frame transport,
and OBS module loading/source registration under a virtual X server. They do
not require a headset. Run a real OpenXR application with Vulkan and OpenXR
validation enabled to verify actual headset/recording behavior before relying
on this experimental release.
