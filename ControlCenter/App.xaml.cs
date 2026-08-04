using Microsoft.UI.Xaml;
using OBSMirror.ControlCenter.Services;

namespace OBSMirror.ControlCenter;

public partial class App : Application
{
    private Window? _window;
    internal static string StartupLogPath { get; } = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "OpenXR-OBSMirror",
        "ControlCenter-startup.log");

    public App()
    {
        LogStartup("App constructor entered");
        try
        {
            InitializeComponent();
            LogStartup("App.InitializeComponent completed");
        }
        catch (Exception ex)
        {
            LogStartup("App.InitializeComponent failed", ex);
            throw;
        }

        UnhandledException += (_, args) =>
        {
            System.Diagnostics.Debug.WriteLine(args.Exception);
            LogStartup("Application.UnhandledException", args.Exception);
        };
    }

    protected override async void OnLaunched(LaunchActivatedEventArgs args)
    {
        LogStartup("App.OnLaunched entered");
        try
        {
            if (ElevatedInstallService.TryGetHelperResultPath(out var resultPath))
            {
                LogStartup("Elevated OBS plugin installer started");
                await ElevatedInstallService.RunHelperAndExitAsync(resultPath);
                return;
            }

            _window = new MainWindow();
            LogStartup("MainWindow constructed");
            _window.Activate();
            LogStartup("MainWindow activated");
        }
        catch (Exception ex)
        {
            LogStartup("App.OnLaunched failed", ex);
            throw;
        }
    }

    internal static void LogStartup(string message, Exception? exception = null)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(StartupLogPath)!);
            File.AppendAllText(
                StartupLogPath,
                $"[{DateTimeOffset.Now:O}] {message}{Environment.NewLine}{exception}{Environment.NewLine}");
        }
        catch
        {
            // Startup diagnostics must never become a second startup failure.
        }
    }
}
