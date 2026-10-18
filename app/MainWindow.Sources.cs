using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;
using CamStudio.Native;
using CamStudio.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Windows.System;

namespace CamStudio;

// Several sources at once: the engine keeps paired phones connected in the background, and
// picking another source moves the virtual camera there without taking it down (cs_switch).
// Sources are known by their engine id; list indices change on every refresh.
public sealed partial class MainWindow
{
    private readonly List<string> _ids = new();   // parallel to _cameras
    private readonly List<int> _kinds = new();    // 0 webcam, 1 phone camera, 2 phone screen
    private uint _sourcesGeneration;
    private int _expectSourceId;                   // the engine source a lens switch waits for
    private readonly Dictionary<string, ScreenWindow> _screenWindows = new(); // by source id
    private (long Handle, string Title)? _windowSource;  // a window on this PC is the camera's source

    // Reads the engine's list into _cameras/_ids/_kinds and finds the current source again by id.
    private void ReadCameraList()
    {
        string? currentId = _camera >= 0 && _camera < _ids.Count ? _ids[_camera] : null;
        _cameras.Clear();
        _ids.Clear();
        _kinds.Clear();
        int count = Engine.cs_refresh_cameras();
        for (int i = 0; i < count; i++)
        {
            _cameras.Add(Engine.CameraName(i));
            _ids.Add(Engine.CameraId(i));
            _kinds.Add(Engine.cs_camera_kind(i));
        }
        _camera = currentId is null ? -1 : _ids.IndexOf(currentId);
        _sourcesGeneration = Engine.cs_sources_generation();
    }

    // The status tick: phones connected or dropped in the background. Only the row; the menu is
    // rebuilt when it opens.
    private void RefreshSourcesIfChanged()
    {
        if (Engine.cs_sources_generation() == _sourcesGeneration) return;
        ReadCameraList();
        UpdateSourceRow();
    }

    private static string? StateText(int kind, LinkState state) => kind == 0 ? null : state switch
    {
        LinkState.Connected => "Đã kết nối",
        LinkState.Connecting => "Đang kết nối",
        LinkState.NeedsPairing => "Cần ghép đôi",
        LinkState.Busy => "Bận với PC khác",
        LinkState.Unreachable => "Không kết nối được",
        LinkState.Elsewhere => "Đang nối qua đường khác",
        _ => null,
    };

    private void BuildCameraMenu()
    {
        CameraMenu.Items.Clear();
        for (int i = 0; i < _cameras.Count; i++)
        {
            if (_kinds[i] == 2) continue; // shared screens: their own section below
            int cam = i;
            var state = Engine.CameraState(i, out string error);
            string? note = StateText(_kinds[i], state);
            var item = new RadioMenuFlyoutItem
            {
                Text = note is null ? _cameras[i] : $"{_cameras[i]}  ·  {note}",
                GroupName = "camera",
                IsChecked = i == _camera,
                Icon = new FontIcon { Glyph = _kinds[i] != 0 ? "" : "" },
            };
            if (error.Length > 0) ToolTipService.SetToolTip(item, error);
            item.Click += (_, _) => OnCameraPicked(cam);
            CameraMenu.Items.Add(item);
        }
        if (_cameras.Count > 0) CameraMenu.Items.Add(new MenuFlyoutSeparator());
        AddScreenItems();
        AddWindowItems();
        var refresh = new MenuFlyoutItem { Text = "Tìm lại camera và điện thoại", Icon = new FontIcon { Glyph = "" } };
        refresh.Click += async (_, _) => await RefreshCamerasAsync(null);
        CameraMenu.Items.Add(refresh);
        var addPhone = new MenuFlyoutItem { Text = "Thêm điện thoại theo địa chỉ IP…", Icon = new FontIcon { Glyph = "" } };
        addPhone.Click += async (_, _) => await AddPhoneByIpAsync();
        CameraMenu.Items.Add(addPhone);
    }

    // ---- Phone screens: a window of their own --------------------------------------------------

    // A shared screen opens in its own window (to share in a meeting app); it can still be the
    // virtual camera's source instead.
    private void AddScreenItems()
    {
        bool any = false;
        for (int i = 0; i < _cameras.Count; i++)
        {
            if (_kinds[i] != 2) continue;
            if (!any) CameraMenu.Items.Add(new MenuFlyoutItem { Text = "Màn hình điện thoại", IsEnabled = false });
            any = true;
            int cam = i;
            var state = Engine.CameraState(i, out string error);
            string? note = StateText(_kinds[i], state);
            bool open = _screenWindows.ContainsKey(_ids[i]);
            var window = new MenuFlyoutItem
            {
                Text = (note is null ? _cameras[i] : $"{_cameras[i]}  ·  {note}") + (open ? "  ·  đang mở" : ""),
                Icon = new FontIcon { Glyph = "\uE737" },
            };
            ToolTipService.SetToolTip(window, error.Length > 0 ? error : "Mở trong cửa sổ riêng, để chia sẻ cửa sổ này trong Discord, Zoom, Meet…");
            window.Click += (_, _) => OpenScreenWindow(cam);
            CameraMenu.Items.Add(window);
            var asCamera = new RadioMenuFlyoutItem
            {
                Text = "      Dùng làm camera ảo",
                GroupName = "camera",
                IsChecked = i == _camera,
            };
            asCamera.Click += (_, _) => UseScreenAsCamera(cam);
            CameraMenu.Items.Add(asCamera);
        }
        if (any) CameraMenu.Items.Add(new MenuFlyoutSeparator());
    }

    // "iPhone · Màn hình (Wi‑Fi)" -> "iPhone · Màn hình": the title a meeting app lists.
    private static string ScreenTitle(string name) => Regex.Replace(name, @"\s*\([^()]*\)$", "");

    private void OpenScreenWindow(int cam)
    {
        if (cam < 0 || cam >= _ids.Count) return;
        string id = _ids[cam];
        if (_screenWindows.TryGetValue(id, out var existing))
        {
            existing.Activate();
            return;
        }
        if (IsCapturing && cam == _camera)
        {
            // One connection per screen: it cannot feed the camera and a window at once.
            ShowError("Màn hình này đang là nguồn của camera ảo. Chọn nguồn khác cho camera ảo rồi mở lại cửa sổ.");
            return;
        }
        var window = new ScreenWindow(cam, id, ScreenTitle(_cameras[cam]));
        _screenWindows[id] = window;
        window.Closed += (_, _) => _screenWindows.Remove(id);
        window.Activate();
    }

    private void UseScreenAsCamera(int cam)
    {
        if (cam < 0 || cam >= _ids.Count) return;
        if (_screenWindows.TryGetValue(_ids[cam], out var window)) window.Close(); // its connection moves to the camera
        OnCameraPicked(cam);
    }

    private void CloseScreenWindows()
    {
        foreach (var window in _screenWindows.Values.ToList()) window.Close();
        _screenWindows.Clear();
    }

    // ---- A window on this PC as the camera's source ----------------------------------------------

    private void AddWindowItems()
    {
        var sub = new MenuFlyoutSubItem { Text = "Cửa sổ trên máy tính", Icon = new FontIcon { Glyph = "\uE7C4" } };
        var windows = Engine.ListWindows(); // cheap: listed each time the menu opens
        foreach (var (handle, title) in windows)
        {
            var item = new RadioMenuFlyoutItem
            {
                Text = title.Length > 60 ? title[..57] + "…" : title,
                GroupName = "camera",
                IsChecked = _windowSource?.Handle == handle,
            };
            ToolTipService.SetToolTip(item, title);
            long h = handle;
            string t = title;
            item.Click += (_, _) => OnWindowPicked(h, t);
            sub.Items.Add(item);
        }
        if (windows.Count == 0) sub.Items.Add(new MenuFlyoutItem { Text = "Không có cửa sổ nào", IsEnabled = false });
        CameraMenu.Items.Add(sub);
        CameraMenu.Items.Add(new MenuFlyoutSeparator());
    }

    // The window becomes the camera's source at once (the camera is turned on if it was off).
    private void OnWindowPicked(long handle, string title)
    {
        int id = Engine.SwitchWindow(handle, CurrentImage());
        if (id < 0)
        {
            ShowError($"Không quay được cửa sổ này (0x{id:X8}).");
            return;
        }
        _windowSource = (handle, title);
        _expectSourceId = id;
        _lensSwitchFrom = -1;
        ++_loadGeneration; // a camera's formats still loading are no longer wanted
        _camera = -1;
        _isPhone = false;
        _phoneCaps = null;
        _formats.Clear();
        SelectFormat(-1);
        FormatButtonText.Text = "Theo cỡ cửa sổ";
        CameraButtonText.Text = "Cửa sổ: " + title;
        ErrorBar.IsOpen = false;
        SetToggleQuietly(true);
        UpdateSourceRow();
        UpdateStatus();
    }

    // ---- Quick switching: the source row, Ctrl+1…9, the tray menu ----------------------------

    // Sources that can take over right now (webcams, connected phones), plus the current one.
    private List<int> QuickSources()
    {
        var list = new List<int>();
        for (int i = 0; i < _cameras.Count && list.Count < 9; i++)
        {
            if (_kinds[i] == 2) continue; // a shared screen opens in its own window
            if (i == _camera || Engine.CameraState(i, out _) == LinkState.Connected) list.Add(i);
        }
        return list;
    }

    private void UpdateSourceRow()
    {
        SourceRow.Children.Clear();
        var list = QuickSources();
        if (list.Count < 2)
        {
            SourceRow.Visibility = Visibility.Collapsed;
            return;
        }
        SourceRow.Visibility = Visibility.Visible;
        for (int n = 0; n < list.Count; n++)
        {
            int cam = list[n];
            var button = new ToggleButton
            {
                Content = new TextBlock { Text = _cameras[cam], MaxWidth = 150, TextTrimming = TextTrimming.CharacterEllipsis },
                IsChecked = cam == _camera,
                Padding = new Thickness(10, 4, 10, 5),
            };
            ToolTipService.SetToolTip(button, $"{_cameras[cam]} (Ctrl+{n + 1})");
            button.Click += (_, _) =>
            {
                button.IsChecked = cam == _camera; // the pick decides, not the toggle
                OnCameraPicked(cam);
            };
            SourceRow.Children.Add(button);
        }
    }

    private void SetupSourceShortcuts()
    {
        for (int n = 1; n <= 9; n++)
        {
            int number = n;
            var accelerator = new KeyboardAccelerator
            {
                Key = (VirtualKey)((int)VirtualKey.Number1 + n - 1),
                Modifiers = VirtualKeyModifiers.Control,
            };
            accelerator.Invoked += (_, e) =>
            {
                e.Handled = true;
                var list = QuickSources();
                if (number - 1 < list.Count) OnCameraPicked(list[number - 1]);
            };
            RootGrid.KeyboardAccelerators.Add(accelerator);
        }
        RootGrid.KeyboardAcceleratorPlacementMode = KeyboardAcceleratorPlacementMode.Hidden;
    }

    private IEnumerable<TrayIcon.MenuItem> TraySourceItems()
    {
        ReadCameraList(); // the window may have been hidden a long time
        foreach (int cam in QuickSources())
        {
            int pick = cam;
            yield return new TrayIcon.MenuItem(_cameras[cam], () => DispatcherQueue.TryEnqueue(() => OnCameraPicked(pick)),
                Checked: cam == _camera);
        }
    }

    // ---- Moving the virtual camera ------------------------------------------------------------

    // The camera is on: move it to the selected source and mode, keeping it registered.
    private void SwitchCapture()
    {
        if (_camera < 0 || _format < 0) return;
        if (_isPhone)
        {
            // A new phone session starts with the phone's default controls.
            _phoneZoom = 1;
            _phoneSent.Clear();
        }
        int id = Engine.Switch(_camera, _format, CurrentImage());
        if (id < 0)
        {
            ShowError($"Không chuyển được nguồn (0x{id:X8}).");
            return;
        }
        _expectSourceId = id;
        ErrorBar.IsOpen = false;
        UpdateCameraOverlay();
        UpdateStatus();
    }

    // Remembers the source and its mode: the next start uses the source, and coming back to a
    // source picks the mode it had.
    private void SaveSource()
    {
        if (_camera < 0 || _format < 0) return;
        var f = _formats[_format].Format;
        // 0 means "native", so another camera also starts at its best mode.
        bool native = f.IsNative != 0;
        _settings.Camera = _cameras[_camera];
        _settings.Width = native ? 0 : f.Width;
        _settings.Height = native ? 0 : f.Height;
        _settings.FpsMilli = native ? 0 : (uint)Math.Round(f.Fps * 1000);
        _settings.Modes[_ids[_camera]] = $"{f.LensText}|{_settings.Width}|{_settings.Height}|{_settings.FpsMilli}";
        _settings.Save();
    }

    // The mode saved for this source, or -1.
    private int SavedFormat(int cam)
    {
        if (cam < 0 || cam >= _ids.Count || !_settings.Modes.TryGetValue(_ids[cam], out var saved)) return -1;
        string[] parts = saved.Split('|');
        if (parts.Length < 4 ||
            !uint.TryParse(parts[^3], out uint w) || !uint.TryParse(parts[^2], out uint h) || !uint.TryParse(parts[^1], out uint fpsMilli))
            return -1;
        string lens = string.Join("|", parts[..^3]);
        var onLens = _formats.Where(f => lens.Length == 0 || f.Format.LensText == lens).ToList();
        if (onLens.Count == 0) return -1;
        var match = w == 0
            ? onLens.FirstOrDefault(f => f.Format.IsNative != 0) ?? onLens[0]
            : onLens.FirstOrDefault(f => f.Format.Width == w && f.Format.Height == h && (uint)Math.Round(f.Format.Fps * 1000) == fpsMilli);
        return match?.Index ?? -1;
    }

    private void OutputSizeCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || OutputSizeCombo.SelectedIndex < 0) return;
        _settings.OutputSize = OutputSizeCombo.SelectedIndex;
        _settings.Save();
        Engine.cs_set_output_size(_settings.OutputSize);
    }
}
