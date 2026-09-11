using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using OBSMirror.ControlCenter.Localization;
using OBSMirror.ControlCenter.Models;
using OBSMirror.ControlCenter.Services;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.WindowsRuntime;
using Windows.Graphics;
using Windows.Storage.Pickers;
using WinRT.Interop;

namespace OBSMirror.ControlCenter;

public sealed partial class MainWindow : Window
{
    private readonly OBSMirrorService _service = new();
    private readonly MirrorPreviewService _previewService = new();
    private readonly LogSharingService _logSharing = new();
    private readonly AppUpdateService _appUpdate = new();
    private readonly Stopwatch _previewFrameClock = new();
    private SystemSnapshot? _snapshot;
    private WriteableBitmap? _previewBitmap;
    private MirrorPreviewResult? _lastPreviewResult;
    private MirrorPreviewWindow? _previewWindow;
    private CancellationTokenSource? _previewLoopCts;
    private readonly HashSet<string> _vrRestartReasons = new(StringComparer.OrdinalIgnoreCase);
    private uint _vrRestartProducerPid;
    private string _vrRestartProducerApp = string.Empty;
    private int _previewFramesSinceSample;
    private double _previewFps;
    private readonly DispatcherTimer _obsCloseWatchTimer = new() { Interval = TimeSpan.FromSeconds(3) };
    private bool _autoUpdateInProgress;
    private bool _autoUpdateFailed;
    private string _lastAutoUpdateKey = string.Empty;
    private bool _loadingControls;
    private bool _loadingSmoothingControls;
    // Settings save themselves as the controls move. The delay collapses a
    // slider drag into a single write instead of one per pixel of travel.
    private readonly DispatcherTimer _overscanSaveTimer =
        new() { Interval = TimeSpan.FromMilliseconds(450) };
    private readonly DispatcherTimer _smoothingSaveTimer =
        new() { Interval = TimeSpan.FromMilliseconds(450) };
    private bool _loadingQuadLayerControls;
    private bool _loadingLayerRegistrationControls;

    public MainWindow()
    {
        App.LogStartup("MainWindow constructor entered");
        InitializeComponent();
        App.LogStartup("MainWindow.InitializeComponent completed");
        Title = Loc.S("Ui_Window_MainTitle", "OBSMirror Control Center");

        ConfigureOverscanSlider(HorizontalSlider, 115);
        ConfigureOverscanSlider(VerticalSlider, 108);
        ConfigureSlider(SmoothingSlider, 0, 100, 1, 35);
        ConfigureSlider(SmoothingCropSlider, 0, 25, 0.5, 8);

        _overscanSaveTimer.Tick += (_, _) =>
        {
            _overscanSaveTimer.Stop();
            _ = SaveOverscanAsync();
        };
        _smoothingSaveTimer.Tick += (_, _) =>
        {
            _smoothingSaveTimer.Stop();
            _ = SaveSmoothingAsync();
        };
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        SystemBackdrop = new MicaBackdrop();
        App.LogStartup("Dark title bar and Mica backdrop configured");

        var hwnd = WindowNative.GetWindowHandle(this);
        var windowId = Win32Interop.GetWindowIdFromWindow(hwnd);
        var appWindow = AppWindow.GetFromWindowId(windowId);
        appWindow.Changed += AppWindow_Changed;
        App.LogStartup("AppWindow acquired");
        SetWindowIcon(appWindow);
        var scale = GetDpiForWindow(hwnd) / 96.0;
        App.LogStartup($"Window DPI scale is {scale:0.00}");
        appWindow.Resize(new SizeInt32(
            (int)Math.Round(1280 * scale),
            (int)Math.Round(820 * scale)));
        App.LogStartup($"AppWindow resized to {appWindow.Size.Width} × {appWindow.Size.Height} physical pixels");
        appWindow.TitleBar.ButtonBackgroundColor = Colors.Transparent;
        appWindow.TitleBar.ButtonInactiveBackgroundColor = Colors.Transparent;
        appWindow.TitleBar.ButtonForegroundColor = Colors.White;
        appWindow.TitleBar.ButtonInactiveForegroundColor = ColorHelper.FromArgb(255, 130, 138, 153);
        App.LogStartup("AppWindow title bar styled");

        _obsCloseWatchTimer.Tick += ObsCloseWatchTimer_Tick;

        Activated += MainWindow_Activated;
        Closed += (_, _) =>
        {
            _obsCloseWatchTimer.Stop();
            StopMirrorPreview();
            _previewWindow?.Close();
            _previewWindow = null;
            _previewService.Dispose();
        };
        App.LogStartup("MainWindow constructor completed");
    }

    private static void SetWindowIcon(AppWindow appWindow)
    {
        var iconPath = Path.Combine(
            AppContext.BaseDirectory,
            "Assets",
            "OBSMirror.ControlCenter.ico");

        if (!File.Exists(iconPath))
        {
            App.LogStartup($"Window icon was not found: {iconPath}");
            return;
        }

        appWindow.SetIcon(iconPath);
        App.LogStartup($"Window icon loaded from {iconPath}");
    }

    private static void ConfigureOverscanSlider(Slider slider, double value)
    {
        // RangeBase validates each assignment immediately. Set the upper bound
        // first so a 100-based percentage range never conflicts with defaults.
        slider.Maximum = 200;
        slider.Minimum = 100;
        slider.StepFrequency = 1;
        slider.Value = value;
    }

    private static void ConfigureSlider(Slider slider, double minimum, double maximum, double step, double value)
    {
        slider.Maximum = maximum;
        slider.Minimum = minimum;
        slider.StepFrequency = step;
        slider.Value = value;
    }

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr hwnd);

    [DllImport("winmm.dll", EntryPoint = "timeBeginPeriod")]
    private static extern uint TimeBeginPeriod(uint period);

    [DllImport("winmm.dll", EntryPoint = "timeEndPeriod")]
    private static extern uint TimeEndPeriod(uint period);

    private async void MainWindow_Activated(object sender, WindowActivatedEventArgs args)
    {
        Activated -= MainWindow_Activated;
        await RefreshSnapshotAsync();
        StartMirrorPreview();
        _ = CheckForAppUpdateAsync();
    }

    // On launch: ask GitHub for a newer release and offer it in one click.
    // A failed check (offline, rate-limited) is logged and stays silent - the
    // app must never nag about updates it cannot fetch.
    private async Task CheckForAppUpdateAsync()
    {
        AppUpdateInfo? update;
        try
        {
            update = await _appUpdate.CheckForUpdateAsync();
        }
        catch (Exception ex)
        {
            App.LogStartup("App update check failed", ex);
            return;
        }

        if (update is null)
        {
            App.LogStartup($"App update check: {AppUpdateService.CurrentVersion} is current");
            return;
        }
        App.LogStartup($"App update check: {update.Version} is available (running {AppUpdateService.CurrentVersion})");

        if (Content?.XamlRoot is not { } xamlRoot)
            return;

        var notes = update.ReleaseNotes;
        if (notes.Length > 1200)
            notes = notes[..1200] + "…";
        var dialog = new ContentDialog
        {
            XamlRoot = xamlRoot,
            Title = Loc.F("Code_Main_UpdateAvailableTitle", "Update available — {0}", update.Title),
            PrimaryButtonText = Loc.S("Code_Main_UpdateAvailableYes", "Yes, update"),
            CloseButtonText = Loc.S("Code_Main_Later", "Later"),
            DefaultButton = ContentDialogButton.Primary,
            Content = new ScrollViewer
            {
                MaxHeight = 340,
                Content = new TextBlock
                {
                    Text = Loc.F("Code_Main_UpdateAvailableBody", "Version {0} is available (you have {1}).", update.Version, AppUpdateService.CurrentVersion) + " " +
                           Loc.S("Code_Main_UpdateAvailableAuto", "It downloads, installs, and reopens the app automatically.") +
                           (string.IsNullOrWhiteSpace(notes) ? string.Empty : $"\n\n{notes}"),
                    TextWrapping = TextWrapping.Wrap,
                },
            },
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
            return;

        SetBusy(true);
        try
        {
            var progress = new Progress<int>(percent =>
                ShowMessage(
                    Loc.F("Code_Main_DownloadingUpdate", "Downloading update {0}", update.Version),
                    $"{update.InstallerName} — {percent}%",
                    InfoBarSeverity.Informational));
            var installerPath = await _appUpdate.DownloadInstallerAsync(update, progress);

            ShowMessage(
                Loc.S("Code_Main_InstallingUpdate", "Installing update"),
                Loc.S("Code_Main_UpdateInstallNote", "The app closes now and reopens automatically when the update finishes."),
                InfoBarSeverity.Informational);
            AppUpdateService.StartUpdateAndRelaunch(installerPath);
            await Task.Delay(500);
            Application.Current.Exit();
        }
        catch (Exception ex)
        {
            App.LogStartup("App update failed", ex);
            ShowMessage(
                Loc.S("Code_Main_UpdateFailed", "Update failed"),
                Loc.F("Code_Main_UpdateFailedBody", "{0} You can retry from the dialog on next launch or download it from GitHub Releases.", ex.Message),
                InfoBarSeverity.Error);
        }
        finally
        {
            SetBusy(false);
        }
    }

    private async Task RefreshSnapshotAsync()
    {
        SetBusy(true);
        try
        {
            _snapshot = await Task.Run(_service.GetSnapshot);
            RenderSnapshot(_snapshot);
            RefreshLogView();
            _ = TryAutoUpdateAsync();
        }
        catch (Exception ex)
        {
            App.LogStartup("RefreshSnapshotAsync failed", ex);
            ShowMessage(Loc.S("Code_Main_CouldNotRefreshStatus", "Could not refresh status"), ex.Message, InfoBarSeverity.Error);
        }
        finally
        {
            SetBusy(false);
        }
    }

    private void RenderSnapshot(SystemSnapshot snapshot)
    {
        SetStatus(LayerDot, LayerStatusText,
            snapshot.LayerRegistered && snapshot.LayerFilesInstalled && snapshot.LayerCurrent,
            !snapshot.LayerRegistered ? Loc.S("Ui_Install_RegistrationState.OffContent", "Disabled")
            : snapshot.LayerCurrent ? Loc.S("Ui_Install_RegistrationState.OnContent", "Enabled")
            : _autoUpdateFailed ? Loc.S("Code_Main_UpdateFailed", "Update failed")
            : Loc.S("Code_Main_Updating", "Updating…"));
        LayerDetailText.Text = snapshot.LayerFilesInstalled
            ? snapshot.LayerCurrent ? Loc.F("Code_Main_LayerInstalled", "Installed • {0}", ShortHash(snapshot.LayerHash))
              : _autoUpdateFailed ? Loc.S("Code_Main_RetryFromInstallationPage", "Retry from the Installation page")
              : Loc.S("Code_Main_InstallingNewLayerAutomatically", "Installing the new layer build automatically")
            : Loc.S("Code_Main_ReleaseFilesNotInstalled", "Release files are not installed");

        SetStatus(PluginDot, PluginStatusText,
            snapshot.PluginInstalled && snapshot.PluginCurrent,
            !snapshot.PluginInstalled ? Loc.S("Code_Main_NotInstalled", "Not installed")
            : snapshot.PluginCurrent ? Loc.S("Code_Main_Current", "Current")
            : _autoUpdateFailed ? Loc.S("Code_Main_UpdateFailed", "Update failed")
            : snapshot.ObsRunning ? Loc.S("Code_Main_CloseObsToUpdate", "Close OBS to update")
            : Loc.S("Code_Main_Updating", "Updating…"));
        PluginDetailText.Text = !snapshot.PluginCurrent && snapshot.PluginInstalled && snapshot.ObsRunning
            ? Loc.S("Code_Main_PluginUpdateWaitsForObs", "OBS is running — the update installs when it closes")
            : snapshot.ObsRunning ? Loc.S("Code_Main_ObsRunning", "OBS is running") : Loc.S("Code_Main_ObsNotRunning", "OBS is not running");

        // A stale copy inside the OBS folder wins source registration over the
        // installed one, so the capture silently runs old code and stays blank.
        var hasConflictingPlugin = !string.IsNullOrWhiteSpace(snapshot.ConflictingPluginPath);
        ConflictingPluginInfoBar.IsOpen = hasConflictingPlugin;
        if (hasConflictingPlugin)
        {
            ConflictingPluginInfoBar.Message = Loc.F(
                "Code_Main_ConflictingPluginMessage",
                "OBS is loading a second, older copy of the capture source from {0}. " +
                "OBS uses whichever copy registers first, so the source can stay blank even though everything here " +
                "reports as current. Removing the old copy leaves the installed source in place.",
                snapshot.ConflictingPluginPath);
        }

        // The automatic OBS source has a native SteamVR fallback. Other legacy
        // VR APIs still cannot be intercepted by an OpenXR API layer.
        var showNonOpenXrVrStatus = !string.IsNullOrWhiteSpace(snapshot.NonOpenXrVrApp) &&
                                    _lastPreviewResult?.Connected != true;
        var isOpenVrCapture = snapshot.NonOpenXrVrPath.Equals(
            "OpenVR/SteamVR", StringComparison.OrdinalIgnoreCase);
        NonOpenXrVrInfoBar.IsOpen = showNonOpenXrVrStatus;
        if (showNonOpenXrVrStatus && isOpenVrCapture)
        {
            NonOpenXrVrInfoBar.Severity = InfoBarSeverity.Success;
            NonOpenXrVrInfoBar.Title = Loc.S("Code_Main_SteamVrCaptureAvailable", "SteamVR capture is available");
            NonOpenXrVrInfoBar.Message = Loc.S(
                "Code_Main_SteamVrCaptureMessage",
                "The automatic OBS source will use SteamVR's native compositor mirror. " +
                "OpenXR layer controls do not apply to this capture.");
        }
        else if (showNonOpenXrVrStatus)
        {
            NonOpenXrVrInfoBar.Severity = InfoBarSeverity.Warning;
            NonOpenXrVrInfoBar.Title = Loc.S("Code_Main_VrApiNotCapturable", "This VR API is not capturable yet");
            NonOpenXrVrInfoBar.Message = Loc.F(
                "Code_Main_VrApiNotCapturableMessage",
                "{0} is running VR through the {1} path, not OpenXR. " +
                "Switch the application to OpenXR or OpenVR/SteamVR, then start it again.",
                snapshot.NonOpenXrVrApp, snapshot.NonOpenXrVrPath);
        }

        var runtimeConfigured = !snapshot.RuntimeName.Equals("Not configured", StringComparison.OrdinalIgnoreCase);
        var runtimeIsSimulator = snapshot.SimulatorRuntimeOverrideActive ||
                                 snapshot.RuntimeName.Contains("Simulator", StringComparison.OrdinalIgnoreCase) ||
                                 snapshot.RuntimePath.Contains("simulator", StringComparison.OrdinalIgnoreCase);
        var runtimeOkay = runtimeConfigured && !runtimeIsSimulator;
        SetStatus(RuntimeDot, RuntimeStatusText, runtimeOkay,
            runtimeIsSimulator ? Loc.S("Code_Main_SimulatorSelected", "Simulator selected") : DisplayRuntimeLabel(snapshot.RuntimeName));
        RuntimeDetailText.Text = runtimeIsSimulator
            ? snapshot.SimulatorRuntimeOverrideActive
                ? Loc.F("Code_Main_RestoreRuntimeForHeadset", "Restore {0} for a headset", DisplayRuntimeLabel(snapshot.SystemRuntimeName))
                : Loc.S("Code_Main_SelectHeadsetRuntime", "Select your headset runtime in its desktop software")
            : DisplayRuntimeLabel(snapshot.RuntimeSource);

        if (runtimeIsSimulator)
        {
            RuntimeModeInfoBar.Severity = InfoBarSeverity.Warning;
            RuntimeModeInfoBar.Title = snapshot.SimulatorRuntimeOverrideActive
                ? Loc.S("Code_Main_SimulatorOverrideActive", "Simulator override is active")
                : Loc.S("Code_Main_SimulatorIsSystemRuntime", "Simulator is the system OpenXR runtime");
            RuntimeModeInfoBar.Message = snapshot.SimulatorRuntimeOverrideActive
                ? Loc.F("Code_Main_SimulatorOverrideMessage", "OpenXR applications will bypass the normal headset runtime. Use headset runtime restores {0} and clears the per-user override.", DisplayRuntimeLabel(snapshot.SystemRuntimeName))
                : Loc.S("Code_Main_SimulatorNotSelectedByApp", "The app did not select this runtime. Choose 'Set as active OpenXR runtime' in your headset or SteamVR software before starting an OpenXR application.");
        }
        else if (!runtimeConfigured)
        {
            RuntimeModeInfoBar.Severity = InfoBarSeverity.Error;
            RuntimeModeInfoBar.Title = Loc.S("Code_Main_NoRuntimeConfigured", "No OpenXR runtime is configured");
            RuntimeModeInfoBar.Message = Loc.S("Code_Main_NoRuntimeConfiguredMessage", "Set your headset software as the active OpenXR runtime, then refresh this page.");
        }
        else
        {
            RuntimeModeInfoBar.Severity = InfoBarSeverity.Success;
            RuntimeModeInfoBar.Title = Loc.S("Code_Main_HeadsetRuntimeSelected", "Headset runtime selected");
            RuntimeModeInfoBar.Message = Loc.F("Code_Main_HeadsetRuntimeSelectedMessage", "OpenXR applications will use {0}. Simulator testing is optional and isolated under Installation.", snapshot.RuntimeName);
        }

        SetStatus(OverscanDot, OverscanStatusText, snapshot.OverscanEnabled,
            snapshot.OverscanEnabled
                ? Loc.S("Ui_Install_RegistrationState.OnContent", "Enabled")
                : Loc.S("Ui_Install_RegistrationState.OffContent", "Disabled"),
            useWarningWhenFalse: false);
        OverscanDetailText.Text = snapshot.OverscanEnabled
            ? $"{snapshot.HorizontalPercent}% × {snapshot.VerticalPercent}%"
            : Loc.S("Code_Main_HeadsetNativeFov", "Headset-native FOV");

        LaunchMetaButton.IsEnabled = !string.IsNullOrWhiteSpace(snapshot.MetaXrExecutable);

        RenderLayerRegistrationControls(snapshot);

        _loadingControls = true;
        OverscanToggle.IsOn = snapshot.OverscanEnabled;
        HorizontalSlider.Value = snapshot.HorizontalPercent;
        VerticalSlider.Value = snapshot.VerticalPercent;
        _loadingControls = false;
        UpdateOverscanPreview();

        _loadingSmoothingControls = true;
        SmoothingManagedToggle.IsOn = snapshot.CameraSmoothingManaged;
        SmoothingSlider.Value = snapshot.CameraSmoothing;
        SmoothingCropSlider.Value = snapshot.SmoothingCrop;
        _loadingSmoothingControls = false;
        UpdateSmoothingPreview();

        _loadingQuadLayerControls = true;
        MirrorQuadLayersToggle.IsOn = snapshot.MirrorQuadLayers;
        _loadingQuadLayerControls = false;
        UpdateMirrorQuadLayersPreview();

        if (!snapshot.PluginInstalled)
        {
            SmoothingAvailabilityInfoBar.Severity = InfoBarSeverity.Warning;
            SmoothingAvailabilityInfoBar.Title = Loc.S("Code_Main_InstallObsSource", "Install the OBS source");
            SmoothingAvailabilityInfoBar.Message = Loc.S("Code_Main_ObsSourceSaveNowNotEffective", "The values can be saved now, but the OBS source must be installed before they can take effect.");
        }
        else if (!snapshot.PluginCurrent)
        {
            SmoothingAvailabilityInfoBar.Severity = InfoBarSeverity.Warning;
            SmoothingAvailabilityInfoBar.Title = Loc.S("Code_Main_SourceUpdateRequired", "Source update required");
            SmoothingAvailabilityInfoBar.Message = Loc.S("Code_Main_ObsSourceUpdateNote", "The values can be saved now. Install the available source update and restart OBS to enable live control.");
        }
        else
        {
            SmoothingAvailabilityInfoBar.Severity = InfoBarSeverity.Informational;
            SmoothingAvailabilityInfoBar.Title = Loc.S("Ui_Smoothing_Availability.Title", "Applies live");
            SmoothingAvailabilityInfoBar.Message = Loc.S("Ui_Smoothing_Availability.Message", "The installed OBS source polls these settings four times per second. Saved values are used on the next session too.");
        }

        if (!snapshot.LayerFilesInstalled)
        {
            QuadLayersAvailabilityInfoBar.Severity = InfoBarSeverity.Warning;
            QuadLayersAvailabilityInfoBar.Title = Loc.S("Code_Main_InstallOpenXrLayer", "Install the OpenXR layer");
            QuadLayersAvailabilityInfoBar.Message = Loc.S("Code_Main_LayerSaveNowNotEffective", "The preference can be saved now, but the updated layer must be installed before it can filter the recording.");
        }
        else if (!snapshot.LayerCurrent)
        {
            QuadLayersAvailabilityInfoBar.Severity = InfoBarSeverity.Warning;
            QuadLayersAvailabilityInfoBar.Title = Loc.S("Code_Main_LayerUpdateRequired", "Layer update required");
            QuadLayersAvailabilityInfoBar.Message = Loc.S("Code_Main_LayerUpdateNote", "Save the preference now, then install the available layer update and restart the VR application once.");
        }
        else
        {
            QuadLayersAvailabilityInfoBar.Severity = InfoBarSeverity.Informational;
            QuadLayersAvailabilityInfoBar.Title = Loc.S("Ui_QuadLayers_Availability.Title", "Applies live");
            QuadLayersAvailabilityInfoBar.Message = Loc.S("Ui_QuadLayers_Availability.Message", "The updated OpenXR layer polls this setting while recording. Restart the VR application once after installing the update.");
        }

        InstallLayerStatusText.Text = snapshot.LayerFilesInstalled
            ? !snapshot.LayerRegistered ? Loc.S("Code_Main_LayerInstalledDisabled", "Installed, disabled") : snapshot.LayerCurrent ? Loc.S("Code_Main_LayerInstalledEnabled", "Installed and enabled") : Loc.S("Code_Main_InstalledUpdatePending", "Installed, update pending (automatic)")
            : Loc.S("Code_Main_NotInstalled", "Not installed");
        InstallLayerPathText.Text = snapshot.LayerManifestPath;
        InstallPluginStatusText.Text = snapshot.PluginInstalled
            ? snapshot.PluginCurrent ? Loc.S("Code_Main_PluginInstalledCurrent", "Installed and current") : snapshot.ObsRunning ? Loc.S("Code_Main_PluginUpdateWaitingForObs", "Installed, update waiting for OBS to close") : Loc.S("Code_Main_InstalledUpdatePending", "Installed, update pending (automatic)")
            : Loc.S("Code_Main_NotInstalled", "Not installed");
        InstallPluginPathText.Text = _service.PluginPath;

        DiagnosticRuntimeNameText.Text = DisplayRuntimeLabel(snapshot.RuntimeName);
        DiagnosticRuntimePathText.Text = $"{DisplayRuntimeLabel(snapshot.RuntimeSource)}\n{snapshot.RuntimePath}\n" +
            Loc.F("Code_Main_SystemDefault", "System default: {0} — {1}", DisplayRuntimeLabel(snapshot.SystemRuntimeName), snapshot.SystemRuntimePath);
        DiagnosticLayerHashText.Text = Loc.F("Code_Main_DiagnosticLayerHash", "Layer   {0}", DisplayHash(snapshot.LayerHash));
        DiagnosticPluginHashText.Text = Loc.F("Code_Main_DiagnosticPluginHash", "Plugin  {0}", DisplayHash(snapshot.PluginHash));
    }

    private void SidebarNav_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: string tag } selectedButton)
            return;

        DashboardPage.Visibility = tag == "dashboard" ? Visibility.Visible : Visibility.Collapsed;
        OverscanPage.Visibility = tag == "overscan" ? Visibility.Visible : Visibility.Collapsed;
        SmoothingPage.Visibility = tag == "smoothing" ? Visibility.Visible : Visibility.Collapsed;
        QuadLayersPage.Visibility = tag == "quadlayers" ? Visibility.Visible : Visibility.Collapsed;
        InstallationPage.Visibility = tag == "installation" ? Visibility.Visible : Visibility.Collapsed;
        DiagnosticsPage.Visibility = tag == "diagnostics" ? Visibility.Visible : Visibility.Collapsed;

        foreach (var button in new[] { DashboardNavButton, OverscanNavButton, SmoothingNavButton, QuadLayersNavButton, InstallationNavButton, DiagnosticsNavButton })
        {
            button.Background = new SolidColorBrush(Colors.Transparent);
            button.Foreground = GetBrush("MutedTextBrush");
        }
        selectedButton.Background = new SolidColorBrush(ColorHelper.FromArgb(38, 58, 142, 150));
        selectedButton.Foreground = new SolidColorBrush(Colors.White);

        if (tag == "dashboard" || _previewWindow is not null || _vrRestartReasons.Count > 0)
            StartMirrorPreview();
        else
            StopMirrorPreview();

        if (tag == "diagnostics")
            RefreshLogView();
    }

    private async void Refresh_Click(object sender, RoutedEventArgs e) => await RefreshSnapshotAsync();

    private void OverscanToggle_Toggled(object sender, RoutedEventArgs e)
    {
        if (_loadingControls)
            return;
        UpdateOverscanPreview();
        QueueOverscanSave();
    }

    private void OverscanSlider_ValueChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        if (_loadingControls || HorizontalValueText is null)
            return;
        UpdateOverscanPreview();
        QueueOverscanSave();
    }

    private void QueueOverscanSave()
    {
        _overscanSaveTimer.Stop();
        _overscanSaveTimer.Start();
    }

    private async Task SaveOverscanAsync()
    {
        try
        {
            var horizontal = (int)Math.Round(HorizontalSlider.Value);
            var vertical = (int)Math.Round(VerticalSlider.Value);
            var enabled = OverscanToggle.IsOn;
            if (_snapshot is { } snapshot &&
                snapshot.OverscanEnabled == enabled &&
                snapshot.HorizontalPercent == horizontal &&
                snapshot.VerticalPercent == vertical)
                return;

            _service.ApplyOverscan(enabled, horizontal, vertical);
            // A running application already built its swapchains from the old
            // values, so this is the one thing the user still has to act on.
            MarkVrRestartRequired(Loc.S("Code_Main_ReasonOverscanChange", "the overscan change"));
            await RefreshSnapshotAsync();
        }
        catch (Exception ex)
        {
            ShowMessage(Loc.S("Code_Main_CouldNotSaveOverscan", "Could not save overscan"), ex.Message, InfoBarSeverity.Error);
        }
    }

    private void UpdateOverscanPreview()
    {
        var horizontal = (int)Math.Round(HorizontalSlider.Value);
        var vertical = (int)Math.Round(VerticalSlider.Value);
        HorizontalValueText.Text = $"{horizontal}%";
        VerticalValueText.Text = $"{vertical}%";
        var pixelCost = horizontal / 100.0 * (vertical / 100.0) - 1.0;
        PixelCostText.Text = $"+{pixelCost * 100:0.0}%";
        ScaleSummaryText.Text = Loc.F("Code_Main_ScaleSummary", "{0:0.00}× horizontal  •  {1:0.00}× vertical", horizontal / 100.0, vertical / 100.0);

        // The per-eye size the runtime recommends is what overscan multiplies,
        // so its shape - not the slider percentages - decides how wide the
        // recording ends up. Report real pixels once a session has taught us
        // that size, and fall back to the raw scales before then.
        if (_snapshot is { BaseViewWidth: > 0, BaseViewHeight: > 0 } snapshot)
        {
            var width = (int)Math.Round(snapshot.BaseViewWidth * (horizontal / 100.0));
            var height = (int)Math.Round(snapshot.BaseViewHeight * (vertical / 100.0));
            ExpectedTextureText.Text = $"{width} × {height}";
            FrameShapeText.Text = Loc.F(
                "Code_Main_FrameShapeKnown",
                "{0:0.00} : 1 per eye, from the runtime's {1} × {2} ({3:0.00} : 1). " +
                "The application's own render scale multiplies both axes, so the shape holds.",
                (double)width / height,
                snapshot.BaseViewWidth,
                snapshot.BaseViewHeight,
                (double)snapshot.BaseViewWidth / snapshot.BaseViewHeight);
        }
        else
        {
            ExpectedTextureText.Text = $"{horizontal / 100.0:0.00}×  ×  {vertical / 100.0:0.00}×";
            FrameShapeText.Text = Loc.S(
                "Code_Main_FrameShapeUnknown",
                "relative to the runtime's recommended per-eye size. Run a VR application once and this will " +
                "show the recording's real pixel size and shape.");
        }
    }

    // Horizontal and vertical expansion are what decide the recording's shape,
    // but only relative to a per-eye render that is already nearly square.
    // Solve for the percentages that reach a requested frame shape instead of
    // leaving the user to work the ratio out from two independent sliders.
    private void Shape_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: string tag })
            return;

        const int maxPercent = 200;
        var baseAspect = _snapshot is { BaseViewWidth: > 0, BaseViewHeight: > 0 } snapshot
            ? (double)snapshot.BaseViewWidth / snapshot.BaseViewHeight
            : 1.0;

        int horizontal;
        int vertical;
        if (tag == "native")
        {
            horizontal = 100;
            vertical = 100;
        }
        else if (tag == "widest")
        {
            horizontal = maxPercent;
            vertical = 100;
        }
        else if (double.TryParse(tag, NumberStyles.Float, CultureInfo.InvariantCulture, out var target) && target > 0)
        {
            // Grow only the axis that needs it, so the pixel cost is the
            // smallest one that reaches the requested shape.
            var ratio = target / baseAspect;
            if (ratio >= 1.0)
            {
                vertical = 100;
                horizontal = Math.Clamp((int)Math.Round(100 * ratio), 100, maxPercent);
            }
            else
            {
                horizontal = 100;
                vertical = Math.Clamp((int)Math.Round(100 / ratio), 100, maxPercent);
            }
        }
        else
        {
            return;
        }

        HorizontalSlider.Value = horizontal;
        VerticalSlider.Value = vertical;
        OverscanToggle.IsOn = horizontal > 100 || vertical > 100;
        UpdateOverscanPreview();

        var reached = baseAspect * horizontal / vertical;
        if (tag != "native" && tag != "widest" &&
            double.TryParse(tag, NumberStyles.Float, CultureInfo.InvariantCulture, out var wanted) &&
            Math.Abs(reached - wanted) > 0.02)
        {
            ShowMessage(
                Loc.S("Code_Main_WidestShapeTitle", "As wide as this build allows"),
                Loc.F(
                    "Code_Main_WidestShapeLimited",
                    "A {0:0.00} : 1 frame needs more than {1}% horizontal expansion from this headset's " +
                    "{2:0.00} : 1 per-eye render. The sliders are set to the widest reachable shape, " +
                    "{3:0.00} : 1.",
                    wanted, maxPercent, baseAspect, reached),
                InfoBarSeverity.Informational);
        }
    }

    private void AppWindow_Changed(AppWindow sender, AppWindowChangedEventArgs args)
    {
        // The capture loop has no audience while the window is hidden or
        // minimized, and pausing it also lets the layer idle when OBS is away.
        if (!args.DidVisibilityChange)
            return;
        if (!sender.IsVisible)
            StopMirrorPreview();
        else
            StartMirrorPreview();
    }

    private void StartMirrorPreview()
    {
        if (DashboardPage.Visibility != Visibility.Visible && _previewWindow is null && _vrRestartReasons.Count == 0)
            return;
        if (_previewLoopCts is { IsCancellationRequested: false })
            return;
        _previewFramesSinceSample = 0;
        _previewFps = 0;
        _previewFrameClock.Restart();
        _previewLoopCts = new CancellationTokenSource();
        _ = Task.Run(() => RunMirrorPreviewLoopAsync(_previewLoopCts.Token));
    }

    private void StopMirrorPreview()
    {
        _previewLoopCts?.Cancel();
        _previewLoopCts = null;
    }

    // Captures continuously on a worker thread and posts finished frames to
    // the UI. A DispatcherTimer cannot drive this at 60 fps: its ticks
    // quantize to the 15.625 ms system timer, so a 16 ms interval fires every
    // 31.25 ms and caps the preview at exactly 32 fps.
    private async Task RunMirrorPreviewLoopAsync(CancellationToken token)
    {
        var highResolutionSleep = false;
        var pacer = new Stopwatch();
        try
        {
            while (!token.IsCancellationRequested)
            {
                pacer.Restart();
                var result = _previewService.CaptureFrame();
                DispatcherQueue.TryEnqueue(() => RenderMirrorPreview(result));

                // Full rate while frames flow. A mapped-but-idle surface still
                // needs a fast poll: the layer only publishes textures while
                // it sees the consumer heartbeat advancing every few of its
                // own frames. Only a missing surface allows a slow reconnect
                // poll.
                var interval = result.IsLive
                    ? TimeSpan.FromMilliseconds(1000.0 / 60.0)
                    : result.Connected
                        ? TimeSpan.FromMilliseconds(50)
                        : TimeSpan.FromMilliseconds(250);

                // Task.Delay is quantized to the same 15.625 ms as the old
                // timer; request 1 ms resolution only while live pacing needs it.
                var wantHighResolution = result.IsLive;
                if (wantHighResolution != highResolutionSleep)
                {
                    _ = wantHighResolution ? TimeBeginPeriod(1) : TimeEndPeriod(1);
                    highResolutionSleep = wantHighResolution;
                }

                var wait = interval - pacer.Elapsed;
                if (wait > TimeSpan.Zero)
                    await Task.Delay(wait, token);
            }
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception ex)
        {
            App.LogStartup("Mirror preview loop failed", ex);
        }
        finally
        {
            if (highResolutionSleep)
                _ = TimeEndPeriod(1);
        }
    }

    private void RefreshPreview_Click(object sender, RoutedEventArgs e) =>
        StartMirrorPreview();

    private void OpenPreviewWindow_Click(object sender, RoutedEventArgs e)
    {
        if (_previewWindow is null)
        {
            _previewWindow = new MirrorPreviewWindow();
            _previewWindow.Closed += (_, _) =>
            {
                _previewWindow = null;
                if (DashboardPage.Visibility != Visibility.Visible && _vrRestartReasons.Count == 0)
                    StopMirrorPreview();
            };
            _previewWindow.Activate();
        }
        else
        {
            _previewWindow.Activate();
        }

        if (_lastPreviewResult is not null)
            _previewWindow.RenderPreview(_lastPreviewResult, _previewFps);
        StartMirrorPreview();
    }

    private void RenderMirrorPreview(MirrorPreviewResult result)
    {
        _lastPreviewResult = result;
        UpdateVrRestartIndicator();
        PreviewStatusText.Text = result.Status;
        PreviewStatusText.Foreground = GetBrush(result.IsLive ? "GoodBrush" : "MutedTextBrush");
        PreviewDetailText.Text = result.Detail;

        if (result.Frame is not { } frame)
        {
            if (_previewBitmap is null)
            {
                PreviewPlaceholder.Visibility = Visibility.Visible;
                PreviewPlaceholderTitle.Text = result.Status;
                PreviewPlaceholderDetail.Text = result.Detail;
            }
            _previewWindow?.RenderPreview(result, _previewFps);
            return;
        }

        if (_previewBitmap is null ||
            _previewBitmap.PixelWidth != frame.Width ||
            _previewBitmap.PixelHeight != frame.Height)
        {
            _previewBitmap = new WriteableBitmap(frame.Width, frame.Height);
            MirrorPreviewImage.Source = _previewBitmap;
        }

        using var stream = _previewBitmap.PixelBuffer.AsStream();
        stream.Position = 0;
        stream.Write(frame.Pixels, 0, frame.Pixels.Length);
        _previewBitmap.Invalidate();
        PreviewPlaceholder.Visibility = Visibility.Collapsed;

        _previewFramesSinceSample++;
        if (_previewFrameClock.Elapsed.TotalSeconds >= 1.0)
        {
            _previewFps = _previewFramesSinceSample / _previewFrameClock.Elapsed.TotalSeconds;
            _previewFramesSinceSample = 0;
            _previewFrameClock.Restart();
        }
        if (_previewFps > 0)
            PreviewDetailText.Text = $"{result.Detail}  •  {_previewFps:0} FPS";

        _previewWindow?.RenderPreview(result, _previewFps);
    }

    private bool MarkVrRestartRequired(string reason)
    {
        var producer = _previewService.GetProducerIdentity();
        if (!producer.Connected)
            return false;

        if (_vrRestartReasons.Count > 0 &&
            _vrRestartProducerPid != 0 &&
            producer.ProcessId != 0 &&
            producer.ProcessId != _vrRestartProducerPid)
        {
            _vrRestartReasons.Clear();
        }

        _vrRestartProducerPid = producer.ProcessId;
        _vrRestartProducerApp = producer.ApplicationName;
        _vrRestartReasons.Add(reason);
        UpdateVrRestartIndicator();
        StartMirrorPreview();
        return true;
    }

    private void UpdateVrRestartIndicator()
    {
        if (_vrRestartReasons.Count == 0)
        {
            VrRestartPanel.Visibility = Visibility.Collapsed;
            return;
        }

        var producer = _previewService.GetProducerIdentity();
        var producerChanged = _vrRestartProducerPid != 0 &&
                              producer.ProcessId != 0 &&
                              producer.ProcessId != _vrRestartProducerPid;
        if (!producer.Connected || producerChanged)
        {
            _vrRestartReasons.Clear();
            _vrRestartProducerPid = 0;
            _vrRestartProducerApp = string.Empty;
            VrRestartPanel.Visibility = Visibility.Collapsed;
            if (DashboardPage.Visibility != Visibility.Visible && _previewWindow is null)
                StopMirrorPreview();
            return;
        }

        var application = !string.IsNullOrWhiteSpace(producer.ApplicationName)
            ? producer.ApplicationName
            : !string.IsNullOrWhiteSpace(_vrRestartProducerApp)
                ? _vrRestartProducerApp
                : Loc.S("Code_Main_RunningVrApp", "the running VR app");
        VrRestartTitleText.Text = Loc.S("Ui_VrRestart_Title.Text", "RESTART VR APP");
        VrRestartReasonText.Text = _vrRestartReasons.Count == 1
            ? Loc.F("Code_Main_RestartToApply", "Restart {0} to apply {1}.", application, _vrRestartReasons.Single())
            : Loc.F("Code_Main_RestartToApplyMultiple", "Restart {0} to apply these changes: {1}.", application, string.Join("; ", _vrRestartReasons));
        VrRestartPanel.Visibility = Visibility.Visible;
    }

    private void SmoothingControl_Changed(object sender, RoutedEventArgs e)
    {
        if (_loadingSmoothingControls || SmoothingValueText is null)
            return;
        UpdateSmoothingPreview();
        QueueSmoothingSave();
    }

    private void SmoothingSlider_ValueChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        if (_loadingSmoothingControls || SmoothingValueText is null || SmoothingCropValueText is null)
            return;
        UpdateSmoothingPreview();
        QueueSmoothingSave();
    }

    private void QueueSmoothingSave()
    {
        _smoothingSaveTimer.Stop();
        _smoothingSaveTimer.Start();
    }

    private async Task SaveSmoothingAsync()
    {
        try
        {
            var managed = SmoothingManagedToggle.IsOn;
            var smoothing = (int)Math.Round(SmoothingSlider.Value);
            var crop = SmoothingCropSlider.Value;
            if (_snapshot is { } snapshot &&
                snapshot.CameraSmoothingManaged == managed &&
                snapshot.CameraSmoothing == smoothing &&
                Math.Abs(snapshot.SmoothingCrop - crop) < 0.01)
                return;

            _service.ApplyCameraSmoothing(managed, smoothing, crop);
            await RefreshSnapshotAsync();
        }
        catch (Exception ex)
        {
            ShowMessage(Loc.S("Code_Main_CouldNotSaveCameraSmoothing", "Could not save camera smoothing"), ex.Message, InfoBarSeverity.Error);
        }
    }

    private void UpdateSmoothingPreview()
    {
        var smoothing = (int)Math.Round(SmoothingSlider.Value);
        var crop = Math.Round(SmoothingCropSlider.Value * 2.0) / 2.0;
        var strength = smoothing / 100.0;
        var responseMs = 40.0 + strength * strength * 760.0;

        SmoothingValueText.Text = smoothing.ToString();
        SmoothingCropValueText.Text = $"{crop:0.0}%";
        SmoothingResponseText.Text = smoothing == 0 || crop == 0 ? Loc.S("Code_Main_Off", "Off") : $"{responseMs:0} ms";
        SmoothingVisibleText.Text = $"{100.0 - crop:0.0}%";
    }

    private void MirrorQuadLayers_Toggled(object sender, RoutedEventArgs e)
    {
        if (!_loadingQuadLayerControls && MirrorQuadLayersSummaryText is not null)
            UpdateMirrorQuadLayersPreview();
    }

    private void UpdateMirrorQuadLayersPreview()
    {
        MirrorQuadLayersSummaryText.Text = MirrorQuadLayersToggle.IsOn
            ? Loc.S("Code_Main_ProjectionQuadLayerUi", "Projection + quad-layer UI")
            : Loc.S("Code_Main_ProjectionOnly", "Projection only");
    }

    private async void QuadLayerPreset_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: string tag } || !bool.TryParse(tag, out var visible))
            return;

        MirrorQuadLayersToggle.IsOn = visible;
        await SaveMirrorQuadLayersAsync();
    }

    private async void ApplyMirrorQuadLayers_Click(object sender, RoutedEventArgs e) =>
        await SaveMirrorQuadLayersAsync();

    private async Task SaveMirrorQuadLayersAsync()
    {
        try
        {
            var visible = MirrorQuadLayersToggle.IsOn;
            _service.ApplyMirrorQuadLayers(visible);
            ShowMessage(
                visible ? Loc.S("Code_Main_QuadLayerUiShown", "Quad-layer UI shown") : Loc.S("Code_Main_QuadLayerUiHidden", "Quad-layer UI hidden"),
                Loc.S("Code_Main_QuadLayerUiSavedNote", "The OBS mirror picks up this recording-only setting live when the updated layer is active. The headset remains unchanged."),
                InfoBarSeverity.Success);
            await RefreshSnapshotAsync();
        }
        catch (Exception ex)
        {
            ShowMessage(Loc.S("Code_Main_CouldNotSaveUiLayerSetting", "Could not save the UI-layer setting"), ex.Message, InfoBarSeverity.Error);
        }
    }

    private void SmoothingPreset_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: string tag })
            return;
        var parts = tag.Split(',');
        if (parts.Length != 2 || !double.TryParse(parts[0], out var smoothing) || !double.TryParse(parts[1], out var crop))
            return;

        SmoothingSlider.Value = smoothing;
        SmoothingCropSlider.Value = crop;
        SmoothingManagedToggle.IsOn = true;
        UpdateSmoothingPreview();
    }

    private void Preset_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: string tag })
            return;
        var parts = tag.Split(',');
        if (parts.Length != 2 || !double.TryParse(parts[0], out var horizontal) || !double.TryParse(parts[1], out var vertical))
            return;
        HorizontalSlider.Value = horizontal;
        VerticalSlider.Value = vertical;
        OverscanToggle.IsOn = true;
        UpdateOverscanPreview();
    }

    // Installed components follow the built release artifacts automatically;
    // only the very first install remains an explicit action. Each new build
    // (hash pair) is attempted once per session, so a failed or cancelled
    // update never nags - the Installation page stays the manual retry path.
    private async Task TryAutoUpdateAsync()
    {
        if (_autoUpdateInProgress || _snapshot is not { } snapshot)
            return;

        var layerOutdated = snapshot.LayerFilesInstalled && !snapshot.LayerCurrent;
        var pluginOutdated = snapshot.PluginInstalled && !snapshot.PluginCurrent;
        if (!layerOutdated && !pluginOutdated)
            return;

        // "Different from my payload" is not the same as "older than my
        // payload". Without this guard an older Control Center reinstalls its
        // own bundled layer over newer components every time it refreshes,
        // silently undoing an update (or a developer build).
        var installedVersion = _service.InstalledComponentsVersion;
        if (!string.IsNullOrWhiteSpace(installedVersion) &&
            AppUpdateService.CompareVersions(installedVersion, AppUpdateService.CurrentVersion) > 0)
        {
            if (_lastAutoUpdateKey != installedVersion)
            {
                _lastAutoUpdateKey = installedVersion;
                ShowMessage(
                    Loc.S("Code_Main_NewerComponentsInstalled", "Newer components are installed"),
                    Loc.F(
                        "Code_Main_NewerComponentsMessage",
                        "The installed layer and OBS source come from {0}, which is newer than this " +
                        "Control Center ({1}). They were left alone; update the app to match.",
                        installedVersion, AppUpdateService.CurrentVersion),
                    InfoBarSeverity.Informational);
            }
            return;
        }

        // The plugin DLL cannot be replaced while OBS holds it; the layer
        // still updates now and the plugin follows once OBS has closed. A
        // watcher finishes the job so the user never has to press Refresh.
        var pluginDeferred = pluginOutdated && snapshot.ObsRunning;
        if (pluginDeferred)
            _obsCloseWatchTimer.Start();
        else
            _obsCloseWatchTimer.Stop();
        var key = $"{snapshot.SourceLayerHash}|{snapshot.SourcePluginHash}|{pluginDeferred}";
        if (key == _lastAutoUpdateKey)
            return;
        _lastAutoUpdateKey = key;
        _autoUpdateFailed = false;

        _autoUpdateInProgress = true;
        SetBusy(true);
        try
        {
            var output = string.Empty;
            if (layerOutdated)
                output = await _service.InstallLayerOnlyAsync();
            if (pluginOutdated && !pluginDeferred)
                output = $"{output} {await ElevatedInstallService.InstallPluginElevatedAsync()}".Trim();

            // Record what the installed components came from, so an older
            // build can recognise them as newer and leave them alone.
            if (!pluginDeferred)
                _service.RecordInstalledComponentsVersion(AppUpdateService.CurrentVersion);

            var restartRequired = layerOutdated && MarkVrRestartRequired(Loc.S("Code_Main_ReasonLayerUpdate", "the OpenXR layer update"));
            var followUp =
                (restartRequired ? " " + Loc.S("Code_Main_RestartVrForNewLayer", "Restart the running VR application to load the new layer build.") : string.Empty) +
                (pluginDeferred ? " " + Loc.S("Code_Main_ObsSourceUpdateOnClose", "The OBS source update installs automatically once OBS is closed.") : string.Empty) +
                (pluginOutdated && !pluginDeferred ? " " + Loc.S("Code_Main_RestartObsForSource", "Restart OBS to load the updated source.") : string.Empty);
            ShowMessage(
                pluginDeferred && !layerOutdated ? Loc.S("Code_Main_ObsSourceUpdateWaiting", "OBS source update waiting") : Loc.S("Code_Main_UpdatedAutomatically", "Updated automatically"),
                (string.IsNullOrWhiteSpace(output) ? Loc.S("Code_Main_NewBuildDetected", "A new build was detected.") : LastLine(output)) + followUp,
                InfoBarSeverity.Success);
        }
        catch (Exception ex)
        {
            _autoUpdateFailed = true;
            ShowMessage(
                Loc.S("Code_Main_AutomaticUpdateFailed", "Automatic update failed"),
                Loc.F("Code_Main_AutomaticUpdateFailedBody", "{0} Use Installation > Install / update to retry.", ex.Message),
                InfoBarSeverity.Warning);
        }
        finally
        {
            SetBusy(false);
            _autoUpdateInProgress = false;
        }
        await RefreshSnapshotAsync();
    }

    // Runs only while an OBS source update is waiting for OBS to close. The
    // probe is a process-name lookup, so polling it every few seconds is far
    // cheaper than taking a full snapshot.
    private async void ObsCloseWatchTimer_Tick(object? sender, object e)
    {
        if (_autoUpdateInProgress)
            return;
        if (await Task.Run(() => Process.GetProcessesByName("obs64").Length > 0))
            return;

        _obsCloseWatchTimer.Stop();
        // Refreshing re-runs the auto-update pass, which now sees OBS closed
        // and installs the pending source update.
        await RefreshSnapshotAsync();
    }

    private async void InstallUpdate_Click(object sender, RoutedEventArgs e)
    {
        await RunActionAsync(Loc.S("Code_Main_ActionInstallingObsmirror", "Installing OBSMirror"), async () =>
        {
            var snapshot = _service.GetSnapshot();
            var layerWillChange = !snapshot.LayerFilesInstalled || !snapshot.LayerCurrent;
            if (snapshot.ObsRunning && !snapshot.PluginCurrent)
                throw new InvalidOperationException("OBS is running and the plugin binary has changed. Stop recording and close OBS before updating the plugin.");
            var output = snapshot.PluginCurrent
                ? await _service.SetupAsync(snapshot.ObsRunning)
                : $"{await _service.InstallLayerOnlyAsync()} {await ElevatedInstallService.InstallPluginElevatedAsync()}";
            _service.RecordInstalledComponentsVersion(AppUpdateService.CurrentVersion);
            var restartRequired = layerWillChange && MarkVrRestartRequired(Loc.S("Code_Main_ReasonLayerUpdate", "the OpenXR layer update"));
            ShowMessage(
                Loc.S("Code_Main_InstallationComplete", "Installation complete"),
                LastLine(output) + (restartRequired ? " " + Loc.S("Code_Main_RestartVrForNewLayer", "Restart the running VR application to load the new layer build.") : string.Empty),
                InfoBarSeverity.Success);
        });
    }

    private async void LayerRegistrationToggle_Toggled(object sender, RoutedEventArgs e)
    {
        if (_loadingLayerRegistrationControls || sender is not ToggleSwitch toggle)
            return;

        var enable = toggle.IsOn;
        await RunActionAsync(enable
            ? Loc.S("Code_Main_ActionEnablingLayer", "Enabling the OpenXR layer")
            : Loc.S("Code_Main_ActionDisablingLayer", "Disabling the OpenXR layer"), async () =>
        {
            if (enable)
            {
                var output = await _service.RegisterLayerAsync();
                var restartRequired = MarkVrRestartRequired(Loc.S("Code_Main_ReasonLayerRegistrationChange", "the OpenXR layer registration change"));
                ShowMessage(
                    Loc.S("Code_Main_LayerEnabled", "OpenXR layer enabled"),
                    LastLine(output) + (restartRequired ? " " + Loc.S("Code_Main_RestartVrToLoadLayer", "Restart the running VR application to load the layer.") : string.Empty),
                    InfoBarSeverity.Success);
            }
            else
            {
                var output = await _service.UnregisterLayerAsync();
                var restartRequired = MarkVrRestartRequired(Loc.S("Code_Main_ReasonLayerRegistrationChange", "the OpenXR layer registration change"));
                ShowMessage(
                    Loc.S("Code_Main_LayerDisabled", "OpenXR layer disabled"),
                    LastLine(output) + " " + Loc.S("Code_Main_FilesLeftInPlace", "Installed files and the OBS source were left in place.") +
                    (restartRequired ? " " + Loc.S("Code_Main_RestartVrToUnloadLayer", "Restart the running VR application to unload the layer.") : string.Empty),
                    InfoBarSeverity.Success);
            }
        });

        // RunActionAsync refreshes successful changes. If an action failed,
        // restore both switches from the last confirmed snapshot.
        if (_snapshot is not null && _snapshot.LayerRegistered != enable)
            RenderLayerRegistrationControls(_snapshot);
    }

    private void RenderLayerRegistrationControls(SystemSnapshot snapshot)
    {
        _loadingLayerRegistrationControls = true;
        try
        {
            DashboardLayerRegistrationToggle.IsOn = snapshot.LayerRegistered;
            InstallationLayerRegistrationToggle.IsOn = snapshot.LayerRegistered;

            // An existing registration can always be disabled. Enabling needs
            // the installed manifest and layer binary to be present.
            var canChangeRegistration = snapshot.LayerRegistered || snapshot.LayerFilesInstalled;
            DashboardLayerRegistrationToggle.IsEnabled = canChangeRegistration;
            InstallationLayerRegistrationToggle.IsEnabled = canChangeRegistration;
        }
        finally
        {
            _loadingLayerRegistrationControls = false;
        }
    }

    private async void RemoveConflictingPlugin_Click(object sender, RoutedEventArgs e)
    {
        await RunActionAsync(Loc.S("Code_Main_ActionRemovingOldSource", "Removing the old OBS source copy"), async () =>
        {
            if (_snapshot?.ObsRunning == true)
                throw new InvalidOperationException("Close OBS first; it is holding the old plugin file open.");

            // The OBS folder is normally under Program Files, so removal needs
            // the same elevation path the plugin installer uses.
            var message = await ElevatedInstallService.RemoveConflictingPluginElevatedAsync();
            ShowMessage(
                Loc.S("Code_Main_OldSourceCopyRemoved", "Old OBS source copy removed"),
                Loc.F("Code_Main_OldSourceCopyRemovedBody", "{0} Start OBS again; the installed source will now be the one that loads.", message),
                InfoBarSeverity.Success);
        });
    }

    private async void LaunchObs_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            if (_snapshot?.ObsRunning == true)
            {
                ShowMessage(
                    Loc.S("Code_Main_ObsAlreadyRunning", "OBS is already running"),
                    Loc.S("Code_Main_ObsAlreadyRunningBody", "The saved OpenXR Mirror Capture source is ready in the current OBS session."),
                    InfoBarSeverity.Informational);
                return;
            }
            _service.LaunchObs();
            ShowMessage(Loc.S("Code_Main_ObsLaunched", "OBS launched"), Loc.S("Code_Main_ObsLaunchedBody", "Refresh status after OBS finishes loading."), InfoBarSeverity.Success);
        }
        catch (OBSMirrorService.ObsNotFoundException ex)
        {
            // Portable and unusual installs register nothing to detect, so let
            // the user point at obs64.exe once instead of dead-ending.
            await BrowseForObsAndLaunchAsync(ex.Message);
        }
        catch (Exception ex)
        {
            ShowMessage(Loc.S("Code_Main_CouldNotLaunchObs", "Could not launch OBS"), ex.Message, InfoBarSeverity.Error);
        }
    }

    private async Task BrowseForObsAndLaunchAsync(string reason)
    {
        if (Content?.XamlRoot is not { } xamlRoot)
            return;

        var dialog = new ContentDialog
        {
            XamlRoot = xamlRoot,
            Title = Loc.S("Code_Main_LocateObsStudio", "Locate OBS Studio"),
            PrimaryButtonText = Loc.S("Code_Main_BrowseForObs64", "Browse for obs64.exe"),
            CloseButtonText = Loc.S("Code_Main_Cancel", "Cancel"),
            DefaultButton = ContentDialogButton.Primary,
            Content = new TextBlock
            {
                Text = $"{reason}\n\n" + Loc.S("Code_Main_ObsExeHint", "obs64.exe is usually in the OBS install folder under bin\\64bit."),
                TextWrapping = TextWrapping.Wrap,
            },
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
            return;

        var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.ComputerFolder };
        picker.FileTypeFilter.Add(".exe");
        // WinUI 3 pickers are window-owned and must be told which window.
        InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(this));

        var file = await picker.PickSingleFileAsync();
        if (file is null)
            return;

        try
        {
            _service.SetObsExecutable(file.Path);
            _service.LaunchObs();
            ShowMessage(
                Loc.S("Code_Main_ObsLaunched", "OBS launched"),
                Loc.F("Code_Main_SavedObsLocation", "Saved this location for future launches: {0}", file.Path),
                InfoBarSeverity.Success);
        }
        catch (Exception ex)
        {
            ShowMessage(Loc.S("Code_Main_CouldNotLaunchObs", "Could not launch OBS"), ex.Message, InfoBarSeverity.Error);
        }
    }

    private async void RestoreHeadsetRuntime_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            var systemRuntimeIsSimulator = _snapshot is not null &&
                (_snapshot.SystemRuntimeName.Contains("Simulator", StringComparison.OrdinalIgnoreCase) ||
                 _snapshot.SystemRuntimePath.Contains("simulator", StringComparison.OrdinalIgnoreCase));
            if (_snapshot?.RuntimeOverrideActive != true && systemRuntimeIsSimulator)
            {
                ShowMessage(
                    Loc.S("Code_Main_SelectHeadsetRuntimeFirst", "Select a headset runtime first"),
                    Loc.S("Code_Main_SystemRuntimeIsSimulator", "The system runtime itself is currently the simulator, so there is no headset runtime for this button to restore. Open your headset or SteamVR desktop software and choose 'Set as active OpenXR runtime'."),
                    InfoBarSeverity.Warning);
                return;
            }

            var systemRuntimeName = _snapshot?.SystemRuntimeName ?? Loc.S("Code_Main_TheSystemOpenXrRuntime", "the system OpenXR runtime");
            var runtimeChanged = _snapshot?.RuntimeOverrideActive == true ||
                                 _snapshot?.SimulatorRuntimeOverrideActive == true;
            var runtimePath = await Task.Run(_service.RestoreSystemRuntime);
            var restartRequired = runtimeChanged && MarkVrRestartRequired(Loc.S("Code_Main_ReasonRuntimeChange", "the OpenXR runtime change"));
            ShowMessage(
                Loc.S("Code_Main_HeadsetRuntimeRestored", "Headset runtime restored"),
                Loc.F("Code_Main_HeadsetRuntimeRestoredBody", "Per-user simulator overrides were cleared. New OpenXR applications will use {0} ({1}).", systemRuntimeName, runtimePath) +
                (restartRequired ? " " + Loc.S("Code_Main_RestartVrToSwitchRuntimes", "Restart the running VR application to switch runtimes.") : string.Empty),
                InfoBarSeverity.Success);
            await RefreshSnapshotAsync();
        }
        catch (Exception ex)
        {
            ShowMessage(Loc.S("Code_Main_CouldNotRestoreRuntime", "Could not restore the headset runtime"), ex.Message, InfoBarSeverity.Error);
        }
    }

    private void LaunchMeta_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            if (_snapshot?.MetaXrRunning == true)
            {
                ShowMessage(
                    Loc.S("Code_Main_SimulatorAlreadyRunning", "Simulator testing tool is already running"),
                    Loc.S("Code_Main_SimulatorAlreadyRunningBody", "The manager has not changed your active OpenXR runtime."),
                    InfoBarSeverity.Informational);
                return;
            }
            _service.LaunchMetaXr(_snapshot?.MetaXrExecutable ?? string.Empty);
            ShowMessage(
                Loc.S("Code_Main_SimulatorLaunched", "Simulator testing tool launched"),
                Loc.S("Code_Main_SimulatorLaunchedBody", "It opened without inheriting a simulator runtime override. Runtime selection remains an explicit testing action."),
                InfoBarSeverity.Success);
        }
        catch (Exception ex)
        {
            ShowMessage(Loc.S("Code_Main_CouldNotLaunchSimulator", "Could not launch the simulator testing tool"), ex.Message, InfoBarSeverity.Error);
        }
    }

    private void OpenInstallFolder_Click(object sender, RoutedEventArgs e)
    {
        Directory.CreateDirectory(_service.InstallDirectory);
        _service.OpenPath(_service.InstallDirectory);
    }

    private void OpenLayerLog_Click(object sender, RoutedEventArgs e) => _service.OpenPath(_service.LayerLogPath);

    private void LogSelector_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (LogTextBox is not null)
            RefreshLogView();
    }

    private void RefreshLogs_Click(object sender, RoutedEventArgs e) => RefreshLogView();

    private void RefreshLogView()
    {
        if (LogTextBox is null || LogSelector?.SelectedItem is not ComboBoxItem item)
            return;
        LogTextBox.Text = item.Tag switch
        {
            "obs" => _service.GetObsLog(),
            "preview" => _previewService.GetLog(),
            _ => _service.GetLayerLog(),
        };
        LogTextBox.Select(LogTextBox.Text.Length, 0);
    }

    private void OpenSelectedLog_Click(object sender, RoutedEventArgs e)
    {
        var path = LogSelector.SelectedItem is ComboBoxItem item
            ? item.Tag switch
            {
                "obs" => _service.GetLatestObsLogPath(),
                "preview" => _previewService.LogPath,
                _ => _service.LayerLogPath,
            }
            : _service.LayerLogPath;
        _service.OpenPath(path);
    }

    private async void ShareLogs_Click(object sender, RoutedEventArgs e)
    {
        await RunActionAsync(Loc.S("Code_Main_ActionLogSharing", "Log sharing"), async () =>
        {
            var files = await Task.Run(() => LogSharingService.CollectDiagnostics(_service, _snapshot, _previewService));
            var result = await _logSharing.UploadAsync(files);

            var copied = LogSharingService.TryCopyToClipboard(result.BinUrl);
            var summary = Loc.F("Code_Main_ShareSummary", "{0} ({1} file(s); the link expires after about a week)", result.BinUrl, result.FileUrls.Count);
            if (result.Failures.Count > 0)
                summary += " " + Loc.F("Code_Main_NotUploaded", "Not uploaded: {0}", string.Join("; ", result.Failures));
            ShowMessage(
                copied ? Loc.S("Code_Main_LogsUploadedLinkCopied", "Logs uploaded - share link copied to the clipboard") : Loc.S("Code_Main_LogsUploaded", "Logs uploaded"),
                summary,
                result.Failures.Count > 0 ? InfoBarSeverity.Warning : InfoBarSeverity.Success);
        });
    }

    private async Task RunActionAsync(string actionName, Func<Task> action)
    {
        SetBusy(true);
        StatusInfoBar.IsOpen = false;
        try
        {
            await action();
            await RefreshSnapshotAsync();
        }
        catch (Exception ex)
        {
            ShowMessage(Loc.F("Code_Main_ActionFailed", "{0} failed", actionName), ex.Message, InfoBarSeverity.Error);
        }
        finally
        {
            SetBusy(false);
        }
    }

    private void SetBusy(bool busy)
    {
        BusyRing.IsActive = busy;
        MainNavigation.IsHitTestVisible = !busy;
    }

    private void ShowMessage(string title, string message, InfoBarSeverity severity)
    {
        StatusInfoBar.Title = title;
        StatusInfoBar.Message = message;
        StatusInfoBar.Severity = severity;
        StatusInfoBar.IsOpen = true;
    }

    private void SetStatus(Border dot, TextBlock label, bool good, string text, bool useWarningWhenFalse = true)
    {
        dot.Background = GetBrush(good ? "GoodBrush" : useWarningWhenFalse ? "WarnBrush" : "MutedTextBrush");
        label.Text = text;
    }

    private SolidColorBrush GetBrush(string key) => (SolidColorBrush)Application.Current.Resources[key];

    private static string ShortHash(string hash) => string.IsNullOrWhiteSpace(hash) ? Loc.S("Code_Main_HashUnavailable", "hash unavailable") : hash[..Math.Min(10, hash.Length)];
    private static string DisplayHash(string hash) => string.IsNullOrWhiteSpace(hash) ? Loc.S("Code_Main_HashNotInstalled", "not installed") : hash;
    private static string LastLine(string text) => text.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries).LastOrDefault() ?? Loc.S("Code_Main_CompletedSuccessfully", "Completed successfully.");

    // 服务层返回的运行时名称同时参与逻辑判断（RuntimeName.Equals("Not configured")、
    // RuntimeName.Contains("Simulator") 等），因此这里只翻译展示用的已知状态短语；
    // 第三方运行时与设备名按原文显示。
    private static string DisplayRuntimeLabel(string value)
    {
        return value switch
        {
            "Not configured" => Loc.S("Code_Main_RuntimeNotConfigured", "Not configured"),
            "No runtime configured" => Loc.S("Code_Main_RuntimeNoneConfigured", "No runtime configured"),
            "System headset runtime" => Loc.S("Code_Main_RuntimeSystemHeadset", "System headset runtime"),
            "Process environment override" => Loc.S("Code_Main_RuntimeProcessOverride", "Process environment override"),
            _ => value,
        };
    }
}
