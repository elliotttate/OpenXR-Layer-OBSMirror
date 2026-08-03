# OpenXR OBS Mirror

An OpenXR API layer that mirrors a VR application's rendered view into an OBS
source. The current implementation supports Direct3D 11 OpenXR applications.

The OpenXR layer template was based on
[OpenXR-Layer-Template](https://github.com/mbucchia/OpenXR-Layer-Template).

## Install a release

1. Download and extract the newest compatible release.
2. Close OBS.
3. Copy the release's `OBS_Plugin/data` and `OBS_Plugin/obs-plugins`
   directories into the OBS installation directory (normally
   `C:\Program Files\obs-studio`).
4. Run `Install-Layer.ps1` from the extracted layer directory.
5. Restart OBS and add an **OpenXR Mirror Capture** source.

The install script defaults to a current-user OpenXR registration and does not
need elevation. Use `-Scope AllUsers` from an elevated PowerShell session only
when a machine-wide registration is required. The matching uninstall command
is:

```powershell
pwsh -File .\Uninstall-Layer.ps1 -Scope CurrentUser
```

Do not loosen the machine or user PowerShell execution policy. If Windows has
marked a downloaded archive as blocked, unblock the archive before extracting
it or invoke the individual trusted script with `pwsh -ExecutionPolicy Bypass`.

## Build and install from source

Initialize submodules and restore the native NuGet packages:

```powershell
git submodule update --init --recursive
nuget restore .\OpenXR-Layer-OBSMirror.sln `
  -Source https://api.nuget.org/v3/index.json
```

Build the x64 layer with Visual Studio 2022:

```powershell
msbuild .\OpenXR-Layer-OBSMirror.sln /m `
  /p:Configuration=Release /p:Platform=x64
```

The OBS plugin must be compiled against source matching the installed OBS
version. For example, for OBS 32.2.1:

```powershell
git clone --depth 1 --branch 32.2.1 `
  https://github.com/obsproject/obs-studio.git C:\src\obs-studio-32.2.1
pwsh -File .\scripts\Build-OBSPlugin.ps1 `
  -OBSSourcePath C:\src\obs-studio-32.2.1
```

With OBS closed, install both freshly built components for the current user:

```powershell
pwsh -File .\scripts\Setup-OBS.ps1
```

To stage a first-time install without interrupting an active OBS recording, add
`-AllowRunningOBS`; the new source will become available after OBS restarts.

This places the layer under `%LOCALAPPDATA%\OpenXR-OBSMirror`, registers its
manifest under `HKCU\Software\Khronos\OpenXR\1\ApiLayers\Implicit`, and
installs the plugin under OBS's Windows discovery path at
`%ProgramData%\obs-studio\plugins\win-openxr\bin\64bit`.

## Control Center

The dark WinUI 3 Control Center provides one place to inspect layer, plugin,
runtime, and OBS status; install or update both components; register the layer;
configure recording overscan; control camera smoothing; show or hide OpenXR
quad-layer UI in the recording; and read live logs.
It is headset-first: the dashboard shows the effective runtime, warns when a
simulator override is active, and provides **Use headset runtime** to clear
per-user simulator selectors and return to the machine-wide OpenXR runtime.

Build a self-contained x64 copy:

```powershell
pwsh -File .\scripts\Build-ControlCenter.ps1
```

Run `bin\x64\Release\ControlCenter\OBSMirror.ControlCenter.exe`. Overscan
changes apply when the OpenXR application next starts. Camera-smoothing changes
are picked up live by an active OBS Mirror source. Quad-layer visibility is
picked up live by the updated OpenXR layer after it has been loaded once.

## Runtime notes

- Start the OpenXR application after installing the layer.
- OBS can load the source before the VR application starts; the source retries
  its IPC connection once the application creates the shared mirror surface.
- Running OBS elevated may improve GPU scheduling priority on some systems, but
  the plugin itself does not require administrator privileges.
- The OpenXR application and OBS must run on the same Windows desktop and use a
  compatible D3D11 adapter for the shared textures to open.
- The machine-wide OpenXR runtime selected by the headset software is the normal
  default. The Control Center never selects a simulator merely by opening its
  optional testing tool, and it strips inherited `XR_RUNTIME_JSON` overrides
  from applications that it launches.
- A simulator can leave per-user `XR_RUNTIME_JSON` or `ActiveRuntime` overrides
  behind. Use **Use headset runtime** in the Control Center to clear both 64-bit
  and 32-bit per-user selectors. Restart any launcher that was already running
  while the old environment override was active.
- Some simulator versions refresh OpenXR API-layer registration while testing.
  If the layer status changes after a simulator session, press
  **Register layer now** before the next OpenXR application launch.

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
