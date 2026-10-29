# VDXR OBS Mirror

A lightweight, PC-side headset-view mirror for Virtual Desktop's VDXR runtime.
This release was tested with **Virtual Desktop Streamer 1.34.22**, **VDXR
1.0.10**, and **OBS Studio 32.2.0 (64-bit)** on Windows 11.

This is an unofficial community project. It is not affiliated with or endorsed
by Virtual Desktop, OBS Studio, or their developers. See `NOTICE.md` for the
corresponding source, dependency licenses, and detailed attribution.

## Install

1. Close OBS Studio and every running OpenXR/VR application.
2. Extract the entire ZIP; do not run the installer from inside the archive.
3. Right-click `Install.ps1`, select **Run with PowerShell**, and approve the
   Windows administrator prompt. Or run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\Install.ps1
   ```

4. Start OBS and add **VR Mirror Capture (Auto: OpenXR / SteamVR)** as a
   source. This slim package contains only the OpenXR/VDXR backend.
5. In Virtual Desktop Streamer, select **VDXR** (or **Automatic** when it
   resolves to VDXR), then launch the VR application.

The administrator prompt is used only to place the native source under OBS's
shared plugin directory. The OpenXR layer itself is registered for the current
Windows user. Restart a VR application after installing because OpenXR layers
are loaded at process start.

To check installed versions and package integrity without changing the system:

```powershell
powershell -ExecutionPolicy Bypass -File .\Install.ps1 -ValidateOnly
```

This compares both native DLLs against the expected SHA-256 digests recorded in
`BUILD-INFO.json`. Also verify the complete ZIP against the separately
published, version-specific `*-SHA256.txt` file before extracting it.

## Lowest-overhead settings

- Capture one eye (right is the default).
- Leave recording overscan and camera smoothing off.
- Leave quad layers enabled if application HUD/UI must match the headset.
- Run OBS on the same GPU as the VR application.
- Hide or deactivate the OBS source when it is not needed.

The fast path stays entirely on the GPU: one copy into a shared D3D11 texture
ring, which OBS opens directly. There is no CPU pixel readback or desktop mirror
window, and the layer stops publishing frames when it has no consumer.

## What the mirror can capture

The source mirrors the selected application's submitted eye image plus OpenXR
quad layers, including the eye-visibility behavior used by Virtual Desktop
Streamer 1.34.22 / VDXR 1.0.10.
OpenXR does not expose VDXR's private compositor output after `xrEndFrame`, so
runtime-added elements are not available: the Virtual Desktop performance
overlay, guardian/boundary, passthrough, lens distortion, spacewarp artifacts,
and final headset video-encoder output.

This is the closest PC-side application mirror available without modifying
VDXR. It uses the standard OpenXR API-layer contract and does not patch Virtual
Desktop binaries.

## Uninstall

Close OBS and VR applications, then run:

```powershell
powershell -ExecutionPolicy Bypass -File .\Uninstall.ps1
```

## Troubleshooting

- If OBS lists `win-openxr` under **Missing Plugins**, reinstall with version
  1.0.1 or newer and approve the administrator prompt.
- If the source is blank, confirm the OBS log says `win-openxr.dll` loaded,
  ensure the application is actually using VDXR/OpenXR, and ensure OBS and the
  VR application use the same GPU.
- The layer log is `%LOCALAPPDATA%\XR_APILAYER_NOVENDOR_OBSMirror.log`.
- The native binaries are currently unsigned. Windows may show a SmartScreen
  warning; verify the ZIP against its version-specific `*-SHA256.txt` file.

## Provenance and support

Based on [OpenXR-Layer-OBSMirror](https://github.com/elliotttate/OpenXR-Layer-OBSMirror)
by Elliot Tate and the OpenXR layer template by Matthieu Bucchianeri. See
`LICENSE`, `LICENSE-OBS-GPL-2.0.txt`, `NOTICE.md`, `THIRD_PARTY`,
`SOURCE-CHANGES.diff`, and `VDXR-COMPATIBILITY.md`. The corresponding source
for this package is published at
[zenmonkeyvr/VDXR-OBS-Mirror](https://github.com/zenmonkeyvr/VDXR-OBS-Mirror/tree/vdxr-v1.0.3).

When reporting a problem, include the Virtual Desktop Streamer, VDXR, and OBS
versions; GPU model; application name; OBS log; and OpenXR layer log. Do not
post logs without checking them for usernames or private paths first.
