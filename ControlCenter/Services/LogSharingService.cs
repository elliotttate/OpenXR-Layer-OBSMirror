using OBSMirror.ControlCenter.Models;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using Windows.ApplicationModel.DataTransfer;

namespace OBSMirror.ControlCenter.Services;

/// <summary>
/// Uploads a diagnostics bundle (snapshot report, OpenXR layer log, latest OBS
/// log) to filebin.net so users can share one link when reporting problems.
/// filebin.net accepts anonymous POSTs of raw bytes to
/// https://filebin.net/{bin}/{fileName}; that request URL is also the share
/// URL, and https://filebin.net/{bin} lists every file uploaded to the bin.
/// Bins auto-expire after about a week. The only privacy control is the
/// unguessable bin name, so the bundle deliberately contains just logs and
/// configuration state.
/// </summary>
public sealed class LogSharingService
{
    private static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(60) };
    private const string FilebinBase = "https://filebin.net";
    // Only the most recent portion of a large log is useful for support, and a
    // capped upload cannot run into the HTTP timeout on slow connections.
    private const int MaxUploadBytesPerFile = 4 * 1024 * 1024;

    public sealed record ShareResult(string BinUrl, IReadOnlyList<string> FileUrls, IReadOnlyList<string> Failures);

    /// <summary>
    /// Gathers the files worth sharing. Logs that do not exist yet are simply
    /// skipped; the report is always included.
    /// </summary>
    public static IReadOnlyList<(string FileName, byte[] Content)> CollectDiagnostics(
        OBSMirrorService service,
        SystemSnapshot? snapshot,
        MirrorPreviewService previewService)
    {
        var files = new List<(string FileName, byte[] Content)>();
        var stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss");

        var report = new StringBuilder();
        report.AppendLine("OpenXR OBS Mirror diagnostics report");
        report.AppendLine($"Machine: {Environment.MachineName}");
        report.AppendLine($"Generated: {DateTime.Now:yyyy-MM-dd HH:mm:ss zzz}");
        report.AppendLine($"OS: {Environment.OSVersion.VersionString}");
        report.AppendLine($"Control Center: {typeof(LogSharingService).Assembly.GetName().Version}");
        report.AppendLine();
        if (snapshot is not null)
        {
            report.AppendLine("[System snapshot]");
            foreach (var property in typeof(SystemSnapshot).GetProperties())
                report.AppendLine($"{property.Name}: {property.GetValue(snapshot)}");
        }
        else
        {
            report.AppendLine("No system snapshot was captured yet.");
        }
        report.AppendLine();
        report.Append(previewService.GetDiagnosticsReport());
        files.Add(($"OBSMirror-Report-{stamp}.txt", Encoding.UTF8.GetBytes(report.ToString())));

        if (TryReadAllBytes(service.LayerLogPath, out var layerLog))
            files.Add((Path.GetFileName(service.LayerLogPath), layerLog));

        var obsLogPath = service.GetLatestObsLogPath();
        if (File.Exists(obsLogPath) && TryReadAllBytes(obsLogPath, out var obsLog))
            files.Add(($"OBS-{Path.GetFileName(obsLogPath)}", obsLog));

        if (TryReadAllBytes(previewService.LogPath, out var previewLog))
            files.Add(($"ControlCenter-{Path.GetFileName(previewService.LogPath)}", previewLog));

        if (TryReadAllBytes(App.StartupLogPath, out var startupLog))
            files.Add((Path.GetFileName(App.StartupLogPath), startupLog));

        return files;
    }

    /// <summary>
    /// Uploads every file into one freshly generated bin and returns the bin
    /// URL to share. Throws when nothing could be uploaded; partial failures
    /// are reported through <see cref="ShareResult.Failures"/>.
    /// </summary>
    public async Task<ShareResult> UploadAsync(IReadOnlyList<(string FileName, byte[] Content)> files)
    {
        if (files.Count == 0)
            throw new InvalidOperationException("No diagnostic files were found to upload.");

        var bin = GenerateBinName();
        var binUrl = $"{FilebinBase}/{bin}";
        var fileUrls = new List<string>();
        var failures = new List<string>();

        foreach (var (fileName, content) in files)
        {
            var url = $"{FilebinBase}/{bin}/{Uri.EscapeDataString(fileName)}";
            try
            {
                using var request = new HttpRequestMessage(HttpMethod.Post, url);
                request.Content = new ByteArrayContent(content);
                request.Content.Headers.ContentType = new MediaTypeHeaderValue("text/plain") { CharSet = "utf-8" };
                request.Headers.UserAgent.ParseAdd("OBSMirror-ControlCenter/1.0 (+filebin.net)");
                request.Headers.Accept.ParseAdd("application/json");

                using var response = await Http.SendAsync(request);
                if (response.IsSuccessStatusCode)
                {
                    fileUrls.Add(url);
                }
                else
                {
                    var body = await response.Content.ReadAsStringAsync();
                    failures.Add($"{fileName}: HTTP {(int)response.StatusCode} {body[..Math.Min(body.Length, 120)]}");
                }
            }
            catch (HttpRequestException ex)
            {
                failures.Add($"{fileName}: {ex.Message}");
            }
            catch (TaskCanceledException)
            {
                failures.Add($"{fileName}: upload timed out");
            }
        }

        if (fileUrls.Count == 0)
            throw new InvalidOperationException(
                "Uploading the logs to filebin.net failed. " + string.Join("; ", failures));

        return new ShareResult(binUrl, fileUrls, failures);
    }

    /// <summary>
    /// Bin names must be unguessable because anyone with the name can read the
    /// bin: 12 CSPRNG characters over a 36-symbol alphabet.
    /// </summary>
    public static string GenerateBinName()
    {
        const string alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
        var bytes = new byte[12];
        RandomNumberGenerator.Fill(bytes);
        var builder = new StringBuilder("obsmirror-");
        foreach (var value in bytes)
            builder.Append(alphabet[value % alphabet.Length]);
        return builder.ToString();
    }

    /// <summary>Must be called on the UI thread.</summary>
    public static bool TryCopyToClipboard(string text)
    {
        try
        {
            var package = new DataPackage();
            package.SetText(text);
            Clipboard.SetContent(package);
            // Flush so the link survives the app closing.
            Clipboard.Flush();
            return true;
        }
        catch
        {
            return false;
        }
    }

    private static bool TryReadAllBytes(string path, out byte[] content)
    {
        content = [];
        try
        {
            if (!File.Exists(path))
                return false;
            using var stream = new FileStream(
                path,
                FileMode.Open,
                FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete);
            using var memory = new MemoryStream();
            if (stream.Length > MaxUploadBytesPerFile)
            {
                var marker = Encoding.UTF8.GetBytes(
                    $"[truncated for upload: last {MaxUploadBytesPerFile / (1024 * 1024)} MB of a {stream.Length}-byte file]{Environment.NewLine}");
                memory.Write(marker, 0, marker.Length);
                stream.Seek(-MaxUploadBytesPerFile, SeekOrigin.End);
            }
            stream.CopyTo(memory);
            content = memory.ToArray();
            return content.Length > 0;
        }
        catch (IOException)
        {
            return false;
        }
    }
}
