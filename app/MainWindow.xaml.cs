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
    public sealed record LaunchOptions(string? Device, bool Tray, bool Updated = false, bool Restarted = false);

    private sealed record FormatItem(int Index, CsFormat Format)
    {
        public string Short => $"{Format.Width} × {Format.Height} · {FpsText} fps";
        public string Long => $"{Short} · {Engine.FourccText(Format.Fourcc)}" + (Format.IsNative != 0 ? "  (gốc)" : "");
        private string FpsText => Format.Fps.ToString(Format.Fps % 1 == 0 ? "0" : "0.##");
    }


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
    private DateTime _cameraRetryAt = DateTime.MaxValue; // re-register the camera after a failure
    private bool _noSource = true;                        // the camera shows "no signal" (no source, or paused)

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

        // Started with Windows (--tray): look for phones only after 20 s, so the PC boots first.
        // The camera itself is registered at once (cheap).
        // At login (--tray) phones are looked for after 20 s; a restart wants its picture back now.
        int hr = Engine.cs_init_delayed(options.Tray && !options.Restarted ? 20000 : 0);
        if (hr < 0) throw new InvalidOperationException($"Engine init failed 0x{hr:X8}");
        Engine.cs_set_output_size(_settings.OutputSize);
        Engine.cs_set_auto_frame(_settings.AutoFrame ? 1 : 0);

        SetupWindow();
        ApplyTheme();
        SetupSourceShortcuts();

        _tray = new TrayIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "CamStudio.ico"));
        _tray.Activated += () => DispatcherQueue.TryEnqueue(ShowFromTray);
        _tray.BuildMenu = BuildTrayMenu;
        SetupHotkeys();
        SetupPhoneAppCode();

        // 30 fps is plenty for an on-screen preview.
        _previewTimer = DispatcherQueue.CreateTimer();
        _previewTimer.Interval = TimeSpan.FromMilliseconds(33);
        _previewTimer.Tick += (_, _) => PumpPreview();
        _statusTimer = DispatcherQueue.CreateTimer();
        _statusTimer.Interval = TimeSpan.FromSeconds(1);
        _statusTimer.Tick += (_, _) => UpdateStatus();

        LoadSettingsIntoUi();
        _loading = false;
        SetupUpdater(options.Tray, options.Updated);

        _statusTimer.Start();
        UpdatePreviewActivity(visible: !options.Tray);
        UpdateStatus();
        InitAsync(options);
    }

    // The virtual camera exists from launch until the app exits, with or without a source:
    // with none, apps that open it see "no signal". A source feeds it as soon as there is one
    // (MaybeAutoPick, on every status tick: phones are found and connected in the background).
    private async void InitAsync(LaunchOptions options)
    {
        ApplyPausePicture(paused: false);
        Engine.cs_switch_none((int)_settings.LastWidth, (int)_settings.LastHeight); // the last size: no rebuild later
        SetToggleQuietly(true); // the picture is on: the first source feeds it
        if (options.Device is { } want)
        {
            // --device: that camera. A phone on Wi‑Fi takes a few seconds to be found.
            for (int attempt = 0; attempt < 6; attempt++)
            {
                int count = Engine.cs_refresh_cameras();
                bool found = false;
                for (int i = 0; i < count && !found; i++)
                    found = Engine.CameraName(i).Contains(want, StringComparison.OrdinalIgnoreCase);
                if (found) break;
                await System.Threading.Tasks.Task.Delay(1000);
            }
            await RefreshCamerasAsync(want); // also starts it
            return;
        }
        // The source picked last, else the best there is now; a better one may follow.
        _autoPick = true;
        ReadCameraList();
        BuildSourceList();
        if (BestSource() < 0) CameraButtonText.Text = "Đang tìm nguồn hình…";
        var s = Engine.GetStatus();
        AutoPickAsync(none: true, lost: false, consumer: s.ConsumerActive != 0);
    }

    // Cheap refresh of the source list: new phones show up without reloading the selected
    // camera's formats (the engine keeps them). Safe while streaming: sources are found again by id.
    private void RefreshCameraList()
    {
        ReadCameraList();
        BuildSourceList();
    }

    // ---- Window chrome ----------------------------------------------------------

    private void SetupWindow()
    {
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        // Windows 10 draws its own caption buttons and ignores the title bar settings below.
        if (AppWindowTitleBar.IsCustomizationSupported())
            AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Tall;
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "CamStudio.ico"));

        double scale = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this)) / 96.0;
        var area = DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Primary).WorkArea;
        AppWindow.Resize(new SizeInt32(Math.Min((int)(1320 * scale), area.Width), Math.Min((int)(820 * scale), area.Height)));
        if (AppWindow.Presenter is OverlappedPresenter p)
        {
            p.PreferredMinimumWidth = (int)(760 * scale);
            p.PreferredMinimumHeight = (int)(540 * scale);
        }
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
        if (!AppWindowTitleBar.IsCustomizationSupported()) return; // Windows 10
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
        if (on)
        {
            _previewTimer.Start();
        }
        else
        {
            _previewTimer.Stop();
            TrimMemory();
        }
    }

    // Hidden or minimized: give the memory the UI no longer needs back to Windows
    // (it pages back in, cheaply, when the window shows again).
    private static void TrimMemory()
    {
        GC.Collect();
        GC.WaitForPendingFinalizers();
        EmptyWorkingSet(System.Diagnostics.Process.GetCurrentProcess().Handle);
    }

    [System.Runtime.InteropServices.DllImport("psapi.dll")]
    private static extern bool EmptyWorkingSet(IntPtr process);

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
        Shutdown(); // Close() from code may not raise AppWindow.Closing
        Close();
    }

    private bool _shutDown;

    // engineTimeoutMs: how long the engine may take to stop (a restart does not wait for a stuck
    // one: the process ends anyway, and Windows frees the camera with it).
    private void Shutdown(int engineTimeoutMs = System.Threading.Timeout.Infinite)
    {
        if (_shutDown) return;
        _shutDown = true;
        _previewTimer.Stop();
        _statusTimer.Stop();
        CloseScreenWindows();
        if (_recording) Engine.cs_record_stop(); // a finished file, not a broken one
        _hotkeys?.Dispose();
        _tray.Dispose();
        if (engineTimeoutMs < 0) Engine.cs_shutdown();
        else System.Threading.Tasks.Task.Run(Engine.cs_shutdown).Wait(engineTimeoutMs);
    }

    private async void Restart_Click(object sender, RoutedEventArgs e)
    {
        if (_consumers.Count > 0 || Engine.GetStatus().ConsumerActive != 0)
        {
            var ask = new ContentDialog
            {
                XamlRoot = RootGrid.XamlRoot,
                Title = "Khởi động lại Cam Studio?",
                Content = $"{(_consumers.Count > 0 ? string.Join(", ", _consumers) : "Ứng dụng đang dùng camera")} sẽ mất hình vài giây. Sau đó bật lại camera trong ứng dụng đó.",
                PrimaryButtonText = "Khởi động lại",
                CloseButtonText = "Hủy",
                DefaultButton = ContentDialogButton.Primary,
                RequestedTheme = RootGrid.ActualTheme,
            };
            try
            {
                if (await ask.ShowAsync() != ContentDialogResult.Primary) return;
            }
            catch (Exception)
            {
                return; // another dialog is open
            }
        }
        RestartApp();
    }

    // Everything from scratch: cameras, phones, the virtual camera. The new process waits until
    // this one is gone (Program.Main, --after), else it would hand itself to this one and quit.
    public void RestartApp()
    {
        if (_exiting) return;
        string args = $"--after {Environment.ProcessId}" + (AppWindow.IsVisible ? "" : " --tray");
        try
        {
            using var _ = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(Environment.ProcessPath!, args)
            {
                UseShellExecute = false,
                WorkingDirectory = AppContext.BaseDirectory,
            });
        }
        catch (Exception ex)
        {
            ShowError("Không khởi động lại được: " + ex.Message);
            return;
        }
        _exiting = true;
        Shutdown(engineTimeoutMs: 4000);
        Environment.Exit(0);
    }

    private IReadOnlyList<TrayIcon.MenuItem?> BuildTrayMenu()
    {
        bool paused = _paused;
        var items = new List<TrayIcon.MenuItem?>
        {
            new("Mở Cam Studio", () => DispatcherQueue.TryEnqueue(ShowFromTray), IsDefault: true),
            null,
        };
        // Quick switching: the sources ready right now.
        var sources = TraySourceItems().ToList();
        if (sources.Count > 1)
        {
            items.AddRange(sources);
            items.Add(null);
        }
        items.AddRange(new List<TrayIcon.MenuItem?>
        {
            new((paused ? "Phát hình" : "Tạm dừng hình (camera ảo vẫn có)") + MenuShortcut("Pause"), () => DispatcherQueue.TryEnqueue(() => VcamToggle.IsOn = paused)),
            new("Lật ngang", () => DispatcherQueue.TryEnqueue(() => { MirrorButton.IsChecked = !_settings.Mirror; Mirror_Click(this, new RoutedEventArgs()); }), Checked: _settings.Mirror),
            null,
            new("Chụp ảnh" + MenuShortcut("Snapshot"), () => DispatcherQueue.TryEnqueue(() => _ = TakeSnapshotAsync())),
            new((_recording ? "Dừng ghi hình" : "Bắt đầu ghi hình") + MenuShortcut("Record"), () => DispatcherQueue.TryEnqueue(() => _ = ToggleRecordingAsync())),
            null,
            new("Khởi động cùng Windows", () => DispatcherQueue.TryEnqueue(() => AutostartToggle.IsOn = !AutostartToggle.IsOn), Checked: Autostart.IsEnabled),
            null,
        });
        var update = TrayUpdateItems().ToList();
        if (update.Count > 0)
        {
            items.AddRange(update);
            items.Add(null);
        }
        items.Add(new("Khởi động lại Cam Studio", () => DispatcherQueue.TryEnqueue(RestartApp)));
        items.Add(new("Thoát", () => DispatcherQueue.TryEnqueue(ExitApp)));
        return items;
    }

    // ---- Settings <-> UI ----------------------------------------------------------

    private void LoadSettingsIntoUi()
    {
        MirrorButton.IsChecked = _settings.Mirror;
        FlipButton.IsChecked = _settings.Flip;
        ShowAspect(_settings.Aspect);
        ZoomSlider.Value = _settings.Zoom;
        AutoFrameToggle.IsOn = _settings.AutoFrame;
        BrightnessSlider.Value = _settings.Brightness;
        ContrastSlider.Value = _settings.Contrast;
        SaturationSlider.Value = _settings.Saturation;
        TemperatureSlider.Value = _settings.Temperature;
        UpdateColorUi();
        ThemeCombo.SelectedIndex = _settings.Theme;
        AutostartToggle.IsOn = Autostart.IsEnabled;
        CloseToTrayToggle.IsOn = _settings.CloseToTray;
        OutputSizeCombo.SelectedIndex = _settings.OutputSize;
        LoadPauseSettingsIntoUi();
        RecordMicToggle.IsOn = _settings.RecordMicrophone;
        UpdateToolIndicators();
        UpdateAutoFrameHint();
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

    // The rotate button shows the angle; the zoom value its factor.
    private void UpdateToolIndicators()
    {
        RotateText.Text = $"{_settings.Rotation}°";
        UpdateZoomText();
    }

    // ---- Cameras and formats -----------------------------------------------------

    private async System.Threading.Tasks.Task RefreshCamerasAsync(string? prefer)
    {
        // While the camera runs, only the list: the running source stays selected.
        if (IsCapturing && string.IsNullOrEmpty(prefer))
        {
            RefreshCameraList();
            return;
        }
        string? current = _camera >= 0 && _camera < _cameras.Count ? _cameras[_camera] : null;
        ReadCameraList();

        string? want = !string.IsNullOrEmpty(prefer) ? prefer : current;
        int pick = _cameras.Count > 0 ? 0 : -1;
        bool found = false;
        if (!string.IsNullOrEmpty(want))
        {
            int byName = _cameras.FindIndex(c => c.Equals(want, StringComparison.OrdinalIgnoreCase));
            if (byName < 0) byName = _cameras.FindIndex(c => c.Contains(want, StringComparison.OrdinalIgnoreCase));
            if (byName < 0 && int.TryParse(want, out int idx) && idx < _cameras.Count) byName = idx;
            if (byName >= 0) pick = byName;
            found = byName >= 0;
        }
        if (IsCapturing || (!found && _autoPick))
        {
            // Streaming: only a match moves it (e.g. a phone just added by IP). Picking by
            // itself: the best source is found on the next status tick.
            if (IsCapturing && found && pick != _camera) OnCameraPicked(pick);
            BuildSourceList();
            return;
        }
        if (found) _autoPick = false; // the user's (or --device's) choice
        await SelectCameraAsync(pick);
        // The picture is on: feed the camera from it.
        if (!_paused && _format >= 0 && !IsCapturing) StartCapture();
    }

    private async System.Threading.Tasks.Task SelectCameraAsync(int index)
    {
        _windowSource = null;
        _camera = index;
        _selectedId = index >= 0 ? _ids[index] : null;
        _isPhone = index >= 0 && Engine.IsPhone(index);
        CameraButtonText.Text = index >= 0 ? ScreenTitle(_cameras[index]) : "Không tìm thấy camera";
        SourceSubtitle.Text = index < 0 ? "" : _isPhone ? TransportOf(_cameras[index]) : IsBuiltIn(index) ? "Camera tích hợp" : "Webcam USB";
        BuildSourceList();
        await LoadFormatsAsync();
    }

    private async System.Threading.Tasks.Task LoadFormatsAsync()
    {
        int generation = ++_loadGeneration;
        _formats.Clear();
        _phoneCaps = null;
        SelectFormat(-1);
        int cam = _camera;
        if (cam < 0) return;
        // The list may be re-read while this waits (status tick, menu): find the source by id after.
        string id = _ids[cam];
        string name = _cameras[cam];

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
                    pairing = PairingDialog(name, code);
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
        cam = _ids.IndexOf(id);
        if (cam < 0 || _selectedId != id) return;  // it left the list meanwhile

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

        // Restore the mode this source had last time; otherwise the last camera's (older
        // settings); otherwise native.
        int pick = _formats.Count > 0 ? 0 : -1;
        int mine = SavedFormat(cam);
        if (mine >= 0)
        {
            pick = mine;
        }
        else if (_settings.Width != 0 && _cameras[cam] == _settings.Camera)
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

    // "192.168.1.23", or "192.168.1.23:47400" when the phone shows another port (47321 was taken).
    private static bool ParsePhoneAddress(string text, out string host, out int port)
    {
        host = text.Trim();
        port = 47321;
        int colon = host.LastIndexOf(':');
        if (colon >= 0)
        {
            if (!int.TryParse(host[(colon + 1)..], out port) || port is <= 0 or > 65535) return false;
            host = host[..colon];
        }
        return host.Length > 0 && !host.Contains(' ');
    }

    private async System.Threading.Tasks.Task AddPhoneByIpAsync()
    {
        var box = new TextBox { PlaceholderText = "192.168.1.23", Header = "Địa chỉ IP hiện trong app Cam Studio trên điện thoại" };
        var problem = new TextBlock
        {
            Text = "Địa chỉ chưa đúng. Ví dụ: 192.168.1.23 (thêm :cổng nếu app hiện cổng khác 47321).",
            TextWrapping = TextWrapping.Wrap, Visibility = Visibility.Collapsed,
            Foreground = (Brush)Application.Current.Resources["SystemFillColorCriticalBrush"],
            Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
        };
        var dialog = new ContentDialog
        {
            XamlRoot = RootGrid.XamlRoot,
            Title = "Thêm điện thoại theo địa chỉ IP",
            Content = new StackPanel { Spacing = 8, Width = 360, Children = { box, problem } },
            PrimaryButtonText = "Thêm",
            CloseButtonText = "Hủy",
            DefaultButton = ContentDialogButton.Primary,
            RequestedTheme = RootGrid.ActualTheme,
        };
        // A wrong address keeps the dialog open and says why, under the field.
        dialog.PrimaryButtonClick += (sender, args) =>
        {
            if (ParsePhoneAddress(box.Text, out _, out _)) return;
            problem.Visibility = Visibility.Visible;
            args.Cancel = true;
        };
        box.TextChanged += (_, _) => problem.Visibility = Visibility.Collapsed;
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;
        ParsePhoneAddress(box.Text, out string host, out int port);
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
            var all = new MenuFlyoutSubItem { Text = $"Tất cả chế độ (nâng cao) · {list.Count}", Icon = new FontIcon { Glyph = Icons.More, FontFamily = (FontFamily)Application.Current.Resources["IconFont"] } };
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
            SourceSubtitle.Text = JoinParts(lens!.Label, _camera >= 0 && _camera < _cameras.Count ? TransportOf(_cameras[_camera]) : null);
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
            ShowFlashState();
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
            FacingButton.Visibility = FlashButton.Visibility = ExposureButton.Visibility = Visibility.Collapsed;
        }
        ConfigureZoomSlider();
        UpdateOverlayVisibility();
        if (_phoneOpen) BuildPhoneControls(); // another lens: other controls
    }

    // What this source has: the camera bar under the preview (a phone's lenses, front / back,
    // flash, exposure), zoom for any running source, the manual controls once the phone's caps
    // are known.
    private void UpdateOverlayVisibility()
    {
        var status = Engine.GetStatus();
        bool running = (EngineState)status.State == EngineState.Running && status.NoSource == 0;
        // A phone keeps its lens chips while stopped, so a lens can be picked
        // (or a failed one left) before starting.
        bool chips = _isPhone && LensChips.Children.Count > 0;
        bool tools = _isPhone && (FacingButton.Visibility == Visibility.Visible || FlashButton.Visibility == Visibility.Visible ||
                                  ExposureButton.Visibility == Visibility.Visible);
        CameraBar.Visibility = chips || tools ? Visibility.Visible : Visibility.Collapsed;
        BarDivider.Visibility = chips && tools ? Visibility.Visible : Visibility.Collapsed;
        ZoomGroup.Visibility = running ? Visibility.Visible : Visibility.Collapsed;
        ManualSection.Visibility = _isPhone && _phoneCaps is not null ? Visibility.Visible : Visibility.Collapsed;
        // Not UpdatePaneActivity: it calls UpdateStatus, which calls here.
        _phoneOpen = InspectorPane.Visibility == Visibility.Visible && ManualSection.IsExpanded && ManualSection.Visibility == Visibility.Visible;
    }

    // Native stays native (else 1× → Macro → 1× ends at the macro's 4:3 size); otherwise the
    // same resolution on the other lens when it has it, else that lens' native mode.
    private void SwitchLens(PhoneCaps.Lens target)
    {
        var current = _format >= 0 ? _formats[_format].Format : default;
        var onLens = _formats.Where(f => f.Format.LensText == target.Label).ToList();
        if (onLens.Count == 0) return;
        var native = onLens.FirstOrDefault(f => f.Format.IsNative != 0) ?? onLens[0];
        var pick = current.IsNative != 0 ? native
                   : onLens.FirstOrDefault(f => f.Format.Width == current.Width && f.Format.Height == current.Height && Math.Abs(f.Format.Fps - current.Fps) < 0.5)
                     ?? onLens.FirstOrDefault(f => f.Format.Height == current.Height)
                     ?? native;
        _phoneZoom = 1;
        _phoneSent.Clear(); // controls belong to the previous lens
        // If the new lens fails to open, go back to this one (see UpdateStatus).
        _lensSwitchFrom = IsCapturing && _format >= 0 ? _format : -1;
        _expectSourceId = 0;
        OnFormatPicked(pick.Index, force: true);
    }

    // The phone refused the lens we switched to: mark it and resume on the previous one.
    private async void RevertLensSwitch(string error)
    {
        int back = _lensSwitchFrom;
        if (back < 0) return; // already reverted
        _lensSwitchFrom = -1;
        if (_format >= 0) _brokenLenses.Add(_formats[_format].Format.LensText);
        string failed = _format >= 0 ? _formats[_format].Format.LensText : "";
        int generation = _loadGeneration;
        bool live = IsCapturing; // the camera is still up (waiting): switch straight back
        OnFormatPicked(back);
        if (live)
        {
            ShowError($"Không mở được {failed}, đã quay lại ống kính trước. {error}");
            return;
        }
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

    private void FlashButton_Click(object sender, RoutedEventArgs e)
    {
        SendPhone("torch", FlashButton.IsChecked == true ? 1.0 : 0.0);
        ShowFlashState();
    }

    // A filled flash while it is on (the iPhone camera's look).
    private void ShowFlashState() =>
        FlashIcon.FontFamily = (FontFamily)Application.Current.Resources[FlashButton.IsChecked == true ? "IconFontFilled" : "IconFont"];

    private void BiasSlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        BiasText.Text = $"{e.NewValue:+0.0;-0.0;0} EV";
        if (_loading) return;
        SendPhone("exposureBias", Math.Round(e.NewValue, 1));
    }

    // Another source: while the camera is on it moves there without being taken down (apps
    // using it keep it, frozen for a moment). A connected phone has its formats already.
    private async void OnCameraPicked(int camera)
    {
        // The same source again does nothing, unless it is waiting (e.g. "chọn lại điện thoại" to pair).
        if (camera < 0 || camera >= _cameras.Count ||
            (camera == _camera && _windowSource is null && _format >= 0 && (EngineState)Engine.GetStatus().State != EngineState.Waiting))
            return;
        _autoPick = false; // the user's choice: kept, never replaced by a "better" one
        string pickedId = _ids[camera];
        int previous = _camera;
        var previousWindow = _windowSource;
        bool wasCapturing = IsCapturing;
        await SelectCameraAsync(camera);
        if (_selectedId != pickedId) return; // another pick meanwhile
        if (_format < 0)
        {
            // Could not load it (pairing declined, phone gone): the camera still shows the
            // previous source, so show that one again.
            if (wasCapturing && previous >= 0 && previous < _cameras.Count)
            {
                string error = ErrorBar.Message;
                await SelectCameraAsync(previous);
                ShowError(error);
            }
            else if (wasCapturing && previousWindow is { } window)
            {
                _windowSource = window;
                _camera = -1;
                CameraButtonText.Text = window.Title;
                SourceSubtitle.Text = "Cửa sổ trên máy tính";
                FormatButtonText.Text = "Theo cỡ cửa sổ";
                BuildSourceList();
            }
            return;
        }
        SaveSourceAndRestart();
    }

    private void OnFormatPicked(int format, bool force = false)
    {
        if (format == _format && !force) return;
        SelectFormat(format);
        SaveSourceAndRestart();
    }

    private void SaveSourceAndRestart()
    {
        SaveSource();
        // Apply right away (unless the picture is turned off), keeping the virtual camera.
        if (!_paused) StartCapture();
    }

    // ---- Capture -----------------------------------------------------------------

    // A source feeds the camera (it may be waiting for that source to come back).
    private bool IsCapturing
    {
        get
        {
            var s = Engine.GetStatus();
            return ((EngineState)s.State is EngineState.Running or EngineState.Starting or EngineState.Waiting) && s.NoSource == 0;
        }
    }

    // The virtual camera is registered (with or without a source).
    private bool EngineUp => (EngineState)Engine.GetStatus().State is not (EngineState.Stopped or EngineState.Failed);

    // Feeds the camera from the selected source. The camera is already up (it lives as long as
    // the app): this switches, without taking it down.
    private void StartCapture()
    {
        if (_windowSource is { } window)
        {
            OnWindowPicked(window.Handle, window.Title);
            return;
        }
        if (_camera < 0 && _selectedId is not null)
        {
            // The chosen source is not listed right now (phone app hidden, cable out): it is
            // used again when it comes back.
            ShowWaiting("Nguồn đã chọn hiện không có");
            return;
        }
        if (_camera < 0 || _format < 0)
        {
            // Nothing picked yet: take the best source there is, now or when one turns up.
            _autoPick = true;
            int best = BestSource();
            if (best >= 0)
            {
                AutoPickAsync(none: true, lost: false, consumer: false);
                return;
            }
            ShowWaiting("Chưa có nguồn hình: cắm webcam, hoặc mở app Cam Studio trên điện thoại");
            return;
        }
        if (EngineUp)
        {
            SwitchCapture();
            SetToggleQuietly(true);
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
        if (hr > 0) _expectSourceId = hr;
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

    // The picture off: the source closes, the camera stays (apps keep it and see "no signal").
    private void StopCapture()
    {
        ApplyPausePicture(_paused);
        int id = Engine.cs_switch_none((int)_settings.LastWidth, (int)_settings.LastHeight);
        if (id > 0) _expectSourceId = id;
        _noSource = true;
        ClearPreview();
        SetToggleQuietly(false);
        UpdateStatus();
    }

    private void VcamToggle_Toggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _paused = !VcamToggle.IsOn;
        if (VcamToggle.IsOn) StartCapture();
        else StopCapture();
    }

    private void PlaceholderStart_Click(object sender, RoutedEventArgs e)
    {
        if (VcamToggle.IsOn) StartCapture(); // already on (e.g. after a failure): try again
        else VcamToggle.IsOn = true;
    }

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

    private void AspectChip_Click(object sender, RoutedEventArgs e)
    {
        int aspect = int.Parse((string)((FrameworkElement)sender).Tag);
        ShowAspect(aspect); // a chip clicked again stays on
        if (aspect == _settings.Aspect) return;
        _settings.Aspect = aspect;
        PushImage();
    }

    private void ShowAspect(int aspect)
    {
        foreach (var chip in AspectChips.Children.OfType<ToggleButton>())
            chip.IsChecked = (string)chip.Tag == aspect.ToString();
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
        ZoomHint.Text = PhoneZoom
            ? "Thu phóng trên điện thoại: giữ độ nét, tự đổi ống kính khi cần."
            : AutoFraming
                ? "Tự căn khung đang điều khiển thu phóng."
                : "Thu phóng trên máy tính. Kéo trên hình để chọn khung, nhấp đúp để đặt lại.";
        ZoomSlider.IsEnabled = !AutoFramingOwnsZoom;
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
        if (AutoFramingOwnsZoom) return;
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
        if (AutoFraming)
        {
            if (PhoneZoom) SetZoom(Math.Max(1, ZoomSlider.Minimum)); // the lens zoom stays the user's
            return;
        }
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
        UpdateColorUi();
        PushImage();
    }

    private void AdjustReset_Click(object sender, RoutedEventArgs e) => ApplyColor(0, 0, 0, 0);

    // A preset is just the four slider values, in its Tag: "brightness,contrast,saturation,temperature".
    private void Preset_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement { Tag: string tag }) return;
        int[] v = Array.ConvertAll(tag.Split(','), int.Parse);
        ApplyColor(v[0], v[1], v[2], v[3]);
    }

    private void ApplyColor(int brightness, int contrast, int saturation, int temperature)
    {
        bool was = _loading;
        _loading = true;
        BrightnessSlider.Value = brightness;
        ContrastSlider.Value = contrast;
        SaturationSlider.Value = saturation;
        TemperatureSlider.Value = temperature;
        _loading = was;
        _settings.Brightness = brightness;
        _settings.Contrast = contrast;
        _settings.Saturation = saturation;
        _settings.Temperature = temperature;
        UpdateColorUi();
        PushImage();
    }

    private static string Signed(int v) => v > 0 ? $"+{v}" : v < 0 ? $"−{-v}" : "0";

    // The values beside the sliders, the preset that matches (checked, and named in the folded
    // header), or "Tùy chỉnh".
    private void UpdateColorUi()
    {
        BrightnessValue.Text = Signed(_settings.Brightness);
        ContrastValue.Text = Signed(_settings.Contrast);
        SaturationValue.Text = _settings.Saturation <= -100 ? "Đen trắng" : Signed(_settings.Saturation);
        TemperatureValue.Text = _settings.Temperature switch
        {
            > 0 => $"Ấm {Signed(_settings.Temperature)}",
            < 0 => $"Lạnh {Signed(_settings.Temperature)}",
            _ => "Trung tính",
        };
        string current = $"{_settings.Brightness},{_settings.Contrast},{_settings.Saturation},{_settings.Temperature}";
        string? name = null;
        foreach (var chip in PresetGrid.Children.OfType<ToggleButton>())
        {
            bool match = (string)chip.Tag == current;
            chip.IsChecked = match;
            if (match) name = ToolTipService.GetToolTip(chip) as string;
        }
        ColorSummary.Text = name ?? "Tùy chỉnh";
    }

    // The zoom and centre the engine shows now: automatic framing's while it runs, else the user's.
    private (double Zoom, double CenterX, double CenterY) CurrentView()
    {
        if (AutoFraming && Engine.GetFraming() is { Active: not 0 } f) return (f.Zoom, f.CenterX, f.CenterY);
        return (_settings.Zoom, _settings.CenterX, _settings.CenterY);
    }

    // Visible part of the oriented camera frame at this zoom, as a fraction of each axis.
    private (double fx, double fy) VisibleFraction(double zoom)
    {
        var s = Engine.GetStatus();
        // The picture's own size: the output may be bigger, with black bars around it.
        uint pw = s.PictureWidth != 0 ? s.PictureWidth : s.Width, ph = s.PictureHeight != 0 ? s.PictureHeight : s.Height;
        if (pw == 0 || s.CameraWidth == 0) return (1 / zoom, 1 / zoom);
        bool swap = (_settings.Rotation + s.SourceRotation) % 180 != 0;
        double ow = swap ? s.CameraHeight : s.CameraWidth;
        double oh = swap ? s.CameraWidth : s.CameraHeight;
        return (pw / zoom / ow, ph / zoom / oh);
    }

    private void ClampCenter()
    {
        var (fx, fy) = VisibleFraction(_settings.Zoom);
        _settings.CenterX = Math.Clamp(_settings.CenterX, fx / 2, 1 - fx / 2);
        _settings.CenterY = Math.Clamp(_settings.CenterY, fy / 2, 1 - fy / 2);
    }

    private void Preview_PointerPressed(object sender, PointerRoutedEventArgs e)
    {
        _pressPoint = e.GetCurrentPoint(PreviewImage).Position;
        _pressMoved = false;
        if (_settings.Zoom <= 1.001 || AutoFraming) return;
        _panning = PreviewImage.CapturePointer(e.Pointer);
        _panLast = _pressPoint;
    }

    private void Preview_PointerMoved(object sender, PointerRoutedEventArgs e)
    {
        if (!_panning || PreviewImage.ActualWidth < 1) return;
        var p = e.GetCurrentPoint(PreviewImage).Position;
        if (Math.Abs(p.X - _pressPoint.X) + Math.Abs(p.Y - _pressPoint.Y) > 6) _pressMoved = true;
        var (fx, fy) = VisibleFraction(_settings.Zoom);
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

        // Undo crop/zoom: the preview shows [source rect] of the oriented frame (framing's while it runs).
        var view = CurrentView();
        var (fx, fy) = VisibleFraction(view.Zoom);
        double ox = Math.Clamp(view.CenterX - fx / 2, 0, 1 - fx);
        double oy = Math.Clamp(view.CenterY - fy / 2, 0, 1 - fy);
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

    // ---- Tự căn khung (automatic framing, done by the engine) ---------------------------

    // Framing runs for cameras only: a webcam or a phone's camera, not a shared screen or a window.
    private bool FramingSource => _windowSource is null && _camera >= 0 && _camera < _kinds.Count && _kinds[_camera] != 2;
    private bool AutoFraming => _settings.AutoFrame && FramingSource;
    // It then owns the PC zoom; a phone's lens zoom stays the user's.
    private bool AutoFramingOwnsZoom => AutoFraming && !PhoneZoom;

    private void AutoFrameToggle_Toggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _settings.AutoFrame = AutoFrameToggle.IsOn;
        _settings.Save();
        Engine.cs_set_auto_frame(_settings.AutoFrame ? 1 : 0);
        ConfigureZoomSlider();
        UpdateAutoFrameHint();
    }

    // Under the switch: who it follows, and how sharp the crop is ("Smart Zoom": the crop keeps
    // the source's own pixels; 720 lines or more look sharp in a call).
    private void UpdateAutoFrameHint()
    {
        ZoomSlider.IsEnabled = !AutoFramingOwnsZoom; // the source may have changed since
        // A short status line; the detail (the crop's size) in its tooltip.
        string text, detail = "";
        if (!_settings.AutoFrame)
            text = "";
        else if (!FramingSource)
            text = "Chỉ cho webcam và camera điện thoại";
        else
        {
            var f = Engine.GetFraming();
            var s = Engine.GetStatus();
            if (f.Available == 0)
            {
                text = "Windows thiếu nhận diện khuôn mặt";
                detail = "Bản Windows N cần cài Media Feature Pack.";
            }
            else if (f.Available < 0)
                text = "Lỗi nhận diện: tắt rồi bật lại";
            else if (f.Active == 0 || s.PictureWidth == 0 || s.PictureHeight == 0)
                text = "Chờ có hình";
            else
            {
                int cw = (int)Math.Round(s.PictureWidth / f.Zoom), ch = (int)Math.Round(s.PictureHeight / f.Zoom);
                string who = f.Faces switch { 0 => "Không thấy ai", _ => $"{f.Faces} người" };
                bool sharp = Math.Min(cw, ch) >= 720;
                text = $"{who} · {f.Zoom:0.0}×{(sharp ? " · nét" : "")}";
                detail = $"Dùng {cw} × {ch} của {s.PictureWidth} × {s.PictureHeight}.";
                if (!sharp && HasLargerMode(s)) detail += " Chọn độ phân giải lớn hơn (như 4K) để zoom nét hơn.";
                if (AutoFramingOwnsZoom)
                {
                    bool was = _loading;
                    _loading = true;
                    ZoomSlider.Value = Math.Clamp(f.Zoom, ZoomSlider.Minimum, ZoomSlider.Maximum);
                    _loading = was;
                    ZoomValueText.Text = $"{f.Zoom:0.0}×";
                }
            }
        }
        if (AutoFrameHint.Text != text) AutoFrameHint.Text = text;
        AutoFrameHint.Visibility = text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        ToolTipService.SetToolTip(AutoFrameHint, detail.Length > 0 ? detail : null);
    }

    // This source (this lens, for a phone) has a mode with more pixels than the one in use.
    private bool HasLargerMode(CsStatus s) =>
        LensFormats().Any(f => (long)f.Format.Width * f.Format.Height > (long)s.CameraWidth * s.CameraHeight);

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

    // ---- Panes: inspector tabs, showing/hiding it, settings ----------------------------------

    // A section (Expander) opened or closed: live values are only refreshed while their section
    // can be seen.
    private void Section_Expanding(Expander sender, ExpanderExpandingEventArgs args) => UpdatePaneActivity();
    private void Section_Collapsed(Expander sender, ExpanderCollapsedEventArgs args) => UpdatePaneActivity();

    private void UpdatePaneActivity()
    {
        bool open = InspectorPane.Visibility == Visibility.Visible;
        bool phoneWasOpen = _phoneOpen;
        _infoOpen = open && DetailsSection.IsExpanded;
        _phoneOpen = open && ManualSection.IsExpanded && ManualSection.Visibility == Visibility.Visible;
        if (_phoneOpen && !phoneWasOpen) BuildPhoneControls();
        if (!_phoneOpen) _phoneReadout = null;
        UpdateStatus();
    }

    // The status colours, one of each: ready / connected, waiting, live (an app uses the camera)
    // or an error, off. The header dot, the preview badge and the source cards share them.
    private static readonly Color StatusGreen = Color.FromArgb(255, 0x0F, 0xA9, 0x58);
    private static readonly Color StatusAmber = Color.FromArgb(255, 0xF7, 0xB5, 0x00);
    private static readonly Color StatusRed = Color.FromArgb(255, 0xE5, 0x48, 0x4D);
    private static readonly Color StatusGrey = Color.FromArgb(255, 0x8A, 0x8A, 0x8A);

    private bool _inspectorByUser; // the user showed or hid it: the window width no longer decides

    private void InspectorToggle_Click(object sender, RoutedEventArgs e)
    {
        _inspectorByUser = true;
        ShowInspector(InspectorToggle.IsChecked == true);
    }

    private void ShowInspector(bool show)
    {
        InspectorToggle.IsChecked = show;
        InspectorPane.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        UpdatePaneActivity();
    }

    // Narrow windows give the picture the room: the inspector folds away, then the source list
    // narrows.
    private void RootGrid_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        double w = e.NewSize.Width;
        SourcesColumn.Width = new GridLength(w < 980 ? 228 : 284);
        SettingsLabel.Visibility = w < 980 ? Visibility.Collapsed : Visibility.Visible; // the icon alone fits
        if (!_inspectorByUser) ShowInspector(w >= 1180);
    }

    // "\tCtrl + Alt + P": a Win32 menu shows the text after a tab on the right.
    private string MenuShortcut(string name) => HotkeyText(name) is { Length: > 0 } t ? "\t" + t : "";

    private async void Settings_Click(object sender, RoutedEventArgs e)
    {
        BuildHotkeyRows();
        SettingsDialog.XamlRoot = RootGrid.XamlRoot;
        try
        {
            await SettingsDialog.ShowAsync();
        }
        catch (Exception)
        {
            // Another dialog (pairing) is open: it comes first.
        }
    }

    private async void AddPhone_Click(object sender, RoutedEventArgs e) => await AddPhoneByIpAsync();

    private async void Refresh_Click(object sender, RoutedEventArgs e) => await RefreshCamerasAsync(null);

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
        // Waiting for the source, paused, or no source: show the placeholder, not the last frame
        // (the engine keeps it).
        if (_lastState == EngineState.Waiting || _paused || _noSource) return;
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
        bool running = state == EngineState.Running && s.NoSource == 0;
        _noSource = s.NoSource != 0;
        _statusTicks++;
        UpdateAutoFrameHint();

        if (state != _lastState)
        {
            if (state == EngineState.Failed)
            {
                // The camera itself could not be registered (a source failing never ends it).
                string error = s.ErrorText;
                ShowError(error);
                Engine.cs_stop(); // join the finished worker
                ClearPreview();
                _cameraRetryAt = DateTime.UtcNow.AddSeconds(10);
                if (_isPhone && _lensSwitchFrom >= 0)
                    DispatcherQueue.TryEnqueue(() => RevertLensSwitch(error));
            }
            if (state == EngineState.Waiting)
            {
                ClearPreview();
                if (_isPhone && _lensSwitchFrom >= 0)
                {
                    // The new lens never delivered a frame: treat it like a failed open. The
                    // camera stays up; the previous lens takes over again.
                    string error = s.ErrorText;
                    DispatcherQueue.TryEnqueue(() => RevertLensSwitch(error));
                }
            }
            if (state == EngineState.Running && _lastState == EngineState.Waiting && ErrorBar.Severity == InfoBarSeverity.Warning)
                ErrorBar.IsOpen = false; // the source is back
            if (state is EngineState.Stopped) ClearPreview();
            _lastState = state;
        }
        // The camera lives as long as the app: after a failure, register it again now and then
        // (an app may have let go of the old one).
        if (state is EngineState.Stopped or EngineState.Failed && !_exiting && DateTime.UtcNow >= _cameraRetryAt)
        {
            _cameraRetryAt = DateTime.UtcNow.AddSeconds(30);
            if (!_paused && (_camera >= 0 && _format >= 0 || _windowSource is not null)) StartCapture();
            else
            {
                ApplyPausePicture(_paused);
                Engine.cs_switch_none((int)_settings.LastWidth, (int)_settings.LastHeight);
            }
        }
        // Remember the camera's size: next launch publishes it at that size before any source.
        if (running && s.Width > 0 && (s.Width != _settings.LastWidth || s.Height != _settings.LastHeight))
        {
            _settings.LastWidth = s.Width;
            _settings.LastHeight = s.Height;
            _settings.Save();
        }
        MaybeAutoPick(s);
        // While waiting the reason can change (app hidden -> phone unreachable…).
        if (state == EngineState.Waiting && _lensSwitchFrom < 0) ShowWaiting(s.ErrorText);
        // A phone says "started" before the camera has really opened: only
        // frames prove the new lens works.
        if (running && s.FramesCaptured > 0 && (_expectSourceId == 0 || s.SourceId == _expectSourceId)) _lensSwitchFrom = -1;

        (string text, string pill, Color dot) = state switch
        {
            EngineState.Starting => ("Đang khởi động…", "Đang khởi động", StatusAmber),
            // Always there for apps; with no source they see "no signal".
            EngineState.Running when s.NoSource != 0 && _paused => ($"Đã tạm dừng · ứng dụng thấy {PauseSeen()}", "Đã tạm dừng hình", StatusGrey),
            EngineState.Running when s.NoSource != 0 => ("Chưa có nguồn hình · ứng dụng thấy “không có tín hiệu”", "Đang chờ nguồn hình", StatusAmber),
            // Standby: the camera is there for apps, the source sleeps until one opens it.
            EngineState.Running when s.Standby != 0 => ($"Sẵn sàng · chờ ứng dụng dùng · {s.Width} × {s.Height}", "Sẵn sàng (nguồn đang nghỉ, tự bật khi có ứng dụng dùng)", StatusGreen),
            EngineState.Running when s.ConsumerActive != 0 => ($"Đang được dùng · {s.Width} × {s.Height}", "Đang được dùng", StatusRed),
            EngineState.Running => ($"Sẵn sàng · {s.Width} × {s.Height}", "Đang phát", StatusGreen),
            EngineState.Failed => ("Lỗi", "Lỗi", StatusRed),
            EngineState.Waiting => ("Đang chờ nguồn hình…", "Đang chờ nguồn hình", StatusAmber),
            _ => ("Đang tắt", "Đã tắt", StatusGrey),
        };
        _tray.SetTooltip($"Cam Studio · {pill}" + (running ? $" · {s.Width} × {s.Height}" : "") + (_recording ? " · đang ghi hình" : ""));

        // Hidden in the tray: the tooltip above is all anyone can see.
        if (!IsOnScreen) return;
        if (_recording) UpdateRecordingUi();
        RefreshSourcesIfChanged();

        StatusText.Text = text;
        StatusDot.Fill = new SolidColorBrush(dot);
        LiveDot.Fill = StatusDot.Fill; // red only while an app uses the camera
        LiveBadge.Visibility = running ? Visibility.Visible : Visibility.Collapsed;
        UpdateOverlayVisibility();
        LiveBadgeText.Text = $"{s.Width} × {s.Height}  ·  {s.Fps:0} fps";
        PlaceholderIcon.Glyph = _paused ? Icons.VideoOffLarge
            : state == EngineState.Running && s.NoSource != 0 ? Icons.PhoneLarge : Icons.VideoLarge;
        PlaceholderTitle.Text = state switch
        {
            EngineState.Starting => "Đang khởi động camera…",
            EngineState.Waiting => "Đang chờ nguồn hình… Camera ảo vẫn bật",
            EngineState.Running when s.NoSource != 0 && _paused => $"Đã tạm dừng hình. Ứng dụng đang thấy {PauseSeen()}",
            EngineState.Running when s.NoSource != 0 => "Đang chờ nguồn hình… Camera ảo vẫn có trong các ứng dụng",
            _ => "Camera ảo chưa chạy",
        };
        PlaceholderStart.Visibility = _paused || state is EngineState.Stopped or EngineState.Failed ? Visibility.Visible : Visibility.Collapsed;
        // No source yet: how to get one.
        var howTo = state == EngineState.Running && s.NoSource != 0 && !_paused ? Visibility.Visible : Visibility.Collapsed;
        PlaceholderHint.Visibility = howTo;
        PlaceholderAdd.Visibility = howTo;
        PlaceholderQrRow.Visibility = howTo;
        UpdateSelectedSubtitle(s);
        // What the folded section holds, at a glance.
        DetailsSummary.Text = running ? $"{s.Fps:0} fps · {s.Width} × {s.Height}" : "";

        // The camera-usage list reads the registry: refresh it every 3 s, or
        // every tick while the details are open.
        if (!running) _consumers.Clear();
        else if (_infoOpen || _statusTicks % 3 == 0) _consumers = CameraUsage.RunningAppsInUse(); // not entries left by apps that were killed
        ConsumerBadge.Visibility = _consumers.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        ConsumerBadgeText.Text = string.Join(", ", _consumers);
        if (running && s.ConsumerActive != 0 && _consumers.Count > 0)
            StatusText.Text = $"{string.Join(", ", _consumers)} đang dùng · {s.Width} × {s.Height}";

        if (_infoOpen)
        {
            UpdateInfo(s, running);
            PhoneInfoSection.Visibility = _isPhone && running ? Visibility.Visible : Visibility.Collapsed;
            if (_isPhone) PhoneInfoText.Text = PhoneReadoutText();
        }
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
