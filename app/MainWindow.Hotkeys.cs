using System;
using System.Collections.Generic;
using System.Linq;
using CamStudio.Services;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.System;
using Windows.UI.Core;

namespace CamStudio;

// System-wide shortcuts (Settings › Phím tắt): they work while the window is hidden and a meeting
// app is in front, which Ctrl+1…9 (window only) cannot. Ctrl+Alt+letter by default; each can be
// changed or turned off. With the window hidden, a silent notification says what happened.
public sealed partial class MainWindow
{
    private sealed record HotkeyAction(int Id, string Name, string Label, int Default, Action Run);

    private GlobalHotkeys? _hotkeys;
    private readonly HashSet<int> _hotkeyConflicts = new(); // ids another app already owns
    private HotkeyAction? _capturing;                        // the row waiting for a new combination

    private List<HotkeyAction> HotkeyActions() => new()
    {
        new(1, "Pause", "Tạm dừng / phát hình", Ctrl_Alt('P'), TogglePauseFromHotkey),
        new(2, "NextSource", "Chuyển sang nguồn kế tiếp", Ctrl_Alt('N'), NextSourceFromHotkey),
        new(3, "Snapshot", "Chụp ảnh", Ctrl_Alt('S'), () => _ = TakeSnapshotAsync()),
        new(4, "Record", "Bắt đầu / dừng ghi hình", Ctrl_Alt('R'), () => _ = ToggleRecordingAsync()),
        new(5, "Background", "Bật / tắt hiệu ứng nền", Ctrl_Alt('B'), ToggleBackgroundFromHotkey),
    };

    private static int Ctrl_Alt(char key) => GlobalHotkeys.Pack(GlobalHotkeys.ModControl | GlobalHotkeys.ModAlt, key);

    private int HotkeyOf(HotkeyAction a) => _settings.Hotkeys.TryGetValue(a.Name, out int packed) ? packed : a.Default;

    private void SetupHotkeys()
    {
        _hotkeys = new GlobalHotkeys();
        _hotkeys.Pressed += id => HotkeyActions().FirstOrDefault(a => a.Id == id)?.Run();
        RegisterHotkeys();
    }

    private void RegisterHotkeys()
    {
        if (_hotkeys is null) return;
        _hotkeyConflicts.Clear();
        foreach (var a in HotkeyActions())
            if (!_hotkeys.Register(a.Id, HotkeyOf(a))) _hotkeyConflicts.Add(a.Id);
    }

    // "Ctrl + Alt + P" for the tray menu and tooltips; "" when off.
    private string HotkeyText(string name)
    {
        var a = HotkeyActions().FirstOrDefault(x => x.Name == name);
        int packed = a is null ? 0 : HotkeyOf(a);
        return packed == 0 || _hotkeyConflicts.Contains(a!.Id) ? "" : GlobalHotkeys.Describe(packed);
    }

    // Hidden in the tray: nothing on screen shows the change, so a silent notification does.
    private void HotkeyFeedback(string title, string text = "")
    {
        if (!IsOnScreen) _tray.ShowBalloon(title, text, silent: true);
    }

    private void TogglePauseFromHotkey()
    {
        bool pause = VcamToggle.IsOn;
        VcamToggle.IsOn = !pause; // Toggled starts or stops the picture
        HotkeyFeedback(pause ? "Đã tạm dừng hình" : "Đã phát hình lại",
            pause ? "Ứng dụng đang dùng camera thấy hình tạm dừng." : "");
    }

    private void NextSourceFromHotkey()
    {
        ReadCameraList(); // the window may have been hidden a long time
        BuildSourceList();
        var list = QuickSources();
        if (list.Count == 0)
        {
            HotkeyFeedback("Không có nguồn nào khác", "Mở app Cam Studio trên điện thoại hoặc cắm webcam.");
            return;
        }
        int at = list.IndexOf(_camera);
        int next = list[(at + 1) % list.Count];
        if (next == _camera && _windowSource is null)
        {
            HotkeyFeedback("Không có nguồn nào khác");
            return;
        }
        OnCameraPicked(next);
        HotkeyFeedback("Nguồn hình: " + ScreenTitle(_cameras[next]));
    }

    // ---- Settings › Phím tắt ---------------------------------------------------------------

    private void BuildHotkeyRows()
    {
        HotkeyRows.Children.Clear();
        foreach (var a in HotkeyActions())
        {
            var row = new Grid { ColumnSpacing = 12 };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            row.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            row.Children.Add(new TextBlock { Text = a.Label, VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap });
            bool capturing = _capturing?.Id == a.Id;
            var button = new Button
            {
                Content = capturing ? "Nhấn tổ hợp phím…" : GlobalHotkeys.Describe(HotkeyOf(a)),
                MinWidth = 160,
                HorizontalContentAlignment = HorizontalAlignment.Center,
            };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(button, "Phím tắt: " + a.Label);
            ToolTipService.SetToolTip(button, "Bấm rồi nhấn tổ hợp mới (có Ctrl hoặc Alt). Backspace: không dùng. Esc: giữ như cũ.");
            button.Click += (_, _) => StartHotkeyCapture(a, button);
            button.PreviewKeyDown += (_, e) => HotkeyCaptureKey(a, e);
            button.LostFocus += (_, _) => { if (_capturing?.Id == a.Id) EndHotkeyCapture(); };
            Grid.SetColumn(button, 1);
            row.Children.Add(button);
            if (_hotkeyConflicts.Contains(a.Id) && !capturing)
            {
                var conflict = new TextBlock
                {
                    Text = "Ứng dụng khác đã dùng tổ hợp này: hãy chọn tổ hợp khác.",
                    Style = Res<Style>("CaptionTextBlockStyle"), TextWrapping = TextWrapping.Wrap,
                    Foreground = Res<Brush>("SystemFillColorCriticalBrush"), Margin = new Thickness(0, 4, 0, 0),
                };
                Grid.SetRow(conflict, 1);
                Grid.SetColumnSpan(conflict, 2);
                row.Children.Add(conflict);
            }
            HotkeyRows.Children.Add(row);
        }
    }

    private void StartHotkeyCapture(HotkeyAction a, Button button)
    {
        _capturing = a;
        _hotkeys?.UnregisterAll(); // else pressing the current combination would run it
        button.Content = "Nhấn tổ hợp phím…";
    }

    private void HotkeyCaptureKey(HotkeyAction a, KeyRoutedEventArgs e)
    {
        if (_capturing?.Id != a.Id) return;
        VirtualKey key = e.Key;
        if (key is VirtualKey.Control or VirtualKey.Shift or VirtualKey.Menu or VirtualKey.LeftWindows or VirtualKey.RightWindows
            or VirtualKey.LeftControl or VirtualKey.RightControl or VirtualKey.LeftShift or VirtualKey.RightShift
            or VirtualKey.LeftMenu or VirtualKey.RightMenu)
            return; // wait for the key itself
        e.Handled = true; // Esc must not close the dialog, Enter must not click again
        if (key == VirtualKey.Escape)
        {
            EndHotkeyCapture();
            return;
        }
        int packed = 0;
        if (key is not (VirtualKey.Back or VirtualKey.Delete))
        {
            uint mods = 0;
            if (Down(VirtualKey.Control)) mods |= GlobalHotkeys.ModControl;
            if (Down(VirtualKey.Menu)) mods |= GlobalHotkeys.ModAlt;
            if (Down(VirtualKey.Shift)) mods |= GlobalHotkeys.ModShift;
            if ((mods & (GlobalHotkeys.ModControl | GlobalHotkeys.ModAlt)) == 0) return; // a bare key would steal typing
            packed = GlobalHotkeys.Pack(mods, (uint)key);
        }
        // One action per combination: the other one gives it up.
        foreach (var other in HotkeyActions())
            if (other.Id != a.Id && packed != 0 && HotkeyOf(other) == packed) _settings.Hotkeys[other.Name] = 0;
        _settings.Hotkeys[a.Name] = packed;
        _settings.Save();
        EndHotkeyCapture();
    }

    private static bool Down(VirtualKey key) => InputKeyboardSource.GetKeyStateForCurrentThread(key).HasFlag(CoreVirtualKeyStates.Down);

    private void EndHotkeyCapture()
    {
        _capturing = null;
        RegisterHotkeys();
        DispatcherQueue.TryEnqueue(BuildHotkeyRows); // not from inside the button's own event
    }
}
