using System.ComponentModel;
using System.Diagnostics;
using System.Text.Json;

namespace OBSMirror.ControlCenter.Services;

internal sealed record ElevatedInstallResult(bool Success, string Message);

internal static class ElevatedInstallService
{
    private const string HelperArgument = "--install-obs-plugin-elevated";
    private const string RemoveHelperArgument = "--remove-conflicting-plugin-elevated";
    private const string ResultArgument = "--result-file";

    public static bool TryGetHelperResultPath(out string resultPath)
    {
        var arguments = Environment.GetCommandLineArgs();
        var helperIndex = Array.IndexOf(arguments, HelperArgument);
        if (helperIndex < 0)
            helperIndex = Array.IndexOf(arguments, RemoveHelperArgument);
        var resultIndex = Array.IndexOf(arguments, ResultArgument);
        if (helperIndex < 0 || resultIndex < 0 || resultIndex + 1 >= arguments.Length)
        {
            resultPath = string.Empty;
            return false;
        }

        resultPath = Path.GetFullPath(arguments[resultIndex + 1]);
        var temporaryDirectory = Path.GetFullPath(Path.GetTempPath());
        if (!resultPath.StartsWith(temporaryDirectory, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("The elevated installer result path must be in the temporary directory.");
        return true;
    }

    public static async Task RunHelperAndExitAsync(string resultPath)
    {
        // The elevated instance re-resolves what to act on itself; nothing
        // from the command line is used as a path, so an elevated run can only
        // ever touch the plugin locations this app already manages.
        var removing = Array.IndexOf(Environment.GetCommandLineArgs(), RemoveHelperArgument) >= 0;

        ElevatedInstallResult result;
        try
        {
            var service = new OBSMirrorService();
            var message = removing
                ? await Task.Run(service.RemoveConflictingPlugin)
                : await service.InstallPluginOnlyAsync();
            result = new ElevatedInstallResult(true, message);
        }
        catch (Exception ex)
        {
            result = new ElevatedInstallResult(false, ex.Message);
        }

        try
        {
            await File.WriteAllTextAsync(resultPath, JsonSerializer.Serialize(result));
        }
        catch (Exception ex)
        {
            App.LogStartup("Could not write the elevated installer result", ex);
            Environment.Exit(1);
        }

        Environment.Exit(result.Success ? 0 : 1);
    }

    public static Task<string> InstallPluginElevatedAsync() => RunElevatedAsync(HelperArgument);

    public static Task<string> RemoveConflictingPluginElevatedAsync() => RunElevatedAsync(RemoveHelperArgument);

    private static async Task<string> RunElevatedAsync(string helperArgument)
    {
        var executable = Environment.ProcessPath;
        if (string.IsNullOrWhiteSpace(executable) || !File.Exists(executable))
            throw new FileNotFoundException("The Control Center executable could not be resolved for elevation.", executable);

        var resultPath = Path.Combine(
            Path.GetTempPath(),
            $"OBSMirror-install-{Guid.NewGuid():N}.json");
        try
        {
            var startInfo = new ProcessStartInfo(executable)
            {
                UseShellExecute = true,
                Verb = "runas",
                WorkingDirectory = AppContext.BaseDirectory
            };
            startInfo.ArgumentList.Add(helperArgument);
            startInfo.ArgumentList.Add(ResultArgument);
            startInfo.ArgumentList.Add(resultPath);

            using var process = Process.Start(startInfo)
                                ?? throw new InvalidOperationException("The elevated OBS plugin installer could not be started.");
            await process.WaitForExitAsync();

            if (!File.Exists(resultPath))
                throw new InvalidOperationException("The elevated OBS plugin installer did not return a result.");
            var result = JsonSerializer.Deserialize<ElevatedInstallResult>(await File.ReadAllTextAsync(resultPath))
                         ?? throw new InvalidDataException("The elevated OBS plugin installer returned an invalid result.");
            if (!result.Success)
                throw new InvalidOperationException(result.Message);
            return result.Message;
        }
        catch (Win32Exception ex) when (ex.NativeErrorCode == 1223)
        {
            throw new InvalidOperationException("Installation was canceled before administrator permission was granted.", ex);
        }
        finally
        {
            File.Delete(resultPath);
        }
    }
}
