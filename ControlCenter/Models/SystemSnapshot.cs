namespace OBSMirror.ControlCenter.Models;

public sealed record SystemSnapshot(
    bool LayerRegistered,
    bool LayerFilesInstalled,
    bool PluginInstalled,
    bool PluginCurrent,
    bool ObsRunning,
    bool MetaXrRunning,
    string RuntimeName,
    string RuntimePath,
    string LayerManifestPath,
    string LayerHash,
    string PluginHash,
    string SourcePluginHash,
    bool OverscanEnabled,
    int HorizontalPercent,
    int VerticalPercent,
    bool CameraSmoothingManaged,
    int CameraSmoothing,
    double SmoothingCrop,
    string LastCaptureSummary,
    string MetaXrExecutable,
    DateTime CapturedAt);
