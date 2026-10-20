using System;
using System.Diagnostics;
using System.IO;
using System.Threading.Tasks;
using CamStudio.Native;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CamStudio;

// "Chụp ảnh" and "Ghi hình": what the virtual camera shows (settings, framing and colour
// included), as a PNG in Pictures\Cam Studio and an MP4 in Videos\Cam Studio. Nothing runs while
// neither is used; a recording keeps the source open even with the window hidden.
public sealed partial class MainWindow
{
    private bool _recording;
    private string? _recordingPath;
    private string? _savedPath; // what "Mở thư mục" shows

    private static string Folder(Environment.SpecialFolder root)
    {
        string dir = Path.Combine(Environment.GetFolderPath(root), "Cam Studio");
        Directory.CreateDirectory(dir);
        return dir;
    }

    private static string StampedName(string ext) => $"Cam Studio {DateTime.Now:yyyy-MM-dd HH.mm.ss}{ext}";

    private async void Snapshot_Click(object sender, RoutedEventArgs e) => await TakeSnapshotAsync();

    private async Task TakeSnapshotAsync()
    {
        string path = Path.Combine(Folder(Environment.SpecialFolder.MyPictures), StampedName(".png"));
        SnapshotButton.IsEnabled = false;
        int hr = await Task.Run(() => Engine.cs_snapshot(path)); // up to 4 s: a sleeping source opens first
        SnapshotButton.IsEnabled = true;
        if (hr < 0)
        {
            ShowSaved("Chưa chụp được", hr == unchecked((int)0x800700E8) ? "Chưa có hình để chụp: chọn một nguồn hình trước."
                : $"Không lưu được ảnh (0x{hr:X8}).", null, InfoBarSeverity.Warning);
            HotkeyFeedback("Chưa chụp được", "Chưa có hình để chụp.");
            return;
        }
        ShowSaved("Đã chụp ảnh", Path.GetFileName(path), path);
        HotkeyFeedback("Đã chụp ảnh", "Lưu trong Ảnh › Cam Studio.");
    }

    private void Record_Click(object sender, RoutedEventArgs e) => _ = ToggleRecordingAsync();

    private async Task ToggleRecordingAsync()
    {
        if (!_recording)
        {
            string path = Path.Combine(Folder(Environment.SpecialFolder.MyVideos), StampedName(".mp4"));
            int hr = Engine.cs_record_start(path, _settings.RecordMicrophone ? 1 : 0);
            if (hr < 0)
            {
                ShowSaved("Không ghi được", $"Chưa có hình để ghi, hoặc máy không mã hóa được H.264 (0x{hr:X8}).", null, InfoBarSeverity.Error);
                HotkeyFeedback("Không ghi được hình");
                return;
            }
            _recording = true;
            _recordingPath = path;
            SavedBar.IsOpen = false;
            UpdateRecordingUi();
            HotkeyFeedback("Đang ghi hình", "Bấm lại phím tắt để dừng.");
            return;
        }
        long ms = Engine.RecordStatus(out _, out string problem);
        _recording = false;
        RecordButton.IsEnabled = false;
        int stop = await Task.Run(Engine.cs_record_stop); // the file is finished here
        RecordButton.IsEnabled = true;
        UpdateRecordingUi();
        string length = TimeSpan.FromMilliseconds(ms).ToString(ms >= 3600_000 ? @"h\:mm\:ss" : @"mm\:ss");
        if (stop < 0)
            ShowSaved("Đoạn ghi bị lỗi", $"Không hoàn tất được file (0x{stop:X8}).", _recordingPath, InfoBarSeverity.Error);
        else
            ShowSaved($"Đã lưu đoạn ghi ({length})", problem.Length > 0 ? problem : Path.GetFileName(_recordingPath!), _recordingPath,
                      problem.Length > 0 ? InfoBarSeverity.Warning : InfoBarSeverity.Success);
        HotkeyFeedback($"Đã lưu đoạn ghi ({length})", "Trong Video › Cam Studio.");
    }

    // Every status tick: the button shows the time while recording.
    private void UpdateRecordingUi()
    {
        long ms = _recording ? Engine.RecordStatus(out _, out _) : 0;
        RecordTime.Text = TimeSpan.FromMilliseconds(ms).ToString(ms >= 3600_000 ? @"h\:mm\:ss" : @"mm\:ss");
        RecordTime.Visibility = _recording ? Visibility.Visible : Visibility.Collapsed;
        RecordIcon.Glyph = _recording ? Icons.Stop : Icons.Record;
        RecordIcon.FontFamily = Res<FontFamily>(_recording ? "IconFontFilled" : "IconFont");
        RecordIcon.Foreground = _recording ? Res<Brush>("LiveBrush") : Res<Brush>("TextFillColorPrimaryBrush");
        string tip = _recording ? "Dừng ghi hình" : "Ghi hình (những gì camera ảo đang phát) vào Video › Cam Studio";
        ToolTipService.SetToolTip(RecordButton, tip + ShortcutHint("Record"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(RecordButton, _recording ? "Dừng ghi hình" : "Ghi hình");
    }

    private string ShortcutHint(string name) => HotkeyText(name) is { Length: > 0 } t ? $" ({t})" : "";

    private void ShowSaved(string title, string message, string? path, InfoBarSeverity severity = InfoBarSeverity.Success)
    {
        _savedPath = path;
        SavedBar.Title = title;
        SavedBar.Message = message;
        SavedBar.Severity = severity;
        SavedOpenButton.Visibility = path is null ? Visibility.Collapsed : Visibility.Visible;
        SavedBar.IsOpen = true;
    }

    // Explorer with the file selected.
    private void SavedOpen_Click(object sender, RoutedEventArgs e)
    {
        if (_savedPath is null) return;
        try
        {
            Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{_savedPath}\"") { UseShellExecute = true })?.Dispose();
        }
        catch (Exception)
        {
        }
    }

    private void RecordMicToggle_Toggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _settings.RecordMicrophone = RecordMicToggle.IsOn;
        _settings.Save();
    }

    private void OpenVideos_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            Process.Start(new ProcessStartInfo(Folder(Environment.SpecialFolder.MyVideos)) { UseShellExecute = true })?.Dispose();
        }
        catch (Exception)
        {
        }
    }
}
