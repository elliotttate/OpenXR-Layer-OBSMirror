# OpenXR OBS Mirror installation

OpenXR OBS Mirror captures the application-rendered OpenXR view directly in
OBS Studio while preserving the headset's normal runtime, view, and tracking.

## Recommended: Windows installer

1. Close OBS Studio and any running OpenXR application.
2. Run the downloaded `OpenXR-OBSMirror-...-Setup.exe`.
3. Accept the Windows administrator prompt. It is used only to place the OBS
   source in OBS Studio's shared plugin directory.
4. Leave **Open Control Center** selected and finish setup.
5. Confirm that **Layer**, **OBS source**, and **Runtime** are green in Control
   Center.
6. Open OBS Studio, add an **OpenXR Mirror Capture** source, then start the
   OpenXR application normally through your headset software.

The installer does not select a simulator or replace the system OpenXR
runtime. If Control Center reports an inherited simulator override, use
**Use headset runtime**, then restart any launcher that was already open.

## Portable package

1. Extract the entire ZIP to a permanent folder.
2. Close OBS Studio.
3. Double-click `Launch OpenXR OBS Mirror.cmd`.
4. Open **Installation** and choose **Install / update**.
   If the OBS plugin needs to be installed or replaced, approve the Windows
   administrator prompt. Layer-only updates remain per-user and do not elevate.
5. Restart OBS Studio and add an **OpenXR Mirror Capture** source.

The portable Control Center is self-contained; a separate .NET installation is
not required. Its installation actions run in the app itself and do not require
PowerShell. Command-line scripts remain available for setup and developer
automation. Keep the extracted folder intact because it contains the native
layer, OBS source, scripts, and app runtime.

## Recording controls

- **Overscan** asks compatible applications to render a wider recording image,
  while the headset receives its original center view unchanged. Restart the
  OpenXR application after changing the overscan dimensions.
- **Camera smoothing** filters the recording camera only. Its strength and crop
  margin update live.
- **UI layers** can include or omit separately submitted OpenXR quad layers in
  the recording without changing the headset.
- **Dashboard preview** shows the layer's shared mirror image in the Control
  Center without requiring OBS to be open.

Start with modest overscan such as 115% horizontal and 108% vertical. The GPU
pixel cost scales approximately with the product of those values.

## Updating and uninstalling

Run a newer installer over the existing version. The OpenXR layer uses an
immutable, hash-versioned native DLL, so an update can be staged without
replacing a DLL already loaded by a headset session. Restart the OpenXR
application to load the new layer. Restart OBS Studio when the OBS source is
updated.

Use **Installed apps > OpenXR OBS Mirror > Uninstall** to remove the Control
Center, OBS source, current-user OpenXR layer registration, and installed layer
files. Close OBS Studio and any OpenXR application first so loaded files can be
removed immediately.

To temporarily disable capture integration without uninstalling anything, turn
off the **Layer** switch on the dashboard or **Enable for the current user** on
the Installation page. Turning it back on restores the current-user OpenXR
registration; both switches always show the same live state.

## Troubleshooting

- If the Dashboard preview or OBS source stays black, open **Diagnostics**, pick
  **Preview diagnostics**, and use **Upload & share logs**. The preview log
  records whether a shared surface exists, whether its frame index is advancing,
  the source size and format, GPU adapter identity, and sampled pixel brightness.
  This distinguishes an idle producer from advancing all-black frames and from a
  consumer-side display problem. The local log is
  `%LOCALAPPDATA%\OpenXR-OBSMirror\ControlCenter-preview.log`.
- If no surface or frame is available, start or resume the OpenXR application.
  If frames and visible pixels are reported but only OBS is black, reopen the OBS
  source properties and compare the OBS and producer adapter identities in the
  uploaded report.
- If OBS was open during an update, close it and run **Install / update** again.
- If the wrong runtime is listed, use **Use headset runtime** and relaunch the
  OpenXR application.
- Check the live layer log on the Control Center **Installation** page for the
  runtime name, graphics API, swapchain dimensions, and active options.
- Builds are currently unsigned, so Windows SmartScreen may show an
  unrecognized-publisher warning. Verify the download against
  `SHA256SUMS.txt` from the same GitHub release.
