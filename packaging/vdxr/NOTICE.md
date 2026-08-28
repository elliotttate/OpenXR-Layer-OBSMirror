# Licensing, source, and product notice

VDXR OBS Mirror is an unofficial community project. It is not affiliated with,
sponsored by, or endorsed by Virtual Desktop, OBS Studio, Guy Godin, Wizards
of the Coast, or the developers of Elements Divided. Product names are used
only to identify tested compatibility.

## Corresponding source

The complete source used for this release, including build and packaging
scripts, is available without charge at:

https://github.com/zenmonkeyvr/VDXR-OBS-Mirror/tree/vdxr-v1.0.3

The project is forked from:

https://github.com/elliotttate/OpenXR-Layer-OBSMirror

The OBS plugin was compiled against the official OBS Studio 32.2.0 source at
commit `7546be7266dde276d82d4681fe1ab4fd8e32cf2b`:

https://github.com/obsproject/obs-studio/tree/32.2.0

Reproduction instructions are in the repository README and
`scripts/Build-OBSPlugin.ps1`. The exact changes relative to upstream are also
included as `SOURCE-CHANGES.diff` in this archive.

## Licenses

- The OpenXR OBS Mirror source is provided under the included MIT `LICENSE`.
- The native OBS plugin is distributed for use with GPLv2-licensed OBS Studio.
  The complete GNU GPL version 2 text is included as
  `LICENSE-OBS-GPL-2.0.txt`, and the corresponding plugin source is available
  at the release source link above.
- Notices for the Khronos OpenXR SDK and other dependencies are retained in
  `THIRD_PARTY`.

No Virtual Desktop binaries, OBS Studio binaries, game files, or proprietary
runtime code are included in this package.
