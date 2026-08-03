using System.Diagnostics;
using System.Security.Cryptography;
using Microsoft.Win32;
using OBSMirror.ControlCenter.Models;

namespace OBSMirror.ControlCenter.Services;

public sealed class OBSMirrorService
{
    private const string ConfigKey = @"Software\OpenXR-OBSMirror";
    private const string LayerRegistryKey = @"Software\Khronos\OpenXR\1\ApiLayers\Implicit";
    private const string ActiveRuntimeKey = @"Software\Khronos\OpenXR\1\ActiveRuntime";
    private const string LayerManifestName = "XR_APILAYER_NOVENDOR_OBSMirror.json";

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
    public string InstalledLayerPath => Path.Combine(InstallDirectory, "XR_APILAYER_NOVENDOR_OBSMirror.dll");
    public string ReleasePluginPath => Path.Combine(RepoRoot, "bin", "x64", "Release", "OBS_Plugin", "win-openxr.dll");
    public string SetupScriptPath => Path.Combine(RepoRoot, "scripts", "Setup-OBS.ps1");
    public string InstallScriptPath => Path.Combine(RepoRoot, "scripts", "Install-Layer.ps1");
    public string UninstallScriptPath => Path.Combine(RepoRoot, "scripts", "Uninstall-Layer.ps1");

    public SystemSnapshot GetSnapshot()
    {
        var (enabled, horizontal, vertical) = ReadOverscan();
        var (smoothingManaged, cameraSmoothing, smoothingCrop) = ReadCameraSmoothing();
        var runtimePath = ResolveRuntimePath();
        var runtimeName = FriendlyRuntimeName(runtimePath);
        var registeredManifest = FindRegisteredLayerManifest();
        var sourcePluginHash = HashFile(ReleasePluginPath);
        var pluginHash = HashFile(PluginPath);
        var metaExe = FindMetaXrExecutable(runtimePath);

        return new SystemSnapshot(
            LayerRegistered: !string.IsNullOrWhiteSpace(registeredManifest),
            LayerFilesInstalled: File.Exists(InstalledManifestPath) && File.Exists(InstalledLayerPath),
            PluginInstalled: File.Exists(PluginPath),
            PluginCurrent: !string.IsNullOrEmpty(sourcePluginHash) &&
                           string.Equals(sourcePluginHash, pluginHash, StringComparison.OrdinalIgnoreCase),
            ObsRunning: IsProcessRunning("obs64"),
            MetaXrRunning: IsProcessRunning("MetaXRSimulator"),
            RuntimeName: runtimeName,
            RuntimePath: runtimePath,
            LayerManifestPath: registeredManifest ?? InstalledManifestPath,
            LayerHash: HashFile(InstalledLayerPath),
            PluginHash: pluginHash,
            SourcePluginHash: sourcePluginHash,
            OverscanEnabled: enabled,
            HorizontalPercent: horizontal,
            VerticalPercent: vertical,
            CameraSmoothingManaged: smoothingManaged,
            CameraSmoothing: cameraSmoothing,
            SmoothingCrop: smoothingCrop,
            LastCaptureSummary: FindLastCaptureSummary(),
            MetaXrExecutable: metaExe,
            CapturedAt: DateTime.Now);
    }

    public void ApplyOverscan(bool enabled, int horizontalPercent, int verticalPercent)
    {
        horizontalPercent = Math.Clamp(horizontalPercent, 100, 150);
        verticalPercent = Math.Clamp(verticalPercent, 100, 150);

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

    public async Task<string> SetupAsync(bool allowRunningObs)
    {
        EnsureFile(SetupScriptPath, "setup script");
        var args = new List<string>();
        if (allowRunningObs)
            args.Add("-AllowRunningOBS");
        return await RunPowerShellAsync(SetupScriptPath, args);
    }

    public async Task<string> RegisterLayerAsync()
    {
        EnsureFile(InstallScriptPath, "layer registration script");
        EnsureFile(InstalledManifestPath, "installed layer manifest");
        return await RunPowerShellAsync(InstallScriptPath, ["-ManifestPath", InstalledManifestPath]);
    }

    public async Task<string> UnregisterLayerAsync()
    {
        EnsureFile(UninstallScriptPath, "layer unregistration script");
        return await RunPowerShellAsync(UninstallScriptPath, ["-Scope", "CurrentUser"]);
    }

    public void LaunchObs()
    {
        const string obsPath = @"C:\Program Files\obs-studio\bin\64bit\obs64.exe";
        EnsureFile(obsPath, "OBS executable");
        Process.Start(new ProcessStartInfo(obsPath)
        {
            WorkingDirectory = Path.GetDirectoryName(obsPath)!,
            UseShellExecute = true
        });
    }

    public void LaunchMetaXr(string path)
    {
        EnsureFile(path, "Meta XR Simulator executable");
        Process.Start(new ProcessStartInfo(path)
        {
            WorkingDirectory = Path.GetDirectoryName(path)!,
            UseShellExecute = true
        });
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
        var horizontal = Math.Clamp(Convert.ToInt32(key?.GetValue("OverscanHorizontalPercent", 115) ?? 115), 100, 150);
        var vertical = Math.Clamp(Convert.ToInt32(key?.GetValue("OverscanVerticalPercent", 108) ?? 108), 100, 150);
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

    private string? FindRegisteredLayerManifest()
    {
        using var key = Registry.CurrentUser.OpenSubKey(LayerRegistryKey);
        return key?.GetValueNames().FirstOrDefault(name =>
            Path.GetFileName(name).Equals(LayerManifestName, StringComparison.OrdinalIgnoreCase) &&
            Convert.ToInt32(key.GetValue(name, 1) ?? 1) == 0);
    }

    private static string ResolveRuntimePath()
    {
        var candidates = new[]
        {
            Environment.GetEnvironmentVariable("XR_RUNTIME_JSON"),
            Environment.GetEnvironmentVariable("XR_RUNTIME_JSON", EnvironmentVariableTarget.User),
            ReadRegistryString(RegistryHive.CurrentUser, ActiveRuntimeKey),
            ReadRegistryString(RegistryHive.LocalMachine, ActiveRuntimeKey)
        };
        return candidates.FirstOrDefault(value => !string.IsNullOrWhiteSpace(value)) ?? "Not configured";
    }

    private static string? ReadRegistryString(RegistryHive hive, string path)
    {
        try
        {
            using var root = RegistryKey.OpenBaseKey(hive, RegistryView.Registry64);
            using var key = root.OpenSubKey(path);
            return key?.GetValue(null) as string;
        }
        catch
        {
            return null;
        }
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

    private static async Task<string> RunPowerShellAsync(string scriptPath, IEnumerable<string> scriptArguments)
    {
        var startInfo = new ProcessStartInfo("pwsh")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        startInfo.ArgumentList.Add("-NoProfile");
        startInfo.ArgumentList.Add("-NonInteractive");
        startInfo.ArgumentList.Add("-ExecutionPolicy");
        startInfo.ArgumentList.Add("Bypass");
        startInfo.ArgumentList.Add("-File");
        startInfo.ArgumentList.Add(scriptPath);
        foreach (var argument in scriptArguments)
            startInfo.ArgumentList.Add(argument);

        using var process = Process.Start(startInfo)
            ?? throw new InvalidOperationException("PowerShell could not be started.");
        var outputTask = process.StandardOutput.ReadToEndAsync();
        var errorTask = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        var output = (await outputTask).Trim();
        var error = (await errorTask).Trim();
        if (process.ExitCode != 0)
            throw new InvalidOperationException(string.IsNullOrEmpty(error) ? output : error);
        return string.IsNullOrWhiteSpace(output) ? "Completed successfully." : output;
    }

    private static string FindRepoRoot()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            if (File.Exists(Path.Combine(current.FullName, "scripts", "Setup-OBS.ps1")) &&
                File.Exists(Path.Combine(current.FullName, "OpenXR-Layer-OBSMirror.sln")))
                return current.FullName;
            current = current.Parent;
        }
        return Directory.GetCurrentDirectory();
    }
}
