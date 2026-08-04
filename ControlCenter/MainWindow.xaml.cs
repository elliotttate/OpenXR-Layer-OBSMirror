using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using OBSMirror.ControlCenter.Models;
using OBSMirror.ControlCenter.Services;
using System.Diagnostics;
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
    private bool _autoUpdateInProgress;
    private bool _autoUpdateFailed;
    private string _lastAutoUpdateKey = string.Empty;
    private bool _loadingControls;
    private bool _loadingSmoothingControls;
    private bool _loadingQuadLayerControls;
    private bool _loadingLayerRegistrationControls;

    public MainWindow()
    {
        App.LogStartup("MainWindow constructor entered");
        InitializeComponent();
        App.LogStartup("MainWindow.InitializeComponent completed");

        ConfigureOverscanSlider(HorizontalSlider, 115);
        ConfigureOverscanSlider(VerticalSlider, 108);
        ConfigureSlider(SmoothingSlider, 0, 100, 1, 35);
        ConfigureSlider(SmoothingCropSlider, 0, 25, 0.5, 8);
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

        Activated += MainWindow_Activated;
        Closed += (_, _) =>
        {
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
            Title = $"Update available — {update.Title}",
            PrimaryButtonText = "Yes, update",
            CloseButtonText = "Later",
            DefaultButton = ContentDialogButton.Primary,
            Content = new ScrollViewer
            {
                MaxHeight = 340,
                Content = new TextBlock
                {
                    Text = $"Version {update.Version} is available (you have {AppUpdateService.CurrentVersion}). " +
                           "It downloads, installs, and reopens the app automatically." +
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
                    $"Downloading update {update.Version}",
                    $"{update.InstallerName} — {percent}%",
                    InfoBarSeverity.Informational));
            var installerPath = await _appUpdate.DownloadInstallerAsync(update, progress);

            ShowMessage(
                "Installing update",
                "The app closes now and reopens automatically when the update finishes.",
                InfoBarSeverity.Informational);
            AppUpdateService.StartUpdateAndRelaunch(installerPath);
            await Task.Delay(500);
            Application.Current.Exit();
        }
        catch (Exception ex)
        {
            App.LogStartup("App update failed", ex);
            ShowMessage(
                "Update failed",
                $"{ex.Message} You can retry from the dialog on next launch or download it from GitHub Releases.",
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
            ShowMessage("Could not refresh status", ex.Message, InfoBarSeverity.Error);
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
            !snapshot.LayerRegistered ? "Disabled"
            : snapshot.LayerCurrent ? "Enabled"
            : _autoUpdateFailed ? "Update failed"
            : "Updating…");
        LayerDetailText.Text = snapshot.LayerFilesInstalled
            ? snapshot.LayerCurrent ? $"Installed • {ShortHash(snapshot.LayerHash)}"
              : _autoUpdateFailed ? "Retry from the Installation page"
              : "Installing the new layer build automatically"
            : "Release files are not installed";

        SetStatus(PluginDot, PluginStatusText,
            snapshot.PluginInstalled && snapshot.PluginCurrent,
            !snapshot.PluginInstalled ? "Not installed"
            : snapshot.PluginCurrent ? "Current"
            : _autoUpdateFailed ? "Update failed"
            : snapshot.ObsRunning ? "Close OBS to update"
            : "Updating…");
        PluginDetailText.Text = !snapshot.PluginCurrent && snapshot.PluginInstalled && snapshot.ObsRunning
            ? "OBS is running — the update installs when it closes"
            : snapshot.ObsRunning ? "OBS is running" : "OBS is not running";

        var runtimeConfigured = !snapshot.RuntimeName.Equals("Not configured", StringComparison.OrdinalIgnoreCase);
        var runtimeOkay = runtimeConfigured && !snapshot.SimulatorRuntimeOverrideActive;
        SetStatus(RuntimeDot, RuntimeStatusText, runtimeOkay,
            snapshot.SimulatorRuntimeOverrideActive ? "Simulator override" : snapshot.RuntimeName);
        RuntimeDetailText.Text = snapshot.SimulatorRuntimeOverrideActive
            ? $"Restore {snapshot.SystemRuntimeName} for a headset"
            : snapshot.RuntimeSource;

        if (snapshot.SimulatorRuntimeOverrideActive)
        {
            RuntimeModeInfoBar.Severity = InfoBarSeverity.Warning;
            RuntimeModeInfoBar.Title = "Simulator override is active";
            RuntimeModeInfoBar.Message = $"OpenXR applications will bypass the normal headset runtime. Use headset runtime restores {snapshot.SystemRuntimeName} and clears the per-user override.";
        }
        else if (!runtimeConfigured)
        {
            RuntimeModeInfoBar.Severity = InfoBarSeverity.Error;
            RuntimeModeInfoBar.Title = "No OpenXR runtime is configured";
            RuntimeModeInfoBar.Message = "Set your headset software as the active OpenXR runtime, then refresh this page.";
        }
        else
        {
            RuntimeModeInfoBar.Severity = InfoBarSeverity.Success;
            RuntimeModeInfoBar.Title = "Headset runtime selected";
            RuntimeModeInfoBar.Message = $"OpenXR applications will use {snapshot.RuntimeName}. Simulator testing is optional and isolated under Installation.";
        }

        SetStatus(OverscanDot, OverscanStatusText, snapshot.OverscanEnabled,
            snapshot.OverscanEnabled ? "Enabled" : "Disabled", useWarningWhenFalse: false);
        OverscanDetailText.Text = snapshot.OverscanEnabled
            ? $"{snapshot.HorizontalPercent}% × {snapshot.VerticalPercent}%"
            : "Headset-native FOV";

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
            SmoothingAvailabilityInfoBar.Title = "Install the OBS source";
            SmoothingAvailabilityInfoBar.Message = "The values can be saved now, but the OBS source must be installed before they can take effect.";
        }
        else if (!snapshot.PluginCurrent)
        {
            SmoothingAvailabilityInfoBar.Severity = InfoBarSeverity.Warning;
            SmoothingAvailabilityInfoBar.Title = "Source update required";
            SmoothingAvailabilityInfoBar.Message = "The values can be saved now. Install the available source update and restart OBS to enable live control.";
        }
        else
        {
            SmoothingAvailabilityInfoBar.Severity = InfoBarSeverity.Informational;
            SmoothingAvailabilityInfoBar.Title = "Applies live";
            SmoothingAvailabilityInfoBar.Message = "The installed OBS source polls these settings four times per second. Saved values are used on the next session too.";
        }

        if (!snapshot.LayerFilesInstalled)
        {
            QuadLayersAvailabilityInfoBar.Severity = InfoBarSeverity.Warning;
            QuadLayersAvailabilityInfoBar.Title = "Install the OpenXR layer";
            QuadLayersAvailabilityInfoBar.Message = "The preference can be saved now, but the updated layer must be installed before it can filter the recording.";
        }
        else if (!snapshot.LayerCurrent)
        {
            QuadLayersAvailabilityInfoBar.Severity = InfoBarSeverity.Warning;
            QuadLayersAvailabilityInfoBar.Title = "Layer update required";
            QuadLayersAvailabilityInfoBar.Message = "Save the preference now, then install the available layer update and restart the VR application once.";
        }
        else
        {
            QuadLayersAvailabilityInfoBar.Severity = InfoBarSeverity.Informational;
            QuadLayersAvailabilityInfoBar.Title = "Applies live";
            QuadLayersAvailabilityInfoBar.Message = "The updated OpenXR layer polls this setting while recording. Restart the VR application once after installing the update.";
        }

        InstallLayerStatusText.Text = snapshot.LayerFilesInstalled
            ? !snapshot.LayerRegistered ? "Installed, disabled" : snapshot.LayerCurrent ? "Installed and enabled" : "Installed, update pending (automatic)"
            : "Not installed";
        InstallLayerPathText.Text = snapshot.LayerManifestPath;
        InstallPluginStatusText.Text = snapshot.PluginInstalled
            ? snapshot.PluginCurrent ? "Installed and current" : snapshot.ObsRunning ? "Installed, update waiting for OBS to close" : "Installed, update pending (automatic)"
            : "Not installed";
        InstallPluginPathText.Text = _service.PluginPath;

        DiagnosticRuntimeNameText.Text = snapshot.RuntimeName;
        DiagnosticRuntimePathText.Text = $"{snapshot.RuntimeSource}\n{snapshot.RuntimePath}\nSystem default: {snapshot.SystemRuntimeName} — {snapshot.SystemRuntimePath}";
        DiagnosticLayerHashText.Text = $"Layer   {DisplayHash(snapshot.LayerHash)}";
        DiagnosticPluginHashText.Text = $"Plugin  {DisplayHash(snapshot.PluginHash)}";
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
        if (!_loadingControls)
            UpdateOverscanPreview();
    }

    private void OverscanSlider_ValueChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        if (!_loadingControls && HorizontalValueText is not null)
            UpdateOverscanPreview();
    }

    private void UpdateOverscanPreview()
    {
        var horizontal = (int)Math.Round(HorizontalSlider.Value);
        var vertical = (int)Math.Round(VerticalSlider.Value);
        HorizontalValueText.Text = $"{horizontal}%";
        VerticalValueText.Text = $"{vertical}%";
        var pixelCost = horizontal / 100.0 * (vertical / 100.0) - 1.0;
        PixelCostText.Text = $"+{pixelCost * 100:0.0}%";
        ScaleSummaryText.Text = $"{horizontal / 100.0:0.00}× horizontal  •  {vertical / 100.0:0.00}× vertical";
        ExpectedTextureText.Text = $"{horizontal / 100.0:0.00}×  ×  {vertical / 100.0:0.00}×";
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
                : "the running VR app";
        VrRestartTitleText.Text = "RESTART VR APP";
        VrRestartReasonText.Text = _vrRestartReasons.Count == 1
            ? $"Restart {application} to apply {_vrRestartReasons.Single()}."
            : $"Restart {application} to apply these changes: {string.Join("; ", _vrRestartReasons)}.";
        VrRestartPanel.Visibility = Visibility.Visible;
    }

    private void SmoothingControl_Changed(object sender, RoutedEventArgs e)
    {
        if (!_loadingSmoothingControls && SmoothingValueText is not null)
            UpdateSmoothingPreview();
    }

    private void SmoothingSlider_ValueChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        if (!_loadingSmoothingControls && SmoothingValueText is not null && SmoothingCropValueText is not null)
            UpdateSmoothingPreview();
    }

    private void UpdateSmoothingPreview()
    {
        var smoothing = (int)Math.Round(SmoothingSlider.Value);
        var crop = Math.Round(SmoothingCropSlider.Value * 2.0) / 2.0;
        var strength = smoothing / 100.0;
        var responseMs = 40.0 + strength * strength * 760.0;

        SmoothingValueText.Text = smoothing.ToString();
        SmoothingCropValueText.Text = $"{crop:0.0}%";
        SmoothingResponseText.Text = smoothing == 0 || crop == 0 ? "Off" : $"{responseMs:0} ms";
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
            ? "Projection + quad-layer UI"
            : "Projection only";
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
                visible ? "Quad-layer UI shown" : "Quad-layer UI hidden",
                "The OBS mirror picks up this recording-only setting live when the updated layer is active. The headset remains unchanged.",
                InfoBarSeverity.Success);
            await RefreshSnapshotAsync();
        }
        catch (Exception ex)
        {
            ShowMessage("Could not save the UI-layer setting", ex.Message, InfoBarSeverity.Error);
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

    private async void ApplySmoothing_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            SmoothingManagedToggle.IsOn = true;
            _service.ApplyCameraSmoothing(
                managed: true,
                smoothing: (int)Math.Round(SmoothingSlider.Value),
                crop: SmoothingCropSlider.Value);
            var appliesLive = _snapshot is { PluginInstalled: true, PluginCurrent: true };
            ShowMessage(
                "Camera smoothing saved",
                appliesLive
                    ? "The OBS mirror source picks up the new recording-camera values live."
                    : "Install the available OBS source update and restart OBS to enable live control.",
                appliesLive ? InfoBarSeverity.Success : InfoBarSeverity.Warning);
            await RefreshSnapshotAsync();
        }
        catch (Exception ex)
        {
            ShowMessage("Could not save camera smoothing", ex.Message, InfoBarSeverity.Error);
        }
    }

    private async void ReleaseSmoothing_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            SmoothingManagedToggle.IsOn = false;
            _service.ApplyCameraSmoothing(
                managed: false,
                smoothing: (int)Math.Round(SmoothingSlider.Value),
                crop: SmoothingCropSlider.Value);
            ShowMessage("OBS source settings restored", "The mirror source now uses the smoothing values stored in OBS.", InfoBarSeverity.Success);
            await RefreshSnapshotAsync();
        }
        catch (Exception ex)
        {
            ShowMessage("Could not release camera smoothing", ex.Message, InfoBarSeverity.Error);
        }
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

    private async void ApplyOverscan_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            var horizontal = (int)Math.Round(HorizontalSlider.Value);
            var vertical = (int)Math.Round(VerticalSlider.Value);
            var changed = _snapshot is null ||
                          _snapshot.OverscanEnabled != OverscanToggle.IsOn ||
                          _snapshot.HorizontalPercent != horizontal ||
                          _snapshot.VerticalPercent != vertical;
            _service.ApplyOverscan(
                OverscanToggle.IsOn,
                horizontal,
                vertical);
            var restartRequired = changed && MarkVrRestartRequired("the overscan change");
            ShowMessage(
                "Overscan settings saved",
                restartRequired
                    ? "The running VR application must be restarted to rebuild its FOV and render targets."
                    : "No running VR application needs a restart; the next launch will use these settings.",
                InfoBarSeverity.Success);
            await RefreshSnapshotAsync();
        }
        catch (Exception ex)
        {
            ShowMessage("Could not save overscan", ex.Message, InfoBarSeverity.Error);
        }
    }

    private async void DisableOverscan_Click(object sender, RoutedEventArgs e)
    {
        var changed = _snapshot?.OverscanEnabled != false;
        OverscanToggle.IsOn = false;
        _service.ApplyOverscan(false, (int)Math.Round(HorizontalSlider.Value), (int)Math.Round(VerticalSlider.Value));
        var restartRequired = changed && MarkVrRestartRequired("the overscan change");
        ShowMessage(
            "Overscan disabled",
            restartRequired
                ? "The running VR application must be restarted to return to the runtime-native render size."
                : "No running VR application needs a restart; the next launch will use the runtime-native render size.",
            InfoBarSeverity.Success);
        await RefreshSnapshotAsync();
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

        // The plugin DLL cannot be replaced while OBS holds it; the layer
        // still updates now and the plugin follows once OBS has closed.
        var pluginDeferred = pluginOutdated && snapshot.ObsRunning;
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

            var restartRequired = layerOutdated && MarkVrRestartRequired("the OpenXR layer update");
            var followUp =
                (restartRequired ? " Restart the running VR application to load the new layer build." : string.Empty) +
                (pluginDeferred ? " The OBS source update installs automatically once OBS is closed." : string.Empty) +
                (pluginOutdated && !pluginDeferred ? " Restart OBS to load the updated source." : string.Empty);
            ShowMessage(
                pluginDeferred && !layerOutdated ? "OBS source update waiting" : "Updated automatically",
                (string.IsNullOrWhiteSpace(output) ? "A new build was detected." : LastLine(output)) + followUp,
                InfoBarSeverity.Success);
        }
        catch (Exception ex)
        {
            _autoUpdateFailed = true;
            ShowMessage(
                "Automatic update failed",
                $"{ex.Message} Use Installation > Install / update to retry.",
                InfoBarSeverity.Warning);
        }
        finally
        {
            SetBusy(false);
            _autoUpdateInProgress = false;
        }
        await RefreshSnapshotAsync();
    }

    private async void InstallUpdate_Click(object sender, RoutedEventArgs e)
    {
        await RunActionAsync("Installing OBSMirror", async () =>
        {
            var snapshot = _service.GetSnapshot();
            var layerWillChange = !snapshot.LayerFilesInstalled || !snapshot.LayerCurrent;
            if (snapshot.ObsRunning && !snapshot.PluginCurrent)
                throw new InvalidOperationException("OBS is running and the plugin binary has changed. Stop recording and close OBS before updating the plugin.");
            var output = snapshot.PluginCurrent
                ? await _service.SetupAsync(snapshot.ObsRunning)
                : $"{await _service.InstallLayerOnlyAsync()} {await ElevatedInstallService.InstallPluginElevatedAsync()}";
            var restartRequired = layerWillChange && MarkVrRestartRequired("the OpenXR layer update");
            ShowMessage(
                "Installation complete",
                LastLine(output) + (restartRequired ? " Restart the running VR application to load the new layer build." : string.Empty),
                InfoBarSeverity.Success);
        });
    }

    private async void LayerRegistrationToggle_Toggled(object sender, RoutedEventArgs e)
    {
        if (_loadingLayerRegistrationControls || sender is not ToggleSwitch toggle)
            return;

        var enable = toggle.IsOn;
        await RunActionAsync(enable ? "Enabling the OpenXR layer" : "Disabling the OpenXR layer", async () =>
        {
            if (enable)
            {
                var output = await _service.RegisterLayerAsync();
                var restartRequired = MarkVrRestartRequired("the OpenXR layer registration change");
                ShowMessage(
                    "OpenXR layer enabled",
                    LastLine(output) + (restartRequired ? " Restart the running VR application to load the layer." : string.Empty),
                    InfoBarSeverity.Success);
            }
            else
            {
                var output = await _service.UnregisterLayerAsync();
                var restartRequired = MarkVrRestartRequired("the OpenXR layer registration change");
                ShowMessage(
                    "OpenXR layer disabled",
                    LastLine(output) + " Installed files and the OBS source were left in place." +
                    (restartRequired ? " Restart the running VR application to unload the layer." : string.Empty),
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

    private async void LaunchObs_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            if (_snapshot?.ObsRunning == true)
            {
                ShowMessage("OBS is already running", "The saved OpenXR Mirror Capture source is ready in the current OBS session.", InfoBarSeverity.Informational);
                return;
            }
            _service.LaunchObs();
            ShowMessage("OBS launched", "Refresh status after OBS finishes loading.", InfoBarSeverity.Success);
        }
        catch (OBSMirrorService.ObsNotFoundException ex)
        {
            // Portable and unusual installs register nothing to detect, so let
            // the user point at obs64.exe once instead of dead-ending.
            await BrowseForObsAndLaunchAsync(ex.Message);
        }
        catch (Exception ex)
        {
            ShowMessage("Could not launch OBS", ex.Message, InfoBarSeverity.Error);
        }
    }

    private async Task BrowseForObsAndLaunchAsync(string reason)
    {
        if (Content?.XamlRoot is not { } xamlRoot)
            return;

        var dialog = new ContentDialog
        {
            XamlRoot = xamlRoot,
            Title = "Locate OBS Studio",
            PrimaryButtonText = "Browse for obs64.exe",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Primary,
            Content = new TextBlock
            {
                Text = $"{reason}\n\nobs64.exe is usually in the OBS install folder under bin\\64bit.",
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
                "OBS launched",
                $"Saved this location for future launches: {file.Path}",
                InfoBarSeverity.Success);
        }
        catch (Exception ex)
        {
            ShowMessage("Could not launch OBS", ex.Message, InfoBarSeverity.Error);
        }
    }

    private async void RestoreHeadsetRuntime_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            var systemRuntimeName = _snapshot?.SystemRuntimeName ?? "the system OpenXR runtime";
            var runtimeChanged = _snapshot?.RuntimeOverrideActive == true ||
                                 _snapshot?.SimulatorRuntimeOverrideActive == true;
            var runtimePath = await Task.Run(_service.RestoreSystemRuntime);
            var restartRequired = runtimeChanged && MarkVrRestartRequired("the OpenXR runtime change");
            ShowMessage(
                "Headset runtime restored",
                $"Per-user simulator overrides were cleared. New OpenXR applications will use {systemRuntimeName} ({runtimePath})." +
                (restartRequired ? " Restart the running VR application to switch runtimes." : string.Empty),
                InfoBarSeverity.Success);
            await RefreshSnapshotAsync();
        }
        catch (Exception ex)
        {
            ShowMessage("Could not restore the headset runtime", ex.Message, InfoBarSeverity.Error);
        }
    }

    private void LaunchMeta_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            if (_snapshot?.MetaXrRunning == true)
            {
                ShowMessage("Simulator testing tool is already running", "The manager has not changed your active OpenXR runtime.", InfoBarSeverity.Informational);
                return;
            }
            _service.LaunchMetaXr(_snapshot?.MetaXrExecutable ?? string.Empty);
            ShowMessage("Simulator testing tool launched", "It opened without inheriting a simulator runtime override. Runtime selection remains an explicit testing action.", InfoBarSeverity.Success);
        }
        catch (Exception ex)
        {
            ShowMessage("Could not launch the simulator testing tool", ex.Message, InfoBarSeverity.Error);
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
        await RunActionAsync("Log sharing", async () =>
        {
            var files = await Task.Run(() => LogSharingService.CollectDiagnostics(_service, _snapshot, _previewService));
            var result = await _logSharing.UploadAsync(files);

            var copied = LogSharingService.TryCopyToClipboard(result.BinUrl);
            var summary = $"{result.BinUrl} ({result.FileUrls.Count} file(s); the link expires after about a week)";
            if (result.Failures.Count > 0)
                summary += $" Not uploaded: {string.Join("; ", result.Failures)}";
            ShowMessage(
                copied ? "Logs uploaded - share link copied to the clipboard" : "Logs uploaded",
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
            ShowMessage(actionName + " failed", ex.Message, InfoBarSeverity.Error);
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

    private static string ShortHash(string hash) => string.IsNullOrWhiteSpace(hash) ? "hash unavailable" : hash[..Math.Min(10, hash.Length)];
    private static string DisplayHash(string hash) => string.IsNullOrWhiteSpace(hash) ? "not installed" : hash;
    private static string LastLine(string text) => text.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries).LastOrDefault() ?? "Completed successfully.";
}
