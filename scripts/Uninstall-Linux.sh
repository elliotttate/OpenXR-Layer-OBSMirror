#!/usr/bin/env bash
set -euo pipefail
data_dir="${XDG_DATA_HOME:-$HOME/.local/share}"
config_dir="${XDG_CONFIG_HOME:-$HOME/.config}"
rm -f -- "$data_dir/openxr/1/api_layers/implicit.d/XR_APILAYER_NOVENDOR_OBSMirror.json"
rm -f -- "$data_dir/openxr-obsmirror/libXR_APILAYER_NOVENDOR_OBSMirror.so"
rm -f -- "$config_dir/obs-studio/plugins/openxr-vulkan-mirror/bin/64bit/openxr-vulkan-mirror.so"
echo 'Removed the Linux layer registration and binaries. Restart OBS and OpenXR applications.'
