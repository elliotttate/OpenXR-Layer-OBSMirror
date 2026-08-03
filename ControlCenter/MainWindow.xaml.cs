using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using OBSMirror.ControlCenter.Models;
using OBSMirror.ControlCenter.Services;
using System.Runtime.InteropServices;
using Windows.Graphics;
using WinRT.Interop;

namespace OBSMirror.ControlCenter;

public sealed partial class MainWindow : Window
{
    private readonly OBSMirrorService _service = new();
    private SystemSnapshot? _snapshot;
    private bool _loadingControls;
    private bool _loadingSmoothingControls;
    private bool _loadingQuadLayerControls;

    public MainWindow()
    {
        App.LogStartup("MainWindow constructor entered");
        InitializeComponent();
        App.LogStartup("MainWindow.InitializeComponent completed");

        ConfigureOverscanSlider(HorizontalSlider, 115);
        ConfigureOverscanSlider(VerticalSlider, 108);
        ConfigureSlider(BoundaryCompensationStrengthSlider, 0, 100, 1, 100);
        ConfigureSlider(SmoothingSlider, 0, 100, 1, 35);
        ConfigureSlider(SmoothingCropSlider, 0, 25, 0.5, 8);
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        SystemBackdrop = new MicaBackdrop();
        App.LogStartup("Dark title bar and Mica backdrop configured");

        var hwnd = WindowNative.GetWindowHandle(this);
        var windowId = Win32Interop.GetWindowIdFromWindow(hwnd);
        var appWindow = AppWindow.GetFromWindowId(windowId);
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
        slider.Maximum = 150;
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

    private async void MainWindow_Activated(object sender, WindowActivatedEventArgs args)
    {
        Activated -= MainWindow_Activated;
        await RefreshSnapshotAsync();
    }

    private async Task RefreshSnapshotAsync()
    {
        SetBusy(true);
        try
        {
            _snapshot = await Task.Run(_service.GetSnapshot);
            RenderSnapshot(_snapshot);
            RefreshLogView();
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
            !snapshot.LayerRegistered ? "Not registered" : snapshot.LayerCurrent ? "Registered" : "Update ready");
        LayerDetailText.Text = snapshot.LayerFilesInstalled
            ? snapshot.LayerCurrent ? $"Installed • {ShortHash(snapshot.LayerHash)}" : "New layer build is ready"
            : "Release files are not installed";

        SetStatus(PluginDot, PluginStatusText,
            snapshot.PluginInstalled && snapshot.PluginCurrent,
            !snapshot.PluginInstalled ? "Not installed" : snapshot.PluginCurrent ? "Current" : "Update ready");
        PluginDetailText.Text = snapshot.ObsRunning ? "OBS is running" : "OBS is not running";

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

        LastCaptureText.Text = snapshot.LastCaptureSummary;
        ObsProcessText.Text = snapshot.ObsRunning ? "RUNNING" : "IDLE";
        ObsProcessText.Foreground = GetBrush(snapshot.ObsRunning ? "GoodBrush" : "MutedTextBrush");
        MetaProcessText.Text = snapshot.MetaXrRunning ? "RUNNING" : "IDLE";
        MetaProcessText.Foreground = GetBrush(snapshot.MetaXrRunning ? "GoodBrush" : "MutedTextBrush");
        LaunchMetaButton.IsEnabled = !string.IsNullOrWhiteSpace(snapshot.MetaXrExecutable);

        _loadingControls = true;
        OverscanToggle.IsOn = snapshot.OverscanEnabled;
        HorizontalSlider.Value = snapshot.HorizontalPercent;
        VerticalSlider.Value = snapshot.VerticalPercent;
        BoundaryCompensationToggle.IsOn = snapshot.OverscanBoundaryCompensation;
        BoundaryCompensationStrengthSlider.Value = snapshot.OverscanBoundaryCompensationStrength;
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
            ? !snapshot.LayerRegistered ? "Installed, registration missing" : snapshot.LayerCurrent ? "Installed and current" : "Installed, update available"
            : "Not installed";
        InstallLayerPathText.Text = snapshot.LayerManifestPath;
        InstallPluginStatusText.Text = snapshot.PluginInstalled
            ? snapshot.PluginCurrent ? "Installed and current" : "Installed, update available"
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
        var boundaryStrength = (int)Math.Round(BoundaryCompensationStrengthSlider.Value);
        BoundaryCompensationStrengthText.Text = $"{boundaryStrength}%";
        BoundaryCompensationStrengthSlider.IsEnabled = BoundaryCompensationToggle.IsOn;
        BoundaryCompensationSummaryText.Text = BoundaryCompensationToggle.IsOn
            ? "Matches projection-baked fullscreen effects across the extra recording perimeter."
            : "Off — the overscan perimeter is copied exactly as rendered.";
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
            _service.ApplyOverscan(
                OverscanToggle.IsOn,
                (int)Math.Round(HorizontalSlider.Value),
                (int)Math.Round(VerticalSlider.Value));
            _service.ApplyOverscanBoundaryCompensation(
                BoundaryCompensationToggle.IsOn,
                (int)Math.Round(BoundaryCompensationStrengthSlider.Value));
            ShowMessage(
                "Overscan settings saved",
                "FOV and render-size changes apply after restarting the VR application. Guard-band matching then updates live.",
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
        OverscanToggle.IsOn = false;
        _service.ApplyOverscan(false, (int)Math.Round(HorizontalSlider.Value), (int)Math.Round(VerticalSlider.Value));
        ShowMessage("Overscan disabled", "Restart the VR application to return to the runtime-native render size.", InfoBarSeverity.Success);
        await RefreshSnapshotAsync();
    }

    private async void InstallUpdate_Click(object sender, RoutedEventArgs e)
    {
        await RunActionAsync("Installing OBSMirror", async () =>
        {
            var snapshot = _service.GetSnapshot();
            if (snapshot.ObsRunning && !snapshot.PluginCurrent)
                throw new InvalidOperationException("OBS is running and the plugin binary has changed. Stop recording and close OBS before updating the plugin.");
            var output = await _service.SetupAsync(snapshot.ObsRunning && snapshot.PluginCurrent);
            ShowMessage("Installation complete", LastLine(output), InfoBarSeverity.Success);
        });
    }

    private async void RegisterLayer_Click(object sender, RoutedEventArgs e)
    {
        await RunActionAsync("Registering the OpenXR layer", async () =>
        {
            var output = await _service.RegisterLayerAsync();
            ShowMessage("Layer registered", LastLine(output), InfoBarSeverity.Success);
        });
    }

    private async void UnregisterLayer_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new ContentDialog
        {
            XamlRoot = AppTitleBar.XamlRoot,
            Title = "Unregister the OpenXR layer?",
            Content = "This removes the current-user OpenXR registration. Installed files and the OBS plugin remain in place.",
            PrimaryButtonText = "Unregister",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
            return;

        await RunActionAsync("Unregistering the OpenXR layer", async () =>
        {
            var output = await _service.UnregisterLayerAsync();
            ShowMessage("Layer unregistered", LastLine(output), InfoBarSeverity.Success);
        });
    }

    private void LaunchObs_Click(object sender, RoutedEventArgs e)
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
            var runtimePath = await Task.Run(_service.RestoreSystemRuntime);
            ShowMessage(
                "Headset runtime restored",
                $"Per-user simulator overrides were cleared. New OpenXR applications will use {systemRuntimeName} ({runtimePath}). Restart any launcher that was already open while the simulator override was active.",
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
        LogTextBox.Text = Equals(item.Tag, "obs") ? _service.GetObsLog() : _service.GetLayerLog();
        LogTextBox.Select(LogTextBox.Text.Length, 0);
    }

    private void OpenSelectedLog_Click(object sender, RoutedEventArgs e)
    {
        var isObs = LogSelector.SelectedItem is ComboBoxItem { Tag: "obs" };
        _service.OpenPath(isObs ? _service.GetLatestObsLogPath() : _service.LayerLogPath);
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
