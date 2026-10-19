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
using Microsoft.UI.Xaml.Media;
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
    // The selected source by id. _camera is its index in the latest list, or -1 while it is not
    // listed (phone app hidden, cable out): it is found again when it comes back.
    private string? _selectedId;

    // Reads the engine's list into _cameras/_ids/_kinds and finds the selected source again by id.
    private void ReadCameraList()
    {
        string? currentId = _selectedId;
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
        BuildSourceList();
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

    // ---- The source list (left pane) ------------------------------------------------------------
    //
    // Phones, webcams, phone screens and a window on this PC, each with its connection state. The
    // selected row's second line follows the camera live (streaming, waiting, phone battery).

    private TextBlock? _selectedSubtitle;
    private string _selectedBase = "";
    private static readonly Regex TransportSuffix = new(@"\s*\(([^()]*)\)$");

    private void BuildSourceList()
    {
        SourceList.Children.Clear();
        _selectedSubtitle = null;
        var quick = QuickSources();

        // One card per device. A phone's routes (cable, Wi‑Fi) and its shared screen fold into the
        // phone's card, so a phone is listed once however many ways it is reachable.
        var devices = new List<(string Name, List<int> Cameras, List<int> Screens)>();
        for (int i = 0; i < _cameras.Count; i++)
        {
            if (_kinds[i] == 0) continue;
            string name = DeviceName(_cameras[i]);
            int d = devices.FindIndex(x => x.Name == name);
            if (d < 0)
            {
                devices.Add((name, new List<int>(), new List<int>()));
                d = devices.Count - 1;
            }
            (_kinds[i] == 2 ? devices[d].Screens : devices[d].Cameras).Add(i);
        }
        foreach (var (name, cameras, screens) in devices) SourceList.Children.Add(PhoneCard(name, cameras, screens, quick));

        for (int i = 0; i < _cameras.Count; i++)
        {
            if (_kinds[i] != 0) continue;
            int cam = i;
            var card = SourceCard("\uE960", ScreenTitle(_cameras[i]), IsBuiltIn(i) ? "Camera tích hợp" : "Webcam USB",
                i == _camera && _windowSource is null, null, _cameras[i] + QuickHint(quick, i));
            card.Click += (_, _) => OnCameraPicked(cam);
            SourceList.Children.Add(card);
        }

        bool windowSelected = _windowSource is not null;
        var windowCard = SourceCard("\uE7C4", windowSelected ? _windowSource!.Value.Title : "Cửa sổ trên máy tính",
            windowSelected ? "Cửa sổ trên máy tính" : "Trình duyệt, bài trình chiếu…", windowSelected, null,
            "Một cửa sổ trên máy tính làm nguồn của camera ảo");
        windowCard.Click += (_, _) => ShowWindowPicker(windowCard);
        SourceList.Children.Add(windowCard);
        UpdateSelectedSubtitle(Engine.GetStatus());
    }

    // "Redmi 9T · Màn hình (USB)" -> "Redmi 9T".
    private static string DeviceName(string name) => ScreenTitle(name).Replace(" · Màn hình", "");

    private static string QuickHint(List<int> quick, int cam) => quick.IndexOf(cam) is var n and >= 0 ? $"   Ctrl+{n + 1}" : "";

    private static string TransportOf(string name) =>
        TransportSuffix.Match(name) is { Success: true } m ? (m.Groups[1].Value == "USB" ? "Cáp USB" : m.Groups[1].Value) : "";

    private static string RouteLabel(string name) => TransportOf(name).Replace("Cáp ", "");

    private static string JoinParts(params string?[] parts) => string.Join(" · ", parts.Where(p => !string.IsNullOrEmpty(p)));

    private bool IsUsb(int cam) => _ids[cam].StartsWith("phone:usb:", StringComparison.Ordinal);

    // Lower is better: a connected route first, the cable before Wi‑Fi.
    private int RouteRank(int cam)
    {
        int rank = Engine.CameraState(cam, out _) switch
        {
            LinkState.Connected => 0,
            LinkState.Elsewhere => 2,
            LinkState.Connecting => 4,
            _ => 6,
        };
        return rank + (IsUsb(cam) ? 0 : 1);
    }

    // A phone: its name, the route it is used (or would be used) through, "USB"/"Wi‑Fi" chips when
    // it is reachable both ways, and its shared screen with "Mở cửa sổ" / "Làm camera".
    private Button PhoneCard(string name, List<int> cameras, List<int> screens, List<int> quick)
    {
        bool onCamera = _windowSource is null;
        int route = onCamera && cameras.Contains(_camera) ? _camera : cameras.OrderBy(RouteRank).DefaultIfEmpty(-1).First();
        int screen = onCamera && screens.Contains(_camera) ? _camera : screens.OrderBy(RouteRank).DefaultIfEmpty(-1).First();
        bool screenIsCamera = onCamera && screens.Contains(_camera);
        bool selected = onCamera && (route >= 0 && route == _camera || screenIsCamera);

        string subtitle, tooltip;
        Windows.UI.Color? dot;
        if (route >= 0)
        {
            var state = Engine.CameraState(route, out string error);
            // A phone added by IP has no name yet: the list shows its address as the name.
            string transport = TransportOf(_cameras[route]);
            subtitle = JoinParts(transport == name ? "Thêm theo IP" : transport, StateText(1, state));
            dot = DotColor(1, state);
            tooltip = (error.Length > 0 ? error : _cameras[route]) + QuickHint(quick, route);
        }
        else
        {
            var state = Engine.CameraState(screen, out string error);
            subtitle = "Chỉ chia sẻ màn hình";
            dot = state == LinkState.Unreachable ? DotColor(2, LinkState.Idle) : DotColor(2, state);
            tooltip = error.Length > 0 ? error : _cameras[screen];
        }

        var extras = new StackPanel { Spacing = 6, Margin = new Thickness(0, 8, 0, 0) };
        if (cameras.Count > 1)
        {
            var chips = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
            foreach (int c in cameras.OrderBy(c => IsUsb(c) ? 0 : 1))
            {
                int pick = c;
                var state = Engine.CameraState(c, out _);
                var chip = new ToggleButton
                {
                    Content = RouteLabel(_cameras[c]),
                    IsChecked = selected && c == route,
                    Style = Res<Style>("RouteChipStyle"),
                };
                ToolTipService.SetToolTip(chip, JoinParts(TransportOf(_cameras[c]), StateText(1, state)));
                chip.Click += (_, _) => OnCameraPicked(pick);
                chips.Children.Add(chip);
            }
            extras.Children.Add(chips);
        }
        if (screen >= 0) extras.Children.Add(ScreenRow(screen, screenIsCamera));

        var card = SourceCard("\uE8EA", name, subtitle, selected, dot, tooltip, extras.Children.Count > 0 ? extras : null);
        if (route >= 0)
        {
            int pick = route;
            card.Click += (_, _) => OnCameraPicked(pick);
        }
        else
        {
            int s = screen;
            card.Click += (_, _) => OpenScreenWindow(s);
        }
        return card;
    }

    // The phone's shared screen: it opens in its own window (to share that window in a meeting
    // app), or "Làm camera" makes it the virtual camera's source.
    private FrameworkElement ScreenRow(int screen, bool isCamera)
    {
        var state = Engine.CameraState(screen, out _);
        bool notShared = state == LinkState.Unreachable;
        bool open = _screenWindows.ContainsKey(_ids[screen]);
        string? what = isCamera ? "đang làm camera" : notShared ? "chưa chia sẻ" : open ? "đang mở cửa sổ" : StateText(2, state)?.ToLowerInvariant();
        var row = new Grid { ColumnSpacing = 8 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.Children.Add(new TextBlock
        {
            Text = JoinParts("Màn hình", what),
            Style = Res<Style>("SourceSubtitleStyle"), VerticalAlignment = VerticalAlignment.Center,
        });
        if (!isCamera)
        {
            int s = screen;
            var links = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2 };
            var window = new HyperlinkButton { Content = "Cửa sổ", Style = Res<Style>("CardLinkStyle") };
            ToolTipService.SetToolTip(window, "Mở trong cửa sổ riêng, để chia sẻ cửa sổ đó trong Discord, Zoom, Meet…");
            window.Click += (_, _) => OpenScreenWindow(s);
            links.Children.Add(window);
            if (!notShared)
            {
                var asCamera = new HyperlinkButton { Content = "Làm camera", Style = Res<Style>("CardLinkStyle") };
                ToolTipService.SetToolTip(asCamera, "Dùng màn hình này làm nguồn của camera ảo");
                asCamera.Click += (_, _) => UseScreenAsCamera(s);
                links.Children.Add(asCamera);
            }
            Grid.SetColumn(links, 1);
            row.Children.Add(links);
        }
        return row;
    }

    private static Windows.UI.Color? DotColor(int kind, LinkState state) => kind == 0 ? null : state switch
    {
        LinkState.Connected => Windows.UI.Color.FromArgb(255, 0x0F, 0xA9, 0x58),
        LinkState.Connecting or LinkState.NeedsPairing => Windows.UI.Color.FromArgb(255, 0xF7, 0xB5, 0x00),
        LinkState.Busy or LinkState.Unreachable => Windows.UI.Color.FromArgb(255, 0xE5, 0x48, 0x4D),
        _ => Windows.UI.Color.FromArgb(255, 0x8A, 0x8A, 0x8A),
    };

    // One card: an icon tile, the name and its state line, a state dot; extras (route chips, the
    // screen row) under them. Theme colours come from styles (App.xaml), so they follow the app's
    // own light/dark choice.
    private Button SourceCard(string glyph, string title, string subtitle, bool selected, Windows.UI.Color? dot, string tooltip,
                              FrameworkElement? extras = null)
    {
        var grid = new Grid { ColumnSpacing = 12 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var tile = new Border
        {
            Style = Res<Style>(selected ? "IconTileSelectedStyle" : "IconTileStyle"),
            Child = new FontIcon { Glyph = glyph, Style = Res<Style>(selected ? "TileGlyphSelectedStyle" : "TileGlyphStyle") },
        };
        grid.Children.Add(tile);
        var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center, Spacing = 1 };
        text.Children.Add(new TextBlock
        {
            Text = title, TextTrimming = TextTrimming.CharacterEllipsis, MaxLines = 1,
            FontWeight = selected ? Microsoft.UI.Text.FontWeights.SemiBold : Microsoft.UI.Text.FontWeights.Normal,
        });
        var second = new TextBlock
        {
            Text = subtitle, Style = Res<Style>("SourceSubtitleStyle"),
            Visibility = subtitle.Length > 0 ? Visibility.Visible : Visibility.Collapsed,
        };
        text.Children.Add(second);
        Grid.SetColumn(text, 1);
        grid.Children.Add(text);
        if (dot is { } c)
        {
            var e = new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 8, Height = 8, Fill = new SolidColorBrush(c), VerticalAlignment = VerticalAlignment.Center };
            Grid.SetColumn(e, 2);
            grid.Children.Add(e);
        }
        if (extras is not null)
        {
            Grid.SetRow(extras, 1);
            Grid.SetColumn(extras, 1);
            Grid.SetColumnSpan(extras, 2);
            grid.Children.Add(extras);
        }
        var button = new Button { Style = Res<Style>(selected ? "SourceCardSelectedStyle" : "SourceCardStyle"), Content = grid };
        if (selected)
        {
            _selectedSubtitle = second;
            _selectedBase = subtitle;
        }
        ToolTipService.SetToolTip(button, tooltip);
        return button;
    }

    // The selected row's second line: what the camera does with it right now.
    private void UpdateSelectedSubtitle(CsStatus s)
    {
        if (_selectedSubtitle is null) return;
        var state = (EngineState)s.State;
        string? live = state switch
        {
            EngineState.Running when s.NoSource == 0 && s.Standby != 0 => "Đang nghỉ",
            EngineState.Running when s.NoSource == 0 => s.ConsumerActive != 0 ? "Đang phát cho ứng dụng" : "Đang phát",
            EngineState.Waiting => "Đang chờ",
            EngineState.Starting => "Đang mở",
            _ => null,
        };
        string transport = _isPhone && _camera >= 0 && _camera < _cameras.Count ? TransportOf(_cameras[_camera]).Replace("Cáp ", "") : "";
        string text = live is null ? _selectedBase : JoinParts(live, transport, _isPhone ? PhoneBattery() : null);
        _selectedSubtitle.Text = text;
        _selectedSubtitle.Visibility = text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
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

    // The windows on this PC, listed when the row is clicked (cheap).
    private void ShowWindowPicker(FrameworkElement anchor)
    {
        var menu = new MenuFlyout { Placement = FlyoutPlacementMode.RightEdgeAlignedTop };
        var windows = Engine.ListWindows();
        foreach (var (handle, title) in windows)
        {
            var item = new RadioMenuFlyoutItem
            {
                Text = title.Length > 60 ? title[..57] + "…" : title,
                GroupName = "window",
                IsChecked = _windowSource?.Handle == handle,
            };
            ToolTipService.SetToolTip(item, title);
            long h = handle;
            string t = title;
            item.Click += (_, _) => OnWindowPicked(h, t);
            menu.Items.Add(item);
        }
        if (windows.Count == 0) menu.Items.Add(new MenuFlyoutItem { Text = "Không có cửa sổ nào", IsEnabled = false });
        menu.ShowAt(anchor);
    }

    // The window becomes the camera's source at once (the camera is turned on if it was off).
    private void OnWindowPicked(long handle, string title)
    {
        _autoPick = false;
        _paused = false;
        _selectedId = null;
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
        CameraButtonText.Text = title;
        SourceSubtitle.Text = "Cửa sổ trên máy tính";
        ErrorBar.IsOpen = false;
        SetToggleQuietly(true);
        BuildSourceList();
        UpdateStatus();
    }

    // ---- Quick switching: Ctrl+1…9, the tray menu ----------------------------

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
        BuildSourceList();
        foreach (int cam in QuickSources())
        {
            int pick = cam;
            yield return new TrayIcon.MenuItem(_cameras[cam], () => DispatcherQueue.TryEnqueue(() => OnCameraPicked(pick)),
                Checked: cam == _camera);
        }
    }

    // ---- Picking a source by itself ---------------------------------------------------------------
    //
    // The virtual camera is there from launch until the app exits. With no source it shows "no
    // signal"; a source feeds it as soon as there is one: the one the user picked last, else a
    // phone (cable, then Wi‑Fi), a USB webcam, the camera built into the PC.

    private bool _autoPick;    // the source was chosen automatically: a better one may take over
    private bool _paused;      // the user turned the picture off: the camera shows "no signal"
    private bool _picking;
    private bool _lastConsumer;
    private DateTime _waitingSince = DateTime.MaxValue;
    private DateTime _lostRefresh;
    private readonly Dictionary<string, bool> _builtIn = new(); // webcam id -> built into the PC
    // Sources whose formats did not load when picked automatically: not tried again before then.
    private readonly Dictionary<string, DateTime> _pickFailedUntil = new();

    // Lower is better; -1: not usable right now.
    private int Priority(int cam)
    {
        if (cam < 0 || cam >= _ids.Count || _kinds[cam] == 2) return -1; // screens open in a window
        if (_pickFailedUntil.TryGetValue(_ids[cam], out var until) && DateTime.UtcNow < until) return -1;
        // Elsewhere: this PC holds the phone the other way (Wi‑Fi / cable), so it can switch over;
        // e.g. launched before adb saw the cable, then the cable the user picked last turns up.
        if (_kinds[cam] == 1 && Engine.CameraState(cam, out _) is not (LinkState.Connected or LinkState.Elsewhere)) return -1;
        bool saved = _settings.CameraId.Length > 0 ? _ids[cam] == _settings.CameraId : _cameras[cam] == _settings.Camera;
        if (saved) return 0;
        if (_kinds[cam] == 1) return _ids[cam].StartsWith("phone:usb:", StringComparison.Ordinal) ? 1 : 2;
        return IsBuiltIn(cam) ? 4 : 3;
    }

    // except: a source not to consider (the lost one).
    private int BestSource(int except = -1)
    {
        int best = -1, bestPriority = int.MaxValue;
        for (int i = 0; i < _cameras.Count; i++)
        {
            if (i == except) continue;
            int p = Priority(i);
            if (p >= 0 && p < bestPriority) (best, bestPriority) = (i, p);
        }
        return best;
    }

    private bool IsBuiltIn(int cam)
    {
        string id = _ids[cam];
        if (_builtIn.TryGetValue(id, out bool known)) return known;
        _builtIn[id] = false; // until the lookup below answers
        DetectBuiltIn(id);
        return false;
    }

    // Cameras built into the PC say which side of it they are on; USB webcams do not.
    private async void DetectBuiltIn(string id)
    {
        if (!id.StartsWith("cam:", StringComparison.Ordinal)) return;
        try
        {
            var info = await Windows.Devices.Enumeration.DeviceInformation.CreateFromIdAsync(id["cam:".Length..]);
            bool builtIn = info.EnclosureLocation is { } where && where.Panel != Windows.Devices.Enumeration.Panel.Unknown;
            _builtIn[id] = builtIn;
            // Picked while this was unknown: a USB webcam may be the better choice now.
            if (builtIn && _autoPick && _selectedId == id)
                AutoPickAsync(none: false, lost: false, consumer: Engine.GetStatus().ConsumerActive != 0);
        }
        catch (Exception)
        {
        }
    }

    // The status tick (also while hidden in the tray): phones came or went, an app opened the
    // camera while it has no source, or the source has been lost for a while.
    private void MaybeAutoPick(CsStatus s)
    {
        var state = (EngineState)s.State;
        _waitingSince = state == EngineState.Waiting ? (_waitingSince == DateTime.MaxValue ? DateTime.UtcNow : _waitingSince) : DateTime.MaxValue;
        if (!_autoPick || _paused || _picking || _exiting) return;
        // A switch asked for and not taken over yet (the engine's status lags up to 200 ms).
        if (_expectSourceId != 0 && s.SourceId != _expectSourceId) return;
        // The camera itself failed: it is registered again by the retry in UpdateStatus.
        if (state is EngineState.Stopped or EngineState.Failed) return;
        bool consumer = s.ConsumerActive != 0;
        bool woke = consumer && !_lastConsumer && s.NoSource != 0; // an app opened the camera: look again
        _lastConsumer = consumer;
        bool changed = Engine.cs_sources_generation() != _sourcesGeneration;
        bool lost = DateTime.UtcNow - _waitingSince > TimeSpan.FromSeconds(10);
        // Webcams plugged or unplugged do not change the generation: look every 5 s while lost.
        bool relist = lost && DateTime.UtcNow - _lostRefresh > TimeSpan.FromSeconds(5);
        if (relist) _lostRefresh = DateTime.UtcNow;
        if (changed || woke || relist)
        {
            ReadCameraList();
            BuildSourceList();
        }
        bool none = s.NoSource != 0;
        if (changed || woke || none || lost) AutoPickAsync(none, lost, consumer);
    }

    // none: nothing feeds the camera (no source yet); lost: the source has been gone for more
    // than 10 s. Then any usable source will do, even during a call.
    private async void AutoPickAsync(bool none, bool lost, bool consumer)
    {
        if (!_autoPick || _paused || _picking || _exiting) return;
        // A lost source is not a candidate; otherwise the current one may stay.
        int best = BestSource(except: lost ? _camera : -1);
        if (best < 0 || (best == _camera && !none)) return;
        bool noSource = none || lost;
        if (!noSource)
        {
            // A better source than the one picked automatically: only while no app uses the
            // camera, never in the middle of a call.
            if (consumer) return;
            int current = Priority(_camera);
            if (current >= 0 && current <= Priority(best)) return;
        }
        _picking = true;
        string bestId = _ids[best];
        try
        {
            await SelectCameraAsync(best);
            if (_selectedId != bestId || !_autoPick || _paused) return; // something else was picked meanwhile
            if (_format < 0)
            {
                // Held by another app, or no usable mode: try others, this one again in 30 s.
                _pickFailedUntil[bestId] = DateTime.UtcNow.AddSeconds(30);
                return;
            }
            StartCapture();
        }
        finally
        {
            _picking = false;
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
        _settings.CameraId = _ids[_camera];
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
