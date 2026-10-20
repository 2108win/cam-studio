using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using CamStudio.Native;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;

namespace CamStudio;

// "Nền" (inspector): the person stays sharp, the background is blurred or replaced by an image.
// Runs on this PC for any camera (webcam, Android, iPhone), never for a screen or a window, and
// only on frames that go somewhere (an app on the camera, the window open).
public sealed partial class MainWindow
{
    public const int BackgroundOff = 0, BackgroundBlur = 1, BackgroundImage = 2; // CS_BACKGROUND_*

    private static string BackgroundModel => Path.Combine(AppContext.BaseDirectory, "Assets", "Models", "pphumanseg.onnx");

    private void PushBackground()
    {
        int mode = _settings.BackgroundMode;
        if (mode == BackgroundImage && !File.Exists(_settings.BackgroundImage)) mode = BackgroundBlur;
        Engine.cs_set_background(mode, _settings.BackgroundStrength, _settings.BackgroundImage, BackgroundModel);
    }

    private void LoadBackgroundIntoUi()
    {
        BackgroundStrengthSlider.Value = _settings.BackgroundStrength;
        SkinSlider.Value = _settings.SkinSmoothing;
        Engine.cs_set_smoothing(_settings.SkinSmoothing);
        UpdateSkinHint();
        ShowBackgroundMode();
        PushBackground();
    }

    private void ShowBackgroundMode()
    {
        foreach (var chip in BackgroundChips.Children.OfType<ToggleButton>())
            chip.IsChecked = (string)chip.Tag == _settings.BackgroundMode.ToString();
        BackgroundStrengthRow.Visibility = _settings.BackgroundMode == BackgroundBlur ? Visibility.Visible : Visibility.Collapsed;
        BackgroundImageRow.Visibility = _settings.BackgroundMode == BackgroundImage ? Visibility.Visible : Visibility.Collapsed;
        BackgroundImageName.Text = File.Exists(_settings.BackgroundImage) ? "Đã chọn ảnh" : "Chưa chọn ảnh: tạm làm mờ";
        string background = _settings.BackgroundMode switch
        {
            BackgroundBlur => "Làm mờ nền",
            BackgroundImage => "Ảnh nền",
            _ => "",
        };
        BackgroundSummary.Text = string.Join(" · ", new[] { background, _settings.SkinSmoothing > 0 ? "Mịn da" : "" }.Where(t => t.Length > 0));
        UpdateBackgroundHint();
    }

    // Every status tick while the section is visible: whether it runs, and why not.
    private void UpdateBackgroundHint()
    {
        if (_settings.BackgroundMode == BackgroundOff)
        {
            BackgroundHint.Text = "Chạy trên máy tính, cho mọi camera (không cho màn hình hay cửa sổ).";
            return;
        }
        Engine.cs_get_background(out var state);
        bool camera = _windowSource is null && _camera >= 0 && _camera < _kinds.Count && _kinds[_camera] != 2;
        BackgroundHint.Text = !File.Exists(BackgroundModel) ? "Thiếu tệp mô hình (Assets\\Models): hãy cài lại Cam Studio."
            : state.Available == 0 ? "Máy này không chạy được (thiếu Windows ML)."
            : state.Available < 0 ? "Đã dừng vì lỗi. Tắt rồi bật lại để thử lại."
            : !camera ? "Chỉ cho camera: màn hình và cửa sổ không bị làm mờ."
            : state.Active != 0 ? $"Đang chạy · {state.InferMs:0} ms mỗi lần tìm người"
            : "Bật khi có ứng dụng dùng camera hoặc cửa sổ này đang mở.";
    }

    private void BackgroundChip_Click(object sender, RoutedEventArgs e)
    {
        int mode = int.Parse((string)((FrameworkElement)sender).Tag);
        if (mode != BackgroundOff) _settings.LastBackgroundMode = mode;
        _settings.BackgroundMode = mode;
        _settings.Save();
        ShowBackgroundMode(); // a chip clicked again stays on
        PushBackground();
        if (mode == BackgroundImage && !File.Exists(_settings.BackgroundImage)) BackgroundImage_Click(sender, e);
    }

    // "Làm mịn da": faces only, found by Windows' face tracker (the one "Tự căn khung" uses).
    private void SkinSlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        if (_loading) return;
        _settings.SkinSmoothing = (int)Math.Round(e.NewValue);
        _settings.Save();
        Engine.cs_set_smoothing(_settings.SkinSmoothing);
        UpdateSkinHint();
        ShowBackgroundMode();
    }

    private void UpdateSkinHint()
    {
        SkinValue.Text = _settings.SkinSmoothing == 0 ? "Tắt" : _settings.SkinSmoothing.ToString();
        if (_settings.SkinSmoothing == 0)
        {
            SkinHint.Text = "Chỉ làm mịn vùng mặt, giữ nét mắt và viền.";
            return;
        }
        Engine.cs_get_background(out var state);
        SkinHint.Text = state.Faces switch
        {
            -2 => "Máy này thiếu bộ nhận khuôn mặt của Windows.",
            -1 => "Bật khi có ứng dụng dùng camera hoặc cửa sổ này đang mở.",
            0 => "Chưa thấy khuôn mặt nào.",
            1 => "Đang làm mịn 1 khuôn mặt.",
            _ => $"Đang làm mịn {state.Faces} khuôn mặt.",
        };
    }

    private void BackgroundStrength_ValueChanged(object sender, RangeBaseValueChangedEventArgs e)
    {
        if (_loading) return;
        _settings.BackgroundStrength = (int)Math.Round(e.NewValue);
        _settings.Save();
        PushBackground();
    }

    // The image is copied next to the settings, like the pause picture.
    private async void BackgroundImage_Click(object sender, RoutedEventArgs e)
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
            string copy = Path.Combine(dir, "background-image" + Path.GetExtension(file.Path).ToLowerInvariant());
            foreach (string old in Directory.GetFiles(dir, "background-image.*")) File.Delete(old);
            File.Copy(file.Path, copy, overwrite: true);
            _settings.BackgroundImage = copy;
            _settings.Save();
            ShowBackgroundMode();
            PushBackground();
        }
        catch (Exception ex)
        {
            BackgroundImageName.Text = "Không dùng được ảnh này: " + ex.Message;
        }
    }

    // Ctrl+Alt+B: off, or back to the last effect.
    private void ToggleBackgroundFromHotkey()
    {
        int mode = _settings.BackgroundMode == BackgroundOff ? Math.Max(BackgroundBlur, _settings.LastBackgroundMode) : BackgroundOff;
        _settings.BackgroundMode = mode;
        _settings.Save();
        ShowBackgroundMode();
        PushBackground();
        HotkeyFeedback(mode == BackgroundOff ? "Đã tắt hiệu ứng nền" : mode == BackgroundBlur ? "Đã bật làm mờ nền" : "Đã bật ảnh nền");
    }
}
