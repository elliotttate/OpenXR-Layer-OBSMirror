# Proposed upstream pull request

## Title

Honor quad-layer eye visibility for VDXR and support Visual Studio 2026 builds

## Body

### Summary

- filters OpenXR quad composition layers using `eyeVisibility` for the eye
  selected by the OBS mirror;
- preserves all quad layers for stereo capture;
- supports Visual Studio 2026 / v145 in the native and OBS-plugin build scripts;
- documents the standard-OpenXR capture boundary and the tested VDXR profile.

### Why

Virtual Desktop Streamer 1.34.22 / VDXR 1.0.10 honors left/right
`XrCompositionLayerQuad.eyeVisibility`. The mirror previously composited every
quad into either selected eye, which could make an eye-specific HUD differ from
what the headset user saw.

### Validation

- Virtual Desktop Streamer 1.34.22.0
- VDXR 1.0.10
- OBS Studio 32.2.0 x64
- Elements Divided
- Direct3D 11 application path
- right-eye capture with the GPU shared-texture path

The tested configuration successfully loaded the implicit OpenXR layer, opened
the shared texture in the native OBS source, and produced a live recording.

### Scope

This does not hook private VDXR internals. It remains a standard implicit
OpenXR API layer and therefore cannot capture compositor-only Virtual Desktop
elements added after `xrEndFrame`.
