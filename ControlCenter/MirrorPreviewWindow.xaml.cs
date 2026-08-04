using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using OBSMirror.ControlCenter.Services;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.WindowsRuntime;
using Windows.Graphics;
using WinRT.Interop;

namespace OBSMirror.ControlCenter;

public sealed partial class MirrorPreviewWindow : Window
{
    private WriteableBitmap? _previewBitmap;

    public MirrorPreviewWindow()
    {
        InitializeComponent();
        SystemBackdrop = new MicaBackdrop();

        var hwnd = WindowNative.GetWindowHandle(this);
        var windowId = Win32Interop.GetWindowIdFromWindow(hwnd);
        var appWindow = AppWindow.GetFromWindowId(windowId);
        var scale = GetDpiForWindow(hwnd) / 96.0;
        var displayArea = DisplayArea.GetFromWindowId(windowId, DisplayAreaFallback.Primary);
        var workArea = displayArea.WorkArea;
        var width = Math.Min((int)Math.Round(1120 * scale), Math.Max(800, workArea.Width - 80));
        var height = Math.Min((int)Math.Round(760 * scale), Math.Max(600, workArea.Height - 80));
        appWindow.Resize(new SizeInt32(width, height));
        appWindow.Move(new PointInt32(
            workArea.X + (workArea.Width - width) / 2,
            workArea.Y + (workArea.Height - height) / 2));

        var titleBar = appWindow.TitleBar;
        titleBar.BackgroundColor = Colors.Black;
        titleBar.InactiveBackgroundColor = Colors.Black;
        titleBar.ForegroundColor = Colors.White;
        titleBar.InactiveForegroundColor = ColorHelper.FromArgb(255, 170, 178, 195);
        titleBar.ButtonBackgroundColor = Colors.Black;
        titleBar.ButtonInactiveBackgroundColor = Colors.Black;
        titleBar.ButtonForegroundColor = Colors.White;
        titleBar.ButtonInactiveForegroundColor = ColorHelper.FromArgb(255, 170, 178, 195);
        titleBar.ButtonHoverBackgroundColor = ColorHelper.FromArgb(255, 42, 48, 56);
        titleBar.ButtonPressedBackgroundColor = ColorHelper.FromArgb(255, 58, 66, 76);

        var iconPath = Path.Combine(AppContext.BaseDirectory, "Assets", "OBSMirror.ControlCenter.ico");
        if (File.Exists(iconPath))
            appWindow.SetIcon(iconPath);
    }

    internal void RenderPreview(MirrorPreviewResult result, double fps)
    {
        LargePreviewStatusText.Text = result.Status;
        LargePreviewStatusText.Foreground = GetBrush(result.IsLive ? "GoodBrush" : "MutedTextBrush");
        LargePreviewDetailText.Text = fps > 0
            ? $"{result.Detail}  •  {fps:0} FPS"
            : result.Detail;

        if (result.Frame is not { } frame)
        {
            if (_previewBitmap is null)
            {
                LargePreviewPlaceholder.Visibility = Visibility.Visible;
                LargePreviewPlaceholderTitle.Text = result.Status;
                LargePreviewPlaceholderDetail.Text = result.Detail;
            }
            return;
        }

        if (_previewBitmap is null ||
            _previewBitmap.PixelWidth != frame.Width ||
            _previewBitmap.PixelHeight != frame.Height)
        {
            _previewBitmap = new WriteableBitmap(frame.Width, frame.Height);
            LargeMirrorPreviewImage.Source = _previewBitmap;
        }

        using var stream = _previewBitmap.PixelBuffer.AsStream();
        stream.Position = 0;
        stream.Write(frame.Pixels, 0, frame.Pixels.Length);
        _previewBitmap.Invalidate();
        LargePreviewPlaceholder.Visibility = Visibility.Collapsed;
    }

    private static Brush GetBrush(string resourceKey) =>
        (Brush)Application.Current.Resources[resourceKey];

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(nint hwnd);
}
