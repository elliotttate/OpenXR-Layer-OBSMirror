# VDXR compatibility profile

This build was validated with Virtual Desktop Streamer 1.34.22, VDXR 1.0.10,
OBS Studio 32.2.0, and Elements Divided on Windows x64. It is implemented as a standard implicit OpenXR API layer, so it
does not patch or depend on private offsets inside `VirtualDesktop.OpenXR.dll`.
VDXR updates that preserve the OpenXR loader contract therefore do not require
a new runtime hook.

## Capture path

1. The API layer observes the projection and quad composition layers submitted
   by the OpenXR application at `xrEndFrame`.
2. The selected eye is copied or drawn into a ring of D3D11 shared textures on
   the same GPU. No CPU readback is performed.
3. The native OBS source opens the current shared texture directly with libobs.
4. Capture work is disabled while no OBS/preview consumer is connected.

The fast path is a single GPU texture copy per published frame. A textured draw
is used only when stereo composition, smoothing, overscan, or quad-layer
composition requires it.

## Virtual Desktop 1.34.22

Virtual Desktop 1.34.22 added VDXR support for quad-layer eye-visibility flags.
This build filters `XR_EYE_VISIBILITY_LEFT` and `XR_EYE_VISIBILITY_RIGHT` quad
layers according to the eye selected in OBS, matching the runtime's behavior.

## Meaning of "headset view"

The mirror reproduces the application image and OpenXR quad layers submitted to
VDXR for the selected eye. Standard OpenXR API layers cannot read VDXR's private
post-submit compositor output, so runtime-generated elements added after
`xrEndFrame` (for example the Virtual Desktop performance overlay, boundary,
passthrough, spacewarp artifacts, lens distortion, or final video-encoder
output) are not available to this PC-side mirror.

For the lowest overhead, select one eye, leave recording overscan and smoothing
off, and keep **Show quad layers** enabled when headset UI must appear.
