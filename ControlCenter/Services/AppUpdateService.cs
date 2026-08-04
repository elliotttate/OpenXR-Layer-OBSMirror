using System.Diagnostics;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;

namespace OBSMirror.ControlCenter.Services;

public sealed record AppUpdateInfo(
    string Version,
    string Title,
    string ReleaseNotes,
    string InstallerName,
    string InstallerUrl,
    long InstallerSize,
    string? ChecksumsUrl);

/// <summary>
/// Checks GitHub Releases for a newer Control Center build, downloads the
/// installer, verifies it against the release's SHA256SUMS.txt manifest, and
/// hands it to Inno Setup for a silent in-place update (the installer
/// relaunches the app when it finishes). Only installers fetched over HTTPS
/// from this repository's releases are ever run, and never without a matching
/// checksum when the release publishes one.
/// </summary>
public sealed class AppUpdateService
{
    private const string ReleasesApiUrl =
        "https://api.github.com/repos/elliotttate/OpenXR-Layer-OBSMirror/releases?per_page=15";
    private const string InstallerSuffix = "-Setup.exe";
    private const string ChecksumsAssetName = "SHA256SUMS.txt";

    private static readonly HttpClient Http = CreateClient();

    public static string CurrentVersion { get; } = ResolveCurrentVersion();

    /// <summary>
    /// Returns the newest installable release that is newer than the running
    /// build, or null when up to date. Pre-releases count: beta users update
    /// between betas, so this reads the full release list rather than
    /// "releases/latest" (which hides pre-releases).
    /// </summary>
    public async Task<AppUpdateInfo?> CheckForUpdateAsync()
    {
        using var response = await Http.GetAsync(ReleasesApiUrl);
        response.EnsureSuccessStatusCode();
        using var document = JsonDocument.Parse(await response.Content.ReadAsStringAsync());

        AppUpdateInfo? best = null;
        foreach (var release in document.RootElement.EnumerateArray())
        {
            if (release.GetProperty("draft").GetBoolean())
                continue;

            var tag = release.GetProperty("tag_name").GetString() ?? string.Empty;
            var version = tag.Trim().TrimStart('v', 'V');
            if (version.Length == 0)
                continue;

            string? installerName = null;
            string? installerUrl = null;
            long installerSize = 0;
            string? checksumsUrl = null;
            foreach (var asset in release.GetProperty("assets").EnumerateArray())
            {
                var name = asset.GetProperty("name").GetString() ?? string.Empty;
                var url = asset.GetProperty("browser_download_url").GetString() ?? string.Empty;
                if (name.EndsWith(InstallerSuffix, StringComparison.OrdinalIgnoreCase))
                {
                    installerName = name;
                    installerUrl = url;
                    installerSize = asset.GetProperty("size").GetInt64();
                }
                else if (name.Equals(ChecksumsAssetName, StringComparison.OrdinalIgnoreCase))
                {
                    checksumsUrl = url;
                }
            }
            if (installerName is null || installerUrl is null)
                continue;

            if (best is null || CompareVersions(version, best.Version) > 0)
            {
                var title = release.GetProperty("name").GetString();
                var notes = release.TryGetProperty("body", out var body)
                    ? body.GetString() ?? string.Empty
                    : string.Empty;
                best = new AppUpdateInfo(
                    version,
                    string.IsNullOrWhiteSpace(title) ? tag : title!,
                    notes.Trim(),
                    installerName,
                    installerUrl,
                    installerSize,
                    checksumsUrl);
            }
        }

        return best is not null && CompareVersions(best.Version, CurrentVersion) > 0 ? best : null;
    }

    /// <summary>
    /// Downloads the installer to the per-user updates folder, reporting whole
    /// percentage points, and verifies it against the release checksum
    /// manifest before returning the path.
    /// </summary>
    public async Task<string> DownloadInstallerAsync(AppUpdateInfo update, IProgress<int>? percentProgress)
    {
        var directory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "OpenXR-OBSMirror",
            "updates");
        Directory.CreateDirectory(directory);
        var installerPath = Path.Combine(directory, update.InstallerName);

        using (var response = await Http.GetAsync(update.InstallerUrl, HttpCompletionOption.ResponseHeadersRead))
        {
            response.EnsureSuccessStatusCode();
            var totalBytes = response.Content.Headers.ContentLength ?? update.InstallerSize;
            await using var source = await response.Content.ReadAsStreamAsync();
            await using var target = new FileStream(installerPath, FileMode.Create, FileAccess.Write, FileShare.None);
            var buffer = new byte[81920];
            long written = 0;
            var lastPercent = -1;
            int read;
            while ((read = await source.ReadAsync(buffer)) > 0)
            {
                await target.WriteAsync(buffer.AsMemory(0, read));
                written += read;
                if (totalBytes > 0 && percentProgress is not null)
                {
                    var percent = (int)Math.Clamp(written * 100 / totalBytes, 0, 100);
                    if (percent != lastPercent)
                    {
                        lastPercent = percent;
                        percentProgress.Report(percent);
                    }
                }
            }
        }

        await VerifyChecksumAsync(update, installerPath);
        return installerPath;
    }

    /// <summary>
    /// Starts the silent in-place update and arranges for the app to reopen
    /// afterwards. Elevation is requested by the installer itself; declining
    /// the UAC prompt surfaces here as an exception (and no relaunch watcher
    /// is left behind).
    ///
    /// The relaunch is done by a detached watcher rather than by the
    /// installer, because the installer that performs any given update was
    /// published before this code existed - relying on its [Run] entry would
    /// only ever fix the update after next. The watcher also survives the app
    /// exiting, which is required for the installer to replace its files.
    /// </summary>
    public static void StartUpdateAndRelaunch(string installerPath)
    {
        var installer = Process.Start(new ProcessStartInfo(installerPath)
        {
            UseShellExecute = true,
            Arguments = "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART",
        }) ?? throw new InvalidOperationException("The update installer could not be started.");

        // Relaunch whatever executable is running now: for an installed copy
        // that is the path the installer updates in place.
        var appPath = Environment.ProcessPath;
        if (string.IsNullOrWhiteSpace(appPath))
            return;

        try
        {
            StartRelaunchWatcher(installerPath, appPath);
        }
        catch
        {
            // A missing watcher only costs the user a manual restart; it must
            // never abort an update that is already running.
        }
    }

    private static void StartRelaunchWatcher(string installerPath, string appPath)
    {
        // Setup is tracked purely by name prefix: the process that actually
        // installs is an elevated temporary copy ("<name>.tmp") rather than
        // the process we started, and process ids get recycled.
        var installerBaseName = Path.GetFileNameWithoutExtension(installerPath);
        var appProcessName = Path.GetFileNameWithoutExtension(appPath);
        var scriptPath = Path.Combine(Path.GetDirectoryName(installerPath)!, "relaunch-after-update.ps1");
        var script = $$"""
            $ErrorActionPreference = 'SilentlyContinue'
            $installerBase = '{{installerBaseName.Replace("'", "''")}}'
            $appPath = '{{appPath.Replace("'", "''")}}'
            $appName = '{{appProcessName.Replace("'", "''")}}'

            function Get-InstallerProcesses {
                @(Get-Process -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -like "$installerBase*" })
            }

            # Phase 1: wait for setup to appear (the UAC prompt can hold it).
            # A declined prompt simply falls through and reopens the old build.
            $deadline = (Get-Date).AddSeconds(120)
            while ((Get-Date) -lt $deadline -and (Get-InstallerProcesses).Count -eq 0) {
                Start-Sleep -Milliseconds 500
            }

            # Phase 2: wait for every setup process to finish.
            $deadline = (Get-Date).AddMinutes(10)
            while ((Get-Date) -lt $deadline -and (Get-InstallerProcesses).Count -gt 0) {
                Start-Sleep -Seconds 1
            }

            Start-Sleep -Seconds 2
            # The installer may already have reopened the app (interactive
            # installs, or a future build that restarts it itself).
            if (-not (Get-Process -Name $appName -ErrorAction SilentlyContinue)) {
                Start-Process -FilePath $appPath -WorkingDirectory (Split-Path $appPath)
            }
            """;
        File.WriteAllText(scriptPath, script);

        Process.Start(new ProcessStartInfo("powershell.exe")
        {
            Arguments = $"-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File \"{scriptPath}\"",
            UseShellExecute = false,
            CreateNoWindow = true,
        });
    }

    private static async Task VerifyChecksumAsync(AppUpdateInfo update, string installerPath)
    {
        if (update.ChecksumsUrl is null)
            return; // The release published no checksum manifest.

        var manifest = await Http.GetStringAsync(update.ChecksumsUrl);
        string? expected = null;
        foreach (var line in manifest.Split('\n'))
        {
            // Build-Release.ps1 writes "<hash> *<fileName>" per line.
            var trimmed = line.Trim();
            if (trimmed.EndsWith(update.InstallerName, StringComparison.OrdinalIgnoreCase))
            {
                expected = trimmed.Split(' ')[0].Trim();
                break;
            }
        }
        if (string.IsNullOrWhiteSpace(expected))
            throw new InvalidOperationException(
                $"The release checksum manifest has no entry for {update.InstallerName}.");

        string actual;
        await using (var stream = File.OpenRead(installerPath))
            actual = Convert.ToHexString(await SHA256.HashDataAsync(stream));

        if (!actual.Equals(expected, StringComparison.OrdinalIgnoreCase))
        {
            File.Delete(installerPath);
            throw new InvalidOperationException(
                "The downloaded installer did not match the release checksum and was deleted. Try again later.");
        }
    }

    private static string ResolveCurrentVersion()
    {
        var informational = Assembly.GetExecutingAssembly()
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion;
        if (!string.IsNullOrWhiteSpace(informational))
        {
            // The SDK appends "+<commit>" when SourceLink is active.
            var metadataStart = informational.IndexOf('+');
            return metadataStart > 0 ? informational[..metadataStart] : informational;
        }
        return Assembly.GetExecutingAssembly().GetName().Version?.ToString(3) ?? "0.0.0";
    }

    private static HttpClient CreateClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromMinutes(5) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("OBSMirror-ControlCenter");
        client.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
        return client;
    }

    /// <summary>
    /// Semver-style comparison with prerelease ordering:
    /// 0.3.0 &gt; 0.3.0-beta.4 &gt; 0.3.0-beta.3 (numeric identifiers compare
    /// numerically, so beta.10 &gt; beta.9).
    /// </summary>
    public static int CompareVersions(string left, string right)
    {
        var (leftCore, leftPre) = SplitVersion(left);
        var (rightCore, rightPre) = SplitVersion(right);

        for (var index = 0; index < Math.Max(leftCore.Length, rightCore.Length); index++)
        {
            var l = index < leftCore.Length ? leftCore[index] : 0;
            var r = index < rightCore.Length ? rightCore[index] : 0;
            if (l != r)
                return l.CompareTo(r);
        }

        if (leftPre is null && rightPre is null)
            return 0;
        if (leftPre is null)
            return 1; // A release outranks its own pre-releases.
        if (rightPre is null)
            return -1;

        var leftIds = leftPre.Split('.');
        var rightIds = rightPre.Split('.');
        for (var index = 0; index < Math.Max(leftIds.Length, rightIds.Length); index++)
        {
            if (index >= leftIds.Length)
                return -1; // Fewer identifiers sorts lower per semver.
            if (index >= rightIds.Length)
                return 1;
            var leftIsNumber = long.TryParse(leftIds[index], out var leftNumber);
            var rightIsNumber = long.TryParse(rightIds[index], out var rightNumber);
            var comparison = leftIsNumber && rightIsNumber
                ? leftNumber.CompareTo(rightNumber)
                : leftIsNumber != rightIsNumber
                    ? (leftIsNumber ? -1 : 1) // Numeric identifiers sort below alphanumeric.
                    : string.CompareOrdinal(leftIds[index], rightIds[index]);
            if (comparison != 0)
                return comparison;
        }
        return 0;
    }

    private static (int[] Core, string? Prerelease) SplitVersion(string version)
    {
        var value = version.Trim().TrimStart('v', 'V');
        string? prerelease = null;
        var dash = value.IndexOf('-');
        if (dash >= 0)
        {
            prerelease = value[(dash + 1)..];
            value = value[..dash];
        }
        var core = value.Split('.')
            .Select(part => int.TryParse(part, out var number) ? number : 0)
            .ToArray();
        return (core, string.IsNullOrEmpty(prerelease) ? null : prerelease);
    }
}
