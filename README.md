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

## Runtime notes

- Start the OpenXR application after installing the layer.
- OBS can load the source before the VR application starts; the source retries
  its IPC connection once the application creates the shared mirror surface.
- Running OBS elevated may improve GPU scheduling priority on some systems, but
  the plugin itself does not require administrator privileges.
- The OpenXR application and OBS must run on the same Windows desktop and use a
  compatible D3D11 adapter for the shared textures to open.

## Recording overscan (experimental)

Recordings normally show exactly the headset's field of view, so head motion
sits at the very edge of the frame. Recording overscan asks the game to render
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

The setting is read once when the VR application starts, so restart the game
after changing it. Caveats:

- Rendering cost grows with the extra pixels (`horizontal × vertical` scale).
- The scale is automatically reduced (or overscan disabled) when the runtime's
  maximum swapchain size leaves no headroom, so the headset never degrades.
- The hidden-area mask is suppressed while overscan is active so games do not
  stencil away the extra perimeter; this adds a small amount of GPU cost.
- Games that ignore `xrLocateViews` FOVs or the recommended render resolution
  fall back to normal behaviour automatically (their submissions pass through
  unmodified).
