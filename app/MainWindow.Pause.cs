using System;
using System.IO;
using CamStudio.Native;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CamStudio;

// What apps see while the picture is paused (Settings › Camera ảo): the last picture blurred with
// "Quay lại ngay" by default, a card with the text, the person's own image, or the camera's
// grey "no signal". The engine renders it once and writes it while the camera has no source.
public sealed partial class MainWindow
{
    public const int PauseNoSignal = 0, PauseCard = 1, PauseBlur = 2, PauseImage = 3; // CS_PAUSE_*

    private string PauseText => string.IsNullOrWhiteSpace(_settings.PauseText) ? "Quay lại ngay" : _settings.PauseText.Trim();

    // Called before every cs_switch_none: the pause picture only while the user paused.
    private void ApplyPausePicture(bool paused)
    {
        int kind = paused ? _settings.PauseKind : PauseNoSignal;
        if (kind == PauseImage && !File.Exists(_settings.PauseImage)) kind = PauseCard;
        Engine.cs_set_pause_picture(kind, PauseText, kind == PauseImage ? _settings.PauseImage : "");
    }

    // "ứng dụng thấy …" in the status line and the empty preview.
    private string PauseSeen() => _settings.PauseKind switch
    {
        PauseCard or PauseBlur => $"“{PauseText}”",
        PauseImage when File.Exists(_settings.PauseImage) => "ảnh bạn chọn",
        PauseImage => $"“{PauseText}”",
        _ => "“không có tín hiệu”",
    };

    private void LoadPauseSettingsIntoUi()
    {
        PauseKindCombo.SelectedIndex = Math.Clamp(_settings.PauseKind, 0, 3);
        PauseTextBox.Text = _settings.PauseText;
        UpdatePauseRows();
    }

    private void UpdatePauseRows()
    {
        int kind = _settings.PauseKind;
        PauseTextBox.Visibility = kind is PauseCard or PauseBlur ? Visibility.Visible : Visibility.Collapsed;
        PauseImageRow.Visibility = kind == PauseImage ? Visibility.Visible : Visibility.Collapsed;
        PauseImageName.Text = File.Exists(_settings.PauseImage) ? "Đã chọn ảnh" : "Chưa chọn ảnh";
    }

    // A change shows at once in apps while paused.
    private void PauseChanged()
    {
        _settings.Save();
        UpdatePauseRows();
        if (_paused) ApplyPausePicture(true);
        UpdateStatus();
    }

    private void PauseKindCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || PauseKindCombo.SelectedIndex < 0) return;
        _settings.PauseKind = PauseKindCombo.SelectedIndex;
        PauseChanged();
    }

    private void PauseTextBox_TextChanged(object sender, TextChangedEventArgs e)
    {
        if (_loading) return;
        _settings.PauseText = PauseTextBox.Text;
        PauseChanged();
    }

    // The image is copied next to the settings: moving or deleting the original keeps it working.
    private async void PauseImage_Click(object sender, RoutedEventArgs e)
    {
        var picker = new Windows.Storage.Pickers.FileOpenPicker
        {
            SuggestedStartLocation = Windows.Storage.Pickers.PickerLocationId.PicturesLibrary,
            ViewMode = Windows.Storage.Pickers.PickerViewMode.Thumbnail,
        };
        foreach (string ext in new[] { ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tif", ".tiff", ".heic", ".webp" }) picker.FileTypeFilter.Add(ext);
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(this));
        try
        {
            var file = await picker.PickSingleFileAsync();
            if (file is null) return;
            string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CamStudio");
            Directory.CreateDirectory(dir);
            string copy = Path.Combine(dir, "pause-image" + Path.GetExtension(file.Path).ToLowerInvariant());
            foreach (string old in Directory.GetFiles(dir, "pause-image.*")) File.Delete(old);
            File.Copy(file.Path, copy, overwrite: true);
            _settings.PauseImage = copy;
            PauseChanged();
        }
        catch (Exception ex)
        {
            PauseImageName.Text = "Không dùng được ảnh này: " + ex.Message;
        }
    }
}
