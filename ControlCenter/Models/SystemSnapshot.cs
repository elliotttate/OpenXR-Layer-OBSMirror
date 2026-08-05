namespace OBSMirror.ControlCenter.Models;

public sealed record SystemSnapshot(
    bool LayerRegistered,
    bool LayerFilesInstalled,
    bool LayerCurrent,
    bool PluginInstalled,
    bool PluginCurrent,
    bool ObsRunning,
    bool MetaXrRunning,
    string RuntimeName,
    string RuntimePath,
    string RuntimeSource,
    bool RuntimeOverrideActive,
    bool SimulatorRuntimeOverrideActive,
    string SystemRuntimeName,
    string SystemRuntimePath,
    string LayerManifestPath,
    string LayerHash,
    string SourceLayerHash,
    string PluginHash,
    string SourcePluginHash,
    bool OverscanEnabled,
    int HorizontalPercent,
    int VerticalPercent,
    bool CameraSmoothingManaged,
    int CameraSmoothing,
    double SmoothingCrop,
    bool MirrorQuadLayers,
    string LastCaptureSummary,
    string MetaXrExecutable,
    // Empty unless a stale plugin copy inside the OBS installation is
    // shadowing the installed one.
    string ConflictingPluginPath,
    // Empty unless a VR application is driving the headset without the OpenXR
    // loader, which no API layer can ever attach to. NonOpenXrVrPath names the
    // API it went through instead.
    string NonOpenXrVrApp,
    string NonOpenXrVrPath,
    DateTime CapturedAt);
