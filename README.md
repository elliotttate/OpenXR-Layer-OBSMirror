# OpenXR + OpenVR OBS Mirror

Vulkan capture is available on Windows and in experimental native Linux packages. See [Linux installation and limitations](docs/LINUX.md) and [Vulkan capture](docs/VULKAN.md).

**Capture native OpenXR applications or the SteamVR/OpenVR compositor directly
in OBS Studio. OpenXR capture can also use a wider, steadier recording camera
while the headset continues to look and track normally.**

OpenXR OBS Mirror combines a native OpenXR API layer, native OpenXR and OpenVR
OBS sources, and a self-contained dark WinUI 3 Control Center. It supports
Direct3D 11, Direct3D 12, and Vulkan OpenXR applications plus SteamVR/OpenVR compositor
capture on Windows x64, while keeping the machine's normal headset runtime as
the default.

Key recording controls include:

- recording-only FOV overscan with an unchanged headset center crop;
- live camera smoothing and crop margin;
- independent show/hide control for OpenXR composition quad layers;
- an in-app preview that follows either OpenXR or OpenVR/SteamVR capture;
- live runtime, layer, plugin, hash, and diagnostic-log status.
- native SteamVR/OpenVR left-eye, right-eye, or stereo mirror capture.

## See it in action

| Wider FOV + smoothed camera | Control Center walkthrough |
| :---: | :---: |
| [![Wider FOV and smoothed VR footage](https://img.youtube.com/vi/0Aa91IXBh3c/maxresdefault.jpg)](https://youtu.be/0Aa91IXBh3c) | [![OBS OpenXR recording UI](https://img.youtube.com/vi/0CDRNip2I10/maxresdefault.jpg)](https://www.youtube.com/watch?v=0CDRNip2I10) |
| Recording-only FOV overscan and camera smoothing in action. | A guided tour of installation, status, and recording controls. |

Click either preview to watch on YouTube.

The OpenXR layer template was based on
[OpenXR-Layer-Template](https://github.com/mbucchia/OpenXR-Layer-Template).

## Quick install

Requires OBS Studio 32 (release builds are tested with 32.2.2). Older OBS
versions are unsupported.

1. Open the [latest GitHub release](https://github.com/elliotttate/OpenXR-Layer-OBSMirror/releases/latest).
2. Close OBS Studio and any running OpenXR application.
3. Download and run the `OpenXR-OBSMirror-...-Setup.exe` installer.
4. Open OBS Studio and add **VR Mirror Capture (Auto: OpenXR / SteamVR)**.
   It selects the correct capture backend automatically.
5. Start the VR application normally through your headset software.

Setup installs the matching OBS source, registers the layer for the current
user, adds Start menu integration, and opens Control Center. It does **not**
select a simulator or replace the system OpenXR runtime. The administrator
prompt is used to place the OBS source in OBS Studio's shared plugin folder.

Prefer a portable install? Extract the complete portable ZIP, double-click
`Launch OpenXR OBS Mirror.cmd`, then use **Install / update** in Control Center.
The app includes its .NET and Windows App SDK runtime files.

See [docs/INSTALL.md](docs/INSTALL.md) for complete setup, update, uninstall,
recording-control, and troubleshooting instructions. Verify downloads against
the release's `SHA256SUMS.txt`; current builds are unsigned and may trigger a
Windows SmartScreen warning.

Manual current-user layer unregistration is also available:

```powershell
pwsh -File .\scripts\Uninstall-Layer.ps1 -Scope CurrentUser
```

## Build and install from source

Initialize submodules and restore the native NuGet packages:

```powershell
git submodule update --init --recursive
nuget restore .\OpenXR-Layer-OBSMirror.sln `
  -Source https://api.nuget.org/v3/index.json
```

Install the [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) and make sure
`VULKAN_SDK` points to its installation directory, then build the x64 layer
with Visual Studio 2022. Only Vulkan headers are needed at build time; the
layer loads the Vulkan driver dynamically for Vulkan sessions.

```powershell
msbuild .\OpenXR-Layer-OBSMirror.sln /m `
  /p:Configuration=Release /p:Platform=x64
```

OBS Studio 32 is the supported baseline on Windows and Linux. Older OBS
versions are unsupported. Releases build and test against OBS 32.2.2, including
loading the packaged Windows DLL and registering both capture sources. For a
Windows release build, extract the official OBS 32.2.2 Windows ZIP and use
matching source:

```powershell
git clone --depth 1 --branch 32.2.2 `
  https://github.com/obsproject/obs-studio.git C:\src\obs-studio-32.2.2
pwsh -File .\scripts\Build-OBSPlugin.ps1 `
  -OBSSourcePath C:\src\obs-studio-32.2.2 `
  -OBSInstallPath C:\build-deps\obs-studio-32.2.2
```

With OBS closed, install both freshly built components for the current user:

```powershell
pwsh -File .\scripts\Setup-OBS.ps1
```

To update the hash-versioned layer without interrupting active OBS, add
`-AllowRunningOBS`. If the plugin binary also changed, close OBS and run setup
again so the new source can be copied safely.

This places the layer under `%LOCALAPPDATA%\OpenXR-OBSMirror`, registers its
manifest under `HKCU\Software\Khronos\OpenXR\1\ApiLayers\Implicit`, and
installs the plugin under OBS's Windows discovery path at
`%ProgramData%\obs-studio\plugins\win-openxr\bin\64bit`.

## Choosing a capture source

- **VR Mirror Capture (Auto: OpenXR / SteamVR)** is the recommended source. It
  reads an OpenXR application's image through this project's API layer when one
  is available, then automatically falls back to SteamVR's native compositor
  mirror for OpenVR applications. Existing scenes saved with the older
  **OpenXR Mirror Capture** name are upgraded in place because the source ID is
  unchanged. Direct3D 11, Direct3D 12, and Vulkan OpenXR applications are supported.
  Vulkan uses asynchronous host readback into the existing D3D11 compositor,
  which adds transfer overhead and can add capture latency. See
  [Vulkan support](docs/VULKAN.md) for formats, limitations, and validation steps.
- **OpenVR / SteamVR Mirror Capture** reads SteamVR's native compositor mirror
  directly and remains available as an explicit advanced source. It offers left,
  right, and side-by-side stereo modes, percentage crop controls, automatic
  recording-canvas fill, and reconnect-on-demand. SteamVR must be running.

The automatic source loads the official Valve OpenVR API from the plugin folder
only when SteamVR is already running and no OpenXR mirror is available. It does
not launch SteamVR or change the active OpenXR runtime. OpenXR overscan,
smoothing, and quad-layer controls
cannot modify an already-composited SteamVR mirror, so those controls apply only
while the automatic source is using its OpenXR backend.

## Control Center

The dark WinUI 3 Control Center provides one place to inspect layer, plugin,
runtime, and OBS status; install or update both components; register the layer;
preview the active OpenXR or OpenVR mirror image; configure recording overscan; control camera
smoothing; show or hide OpenXR quad-layer UI in the recording; and read live logs.
It is headset-first: the dashboard shows the effective runtime, warns when a
simulator override is active, and provides **Use headset runtime** to clear
per-user simulator selectors and return to the machine-wide OpenXR runtime.

Build a self-contained x64 copy:

```powershell
pwsh -File .\scripts\Build-ControlCenter.ps1
```

Build the native layer, OBS plugin using the 32.2.2 baseline, Control Center, installer,
portable ZIP, and checksums in one reproducible command:

```powershell
pwsh -File .\scripts\Build-Release.ps1 `
  -Version 0.4.0-beta.2 `
  -OBSSourcePath C:\src\obs-studio-32.2.2 `
  -OBSInstallPath C:\build-deps\obs-studio-32.2.2
```

Run `bin\x64\Release\ControlCenter\OBSMirror.ControlCenter.exe`. Overscan
changes apply when the OpenXR application next starts. Camera-smoothing changes
are picked up live by an active OBS Mirror source. Quad-layer visibility is
picked up live by the updated OpenXR layer after it has been loaded once. The
Dashboard preview connects directly to the active OpenXR shared image or
SteamVR compositor mirror used by OBS and pauses when another Control Center
page is selected. Opening Control Center never starts SteamVR or changes the
active OpenXR runtime.

## Runtime notes

- Start the OpenXR application after installing the layer.
- OBS can load the source before the VR application starts; the source retries
  its IPC connection once the application creates the shared mirror surface.
- Running OBS elevated may improve GPU scheduling priority on some systems, but
  the plugin itself does not require administrator privileges.
- The OpenXR application and OBS must run on the same Windows desktop and use a
  compatible D3D11 adapter for the shared textures to open.
- The OpenVR source also requires OBS and SteamVR to use the same GPU. Its OBS
  interop path is D3D11 because that is the mirror interface SteamVR exposes;
  it does not change the rendering API used by the VR application.
- The machine-wide OpenXR runtime selected by the headset software is the normal
  default. The Control Center never selects a simulator merely by opening its
  optional testing tool, and it strips inherited `XR_RUNTIME_JSON` overrides
  from applications that it launches.
- A simulator can leave per-user `XR_RUNTIME_JSON` or `ActiveRuntime` overrides
  behind. Use **Use headset runtime** in the Control Center to clear both 64-bit
  and 32-bit per-user selectors. Restart any launcher that was already running
  while the old environment override was active.
- Some simulator versions refresh OpenXR API-layer registration while testing.
  If the layer status changes after a simulator session, turn the **Layer**
  switch back on before the next OpenXR application launch.

## Recording overscan (experimental)

Recordings normally show exactly the headset's field of view, so head motion
sits at the very edge of the frame. Recording overscan asks the OpenXR application to render
a wider field of view and a proportionally larger image, feeds the full wide
image to OBS, and submits only the original central crop to the OpenXR runtime
— the headset view is unchanged, including its pixels-per-degree.

```powershell
# Enable with the defaults (115% horizontal, 108% vertical, ~24% more pixels)
pwsh -File .\scripts\Set-RecordingOverscan.ps1 -Enable

# Custom scale
pwsh -File .\scripts\Set-RecordingOverscan.ps1 -Enable -HorizontalPercent 120 -VerticalPercent 110

# Turn it off again
pwsh -File .\scripts\Set-RecordingOverscan.ps1 -Disable
```

### Why a recording still looks square

The capture is one eye of the headset, and headsets render each eye at close to
a 1:1 aspect — SteamVR asks for 3344 × 3344 on an Index-class headset, for
example. The two expansion percentages scale that square, so what decides the
recording's shape is the *ratio* between them, not either one alone: 130% × 115%
still comes out at 1.13:1. A widescreen recording needs the horizontal
expansion to outrun the vertical one by the target aspect — 178% × 100% for
16:9.

The Control Center's overscan page does that arithmetic. Its **Recording shape**
buttons set both sliders from a target aspect, and the **Recording frame** card
reports the pixel size and shape the current settings produce, using the per-eye
size the layer learned from the last VR session.

Vertical expansion cannot make a recording wider, so when the goal is a
widescreen frame it only spends GPU time on pixels the frame will not show.
That is why the shape buttons leave it at 100%.

### Black bars beside the picture

Bars appear when a nearly-square mirror is placed on a 16:9 canvas: the scene
item preserves the source's aspect, so the canvas shows through either side.
There are two ways to remove them.

- **Fill the recording canvas** on the OBS source crops the mirror to the
  canvas's shape, centred, so the source fills the frame with no bars and no
  manual crop values. It is free and takes effect immediately, but a square
  mirror on a 16:9 canvas loses roughly 22% off the top and the bottom.
- **178% horizontal overscan** (the *16:9* shape button) makes the mirror
  itself 16:9, so there is nothing to crop and nothing to lose — the recording
  keeps the whole headset view and adds to it. It costs 78% more rendered
  pixels and a VR application restart.

The two combine: at any horizontal expansion of 178% or more, filling the canvas
has nothing left to trim from the headset view. Below that, the trim comes out of
the overscan guard band first and then out of the headset view.

The setting is read once when the VR application starts, so restart the application
after changing it. Caveats:

- Rendering cost grows with the extra pixels (`horizontal × vertical` scale).
- The scale is automatically reduced (or overscan disabled) when the runtime's
  maximum swapchain size leaves no headroom, so the headset never degrades.
- The hidden-area mask is suppressed while overscan is active so applications do not
  stencil away the extra perimeter; this adds a small amount of GPU cost.
- Applications that ignore `xrLocateViews` FOVs or the recommended render resolution
  fall back to normal behaviour automatically (their submissions pass through
  unmodified).
- A projection-baked fullscreen blur, tint, vignette, or fade can cover only the
  headset-native FOV and reveal a hard edge in the added recording perimeter.
  Reduce or disable overscan for titles that render those effects this way.

## Camera smoothing (experimental)

Raw VR footage carries every micro-movement of the head. Camera smoothing runs
a low-pass-filtered virtual camera in the mirror and reprojects each frame from
it, using a small tan-space crop as the pan margin that absorbs the jitter. The
headset is completely unaffected — the smoothing only exists in the OBS image.

Both controls live on the OBS source and apply live, no restarts. The Control
Center can also manage them globally; turn off its override at any time to
return to the values saved on the individual OBS source:

- **Camera smoothing** (0-100): filter strength, from off to very floaty
  (about 40 ms to 800 ms time constant). Start around 30-50.
- **Smoothing crop percentage** (0-25, default 8): how much of the image edge
  the smoother may pan within. More crop allows stronger smoothing before the
  camera has to catch up; the output is upscaled accordingly.

Notes:

- The smoothed camera is clamped so the crop window never leaves the rendered
  image — fast motion degrades to following the head rather than showing black
  edges. Snap turns and teleports are followed instantly by design.
- Pairs well with recording overscan: with overscan enabled the crop margin can
  come out of the overscan perimeter, so the recording keeps the full headset
  field of view.
- Positional smoothing uses a flat reprojection plane at 2 m; very close
  geometry can shimmer slightly during strong positional motion.
- Cost is one textured-quad draw per eye on the mirror device — negligible.

## OpenXR quad-layer UI

The Control Center's **UI layers** page controls whether separately submitted
OpenXR quad layers appear in the OBS mirror. **Show in recording** preserves the
default composite. **Hide from recording** records the projection image without
`XR_COMPOSITION_LAYER_QUAD` content. The layer polls this preference live, and
the headset submission is never modified, so the headset continues to show all
of its original layers.

This filter can separate only UI submitted as a genuine OpenXR composition quad
layer. UI drawn into the projection eye texture, including world-space UI and
post-processed overlays, is already part of the projection image and cannot be
removed independently. After installing a build containing this feature,
restart the OpenXR application once so it loads the updated layer; later
show/hide changes apply live.

Layer updates are installed as hash-versioned binaries. This allows the Control
Center to stage a new build even while the previous DLL is loaded; the running
session keeps its existing code, and the next OpenXR launch follows the updated
manifest automatically.
