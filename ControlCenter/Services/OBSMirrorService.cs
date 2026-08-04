using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Win32;
using OBSMirror.ControlCenter.Models;

namespace OBSMirror.ControlCenter.Services;

public sealed class OBSMirrorService
{
    private const string ConfigKey = @"Software\OpenXR-OBSMirror";
    private const string LayerRegistryKey = @"Software\Khronos\OpenXR\1\ApiLayers\Implicit";
    private const string OpenXrRegistryKey = @"Software\Khronos\OpenXR\1";
    private const string LegacyActiveRuntimeKey = @"Software\Khronos\OpenXR\1\ActiveRuntime";
    private const string ActiveRuntimeValue = "ActiveRuntime";
    private const string LayerManifestName = "XR_APILAYER_NOVENDOR_OBSMirror.json";
    private const uint WmSettingChange = 0x001A;
    private static readonly IntPtr HwndBroadcast = new(0xffff);

    public string RepoRoot { get; } = FindRepoRoot();
    public string InstallDirectory { get; } = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OpenXR-OBSMirror");
    public string PluginPath { get; } = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
        "obs-studio", "plugins", "win-openxr", "bin", "64bit", "win-openxr.dll");
    public string LayerLogPath { get; } = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "XR_APILAYER_NOVENDOR_OBSMirror.log");

    public string InstalledManifestPath => Path.Combine(InstallDirectory, LayerManifestName);
    public string InstalledLayerPath => ResolveManifestLibraryPath(InstalledManifestPath);
    public string ReleaseLayerPath => Path.Combine(RepoRoot, "bin", "x64", "Release", "XR_APILAYER_NOVENDOR_OBSMirror.dll");
    public string ReleaseLayerManifestPath => Path.Combine(RepoRoot, "bin", "x64", "Release", LayerManifestName);
    public string ReleasePluginPath => Path.Combine(RepoRoot, "bin", "x64", "Release", "OBS_Plugin", "win-openxr.dll");
    public string SetupScriptPath => Path.Combine(RepoRoot, "scripts", "Setup-OBS.ps1");
    public string InstallScriptPath => Path.Combine(RepoRoot, "scripts", "Install-Layer.ps1");
    public string UninstallScriptPath => Path.Combine(RepoRoot, "scripts", "Uninstall-Layer.ps1");

    public SystemSnapshot GetSnapshot()
    {
        var (enabled, horizontal, vertical) = ReadOverscan();
        var (smoothingManaged, cameraSmoothing, smoothingCrop) = ReadCameraSmoothing();
        var mirrorQuadLayers = ReadMirrorQuadLayers();
        var runtime = ResolveRuntimeSelection();
        var systemRuntimePath = ResolveSystemRuntimePath();
        var runtimeName = FriendlyRuntimeName(runtime.Path);
        var systemRuntimeName = FriendlyRuntimeName(systemRuntimePath);
        var registeredManifest = FindRegisteredLayerManifest();
        var sourceLayerHash = HashFile(ReleaseLayerPath);
        var layerHash = HashFile(InstalledLayerPath);
        var sourcePluginHash = HashFile(ReleasePluginPath);
        var pluginHash = HashFile(PluginPath);
        var metaExe = FindMetaXrExecutable(runtime.Path);

        return new SystemSnapshot(
            LayerRegistered: !string.IsNullOrWhiteSpace(registeredManifest),
            LayerFilesInstalled: File.Exists(InstalledManifestPath) && File.Exists(InstalledLayerPath),
            LayerCurrent: !string.IsNullOrEmpty(sourceLayerHash) &&
                          string.Equals(sourceLayerHash, layerHash, StringComparison.OrdinalIgnoreCase),
            PluginInstalled: File.Exists(PluginPath),
            PluginCurrent: !string.IsNullOrEmpty(sourcePluginHash) &&
                           string.Equals(sourcePluginHash, pluginHash, StringComparison.OrdinalIgnoreCase),
            ObsRunning: IsProcessRunning("obs64"),
            MetaXrRunning: IsProcessRunning("MetaXRSimulator"),
            RuntimeName: runtimeName,
            RuntimePath: runtime.Path,
            RuntimeSource: runtime.Source,
            RuntimeOverrideActive: runtime.IsOverride,
            SimulatorRuntimeOverrideActive: runtime.IsOverride && IsSimulatorRuntime(runtime.Path),
            SystemRuntimeName: systemRuntimeName,
            SystemRuntimePath: systemRuntimePath,
            LayerManifestPath: registeredManifest ?? InstalledManifestPath,
            LayerHash: layerHash,
            SourceLayerHash: sourceLayerHash,
            PluginHash: pluginHash,
            SourcePluginHash: sourcePluginHash,
            OverscanEnabled: enabled,
            HorizontalPercent: horizontal,
            VerticalPercent: vertical,
            CameraSmoothingManaged: smoothingManaged,
            CameraSmoothing: cameraSmoothing,
            SmoothingCrop: smoothingCrop,
            MirrorQuadLayers: mirrorQuadLayers,
            LastCaptureSummary: FindLastCaptureSummary(),
            MetaXrExecutable: metaExe,
            ConflictingPluginPath: FindConflictingPluginPath() ?? string.Empty,
            CapturedAt: DateTime.Now);
    }

    public void ApplyOverscan(bool enabled, int horizontalPercent, int verticalPercent)
    {
        horizontalPercent = Math.Clamp(horizontalPercent, 100, 200);
        verticalPercent = Math.Clamp(verticalPercent, 100, 200);

        using var key = Registry.CurrentUser.CreateSubKey(ConfigKey, writable: true)
            ?? throw new InvalidOperationException("Could not open the per-user OBSMirror settings key.");
        key.SetValue("RecordingOverscan", enabled ? 1 : 0, RegistryValueKind.DWord);
        key.SetValue("OverscanHorizontalPercent", horizontalPercent, RegistryValueKind.DWord);
        key.SetValue("OverscanVerticalPercent", verticalPercent, RegistryValueKind.DWord);
    }

    public void ApplyCameraSmoothing(bool managed, int smoothing, double crop)
    {
        smoothing = Math.Clamp(smoothing, 0, 100);
        crop = Math.Clamp(crop, 0.0, 25.0);

        using var key = Registry.CurrentUser.CreateSubKey(ConfigKey, writable: true)
            ?? throw new InvalidOperationException("Could not open the per-user OBSMirror settings key.");
        key.SetValue("CameraSmoothingManaged", managed ? 1 : 0, RegistryValueKind.DWord);
        key.SetValue("CameraSmoothing", smoothing, RegistryValueKind.DWord);
        key.SetValue("SmoothingCropTenths", (int)Math.Round(crop * 10.0), RegistryValueKind.DWord);
    }

    public void ApplyMirrorQuadLayers(bool visible)
    {
        using var key = Registry.CurrentUser.CreateSubKey(ConfigKey, writable: true)
            ?? throw new InvalidOperationException("Could not open the per-user OBSMirror settings key.");
        key.SetValue("MirrorQuadLayers", visible ? 1 : 0, RegistryValueKind.DWord);
    }

    public Task<string> SetupAsync(bool allowRunningObs) =>
        Task.Run(() => Setup(allowRunningObs));

    public Task<string> InstallLayerOnlyAsync() =>
        Task.Run(InstallLayerOnly);

    public Task<string> InstallPluginOnlyAsync() =>
        Task.Run(InstallPluginOnly);

    public Task<string> RegisterLayerAsync() =>
        Task.Run(() => RegisterLayer(InstalledManifestPath));

    public Task<string> UnregisterLayerAsync() =>
        Task.Run(UnregisterLayer);

    private string Setup(bool allowRunningObs)
    {
        var obsRunning = IsProcessRunning("obs64");
        if (obsRunning && !allowRunningObs)
            throw new InvalidOperationException("OBS is running. Close it before updating the OBS plugin.");

        var layerResult = InstallLayerOnly();
        var pluginResult = InstallPluginOnly();
        return $"{layerResult} {pluginResult}";
    }

    private string InstallLayerOnly()
    {
        EnsureFile(ReleaseLayerPath, "release layer binary");
        EnsureFile(ReleaseLayerManifestPath, "release layer manifest");

        Directory.CreateDirectory(InstallDirectory);
        var layerHash = ComputeFileHash(ReleaseLayerPath);
        var versionedLayerName = $"XR_APILAYER_NOVENDOR_OBSMirror.{layerHash[..12].ToLowerInvariant()}.dll";
        var versionedLayerPath = Path.Combine(InstallDirectory, versionedLayerName);
        CopyFileUnlessCurrent(ReleaseLayerPath, versionedLayerPath, layerHash);

        WriteInstalledManifest(versionedLayerName);
        CopyFileUnlessCurrent(InstallScriptPath, Path.Combine(InstallDirectory, Path.GetFileName(InstallScriptPath)));
        CopyFileUnlessCurrent(UninstallScriptPath, Path.Combine(InstallDirectory, Path.GetFileName(UninstallScriptPath)));

        RegisterLayer(InstalledManifestPath);
        return $"Installed and enabled layer {layerHash[..12].ToLowerInvariant()}.";
    }

    private string InstallPluginOnly()
    {
        EnsureFile(ReleasePluginPath, "release OBS plugin");

        var pluginSourceHash = ComputeFileHash(ReleasePluginPath);
        var pluginCurrent = File.Exists(PluginPath) &&
                            string.Equals(ComputeFileHash(PluginPath), pluginSourceHash, StringComparison.OrdinalIgnoreCase);
        if (pluginCurrent)
            return "OBS plugin already current.";

        if (IsProcessRunning("obs64"))
        {
            throw new InvalidOperationException(
                "OBS is running and the plugin binary has changed. Stop recording and close OBS before updating the plugin.");
        }

        var pluginBinDirectory = Path.GetDirectoryName(PluginPath)
                                 ?? throw new InvalidOperationException("The OBS plugin directory could not be resolved.");
        var pluginRoot = Directory.GetParent(pluginBinDirectory)?.Parent?.FullName
                         ?? throw new InvalidOperationException("The OBS plugin root directory could not be resolved.");
        Directory.CreateDirectory(pluginBinDirectory);
        File.Copy(ReleasePluginPath, PluginPath, overwrite: true);

        var pluginDataSource = Path.Combine(RepoRoot, "OBSPlugin", "win-openxr", "data");
        if (!Directory.Exists(pluginDataSource))
            throw new DirectoryNotFoundException($"The OBS plugin data directory was not found: {pluginDataSource}");
        CopyDirectoryContents(pluginDataSource, Path.Combine(pluginRoot, "data"));
        return "Updated the OBS plugin.";
    }

    private string RegisterLayer(string manifestPath)
    {
        var manifestFullPath = Path.GetFullPath(manifestPath);
        EnsureFile(manifestFullPath, "installed layer manifest");
        var libraryPath = ResolveManifestLibraryPath(manifestFullPath);
        EnsureFile(libraryPath, "layer binary referenced by the installed manifest");

        using var key = Registry.CurrentUser.CreateSubKey(LayerRegistryKey, writable: true)
                        ?? throw new InvalidOperationException("Could not open the current-user OpenXR implicit-layer registry key.");
        foreach (var valueName in key.GetValueNames())
        {
            if (Path.GetFileName(valueName).Equals(LayerManifestName, StringComparison.OrdinalIgnoreCase) &&
                !valueName.Equals(manifestFullPath, StringComparison.OrdinalIgnoreCase))
            {
                key.DeleteValue(valueName, throwOnMissingValue: false);
            }
        }
        key.SetValue(manifestFullPath, 0, RegistryValueKind.DWord);
        return $"Registered OpenXR OBS Mirror for the current user: {manifestFullPath}";
    }

    private string UnregisterLayer()
    {
        using var key = Registry.CurrentUser.OpenSubKey(LayerRegistryKey, writable: true);
        if (key is null)
            return "No current-user OpenXR implicit-layer registry key exists.";

        var removed = 0;
        foreach (var valueName in key.GetValueNames())
        {
            if (!Path.GetFileName(valueName).Equals(LayerManifestName, StringComparison.OrdinalIgnoreCase))
                continue;
            key.DeleteValue(valueName, throwOnMissingValue: false);
            removed++;
        }
        return $"Removed {removed} OpenXR OBS Mirror registration(s) for the current user.";
    }

    private void WriteInstalledManifest(string versionedLayerName)
    {
        var root = JsonNode.Parse(File.ReadAllText(ReleaseLayerManifestPath)) as JsonObject
                   ?? throw new InvalidDataException("The release layer manifest does not contain a JSON object.");
        var apiLayer = root["api_layer"] as JsonObject
                       ?? throw new InvalidDataException("The release layer manifest does not contain api_layer.");
        apiLayer["library_path"] = $".\\{versionedLayerName}";

        var temporaryPath = $"{InstalledManifestPath}.{Guid.NewGuid():N}.tmp";
        try
        {
            File.WriteAllText(
                temporaryPath,
                root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }),
                new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
            File.Move(temporaryPath, InstalledManifestPath, overwrite: true);
        }
        finally
        {
            File.Delete(temporaryPath);
        }
    }

    private static void CopyDirectoryContents(string sourceDirectory, string destinationDirectory)
    {
        Directory.CreateDirectory(destinationDirectory);
        foreach (var file in Directory.EnumerateFiles(sourceDirectory))
            File.Copy(file, Path.Combine(destinationDirectory, Path.GetFileName(file)), overwrite: true);
        foreach (var directory in Directory.EnumerateDirectories(sourceDirectory))
            CopyDirectoryContents(directory, Path.Combine(destinationDirectory, Path.GetFileName(directory)));
    }

    private static void CopyFileUnlessCurrent(string sourcePath, string destinationPath, string? sourceHash = null)
    {
        sourceHash ??= ComputeFileHash(sourcePath);
        if (File.Exists(destinationPath) &&
            string.Equals(ComputeFileHash(destinationPath), sourceHash, StringComparison.OrdinalIgnoreCase))
        {
            return;
        }
        Directory.CreateDirectory(Path.GetDirectoryName(destinationPath)
                                  ?? throw new InvalidOperationException($"The destination directory could not be resolved: {destinationPath}"));
        File.Copy(sourcePath, destinationPath, overwrite: true);
    }

    private static string ComputeFileHash(string path)
    {
        EnsureFile(path, "file to hash");
        using var stream = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        return Convert.ToHexString(SHA256.HashData(stream));
    }

    /// <summary>
    /// Thrown when OBS cannot be located, so the UI can offer to browse for it
    /// instead of only reporting a failure.
    /// </summary>
    public sealed class ObsNotFoundException(string message) : Exception(message);

    /// <summary>Best-effort path of obs64.exe, or null when it cannot be found.</summary>
    public string? FindObsExecutable() => ResolveObsExecutablePath();

    /// <summary>
    /// Version of the build whose layer and OBS source are installed. Compared
    /// against this app's own version so an older Control Center never
    /// reinstalls its bundled payload over newer components - the automatic
    /// update is only ever allowed to move components forward.
    /// </summary>
    public string InstalledComponentsVersion
    {
        get
        {
            using var key = Registry.CurrentUser.OpenSubKey(ConfigKey);
            return key?.GetValue("ComponentsVersion") as string ?? string.Empty;
        }
    }

    public void RecordInstalledComponentsVersion(string version)
    {
        if (string.IsNullOrWhiteSpace(version))
            return;
        using var key = Registry.CurrentUser.CreateSubKey(ConfigKey, writable: true);
        key?.SetValue("ComponentsVersion", version, RegistryValueKind.String);
    }

    /// <summary>
    /// A win-openxr.dll inside the OBS installation directory, which is where
    /// the plugin used to be installed by hand. OBS loads that folder before
    /// the shared plugin folder, so the stale copy wins the source
    /// registration ("Source 'openxrmirror_capture' already exists!") and
    /// every capture source ends up driven by the old build - which then fails
    /// to open the current layer's shared textures and shows nothing.
    /// Returns null when there is no such file.
    /// </summary>
    public string? FindConflictingPluginPath()
    {
        var obsExecutable = ResolveObsExecutablePath();
        if (string.IsNullOrWhiteSpace(obsExecutable))
            return null;

        // obs64.exe lives in <root>\bin\64bit.
        var obsRoot = Directory.GetParent(obsExecutable)?.Parent?.Parent?.FullName;
        if (string.IsNullOrWhiteSpace(obsRoot))
            return null;

        var candidate = Path.Combine(obsRoot, "obs-plugins", "64bit", "win-openxr.dll");
        return File.Exists(candidate) ? candidate : null;
    }

    /// <summary>
    /// Removes the shadowing copy described by <see cref="FindConflictingPluginPath"/>
    /// along with its data folder. Requires administrator rights when OBS is
    /// installed under Program Files.
    /// </summary>
    public string RemoveConflictingPlugin()
    {
        var conflicting = FindConflictingPluginPath();
        if (conflicting is null)
            return "No conflicting OBS plugin copy was found.";
        if (IsProcessRunning("obs64"))
            throw new InvalidOperationException("Close OBS before removing the old plugin copy; OBS is holding the file.");

        File.Delete(conflicting);

        // The matching data folder would otherwise leave stale locale files.
        var obsRoot = Directory.GetParent(conflicting)?.Parent?.Parent?.FullName;
        if (!string.IsNullOrWhiteSpace(obsRoot))
        {
            var dataDirectory = Path.Combine(obsRoot, "data", "obs-plugins", "win-openxr");
            if (Directory.Exists(dataDirectory))
                Directory.Delete(dataDirectory, recursive: true);
        }
        return $"Removed the conflicting plugin copy at {conflicting}.";
    }

    /// <summary>
    /// Remembers a user-picked obs64.exe so every later launch uses it.
    /// </summary>
    public void SetObsExecutable(string path)
    {
        EnsureFile(path, "OBS executable");
        if (!Path.GetFileName(path).Equals("obs64.exe", StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Select obs64.exe (the 64-bit OBS Studio executable).");

        using var key = Registry.CurrentUser.CreateSubKey(ConfigKey, writable: true)
            ?? throw new InvalidOperationException("Could not open the per-user OBSMirror settings key.");
        key.SetValue("ObsExecutable", path, RegistryValueKind.String);
    }

    public void LaunchObs()
    {
        var obsPath = ResolveObsExecutablePath()
            ?? throw new ObsNotFoundException(
                "OBS Studio (obs64.exe) could not be found automatically. This is normal for a custom or " +
                "portable install - browse to obs64.exe once and it will be remembered.");
        var startInfo = new ProcessStartInfo(obsPath)
        {
            WorkingDirectory = Path.GetDirectoryName(obsPath)!,
            UseShellExecute = false
        };
        // A stale simulator override inherited by Control Center must never be
        // forwarded to applications that it launches.
        startInfo.Environment.Remove("XR_RUNTIME_JSON");
        Process.Start(startInfo);
    }

    public void LaunchMetaXr(string path)
    {
        EnsureFile(path, "Meta XR Simulator executable");
        var startInfo = new ProcessStartInfo(path)
        {
            WorkingDirectory = Path.GetDirectoryName(path)!,
            UseShellExecute = false
        };
        // Opening the testing tool must not perpetuate an inherited runtime
        // override. Runtime selection remains an explicit action in that tool.
        startInfo.Environment.Remove("XR_RUNTIME_JSON");
        Process.Start(startInfo);
    }

    public string RestoreSystemRuntime()
    {
        Environment.SetEnvironmentVariable("XR_RUNTIME_JSON", null, EnvironmentVariableTarget.Process);
        Environment.SetEnvironmentVariable("XR_RUNTIME_JSON", null, EnvironmentVariableTarget.User);

        foreach (var view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
        {
            DeleteRegistryValue(RegistryHive.CurrentUser, view, OpenXrRegistryKey, ActiveRuntimeValue);
            // Older simulator builds also used a default value in this subkey.
            DeleteRegistryValue(RegistryHive.CurrentUser, view, LegacyActiveRuntimeKey, string.Empty);
        }

        BroadcastEnvironmentChange();
        return ResolveSystemRuntimePath();
    }

    public void OpenPath(string path)
    {
        var target = File.Exists(path) ? $"/select,\"{path}\"" : $"\"{path}\"";
        Process.Start(new ProcessStartInfo("explorer.exe", target) { UseShellExecute = true });
    }

    public string GetLayerLog() => ReadTail(LayerLogPath, 260);

    public string GetObsLog()
    {
        var logDirectory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
            "obs-studio", "logs");
        var latest = Directory.Exists(logDirectory)
            ? new DirectoryInfo(logDirectory).EnumerateFiles("*.txt")
                .OrderByDescending(file => file.LastWriteTimeUtc)
                .FirstOrDefault()?.FullName
            : null;
        return latest is null ? "OBS has not created a log yet." : ReadTail(latest, 260);
    }

    public string GetLatestObsLogPath()
    {
        var logDirectory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
            "obs-studio", "logs");
        return Directory.Exists(logDirectory)
            ? new DirectoryInfo(logDirectory).EnumerateFiles("*.txt")
                .OrderByDescending(file => file.LastWriteTimeUtc)
                .FirstOrDefault()?.FullName ?? logDirectory
            : logDirectory;
    }

    private (bool Enabled, int Horizontal, int Vertical) ReadOverscan()
    {
        using var key = Registry.CurrentUser.OpenSubKey(ConfigKey);
        var enabled = Convert.ToInt32(key?.GetValue("RecordingOverscan", 0) ?? 0) != 0;
        var horizontal = Math.Clamp(Convert.ToInt32(key?.GetValue("OverscanHorizontalPercent", 115) ?? 115), 100, 200);
        var vertical = Math.Clamp(Convert.ToInt32(key?.GetValue("OverscanVerticalPercent", 108) ?? 108), 100, 200);
        return (enabled, horizontal, vertical);
    }

    private (bool Managed, int Smoothing, double Crop) ReadCameraSmoothing()
    {
        using var key = Registry.CurrentUser.OpenSubKey(ConfigKey);
        var managed = Convert.ToInt32(key?.GetValue("CameraSmoothingManaged", 0) ?? 0) != 0;
        var smoothing = Math.Clamp(Convert.ToInt32(key?.GetValue("CameraSmoothing", 35) ?? 35), 0, 100);
        var cropTenths = Math.Clamp(Convert.ToInt32(key?.GetValue("SmoothingCropTenths", 80) ?? 80), 0, 250);
        return (managed, smoothing, cropTenths / 10.0);
    }

    private bool ReadMirrorQuadLayers()
    {
        using var key = Registry.CurrentUser.OpenSubKey(ConfigKey);
        return Convert.ToInt32(key?.GetValue("MirrorQuadLayers", 1) ?? 1) != 0;
    }

    private string? FindRegisteredLayerManifest()
    {
        using var key = Registry.CurrentUser.OpenSubKey(LayerRegistryKey);
        return key?.GetValueNames().FirstOrDefault(name =>
            Path.GetFileName(name).Equals(LayerManifestName, StringComparison.OrdinalIgnoreCase) &&
            Convert.ToInt32(key.GetValue(name, 1) ?? 1) == 0);
    }

    private static RuntimeSelection ResolveRuntimeSelection()
    {
        var systemRuntimePath = ResolveSystemRuntimePath();
        var candidates = new[]
        {
            new RuntimeSelection(Environment.GetEnvironmentVariable("XR_RUNTIME_JSON") ?? string.Empty,
                "Process environment override", true),
            new RuntimeSelection(Environment.GetEnvironmentVariable("XR_RUNTIME_JSON", EnvironmentVariableTarget.User) ?? string.Empty,
                "User environment override", true),
            new RuntimeSelection(ReadActiveRuntime(RegistryHive.CurrentUser, RegistryView.Registry64) ?? string.Empty,
                "Current-user runtime override", true),
            new RuntimeSelection(ReadActiveRuntime(RegistryHive.CurrentUser, RegistryView.Registry32) ?? string.Empty,
                "Current-user 32-bit runtime override", true),
            new RuntimeSelection(systemRuntimePath, "System headset runtime", false)
        };
        var selected = candidates.FirstOrDefault(candidate => !string.IsNullOrWhiteSpace(candidate.Path))
                       ?? new RuntimeSelection("Not configured", "No runtime configured", false);
        return selected.IsOverride && RuntimePathsEqual(selected.Path, systemRuntimePath)
            ? selected with { Source = $"{selected.Source} (matches system)", IsOverride = false }
            : selected;
    }

    private static string ResolveSystemRuntimePath()
    {
        return ReadActiveRuntime(RegistryHive.LocalMachine, RegistryView.Registry64)
               ?? ReadActiveRuntime(RegistryHive.LocalMachine, RegistryView.Registry32)
               ?? "Not configured";
    }

    private static string? ReadActiveRuntime(RegistryHive hive, RegistryView view)
    {
        return ReadRegistryString(hive, view, OpenXrRegistryKey, ActiveRuntimeValue)
               ?? ReadRegistryString(hive, view, LegacyActiveRuntimeKey, null);
    }

    private static string? ReadRegistryString(RegistryHive hive, RegistryView view, string path, string? valueName)
    {
        try
        {
            using var root = RegistryKey.OpenBaseKey(hive, view);
            using var key = root.OpenSubKey(path);
            return key?.GetValue(valueName) as string;
        }
        catch
        {
            return null;
        }
    }

    private string? ResolveObsExecutablePath()
    {
        using (var config = Registry.CurrentUser.OpenSubKey(ConfigKey))
        {
            if (config?.GetValue("ObsExecutable") is string configured)
            {
                var trimmed = configured.Trim().Trim('"');
                if (File.Exists(trimmed))
                    return trimmed;
            }
        }

        // A running OBS is the most reliable source of truth, and it covers
        // portable copies that register nothing at all.
        try
        {
            foreach (var process in Process.GetProcessesByName("obs64"))
            {
                using (process)
                {
                    var runningPath = process.MainModule?.FileName;
                    if (!string.IsNullOrWhiteSpace(runningPath) && File.Exists(runningPath))
                        return runningPath;
                }
            }
        }
        catch
        {
            // Reading another process's module list can fail on permissions.
        }

        foreach (var hive in new[] { RegistryHive.LocalMachine, RegistryHive.CurrentUser })
        {
            foreach (var view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
            {
                // The OBS installer records its install directory in this key's
                // default value, wherever the user chose to install it.
                var installDirectory = ReadRegistryString(hive, view, @"SOFTWARE\OBS Studio", null);
                if (ObsExecutableFromInstallDirectory(installDirectory) is { } fromInstallKey)
                    return fromInstallKey;

                foreach (var uninstallKey in new[]
                         {
                             @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio",
                             @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{5E4EFC30-6A6B-4F53-B21A-9D1E01F76B18}_is1",
                         })
                {
                    foreach (var valueName in new[] { "InstallLocation", "UninstallString", "DisplayIcon" })
                    {
                        var value = ReadRegistryString(hive, view, uninstallKey, valueName);
                        if (string.IsNullOrWhiteSpace(value))
                            continue;
                        // DisplayIcon can be "<path>,0" and points at an exe.
                        var cleaned = value.Trim().Trim('"').Split(',')[0].Trim();
                        var directory = File.Exists(cleaned) ? Path.GetDirectoryName(cleaned) : cleaned;
                        if (ObsExecutableFromInstallDirectory(directory) is { } fromUninstall)
                            return fromUninstall;
                        // InstallLocation may already be the bin\64bit folder.
                        if (!string.IsNullOrWhiteSpace(directory))
                        {
                            var direct = Path.Combine(directory, "obs64.exe");
                            if (File.Exists(direct))
                                return direct;
                        }
                    }
                }

                // Windows records launchable executables here as well.
                var appPath = ReadRegistryString(
                    hive, view, @"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\obs64.exe", null);
                if (!string.IsNullOrWhiteSpace(appPath))
                {
                    var cleaned = appPath.Trim().Trim('"');
                    if (File.Exists(cleaned))
                        return cleaned;
                }
            }
        }

        // Last resort: the default folder name on every fixed drive, which
        // catches "D:\Program Files\obs-studio" style relocations.
        foreach (var root in EnumerateFixedDriveRoots())
        {
            foreach (var relative in new[]
                     {
                         @"Program Files\obs-studio",
                         @"Program Files (x86)\obs-studio",
                         "obs-studio",
                         @"Games\obs-studio",
                     })
            {
                if (ObsExecutableFromInstallDirectory(Path.Combine(root, relative)) is { } found)
                    return found;
            }
        }

        return null;
    }

    private static IEnumerable<string> EnumerateFixedDriveRoots()
    {
        DriveInfo[] drives;
        try
        {
            drives = DriveInfo.GetDrives();
        }
        catch
        {
            yield break;
        }
        foreach (var drive in drives)
        {
            var isReady = false;
            try
            {
                isReady = drive.DriveType == DriveType.Fixed && drive.IsReady;
            }
            catch
            {
                // Unreadable drives are simply skipped.
            }
            if (isReady)
                yield return drive.RootDirectory.FullName;
        }
    }

    private static string? ObsExecutableFromInstallDirectory(string? installDirectory)
    {
        if (string.IsNullOrWhiteSpace(installDirectory))
            return null;
        var candidate = Path.Combine(
            installDirectory.Trim().Trim('"'), "bin", "64bit", "obs64.exe");
        return File.Exists(candidate) ? candidate : null;
    }

    private static string FriendlyRuntimeName(string path)
    {
        if (path.Contains("meta", StringComparison.OrdinalIgnoreCase) &&
            path.Contains("simulator", StringComparison.OrdinalIgnoreCase))
            return "Meta XR Simulator";
        if (path.Contains("steam", StringComparison.OrdinalIgnoreCase))
            return "SteamVR";
        if (path.Contains("oculus", StringComparison.OrdinalIgnoreCase))
            return "Meta Quest Link";
        if (path.Equals("Not configured", StringComparison.Ordinal))
            return path;
        return Path.GetFileNameWithoutExtension(path).Replace('_', ' ');
    }

    private static bool IsSimulatorRuntime(string path) =>
        path.Contains("simulator", StringComparison.OrdinalIgnoreCase) ||
        path.Contains("xrsim", StringComparison.OrdinalIgnoreCase);

    private static bool RuntimePathsEqual(string left, string right) =>
        string.Equals(
            left.Trim().Trim('"').Replace('/', '\\'),
            right.Trim().Trim('"').Replace('/', '\\'),
            StringComparison.OrdinalIgnoreCase);

    private static void DeleteRegistryValue(RegistryHive hive, RegistryView view, string path, string valueName)
    {
        using var root = RegistryKey.OpenBaseKey(hive, view);
        using var key = root.OpenSubKey(path, writable: true);
        key?.DeleteValue(valueName, throwOnMissingValue: false);
    }

    private static void BroadcastEnvironmentChange()
    {
        _ = SendNotifyMessage(
            HwndBroadcast,
            WmSettingChange,
            UIntPtr.Zero,
            "Environment");
    }

    private static string FindMetaXrExecutable(string runtimePath)
    {
        var candidates = new List<string>();
        if (File.Exists(runtimePath) && Path.GetDirectoryName(runtimePath) is { } runtimeDirectory)
            candidates.Add(Path.Combine(runtimeDirectory, "MetaXRSimulator.exe"));

        var configPath = Environment.GetEnvironmentVariable("META_XRSIM_CONFIG_JSON", EnvironmentVariableTarget.User)
                         ?? Environment.GetEnvironmentVariable("META_XRSIM_CONFIG_JSON");
        if (File.Exists(configPath) && Directory.GetParent(configPath!)?.Parent?.FullName is { } configRoot)
            candidates.Add(Path.Combine(configRoot, "MetaXRSimulator.exe"));

        return candidates.FirstOrDefault(File.Exists) ?? string.Empty;
    }

    private string FindLastCaptureSummary()
    {
        if (!File.Exists(LayerLogPath))
            return "No OpenXR capture session has been logged yet.";

        try
        {
            return File.ReadLines(LayerLogPath).Reverse().FirstOrDefault(line =>
                       line.Contains("Recording overscan headset crop", StringComparison.OrdinalIgnoreCase) ||
                       line.Contains("Creating mirror textures", StringComparison.OrdinalIgnoreCase))
                   ?? "Layer log found; no completed mirror texture yet.";
        }
        catch (IOException)
        {
            return "Layer log is currently busy.";
        }
    }

    private static string HashFile(string path)
    {
        if (!File.Exists(path))
            return string.Empty;
        try
        {
            using var stream = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            return Convert.ToHexString(SHA256.HashData(stream));
        }
        catch
        {
            return string.Empty;
        }
    }

    private static string ResolveManifestLibraryPath(string manifestPath)
    {
        var fallback = Path.Combine(
            Path.GetDirectoryName(manifestPath) ?? string.Empty,
            "XR_APILAYER_NOVENDOR_OBSMirror.dll");
        if (!File.Exists(manifestPath))
            return fallback;

        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(manifestPath));
            var libraryPath = document.RootElement
                .GetProperty("api_layer")
                .GetProperty("library_path")
                .GetString();
            if (string.IsNullOrWhiteSpace(libraryPath))
                return fallback;

            var normalized = libraryPath.Replace('/', Path.DirectorySeparatorChar)
                .Replace('\\', Path.DirectorySeparatorChar);
            return Path.GetFullPath(
                Path.IsPathRooted(normalized)
                    ? normalized
                    : Path.Combine(Path.GetDirectoryName(manifestPath) ?? string.Empty, normalized));
        }
        catch
        {
            return fallback;
        }
    }

    private static bool IsProcessRunning(string processName)
    {
        try
        {
            return Process.GetProcessesByName(processName).Length > 0;
        }
        catch
        {
            return false;
        }
    }

    private static string ReadTail(string path, int lineCount)
    {
        if (!File.Exists(path))
            return $"Log not found: {path}";
        try
        {
            using var stream = new FileStream(
                path,
                FileMode.Open,
                FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete);
            using var reader = new StreamReader(stream, detectEncodingFromByteOrderMarks: true);

            var lines = new Queue<string>(lineCount);
            while (reader.ReadLine() is { } line)
            {
                if (lines.Count == lineCount)
                    lines.Dequeue();
                lines.Enqueue(line);
            }

            return string.Join(Environment.NewLine, lines);
        }
        catch (IOException ex)
        {
            return $"Could not read the log while it is being updated: {ex.Message}";
        }
    }

    private static void EnsureFile(string path, string description)
    {
        if (!File.Exists(path))
            throw new FileNotFoundException($"The {description} was not found.", path);
    }

    private static string FindRepoRoot()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            var setupScript = Path.Combine(current.FullName, "scripts", "Setup-OBS.ps1");
            var sourceMarker = Path.Combine(current.FullName, "OpenXR-Layer-OBSMirror.sln");
            var releaseMarker = Path.Combine(
                current.FullName,
                "bin",
                "x64",
                "Release",
                "XR_APILAYER_NOVENDOR_OBSMirror.dll");
            if (File.Exists(setupScript) &&
                (File.Exists(sourceMarker) || File.Exists(releaseMarker)))
                return current.FullName;
            current = current.Parent;
        }
        return Directory.GetCurrentDirectory();
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SendNotifyMessage(
        IntPtr hWnd,
        uint message,
        UIntPtr wParam,
        string lParam);

    private sealed record RuntimeSelection(string Path, string Source, bool IsOverride);
}
