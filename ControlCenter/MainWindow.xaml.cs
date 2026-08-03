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
        App.LogStartup("AppWindow acquired");
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
            snapshot.LayerRegistered && snapshot.LayerFilesInstalled,
            snapshot.LayerRegistered ? "Registered" : "Not registered");
        LayerDetailText.Text = snapshot.LayerFilesInstalled
            ? $"Installed • {ShortHash(snapshot.LayerHash)}"
            : "Release files are not installed";

        SetStatus(PluginDot, PluginStatusText,
            snapshot.PluginInstalled && snapshot.PluginCurrent,
            !snapshot.PluginInstalled ? "Not installed" : snapshot.PluginCurrent ? "Current" : "Update ready");
        PluginDetailText.Text = snapshot.ObsRunning ? "OBS is running" : "OBS is not running";

        var runtimeOkay = !snapshot.RuntimeName.Equals("Not configured", StringComparison.OrdinalIgnoreCase);
        SetStatus(RuntimeDot, RuntimeStatusText, runtimeOkay, snapshot.RuntimeName);
        RuntimeDetailText.Text = snapshot.RuntimePath;

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
        _loadingControls = false;
        UpdateOverscanPreview();

        _loadingSmoothingControls = true;
        SmoothingManagedToggle.IsOn = snapshot.CameraSmoothingManaged;
        SmoothingSlider.Value = snapshot.CameraSmoothing;
        SmoothingCropSlider.Value = snapshot.SmoothingCrop;
        _loadingSmoothingControls = false;
        UpdateSmoothingPreview();

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

        InstallLayerStatusText.Text = snapshot.LayerFilesInstalled
            ? snapshot.LayerRegistered ? "Installed and registered" : "Installed, registration missing"
            : "Not installed";
        InstallLayerPathText.Text = snapshot.LayerManifestPath;
        InstallPluginStatusText.Text = snapshot.PluginInstalled
            ? snapshot.PluginCurrent ? "Installed and current" : "Installed, update available"
            : "Not installed";
        InstallPluginPathText.Text = _service.PluginPath;

        DiagnosticRuntimeNameText.Text = snapshot.RuntimeName;
        DiagnosticRuntimePathText.Text = snapshot.RuntimePath;
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
        InstallationPage.Visibility = tag == "installation" ? Visibility.Visible : Visibility.Collapsed;
        DiagnosticsPage.Visibility = tag == "diagnostics" ? Visibility.Visible : Visibility.Collapsed;

        foreach (var button in new[] { DashboardNavButton, OverscanNavButton, SmoothingNavButton, InstallationNavButton, DiagnosticsNavButton })
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
            ShowMessage("Overscan settings saved", "Restart the VR application for the new FOV and render size to take effect.", InfoBarSeverity.Success);
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

    private void LaunchMeta_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            if (_snapshot?.MetaXrRunning == true)
            {
                ShowMessage("MetaXR is already running", "Register the layer now before launching the OpenXR application.", InfoBarSeverity.Informational);
                return;
            }
            _service.LaunchMetaXr(_snapshot?.MetaXrExecutable ?? string.Empty);
            ShowMessage("Meta XR Simulator launched", "Wait for startup, then press Register layer now.", InfoBarSeverity.Success);
        }
        catch (Exception ex)
        {
            ShowMessage("Could not launch MetaXR", ex.Message, InfoBarSeverity.Error);
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
