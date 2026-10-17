using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using CamStudio.Native;
using CamStudio.Services;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics;
using Windows.UI;

namespace CamStudio;

public sealed partial class MainWindow : Window
{
    public sealed record LaunchOptions(string? Device, bool Tray);

    private sealed record FormatItem(int Index, CsFormat Format)
    {
        public string Short => $"{Format.Width} × {Format.Height} · {FpsText} fps";
        public string Long => $"{Short} · {Engine.FourccText(Format.Fourcc)}" + (Format.IsNative != 0 ? "  (gốc)" : "");
        private string FpsText => Format.Fps.ToString(Format.Fps % 1 == 0 ? "0" : "0.##");
    }

    private static readonly string[] AspectNames = { "Gốc", "16:9", "4:3", "1:1", "9:16" };

    private readonly AppSettings _settings = AppSettings.Load();
    private readonly DispatcherQueueTimer _previewTimer;
    private readonly DispatcherQueueTimer _statusTimer;
    private readonly TrayIcon _tray;
    private readonly Process _self = Process.GetCurrentProcess();
    private readonly List<string> _cameras = new();
    private readonly List<FormatItem> _formats = new();
    private int _camera = -1;
    private int _format = -1;
    private bool _isPhone;
    private int _loadGeneration;
    private PhoneCaps? _phoneCaps;
    private double _phoneZoom = 1;
    private int _lensSwitchFrom = -1;                          // format to return to if a lens switch fails
    private readonly HashSet<string> _brokenLenses = new();   // lenses the phone refused to open
    private Windows.Foundation.Point _pressPoint;
    private bool _pressMoved;
    private DispatcherQueueTimer? _focusRingTimer;

    private bool _loading = true;   // suppresses change handlers while filling the UI
    private bool _exiting;
    private bool _infoOpen;
    private int _statusTicks;
    private List<string> _consumers = new();
    private EngineState _lastState = EngineState.Stopped;

    // Preview: the engine writes straight into the bitmap's pixel buffer.
    private WriteableBitmap? _bitmap;
    private unsafe byte* _bitmapPixels;
    private ulong _lastFrameId;

    // Pan by dragging the preview
    private bool _panning;
    private Windows.Foundation.Point _panLast;

    private TimeSpan _lastCpu;
    private DateTime _lastCpuTime = DateTime.UtcNow;

    public MainWindow(LaunchOptions options)
    {
        InitializeComponent();

        int hr = Engine.cs_init();
        if (hr < 0) throw new InvalidOperationException($"Engine init failed 0x{hr:X8}");

        SetupWindow();
        ApplyTheme();

        _tray = new TrayIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "CamStudio.ico"));
        _tray.Activated += () => DispatcherQueue.TryEnqueue(ShowFromTray);
        _tray.BuildMenu = BuildTrayMenu;

        // 30 fps is plenty for an on-screen preview.
        _previewTimer = DispatcherQueue.CreateTimer();
        _previewTimer.Interval = TimeSpan.FromMilliseconds(33);
        _previewTimer.Tick += (_, _) => PumpPreview();
        _statusTimer = DispatcherQueue.CreateTimer();
        _statusTimer.Interval = TimeSpan.FromSeconds(1);
        _statusTimer.Tick += (_, _) => UpdateStatus();

        LoadSettingsIntoUi();
        _loading = false;

        _statusTimer.Start();
        UpdatePreviewActivity(visible: !options.Tray);
        UpdateStatus();
        InitAsync(options);
    }

    // Phones connect over the network, so loading the source list is async.
    private async void InitAsync(LaunchOptions options)
    {
        string? want = options.Device ?? _settings.Camera;
        // Phones on Wi‑Fi take a few seconds to be discovered: if the wanted
        // camera is not listed yet, wait for it (up to ~6 s) before settling.
        for (int attempt = 0; attempt < 6 && !string.IsNullOrEmpty(want); attempt++)
        {
            int count = Engine.cs_refresh_cameras();
            bool found = false;
            for (int i = 0; i < count && !found; i++)
                found = Engine.CameraName(i).Contains(want, StringComparison.OrdinalIgnoreCase);
            if (found) break;
            await System.Threading.Tasks.Task.Delay(1000);
        }
        await RefreshCamerasAsync(want);
        if ((options.Device is not null || options.Tray) && _camera >= 0 && _format >= 0) StartCapture();
        CameraMenu.Opening += (_, _) => RefreshCameraList();
    }

    // Cheap refresh when the camera menu opens: new phones show up without
    // reloading the selected camera's formats (the engine keeps them).
    private void RefreshCameraList()
    {
        if (IsCapturing && _isPhone) return; // the streaming phone's index must not move
        string? current = _camera >= 0 && _camera < _cameras.Count ? _cameras[_camera] : null;
        _cameras.Clear();
        int count = Engine.cs_refresh_cameras();
        for (int i = 0; i < count; i++) _cameras.Add(Engine.CameraName(i));
        _camera = current is null ? -1 : _cameras.IndexOf(current);
        BuildCameraMenu();
    }

    // ---- Window chrome ----------------------------------------------------------

    private void SetupWindow()
    {
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Tall;
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "CamStudio.ico"));

        double scale = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this)) / 96.0;
        AppWindow.Resize(new SizeInt32((int)(1100 * scale), (int)(760 * scale)));
        if (AppWindow.Presenter is OverlappedPresenter p)
        {
            p.PreferredMinimumWidth = (int)(720 * scale);
            p.PreferredMinimumHeight = (int)(520 * scale);
        }
        var area = DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Primary).WorkArea;
        AppWindow.Move(new PointInt32(area.X + (area.Width - AppWindow.Size.Width) / 2,
                                      area.Y + (area.Height - AppWindow.Size.Height) / 2));

        AppWindow.Closing += AppWindow_Closing;
        // Minimizing stops the preview work, restoring resumes it.
        AppWindow.Changed += (_, e) => { if (e.DidPresenterChange || e.DidVisibilityChange) UpdatePreviewActivity(); };
        RootGrid.ActualThemeChanged += (_, _) => UpdateCaptionButtons();
    }

    private void ApplyTheme()
    {
        RootGrid.RequestedTheme = _settings.Theme switch
        {
            1 => ElementTheme.Light,
            2 => ElementTheme.Dark,
            _ => ElementTheme.Default,
        };
        UpdateCaptionButtons();
    }

    // Caption buttons sit on our Mica content, so they follow the theme.
    private void UpdateCaptionButtons()
    {
        bool dark = RootGrid.ActualTheme == ElementTheme.Dark;
        var tb = AppWindow.TitleBar;
        tb.ButtonBackgroundColor = Colors.Transparent;
        tb.ButtonInactiveBackgroundColor = Colors.Transparent;
        tb.ButtonForegroundColor = dark ? Colors.White : Colors.Black;
        tb.ButtonInactiveForegroundColor = Color.FromArgb(0xFF, 0x88, 0x88, 0x88);
        tb.ButtonHoverBackgroundColor = dark ? Color.FromArgb(0x18, 0xFF, 0xFF, 0xFF) : Color.FromArgb(0x10, 0, 0, 0);
        tb.ButtonHoverForegroundColor = tb.ButtonForegroundColor;
        tb.ButtonPressedBackgroundColor = dark ? Color.FromArgb(0x10, 0xFF, 0xFF, 0xFF) : Color.FromArgb(0x08, 0, 0, 0);
        tb.ButtonPressedForegroundColor = tb.ButtonForegroundColor;
    }

    private bool IsOnScreen =>
        AppWindow.IsVisible && AppWindow.Presenter is not OverlappedPresenter { State: OverlappedPresenterState.Minimized };

    // The engine only converts preview frames while someone can see them.
    private void UpdatePreviewActivity(bool? visible = null)
    {
        bool on = visible ?? IsOnScreen;
        Engine.cs_set_preview_enabled(on ? 1 : 0);
        if (on) _previewTimer.Start();
        else _previewTimer.Stop();
    }

    private void AppWindow_Closing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        if (!_exiting && _settings.CloseToTray)
        {
            args.Cancel = true;
            HideToTray();
            return;
        }
        Shutdown();
    }

    public void ShowFromTray()
    {
        AppWindow.Show();
        if (AppWindow.Presenter is OverlappedPresenter { State: OverlappedPresenterState.Minimized } p) p.Restore();
        Activate();
        SetForegroundWindow(WinRT.Interop.WindowNative.GetWindowHandle(this));
        UpdatePreviewActivity(visible: true);
        UpdateStatus();
    }

    private void HideToTray()
    {
        AppWindow.Hide();
        UpdatePreviewActivity(visible: false);
        if (!_settings.TrayHintShown)
        {
            _tray.ShowBalloon("Cam Studio vẫn đang chạy",
                "Camera ảo vẫn hoạt động. Bấm biểu tượng ở khay để mở lại, chuột phải để Thoát.");
            _settings.TrayHintShown = true;
            _settings.Save();
        }
    }

    public void ExitApp()
    {
        _exiting = true;
        Close();
    }

    private void Shutdown()
    {
        _previewTimer.Stop();
        _statusTimer.Stop();
        _tray.Dispose();
        Engine.cs_shutdown();
    }

    private IReadOnlyList<TrayIcon.MenuItem?> BuildTrayMenu()
    {
        bool running = IsCapturing;
        return new List<TrayIcon.MenuItem?>
        {
            new("Mở Cam Studio", () => DispatcherQueue.TryEnqueue(ShowFromTray), IsDefault: true),
            null,
            new(running ? "Tắt camera ảo" : "Bật camera ảo", () => DispatcherQueue.TryEnqueue(() => VcamToggle.IsOn = !running)),
            new("Lật ngang", () => DispatcherQueue.TryEnqueue(() => { MirrorButton.IsChecked = !_settings.Mirror; Mirror_Click(this, new RoutedEventArgs()); }), Checked: _settings.Mirror),
            null,
            new("Khởi động cùng Windows", () => DispatcherQueue.TryEnqueue(() => AutostartToggle.IsOn = !AutostartToggle.IsOn), Checked: Autostart.IsEnabled),
            null,
            new("Thoát", () => DispatcherQueue.TryEnqueue(ExitApp)),
        };
    }

    // ---- Settings <-> UI ----------------------------------------------------------

    private void LoadSettingsIntoUi()
    {
        MirrorButton.IsChecked = _settings.Mirror;
        FlipButton.IsChecked = _settings.Flip;
        AspectRadios.SelectedIndex = _settings.Aspect;
        ZoomSlider.Value = _settings.Zoom;
        BrightnessSlider.Value = _settings.Brightness;
        ContrastSlider.Value = _settings.Contrast;
        SaturationSlider.Value = _settings.Saturation;
        TemperatureSlider.Value = _settings.Temperature;
        ThemeCombo.SelectedIndex = _settings.Theme;
        AutostartToggle.IsOn = Autostart.IsEnabled;
        CloseToTrayToggle.IsOn = _settings.CloseToTray;
        UpdateToolIndicators();
    }

    private CsImage CurrentImage() => new()
    {
        Rotation = _settings.Rotation,
        Mirror = _settings.Mirror ? 1 : 0,
        Flip = _settings.Flip ? 1 : 0,
        Aspect = _settings.Aspect,
        Zoom = _settings.Zoom,
        CenterX = _settings.CenterX,
        CenterY = _settings.CenterY,
        Brightness = _settings.Brightness,
        Contrast = _settings.Contrast,
        Saturation = _settings.Saturation,
        Temperature = _settings.Temperature,
    };

    private void PushImage(bool save = true)
    {
        if (save) _settings.Save();
        UpdateToolIndicators();
        if (IsCapturing) Engine.SetImage(CurrentImage()); // restarts by itself if the size changes
    }

    // Labels and dots on the toolbar reflect non-default settings.
    private void UpdateToolIndicators()
    {
        RotateText.Text = $"{_settings.Rotation}°";
        UpdateZoomText();
        AspectDot.Visibility = _settings.Aspect != 0 ? Visibility.Visible : Visibility.Collapsed;
        AdjustDot.Visibility = _settings.Brightness != 0 || _settings.Contrast != 0 || _settings.Saturation != 0 || _settings.Temperature != 0
            ? Visibility.Visible : Visibility.Collapsed;
        ToolTipService.SetToolTip(AspectButton, $"Tỉ lệ khung: {AspectNames[_settings.Aspect]}");
    }

    // ---- Cameras and formats -----------------------------------------------------

    private async System.Threading.Tasks.Task RefreshCamerasAsync(string? prefer)
    {
        // Indices change on refresh: never refresh under a running session.
        if (IsCapturing) return;
        string? current = _camera >= 0 && _camera < _cameras.Count ? _cameras[_camera] : null;
        _cameras.Clear();
        int count = Engine.cs_refresh_cameras();
        for (int i = 0; i < count; i++) _cameras.Add(Engine.CameraName(i));

        string? want = !string.IsNullOrEmpty(prefer) ? prefer : current;
        int pick = _cameras.Count > 0 ? 0 : -1;
        if (!string.IsNullOrEmpty(want))
        {
            int byName = _cameras.FindIndex(c => c.Equals(want, StringComparison.OrdinalIgnoreCase));
            if (byName < 0) byName = _cameras.FindIndex(c => c.Contains(want, StringComparison.OrdinalIgnoreCase));
            if (byName < 0 && int.TryParse(want, out int idx) && idx < _cameras.Count) byName = idx;
            if (byName >= 0) pick = byName;
        }
        await SelectCameraAsync(pick);
    }

    private async System.Threading.Tasks.Task SelectCameraAsync(int index)
    {
        _camera = index;
        _isPhone = index >= 0 && Engine.IsPhone(index);
        CameraButtonText.Text = index >= 0 ? _cameras[index] : "Không tìm thấy camera";
        BuildCameraMenu();
        await LoadFormatsAsync();
    }

    private void BuildCameraMenu()
    {
        CameraMenu.Items.Clear();
        for (int i = 0; i < _cameras.Count; i++)
        {
            int cam = i;
            var item = new RadioMenuFlyoutItem
            {
                Text = _cameras[i], GroupName = "camera", IsChecked = i == _camera,
                Icon = new FontIcon { Glyph = Engine.IsPhone(i) ? "" : "" },
            };
            item.Click += (_, _) => OnCameraPicked(cam);
            CameraMenu.Items.Add(item);
        }
        if (_cameras.Count > 0) CameraMenu.Items.Add(new MenuFlyoutSeparator());
        var refresh = new MenuFlyoutItem { Text = "Tìm lại camera và điện thoại", Icon = new FontIcon { Glyph = "" } };
        refresh.Click += async (_, _) => await RefreshCamerasAsync(null);
        CameraMenu.Items.Add(refresh);
        var addPhone = new MenuFlyoutItem { Text = "Thêm điện thoại theo địa chỉ IP…", Icon = new FontIcon { Glyph = "" } };
        addPhone.Click += async (_, _) => await AddPhoneByIpAsync();
        CameraMenu.Items.Add(addPhone);
    }

    private async System.Threading.Tasks.Task LoadFormatsAsync()
    {
        int generation = ++_loadGeneration;
        _formats.Clear();
        _phoneCaps = null;
        SelectFormat(-1);
        int cam = _camera;
        if (cam < 0) return;

        int n;
        if (_isPhone)
        {
            // Connecting (and pairing the first time) blocks: run it off the
            // UI thread and show the pairing code while the phone asks.
            FormatButtonText.Text = "Đang kết nối điện thoại…";
            var load = System.Threading.Tasks.Task.Run(() => Engine.cs_load_formats(cam));
            ContentDialog? pairing = null;
            bool answered = false;
            while (!load.IsCompleted)
            {
                await System.Threading.Tasks.Task.WhenAny(load, System.Threading.Tasks.Task.Delay(250));
                string code = Engine.PairingCode();
                if (code.Length > 0 && pairing is null && !answered)
                {
                    // Both sides must accept: the PC user answers here.
                    pairing = PairingDialog(_cameras[cam], code);
                    var shown = pairing;
                    _ = shown.ShowAsync().AsTask().ContinueWith(t =>
                    {
                        if (t.Result == ContentDialogResult.Primary) Engine.cs_pairing_answer(1);
                        else if (t.Result == ContentDialogResult.None && !ReferenceEquals(shown.Tag, "hidden")) Engine.cs_pairing_answer(0);
                        answered = true;
                    });
                }
                else if (code.Length == 0 && pairing is not null)
                {
                    pairing.Tag = "hidden"; // closed by us, not a "decline"
                    pairing.Hide();
                    pairing = null;
                }
            }
            if (pairing is not null)
            {
                pairing.Tag = "hidden";
                pairing.Hide();
            }
            n = load.Result;
        }
        else
        {
            n = Engine.cs_load_formats(cam);
        }
        if (generation != _loadGeneration) return; // another camera was picked meanwhile

        if (n < 0)
        {
            string message = Engine.LastError();
            ShowError(string.IsNullOrEmpty(message) ? "Không đọc được định dạng của camera." : message);
            FormatButtonText.Text = "—";
            return;
        }
        ErrorBar.IsOpen = false;
        for (int i = 0; i < n; i++) _formats.Add(new FormatItem(i, Engine.GetFormat(i)));
        _brokenLenses.Clear();
        _lensSwitchFrom = -1;
        if (_isPhone)
        {
            string capsJson = Engine.PhoneCapsJson();
            _phoneCaps = PhoneCaps.Parse(capsJson);
            AppSettings.WriteDiagnostic("last-caps.json", capsJson);
        }

        // Restore the saved mode for the same camera; otherwise native.
        int pick = _formats.Count > 0 ? 0 : -1;
        if (_settings.Width != 0 && _cameras[cam] == _settings.Camera)
        {
            int saved = _formats.FindIndex(f => f.Format.Width == _settings.Width && f.Format.Height == _settings.Height &&
                                                (uint)Math.Round(f.Format.Fps * 1000) == _settings.FpsMilli);
            if (saved >= 0) pick = saved;
        }
        SelectFormat(pick);
    }

    private ContentDialog PairingDialog(string phoneName, string code) => new()
    {
        XamlRoot = RootGrid.XamlRoot,
        Title = "Ghép đôi với " + phoneName,
        PrimaryButtonText = "Đồng ý (mã giống nhau)",
        CloseButtonText = "Từ chối",
        DefaultButton = ContentDialogButton.Close,
        Content = new StackPanel
        {
            Spacing = 16,
            Children =
            {
                new TextBlock
                {
                    Text = "Kiểm tra mã trên điện thoại có giống mã dưới đây không. Nếu giống, bấm Đồng ý ở đây và trên điện thoại.",
                    TextWrapping = TextWrapping.Wrap,
                },
                new TextBlock
                {
                    Text = code, FontSize = 44, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                    CharacterSpacing = 120, HorizontalAlignment = HorizontalAlignment.Center,
                },
                new TextBlock
                {
                    Text = "Chỉ cần ghép đôi một lần. Mã khác nhau nghĩa là có thiết bị lạ chen vào: hãy bấm Từ chối trên điện thoại.",
                    TextWrapping = TextWrapping.Wrap,
                    Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"],
                    Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
                },
            },
        },
    };

    private async System.Threading.Tasks.Task AddPhoneByIpAsync()
    {
        var box = new TextBox { PlaceholderText = "192.168.1.23:45678", Header = "Địa chỉ hiện trên màn hình app Cam Studio của điện thoại" };
        var dialog = new ContentDialog
        {
            XamlRoot = RootGrid.XamlRoot,
            Title = "Thêm điện thoại theo địa chỉ IP",
            Content = box,
            PrimaryButtonText = "Thêm",
            CloseButtonText = "Hủy",
            DefaultButton = ContentDialogButton.Primary,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;
        string text = box.Text.Trim();
        int colon = text.LastIndexOf(':');
        if (colon <= 0 || !int.TryParse(text[(colon + 1)..], out int port) || port is <= 0 or > 65535)
        {
            ShowError("Địa chỉ phải có dạng IP:cổng, ví dụ 192.168.1.23:45678");
            return;
        }
        string host = text[..colon];
        Engine.cs_add_phone(host, port);
        await RefreshCamerasAsync(host);
    }

    private static string ResolutionName(uint height) => height switch
    {
        >= 2160 => "4K",
        >= 1440 => "2K",
        >= 1080 => "Full HD",
        >= 720 => "HD",
        >= 480 => "SD",
        _ => $"{height}p",
    };

    private string FpsText(CsFormat f) => f.Fps.ToString(f.Fps % 1 == 0 ? "0" : "0.##");

    // Formats of the lens in use (phones), or all of them (webcams).
    private List<FormatItem> LensFormats()
    {
        if (!_isPhone || _format < 0) return _formats;
        string lens = _formats[_format].Format.LensText;
        return _formats.Where(f => f.Format.LensText == lens).ToList();
    }

    // The short list: native, then one entry per standard resolution
    // (16:9 preferred, 30 fps preferred), like a camera app.
    private List<FormatItem> PresetFormats(List<FormatItem> list)
    {
        var presets = new List<FormatItem>();
        var native = list.FirstOrDefault(f => f.Format.IsNative != 0) ?? list.FirstOrDefault();
        if (native is null) return presets;
        presets.Add(native);
        if (native.Format.Fps > 31) // also the native size at 30 fps
        {
            var at30 = list.FirstOrDefault(f => f.Format.Width == native.Format.Width && f.Format.Height == native.Format.Height && Math.Abs(f.Format.Fps - 30) < 0.5);
            if (at30 is not null) presets.Add(at30);
        }
        foreach (uint h in new uint[] { 2160, 1440, 1080, 720, 480 })
        {
            if (h >= native.Format.Height) continue;
            var pick = list.Where(f => f.Format.Height == h)
                .OrderByDescending(f => Math.Abs((double)f.Format.Width / f.Format.Height - 16.0 / 9) < 0.02)
                .ThenByDescending(f => Math.Abs(f.Format.Fps - 30) < 0.5)
                .ThenByDescending(f => f.Format.Fps)
                .FirstOrDefault();
            if (pick is not null) presets.Add(pick);
        }
        return presets;
    }

    private void SelectFormat(int index)
    {
        _format = index;
        if (index >= 0)
        {
            var f = _formats[index].Format;
            FormatButtonText.Text = $"{(f.IsNative != 0 ? "Gốc" : ResolutionName(f.Height))} · {f.Width} × {f.Height} · {FpsText(f)} fps";
        }
        else
        {
            FormatButtonText.Text = "—";
        }

        FormatMenu.Items.Clear();
        var list = LensFormats();
        var presets = PresetFormats(list);
        if (index >= 0 && !presets.Contains(_formats[index])) presets.Add(_formats[index]); // keep the current one visible
        foreach (var f in presets)
        {
            var c = f.Format;
            string name = c.IsNative != 0 ? "Gốc (lớn nhất)" : ResolutionName(c.Height);
            int fmt = f.Index;
            var item = new RadioMenuFlyoutItem
            {
                Text = $"{name}  ·  {c.Width} × {c.Height}  ·  {FpsText(c)} fps",
                GroupName = "format", IsChecked = f.Index == index,
            };
            item.Click += (_, _) => OnFormatPicked(fmt);
            FormatMenu.Items.Add(item);
        }

        // Everything else, for when the short list is not enough.
        if (list.Count > presets.Count)
        {
            var all = new MenuFlyoutSubItem { Text = $"Tất cả chế độ (nâng cao) · {list.Count}", Icon = new FontIcon { Glyph = "" } };
            uint lastHeight = 0;
            foreach (var f in list)
            {
                if (lastHeight != 0 && f.Format.Height != lastHeight) all.Items.Add(new MenuFlyoutSeparator());
                lastHeight = f.Format.Height;
                int fmt = f.Index;
                var item = new RadioMenuFlyoutItem { Text = f.Long, GroupName = "formatAll", IsChecked = f.Index == index };
                item.Click += (_, _) => OnFormatPicked(fmt);
                all.Items.Add(item);
            }
            FormatMenu.Items.Add(new MenuFlyoutSeparator());
            FormatMenu.Items.Add(all);
        }
        UpdateCameraOverlay();
    }

    // ---- In-frame camera controls ------------------------------------------------

    private void UpdateCameraOverlay()
    {
        LensChips.Children.Clear();
        var lens = CurrentLens();
        bool phone = _isPhone && _phoneCaps is not null && lens is not null;
        if (phone)
        {
            // One chip per lens on the same side, in zoom order (0.6× 1× 2× Macro).
            var sameSide = _phoneCaps!.Lenses
                .Where(l => l.Facing == lens!.Facing && _formats.Any(f => f.Format.LensText == l.Label))
                .OrderBy(l => l.Kind == "macro" ? 1000 : l.FocalLength35 > 0 ? l.FocalLength35 : 500)
                .ToList();
            if (sameSide.Count > 1)
            {
                foreach (var l in sameSide)
                {
                    bool broken = _brokenLenses.Contains(l.Label);
                    var chip = new ToggleButton
                    {
                        Content = _phoneCaps.ChipText(l),
                        IsChecked = l.Label == lens!.Label,
                        Style = Res<Style>("LensChipStyle"),
                        Opacity = broken ? 0.45 : 1,
                    };
                    ToolTipService.SetToolTip(chip, broken ? l.Label + " · điện thoại không cho mở ống kính này" : l.Label);
                    var target = l;
                    chip.Click += (_, _) => SwitchLens(target);
                    LensChips.Children.Add(chip);
                }
            }
            bool hasOtherSide = _phoneCaps.Lenses.Any(l => l.Facing != lens!.Facing && _formats.Any(f => f.Format.LensText == l.Label));
            FacingButton.Visibility = hasOtherSide ? Visibility.Visible : Visibility.Collapsed;

            FlashButton.Visibility = lens!.Torch ? Visibility.Visible : Visibility.Collapsed;
            FlashButton.IsChecked = Sent("torch", 0.0) > 0;
            ExposureButton.Visibility = lens.Bias is null ? Visibility.Collapsed : Visibility.Visible;
            if (lens.Bias is { } b)
            {
                bool was = _loading;
                _loading = true;
                BiasSlider.Minimum = Math.Max(b.Min, -4);
                BiasSlider.Maximum = Math.Min(b.Max, 4);
                BiasSlider.Value = Sent("exposureBias", 0.0);
                _loading = was;
            }
        }
        else
        {
            FacingButton.Visibility = Visibility.Collapsed;
        }
        ConfigureZoomSlider();
        UpdateOverlayVisibility();
    }

    private void UpdateOverlayVisibility()
    {
        bool running = (EngineState)Engine.GetStatus().State == EngineState.Running;
        // A phone keeps its lens chips while stopped, so a lens can be picked
        // (or a failed one left) before starting.
        bool lensPicker = _isPhone && (LensChips.Children.Count > 0 || FacingButton.Visibility == Visibility.Visible);
        CameraBar.Visibility = running || lensPicker ? Visibility.Visible : Visibility.Collapsed;
        ZoomGroup.Visibility = running ? Visibility.Visible : Visibility.Collapsed;
        CameraBarDivider.Visibility = running && lensPicker ? Visibility.Visible : Visibility.Collapsed;
        CameraSide.Visibility = running && _isPhone ? Visibility.Visible : Visibility.Collapsed;
    }

    // Same resolution on the other lens when it has it, else that lens' native mode.
    private void SwitchLens(PhoneCaps.Lens target)
    {
        var current = _format >= 0 ? _formats[_format].Format : default;
        var onLens = _formats.Where(f => f.Format.LensText == target.Label).ToList();
        if (onLens.Count == 0) return;
        var pick = onLens.FirstOrDefault(f => f.Format.Width == current.Width && f.Format.Height == current.Height && Math.Abs(f.Format.Fps - current.Fps) < 0.5)
                   ?? onLens.FirstOrDefault(f => f.Format.Height == current.Height)
                   ?? onLens[0];
        _phoneZoom = 1;
        _phoneSent.Clear(); // controls belong to the previous lens
        // If the new lens fails to open, go back to this one (see UpdateStatus).
        _lensSwitchFrom = IsCapturing && _format >= 0 ? _format : -1;
        OnFormatPicked(pick.Index, force: true);
    }

    // The phone refused the lens we switched to: mark it and resume on the previous one.
    private async void RevertLensSwitch(string error)
    {
        int back = _lensSwitchFrom;
        _lensSwitchFrom = -1;
        if (_format >= 0) _brokenLenses.Add(_formats[_format].Format.LensText);
        string failed = _format >= 0 ? _formats[_format].Format.LensText : "";
        int generation = _loadGeneration;
        OnFormatPicked(back);
        ShowError($"Không mở được {failed}, đang quay lại ống kính trước… {error}");
        // Some phones (MIUI) keep every camera busy for a moment after a refused open.
        await System.Threading.Tasks.Task.Delay(3000);
        if (generation != _loadGeneration || _format != back || IsCapturing) return; // the user moved on
        StartCapture();
        ShowError($"Không mở được {failed}, đã quay lại ống kính trước. {error}");
    }

    private void FacingButton_Click(object sender, RoutedEventArgs e)
    {
        var lens = CurrentLens();
        if (lens is null || _phoneCaps is null) return;
        var other = _phoneCaps.Lenses
            .Where(l => l.Facing != lens.Facing && _formats.Any(f => f.Format.LensText == l.Label))
            .OrderBy(l => l.Kind is "wide" or "front" or "trueDepth" ? 0 : 1)
            .FirstOrDefault();
        if (other is not null) SwitchLens(other);
    }

    private void FlashButton_Click(object sender, RoutedEventArgs e) => SendPhone("torch", FlashButton.IsChecked == true ? 1.0 : 0.0);

    private void BiasSlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        BiasText.Text = $"{e.NewValue:+0.0;-0.0;0} EV";
        if (_loading) return;
        SendPhone("exposureBias", Math.Round(e.NewValue, 1));
    }

    private async void OnCameraPicked(int camera)
    {
        if (camera == _camera) return;
        bool wasCapturing = IsCapturing;
        if (wasCapturing) StopCapture(); // a phone needs its own connection setup first
        await SelectCameraAsync(camera);
        SaveSourceAndRestart();
        if (wasCapturing && _format >= 0) StartCapture();
    }

    private void OnFormatPicked(int format, bool force = false)
    {
        if (format == _format && !force) return;
        SelectFormat(format);
        SaveSourceAndRestart();
    }

    private void SaveSourceAndRestart()
    {
        if (_camera >= 0 && _format >= 0)
        {
            var f = _formats[_format].Format;
            _settings.Camera = _cameras[_camera];
            // 0 means "native", so another camera also starts at its best mode.
            bool native = f.IsNative != 0;
            _settings.Width = native ? 0 : f.Width;
            _settings.Height = native ? 0 : f.Height;
            _settings.FpsMilli = native ? 0 : (uint)Math.Round(f.Fps * 1000);
            _settings.Save();
        }
        if (IsCapturing) StartCapture(); // apply right away
    }

    // ---- Capture -----------------------------------------------------------------

    private bool IsCapturing => (EngineState)Engine.GetStatus().State is EngineState.Running or EngineState.Starting or EngineState.Waiting;

    private void StartCapture()
    {
        if (_camera < 0 || _format < 0)
        {
            ShowError("Chưa có camera. Cắm camera rồi chọn \"Tìm lại camera\".");
            SetToggleQuietly(false);
            return;
        }
        ErrorBar.IsOpen = false;
        if (_isPhone)
        {
            // A new session starts with the phone's default controls.
            _phoneZoom = 1;
            _phoneSent.Clear();
        }
        int hr = Engine.Start(_camera, _format, CurrentImage());
        if (hr < 0)
        {
            ShowError($"Không khởi động được camera (0x{hr:X8}).");
            if (_isPhone && _lensSwitchFrom >= 0)
            {
                DispatcherQueue.TryEnqueue(() => RevertLensSwitch($"(0x{hr:X8})"));
                return;
            }
        }
        UpdateCameraOverlay();
        SetToggleQuietly(true);
        UpdateStatus();
    }

    private void StopCapture()
    {
        Engine.cs_stop();
        ClearPreview();
        SetToggleQuietly(false);
        UpdateStatus();
    }

    private void VcamToggle_Toggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        if (VcamToggle.IsOn) StartCapture();
        else StopCapture();
    }

    private void PlaceholderStart_Click(object sender, RoutedEventArgs e) => VcamToggle.IsOn = true;

    private void SetToggleQuietly(bool on)
    {
        bool was = _loading;
        _loading = true;
        VcamToggle.IsOn = on;
        _loading = was;
    }

    private void ShowError(string message)
    {
        ErrorBar.Severity = InfoBarSeverity.Error;
        ErrorBar.Title = "Không bật được camera";
        ErrorBar.Message = message;
        // Frame Server sometimes refuses every virtual camera after it was restarted
        // while in use; only a fresh Frame Server fixes that.
        if (message.Contains("0x80070005"))
        {
            var restart = new Button { Content = "Khởi động lại dịch vụ camera" };
            ToolTipService.SetToolTip(restart, "Cần quyền Admin. App khác đang dùng camera sẽ mất hình vài giây.");
            restart.Click += async (_, _) => await RestartCameraServiceAsync();
            ErrorBar.ActionButton = restart;
        }
        else
        {
            ErrorBar.ActionButton = null;
        }
        ErrorBar.IsOpen = true;
    }

    // Source lost but the virtual camera is still up: apps keep it selected.
    private void ShowWaiting(string reason)
    {
        string message = $"{reason.TrimEnd('.', ' ')}. Camera ảo vẫn bật, hình sẽ tự quay lại khi nguồn sẵn sàng.";
        if (ErrorBar.IsOpen && ErrorBar.Severity == InfoBarSeverity.Warning && ErrorBar.Message == message) return;
        ErrorBar.Severity = InfoBarSeverity.Warning;
        ErrorBar.Title = "Đang kết nối lại";
        ErrorBar.Message = message;
        ErrorBar.ActionButton = null;
        ErrorBar.IsOpen = true;
    }

    private async System.Threading.Tasks.Task RestartCameraServiceAsync()
    {
        var psi = new System.Diagnostics.ProcessStartInfo("powershell.exe",
            "-NoProfile -Command \"Stop-Service FrameServerMonitor -Force -ErrorAction SilentlyContinue; Restart-Service FrameServer -Force\"")
        {
            UseShellExecute = true,
            Verb = "runas",
            WindowStyle = System.Diagnostics.ProcessWindowStyle.Hidden,
        };
        try
        {
            using var p = System.Diagnostics.Process.Start(psi);
            if (p is not null) await p.WaitForExitAsync();
        }
        catch (System.ComponentModel.Win32Exception)
        {
            return; // UAC declined
        }
        ErrorBar.IsOpen = false;
        StartCapture();
    }

    // ---- Tools -------------------------------------------------------------------

    private void Mirror_Click(object sender, RoutedEventArgs e)
    {
        _settings.Mirror = MirrorButton.IsChecked == true;
        PushImage();
    }

    private void Flip_Click(object sender, RoutedEventArgs e)
    {
        _settings.Flip = FlipButton.IsChecked == true;
        PushImage();
    }

    private void Rotate_Click(object sender, RoutedEventArgs e)
    {
        _settings.Rotation = (_settings.Rotation + 90) % 360;
        PushImage();
    }

    private void AspectRadios_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || AspectRadios.SelectedIndex < 0) return;
        _settings.Aspect = AspectRadios.SelectedIndex;
        PushImage();
    }

    private void ZoomSlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        if (_loading) return;
        SetZoom(e.NewValue);
    }

    // Phones zoom on the phone (sensor crop / lens switching keeps detail);
    // webcams zoom digitally in our pipeline.
    private bool PhoneZoom => _isPhone && CurrentLens()?.Zoom is { } z && z.Max > z.Min;
    private double CurrentZoom => PhoneZoom ? _phoneZoom : _settings.Zoom;

    private void ConfigureZoomSlider()
    {
        bool was = _loading;
        _loading = true;
        if (PhoneZoom)
        {
            var z = CurrentLens()!.Zoom!;
            ZoomSlider.Minimum = z.Min;
            ZoomSlider.Maximum = Math.Min(z.Max, 10);
            _phoneZoom = Math.Clamp(_phoneZoom, ZoomSlider.Minimum, ZoomSlider.Maximum);
            ZoomSlider.Value = _phoneZoom;
        }
        else
        {
            ZoomSlider.Minimum = 1;
            ZoomSlider.Maximum = 4;
            ZoomSlider.Value = _settings.Zoom;
        }
        _loading = was;
        UpdateZoomText();
    }

    private void UpdateZoomText() => ZoomValueText.Text = $"{CurrentZoom:0.0}×";

    private void SetZoom(double zoom)
    {
        zoom = Math.Clamp(Math.Round(zoom, 2), ZoomSlider.Minimum, ZoomSlider.Maximum);
        bool was = _loading;
        _loading = true;
        ZoomSlider.Value = zoom;
        _loading = was;
        if (Math.Abs(zoom - CurrentZoom) < 0.001) return;
        if (PhoneZoom)
        {
            _phoneZoom = zoom;
            SendPhone("zoom", zoom);
            UpdateZoomText();
            return;
        }
        _settings.Zoom = zoom;
        ClampCenter();
        PushImage();
    }

    private void ZoomIn_Click(object sender, RoutedEventArgs e) => SetZoom(CurrentZoom * 1.25);
    private void ZoomOut_Click(object sender, RoutedEventArgs e) => SetZoom(CurrentZoom / 1.25);

    private void ResetFraming()
    {
        _settings.CenterX = 0.5;
        _settings.CenterY = 0.5;
        SetZoom(PhoneZoom ? Math.Max(1, ZoomSlider.Minimum) : 1.0);
        PushImage();
    }

    private void AdjustSlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        if (_loading) return;
        _settings.Brightness = (int)BrightnessSlider.Value;
        _settings.Contrast = (int)ContrastSlider.Value;
        _settings.Saturation = (int)SaturationSlider.Value;
        _settings.Temperature = (int)TemperatureSlider.Value;
        PushImage();
    }

    private void AdjustReset_Click(object sender, RoutedEventArgs e)
    {
        _loading = true;
        BrightnessSlider.Value = ContrastSlider.Value = SaturationSlider.Value = TemperatureSlider.Value = 0;
        _loading = false;
        _settings.Brightness = _settings.Contrast = _settings.Saturation = _settings.Temperature = 0;
        PushImage();
    }

    // Visible part of the oriented camera frame, as a fraction of each axis.
    private (double fx, double fy) VisibleFraction()
    {
        var s = Engine.GetStatus();
        if (s.Width == 0 || s.CameraWidth == 0) return (1 / _settings.Zoom, 1 / _settings.Zoom);
        bool swap = (_settings.Rotation + s.SourceRotation) % 180 != 0;
        double ow = swap ? s.CameraHeight : s.CameraWidth;
        double oh = swap ? s.CameraWidth : s.CameraHeight;
        return (s.Width / _settings.Zoom / ow, s.Height / _settings.Zoom / oh);
    }

    private void ClampCenter()
    {
        var (fx, fy) = VisibleFraction();
        _settings.CenterX = Math.Clamp(_settings.CenterX, fx / 2, 1 - fx / 2);
        _settings.CenterY = Math.Clamp(_settings.CenterY, fy / 2, 1 - fy / 2);
    }

    private void Preview_PointerPressed(object sender, PointerRoutedEventArgs e)
    {
        _pressPoint = e.GetCurrentPoint(PreviewImage).Position;
        _pressMoved = false;
        if (_settings.Zoom <= 1.001) return;
        _panning = PreviewImage.CapturePointer(e.Pointer);
        _panLast = _pressPoint;
    }

    private void Preview_PointerMoved(object sender, PointerRoutedEventArgs e)
    {
        if (!_panning || PreviewImage.ActualWidth < 1) return;
        var p = e.GetCurrentPoint(PreviewImage).Position;
        if (Math.Abs(p.X - _pressPoint.X) + Math.Abs(p.Y - _pressPoint.Y) > 6) _pressMoved = true;
        var (fx, fy) = VisibleFraction();
        // Dragging the picture right reveals what is to the left.
        _settings.CenterX -= (p.X - _panLast.X) / PreviewImage.ActualWidth * fx;
        _settings.CenterY -= (p.Y - _panLast.Y) / PreviewImage.ActualHeight * fy;
        _panLast = p;
        ClampCenter();
        if (IsCapturing) Engine.SetImage(CurrentImage()); // saved on release
    }

    private void Preview_PointerReleased(object sender, PointerRoutedEventArgs e)
    {
        var p = e.GetCurrentPoint(PreviewImage).Position;
        bool tap = !_pressMoved && Math.Abs(p.X - _pressPoint.X) + Math.Abs(p.Y - _pressPoint.Y) <= 6;
        if (_panning)
        {
            _panning = false;
            PreviewImage.ReleasePointerCapture(e.Pointer);
            _settings.Save();
        }
        if (tap) TapToFocus(p);
    }

    // A click on the picture focuses and meters there (phones). The point is
    // mapped back through our rotation/mirror/crop to the phone's sensor frame.
    private void TapToFocus(Windows.Foundation.Point p)
    {
        if (!_isPhone || !IsCapturing || PreviewImage.ActualWidth < 1) return;
        var lens = CurrentLens();
        if (lens is null) return;
        double u = p.X / PreviewImage.ActualWidth, v = p.Y / PreviewImage.ActualHeight;
        if (u < 0 || u > 1 || v < 0 || v > 1) return;

        // Undo crop/zoom: the preview shows [source rect] of the oriented frame.
        var (fx, fy) = VisibleFraction();
        double ox = Math.Clamp(_settings.CenterX - fx / 2, 0, 1 - fx);
        double oy = Math.Clamp(_settings.CenterY - fy / 2, 0, 1 - fy);
        u = ox + u * fx;
        v = oy + v * fy;
        // Undo mirror/flip, then the total clockwise rotation (ours + the phone's).
        if (_settings.Mirror) u = 1 - u;
        if (_settings.Flip) v = 1 - v;
        int rotation = (_settings.Rotation + Engine.GetStatus().SourceRotation) % 360;
        (double sx, double sy) = rotation switch
        {
            90 => (v, 1 - u),
            180 => (1 - u, 1 - v),
            270 => (1 - v, u),
            _ => (u, v),
        };
        var point = new System.Text.Json.Nodes.JsonArray(Math.Round(sx, 3), Math.Round(sy, 3));
        var msg = new System.Text.Json.Nodes.JsonObject { ["type"] = "set", ["focusPoint"] = point, ["exposurePoint"] = point.DeepClone() };
        if (lens.FocusModes.Contains("auto")) msg["focusMode"] = "auto";
        Engine.PhoneSend(msg.ToJsonString());
        ShowFocusRing(p);
    }

    private void ShowFocusRing(Windows.Foundation.Point p)
    {
        // Position in the preview host's coordinates (the canvas fills it).
        var t = PreviewImage.TransformToVisual(PreviewHost).TransformPoint(p);
        Canvas.SetLeft(FocusRing, t.X - FocusRing.Width / 2);
        Canvas.SetTop(FocusRing, t.Y - FocusRing.Height / 2);
        FocusRing.Opacity = 1;
        _focusRingTimer ??= DispatcherQueue.CreateTimer();
        _focusRingTimer.Interval = TimeSpan.FromMilliseconds(1200);
        _focusRingTimer.IsRepeating = false;
        _focusRingTimer.Tick -= HideFocusRing;
        _focusRingTimer.Tick += HideFocusRing;
        _focusRingTimer.Start();
    }

    private void HideFocusRing(DispatcherQueueTimer sender, object args) => FocusRing.Opacity = 0;

    private void Preview_PointerWheelChanged(object sender, PointerRoutedEventArgs e)
    {
        int delta = e.GetCurrentPoint(PreviewImage).Properties.MouseWheelDelta;
        SetZoom(CurrentZoom * Math.Pow(1.1, delta / 120.0));
        e.Handled = true;
    }

    private void Preview_DoubleTapped(object sender, DoubleTappedRoutedEventArgs e) => ResetFraming();

    // ---- Settings flyout -------------------------------------------------------------

    private void ThemeCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading) return;
        _settings.Theme = Math.Max(0, ThemeCombo.SelectedIndex);
        _settings.Save();
        ApplyTheme();
    }

    private void AutostartToggle_Toggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        try
        {
            Autostart.Set(AutostartToggle.IsOn);
        }
        catch (Exception ex)
        {
            ShowError("Không đổi được tùy chọn khởi động cùng Windows: " + ex.Message);
        }
    }

    private void CloseToTrayToggle_Toggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _settings.CloseToTray = CloseToTrayToggle.IsOn;
        _settings.Save();
    }

    private void InfoFlyout_Opening(object sender, object e)
    {
        _infoOpen = true;
        UpdateStatus();
    }

    private void InfoFlyout_Closed(object sender, object e) => _infoOpen = false;

    // ---- Preview -------------------------------------------------------------------

    private void PreviewHost_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        // Ask the engine for no more pixels than the panel shows.
        double scale = PreviewHost.XamlRoot?.RasterizationScale ?? 1.0;
        Engine.cs_set_preview_size((int)(e.NewSize.Width * scale), (int)(e.NewSize.Height * scale));
    }

    private unsafe void PumpPreview()
    {
        uint w, h;
        ulong id;
        // Waiting for the source: show the placeholder, not the last frame frozen.
        if (_lastState == EngineState.Waiting) return;
        Engine.cs_copy_preview(null, 0, &w, &h, &id); // size + id only
        if (id == _lastFrameId || w == 0 || h == 0) return;

        if (_bitmap is null || _bitmap.PixelWidth != w || _bitmap.PixelHeight != h)
        {
            _bitmap = new WriteableBitmap((int)w, (int)h);
            _bitmapPixels = BufferAccess.GetPointer(_bitmap.PixelBuffer);
            PreviewImage.Source = _bitmap;
        }
        if (Engine.cs_copy_preview((uint*)_bitmapPixels, (int)(w * h), &w, &h, &id) == 0) return;
        _lastFrameId = id;
        _bitmap.Invalidate();
        PreviewPlaceholder.Visibility = Visibility.Collapsed;
    }

    private unsafe void ClearPreview()
    {
        PreviewImage.Source = null;
        _bitmap = null;
        _bitmapPixels = null;
        _lastFrameId = 0;
        PreviewPlaceholder.Visibility = Visibility.Visible;
    }

    // ---- Status --------------------------------------------------------------------

    private void UpdateStatus()
    {
        var s = Engine.GetStatus();
        var state = (EngineState)s.State;
        bool running = state == EngineState.Running;
        _statusTicks++;

        if (state != _lastState)
        {
            if (state == EngineState.Failed)
            {
                string error = s.ErrorText;
                ShowError(error);
                Engine.cs_stop(); // join the finished worker
                ClearPreview();
                SetToggleQuietly(false);
                if (_isPhone && _lensSwitchFrom >= 0)
                    DispatcherQueue.TryEnqueue(() => RevertLensSwitch(error));
            }
            if (state == EngineState.Waiting)
            {
                ClearPreview();
                if (_isPhone && _lensSwitchFrom >= 0)
                {
                    // The new lens never delivered a frame: treat it like a failed open.
                    string error = s.ErrorText;
                    Engine.cs_stop();
                    SetToggleQuietly(false);
                    DispatcherQueue.TryEnqueue(() => RevertLensSwitch(error));
                }
            }
            if (state == EngineState.Running && _lastState == EngineState.Waiting && ErrorBar.Severity == InfoBarSeverity.Warning)
                ErrorBar.IsOpen = false; // the source is back
            if (state is EngineState.Stopped) ClearPreview();
            _lastState = state;
        }
        // While waiting the reason can change (app hidden -> phone unreachable…).
        if (state == EngineState.Waiting && _lensSwitchFrom < 0) ShowWaiting(s.ErrorText);
        // A phone says "started" before the camera has really opened: only
        // frames prove the new lens works.
        if (running && s.FramesCaptured > 0) _lensSwitchFrom = -1;

        (string text, string pill, Color dot) = state switch
        {
            EngineState.Starting => ("Đang khởi động…", "Đang khởi động", Color.FromArgb(255, 0xF7, 0xB5, 0x00)),
            EngineState.Running when s.ConsumerActive != 0 => ($"Đang được dùng · {s.Width} × {s.Height}", "Đang được dùng", Color.FromArgb(255, 0xE8, 0x11, 0x23)),
            EngineState.Running => ($"Sẵn sàng · {s.Width} × {s.Height}", "Đang phát", Color.FromArgb(255, 0x0F, 0xA9, 0x58)),
            EngineState.Failed => ("Lỗi", "Lỗi", Color.FromArgb(255, 0xE8, 0x11, 0x23)),
            EngineState.Waiting => ("Đang chờ nguồn hình…", "Đang chờ nguồn hình", Color.FromArgb(255, 0xF7, 0xB5, 0x00)),
            _ => ("Đang tắt", "Đã tắt", Color.FromArgb(255, 0x8A, 0x8A, 0x8A)),
        };
        _tray.SetTooltip($"Cam Studio · {pill}" + (running ? $" · {s.Width} × {s.Height}" : ""));

        // Hidden in the tray: the tooltip above is all anyone can see.
        if (!IsOnScreen) return;

        StatusText.Text = text;
        StatusDot.Fill = new SolidColorBrush(dot);
        LiveBadge.Visibility = running ? Visibility.Visible : Visibility.Collapsed;
        UpdateOverlayVisibility();
        LiveBadgeText.Text = $"{s.Width} × {s.Height}  ·  {s.Fps:0} fps";
        PlaceholderTitle.Text = state switch
        {
            EngineState.Starting => "Đang khởi động camera…",
            EngineState.Waiting => "Đang chờ nguồn hình… Camera ảo vẫn bật",
            _ => "Camera ảo đang tắt",
        };
        PlaceholderStart.Visibility = running || state is EngineState.Starting or EngineState.Waiting ? Visibility.Collapsed : Visibility.Visible;

        // The camera-usage list reads the registry: refresh it every 3 s, or
        // every tick while the details are open.
        if (!running) _consumers.Clear();
        else if (_infoOpen || _statusTicks % 3 == 0) _consumers = CameraUsage.AppsInUse();
        ConsumerBadge.Visibility = _consumers.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        ConsumerBadgeText.Text = string.Join(", ", _consumers);

        if (_infoOpen) UpdateInfo(s, running);
        if (_phoneOpen) UpdatePhoneReadout();
    }

    private void UpdateInfo(CsStatus s, bool running)
    {
        if (running)
        {
            string camFps = _format >= 0 ? $" · {_formats[_format].Format.Fps:0.##} fps" : "";
            InfoCamera.Text = $"{s.CameraWidth} × {s.CameraHeight}{camFps} · {Engine.FourccText(s.CameraFourcc)}";
            InfoOutput.Text = $"{s.Width} × {s.Height} · NV12";
            InfoFps.Text = $"{s.Fps:0.0} fps";
            InfoDropped.Text = $"{s.FramesDropped:N0} / {s.FramesCaptured:N0}";
            InfoProcess.Text = s.ProcessMs > 0 ? $"{s.ProcessMs:0.0} ms" : "0 ms (không có ai xem)";
            InfoConsumers.Text = _consumers.Count > 0 ? string.Join(", ", _consumers)
                               : s.ConsumerActive != 0 ? "Có (không rõ tên)" : "Chưa có ứng dụng nào";
        }
        else
        {
            InfoCamera.Text = InfoOutput.Text = InfoFps.Text = InfoDropped.Text = InfoProcess.Text = InfoConsumers.Text = "—";
        }

        _self.Refresh();
        var now = DateTime.UtcNow;
        var cpu = _self.TotalProcessorTime;
        double elapsed = (now - _lastCpuTime).TotalMilliseconds;
        if (elapsed > 0 && _lastCpu != TimeSpan.Zero)
        {
            double pct = (cpu - _lastCpu).TotalMilliseconds / elapsed / Environment.ProcessorCount * 100;
            InfoCpu.Text = $"CPU {pct:0.0} % · RAM {_self.PrivateMemorySize64 / (1024 * 1024)} MB";
        }
        _lastCpu = cpu;
        _lastCpuTime = now;
    }

    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(nint hwnd);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(nint hwnd);
}
