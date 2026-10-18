using System;
using System.IO;
using System.Runtime.InteropServices;
using CamStudio.Native;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics;

namespace CamStudio;

// A phone's shared screen in a window of its own, titled "<tên máy> · Màn hình", so a meeting
// app can share exactly this window. Not the virtual camera: the engine's viewer has its own
// connection and decoder, and draws at this window's size (cs_viewer_*). Built in code, no XAML.
public sealed unsafe partial class ScreenWindow : Window
{
    public string SourceId { get; }

    private readonly int _viewer;
    private readonly nint _hwnd;
    private readonly Grid _root = new() { Background = new SolidColorBrush(Colors.Black) };
    private readonly Image _image = new() { Stretch = Stretch.Uniform };
    private readonly TextBlock _status = new()
    {
        Foreground = new SolidColorBrush(Colors.White),
        HorizontalAlignment = HorizontalAlignment.Center,
        VerticalAlignment = VerticalAlignment.Center,
        TextAlignment = TextAlignment.Center,
        TextWrapping = TextWrapping.Wrap,
        Margin = new Thickness(24),
        Text = "Đang kết nối điện thoại…",
    };
    private readonly DispatcherQueueTimer _timer;
    private readonly WndProcDelegate _wndProc; // kept alive for the window's lifetime
    private nint _oldWndProc;
    private WriteableBitmap? _bitmap;
    private byte* _pixels;
    private ulong _lastFrame;
    private double _aspect;   // the screen's width / height, kept while resizing
    private int _ticks;

    public ScreenWindow(int camera, string sourceId, string title)
    {
        SourceId = sourceId;
        Title = title;
        _viewer = Engine.cs_viewer_open(camera);
        _hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);

        _root.Children.Add(_image);
        _root.Children.Add(_status);
        var menu = new MenuFlyout();
        var onTop = new ToggleMenuFlyoutItem { Text = "Luôn nằm trên cùng" };
        onTop.Click += (_, _) =>
        {
            if (AppWindow.Presenter is OverlappedPresenter p) p.IsAlwaysOnTop = onTop.IsChecked;
        };
        menu.Items.Add(onTop);
        _root.ContextFlyout = menu;
        ToolTipService.SetToolTip(_root, "Chuột phải: luôn nằm trên cùng");
        _root.SizeChanged += (_, _) => SendSize();
        Content = _root;

        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "CamStudio.ico"));
        double scale = GetDpiForWindow(_hwnd) / 96.0;
        AppWindow.ResizeClient(new SizeInt32((int)(400 * scale), (int)(820 * scale))); // a phone held upright
        AppWindow.Changed += (_, e) =>
        {
            if (e.DidPresenterChange || e.DidVisibilityChange) SendSize(); // minimized: nothing to draw
        };

        // Resizing keeps the screen's aspect (WM_SIZING), so there are no bars around it.
        _wndProc = WndProc;
        _oldWndProc = SetWindowLongPtrW(_hwnd, GWLP_WNDPROC, Marshal.GetFunctionPointerForDelegate(_wndProc));

        _timer = DispatcherQueue.CreateTimer();
        _timer.Interval = TimeSpan.FromMilliseconds(33);
        _timer.Tick += (_, _) => Pump();
        _timer.Start();

        Closed += (_, _) =>
        {
            _timer.Stop();
            if (_oldWndProc != 0) SetWindowLongPtrW(_hwnd, GWLP_WNDPROC, _oldWndProc);
            _oldWndProc = 0;
            Engine.cs_viewer_close(_viewer);
        };
        if (_viewer < 0) _status.Text = $"Không mở được màn hình điện thoại (0x{_viewer:X8}).";
    }

    private bool Minimized => AppWindow.Presenter is OverlappedPresenter { State: OverlappedPresenterState.Minimized };

    private void SendSize()
    {
        double scale = _root.XamlRoot?.RasterizationScale ?? 1.0;
        bool hidden = Minimized || !AppWindow.IsVisible;
        Engine.cs_viewer_set_size(_viewer, hidden ? 0 : (int)(_root.ActualWidth * scale), hidden ? 0 : (int)(_root.ActualHeight * scale));
    }

    private void Pump()
    {
        if (_viewer < 0) return;
        if (++_ticks % 15 == 0 || _bitmap is null) UpdateStatus();
        uint w, h;
        ulong id;
        Engine.cs_viewer_copy(_viewer, null, 0, &w, &h, &id); // size + id only
        if (id == _lastFrame || w == 0 || h == 0) return;
        if (_bitmap is null || _bitmap.PixelWidth != w || _bitmap.PixelHeight != h)
        {
            _bitmap = new WriteableBitmap((int)w, (int)h);
            _pixels = BufferAccess.GetPointer(_bitmap.PixelBuffer);
            _image.Source = _bitmap;
        }
        if (Engine.cs_viewer_copy(_viewer, (uint*)_pixels, (int)(w * h), &w, &h, &id) == 0) return;
        _lastFrame = id;
        _bitmap.Invalidate();
    }

    // Status text over the picture, and the window's shape when the phone is turned.
    private void UpdateStatus()
    {
        uint sw, sh;
        char* buf = stackalloc char[512];
        int state = Engine.cs_viewer_state(_viewer, &sw, &sh, buf, 512);
        string error = new string(buf);
        _status.Visibility = state == 2 ? Visibility.Collapsed : Visibility.Visible;
        _image.Opacity = state == 2 ? 1 : 0.35;
        _status.Text = state switch
        {
            1 => "Đang kết nối điện thoại…",
            3 => error.Length > 0 ? error + "\nĐang thử lại…" : "Đang chờ điện thoại…",
            _ => _status.Text,
        };
        if (sw == 0 || sh == 0) return;
        double aspect = (double)sw / sh;
        if (Math.Abs(aspect - _aspect) < 0.01) return;
        bool first = _aspect == 0;
        _aspect = aspect;
        FitToAspect(keepArea: !first);
    }

    // The same area (or, the first time, the same height) in the screen's shape, inside the
    // work area.
    private void FitToAspect(bool keepArea)
    {
        var size = AppWindow.ClientSize;
        if (size.Width <= 0 || size.Height <= 0) return;
        double h = keepArea ? Math.Sqrt(size.Width * (double)size.Height / _aspect) : size.Height;
        double w = h * _aspect;
        var area = DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Primary).WorkArea;
        double fit = Math.Min(1.0, Math.Min(area.Width * 0.9 / w, area.Height * 0.9 / h));
        AppWindow.ResizeClient(new SizeInt32((int)Math.Round(w * fit), (int)Math.Round(h * fit)));
    }

    // ---- aspect-locked resizing ----------------------------------------------------------------

    private const int GWLP_WNDPROC = -4;
    private const uint WM_SIZING = 0x0214;
    private const int WMSZ_LEFT = 1, WMSZ_TOP = 3, WMSZ_TOPLEFT = 4, WMSZ_TOPRIGHT = 5, WMSZ_BOTTOM = 6, WMSZ_BOTTOMLEFT = 7;

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }

    private delegate nint WndProcDelegate(nint hwnd, uint msg, nint wParam, nint lParam);

    private nint WndProc(nint hwnd, uint msg, nint wParam, nint lParam)
    {
        if (msg == WM_SIZING && _aspect > 0)
        {
            KeepAspect((int)wParam, (RECT*)lParam);
            return 1;
        }
        return CallWindowProcW(_oldWndProc, hwnd, msg, wParam, lParam);
    }

    // The edge being dragged sets the size; the opposite edges stay where they are.
    private void KeepAspect(int edge, RECT* r)
    {
        GetWindowRect(_hwnd, out RECT window);
        GetClientRect(_hwnd, out RECT client);
        int frameW = (window.Right - window.Left) - (client.Right - client.Left);
        int frameH = (window.Bottom - window.Top) - (client.Bottom - client.Top);
        int cw = Math.Max(1, r->Right - r->Left - frameW);
        int ch = Math.Max(1, r->Bottom - r->Top - frameH);
        if (edge is WMSZ_TOP or WMSZ_BOTTOM) cw = (int)Math.Round(ch * _aspect);
        else ch = (int)Math.Round(cw / _aspect);
        int w = cw + frameW, h = ch + frameH;
        if (edge is WMSZ_LEFT or WMSZ_TOPLEFT or WMSZ_BOTTOMLEFT) r->Left = r->Right - w;
        else r->Right = r->Left + w;
        if (edge is WMSZ_TOP or WMSZ_TOPLEFT or WMSZ_TOPRIGHT) r->Top = r->Bottom - h;
        else r->Bottom = r->Top + h;
    }

    [LibraryImport("user32.dll")] private static partial nint SetWindowLongPtrW(nint hwnd, int index, nint value);
    [LibraryImport("user32.dll")] private static partial nint CallWindowProcW(nint proc, nint hwnd, uint msg, nint wParam, nint lParam);
    [LibraryImport("user32.dll")] private static partial int GetWindowRect(nint hwnd, out RECT rect);
    [LibraryImport("user32.dll")] private static partial int GetClientRect(nint hwnd, out RECT rect);
    [LibraryImport("user32.dll")] private static partial uint GetDpiForWindow(nint hwnd);
}
