#!/usr/bin/env bash
set -euo pipefail
payload_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
data_dir="${XDG_DATA_HOME:-$HOME/.local/share}"
config_dir="${XDG_CONFIG_HOME:-$HOME/.config}"
install_dir="$data_dir/openxr-obsmirror"
manifest_dir="$data_dir/openxr/1/api_layers/implicit.d"
plugin_dir="$config_dir/obs-studio/plugins/openxr-vulkan-mirror/bin/64bit"
test -f "$payload_dir/lib/libXR_APILAYER_NOVENDOR_OBSMirror.so"
test -f "$payload_dir/lib/openxr-vulkan-mirror.so"
mkdir -p "$install_dir" "$manifest_dir" "$plugin_dir"
install -m 755 "$payload_dir/lib/libXR_APILAYER_NOVENDOR_OBSMirror.so" "$install_dir/"
install -m 755 "$payload_dir/lib/openxr-vulkan-mirror.so" "$plugin_dir/"
python3 - "$manifest_dir/XR_APILAYER_NOVENDOR_OBSMirror.json" "$install_dir/libXR_APILAYER_NOVENDOR_OBSMirror.so" <<'PY'
import json, sys
with open(sys.argv[1], 'w') as f:
    json.dump({'file_format_version': '1.0.0', 'api_layer': {
        'name': 'XR_APILAYER_NOVENDOR_OBSMirror', 'library_path': sys.argv[2],
        'api_version': '1.0', 'implementation_version': '4',
        'description': 'OpenXR Vulkan mirror capture for OBS Studio (Linux)',
        'disable_environment': 'DISABLE_XR_APILAYER_NOVENDOR_OBSMirror'}}, f, indent=2)
PY
echo 'Installed for the current user. Restart OBS and the OpenXR application.'
echo 'In OBS, add the OpenXR Vulkan Mirror source.'
