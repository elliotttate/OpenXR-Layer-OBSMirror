namespace OBSMirror.ControlCenter.Services;

internal readonly record struct VrModuleDetection(bool OpenXrPath, string NonOpenXrPath);

internal static class VrModuleDetector
{
    internal static VrModuleDetection Analyze(IEnumerable<string> moduleNames)
    {
        var openXrPath = false;
        var oculusPath = false;
        var openVrApi = false;
        var openVrClient = false;

        foreach (var name in moduleNames)
        {
            if (string.IsNullOrWhiteSpace(name))
                continue;

            if (name.StartsWith("openxr_loader", StringComparison.OrdinalIgnoreCase) ||
                name.StartsWith("XR_APILAYER_", StringComparison.OrdinalIgnoreCase) ||
                name.Contains("virtualdesktop-openxr", StringComparison.OrdinalIgnoreCase) ||
                name.Contains("openxr-oculus-compatibility", StringComparison.OrdinalIgnoreCase) ||
                name.StartsWith("OpenCompositeInput", StringComparison.OrdinalIgnoreCase) ||
                name.StartsWith("OpenComposite", StringComparison.OrdinalIgnoreCase))
            {
                // OCU and some other OpenComposite builds embed the OpenXR
                // loader, so no openxr_loader.dll appears in the module list.
                openXrPath = true;
            }
            // A plain LibOVR module still identifies the direct Oculus API.
            else if (name.Contains("libovr", StringComparison.OrdinalIgnoreCase))
            {
                oculusPath = true;
            }
            else if (name.StartsWith("openvr_api", StringComparison.OrdinalIgnoreCase))
            {
                openVrApi = true;
            }
            else if (name.StartsWith("vrclient", StringComparison.OrdinalIgnoreCase))
            {
                openVrClient = true;
            }
        }

        if (openXrPath)
            return new VrModuleDetection(true, string.Empty);
        if (oculusPath)
            return new VrModuleDetection(false, "Oculus/LibOVR");

        // openvr_api.dll alone proves nothing: Unreal Engine titles ship it and
        // load it while running perfectly flat. Only vrclient, which SteamVR
        // injects once a session actually starts, makes it a VR signal.
        return openVrApi && openVrClient
            ? new VrModuleDetection(false, "OpenVR/SteamVR")
            : new VrModuleDetection(false, string.Empty);
    }
}
