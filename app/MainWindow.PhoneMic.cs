using System;
using System.Collections.Generic;
using System.Linq;
using CamStudio.Native;
using CamStudio.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CamStudio;

// "Micro điện thoại": the phone's microphone played into an output device, VB-Audio's "CABLE
// Input" by default, so Discord or Zoom take "CABLE Output" as their microphone. Windows has no
// virtual microphone without a signed driver; VB-Audio Virtual Cable is one the person installs.
// Never the speakers by default: the phone would hear itself. Android only for now (caps.audio).
public sealed partial class MainWindow
{
    private const string CableUrl = "https://vb-audio.com/Cable/";
    private DateTime _micUsageAt;
    private bool _micInUse;
    private List<(string Id, string Name)>? _outputs;
    private DateTime _outputsAt;
    private string _micTip = "";

    // Output devices (endpoint id, name), read again at most every 5 s (the status tick asks).
    private List<(string Id, string Name)> AudioOutputs(bool fresh = false)
    {
        if (!fresh && _outputs is not null && DateTime.UtcNow - _outputsAt < TimeSpan.FromSeconds(5)) return _outputs;
        string all = Engine.AudioOutputs();
        _outputs = all.Split('\n', StringSplitOptions.RemoveEmptyEntries)
            .Select(l => l.Split('\t'))
            .Where(p => p.Length == 2)
            .Select(p => (p[0], p[1]))
            .ToList();
        _outputsAt = DateTime.UtcNow;
        return _outputs;
    }

    // The source now is a phone that can send its microphone (caps.audio: Android, new app).
    private bool PhoneOffersMic => _isPhone && _phoneCaps is { HasAudio: true };

    private static bool IsCable(string name) =>
        name.Contains("CABLE Input", StringComparison.OrdinalIgnoreCase) || name.Contains("VB-Audio", StringComparison.OrdinalIgnoreCase);

    // Where the sound goes: the device picked in Settings, else VB-Audio's cable, else nowhere.
    private string PhoneAudioTarget(out string name)
    {
        name = "";
        if (_settings.PhoneAudioOutput.StartsWith("wav:", StringComparison.Ordinal))
        {
            name = "tệp WAV";
            return _settings.PhoneAudioOutput; // tests
        }
        var outputs = AudioOutputs();
        var picked = outputs.FirstOrDefault(o => o.Id == _settings.PhoneAudioOutput);
        if (picked.Id is null) picked = outputs.FirstOrDefault(o => IsCable(o.Name));
        name = picked.Name ?? "";
        return picked.Id ?? "";
    }

    // Nothing asked of a source that cannot send it (a webcam, an iPhone, an older app).
    private void ApplyPhoneAudio()
    {
        string target = _settings.PhoneMic && PhoneOffersMic ? PhoneAudioTarget(out _) : "";
        Engine.cs_set_phone_audio(target);
    }

    // Every status tick: while the setting is on and some app uses a microphone (a call, even
    // with the camera off), the phone source stays open for its sound.
    private void UpdatePhoneAudioWanted()
    {
        // Only when the sound can go somewhere: else a phone would stream for nothing.
        if (!_settings.PhoneMic || !PhoneOffersMic || PhoneAudioTarget(out _).Length == 0)
        {
            Engine.cs_set_audio_wanted(0);
            return;
        }
        if (DateTime.UtcNow - _micUsageAt > TimeSpan.FromSeconds(3))
        {
            _micUsageAt = DateTime.UtcNow;
            _micInUse = CameraUsage.RunningAppsUsingMicrophone().Count > 0;
        }
        Engine.cs_set_audio_wanted(_micInUse ? 1 : 0);
    }

    private void MicButton_Click(object sender, RoutedEventArgs e)
    {
        _settings.PhoneMic = MicButton.IsChecked == true;
        _settings.Save();
        ApplyPhoneAudio();
        UpdateMicButton();
        if (_settings.PhoneMic && PhoneAudioTarget(out _).Length == 0)
            ShowSaved("Chưa có nơi nhận tiếng", "Cài VB-Audio Virtual Cable, rồi trong Discord hay Zoom chọn micro \"CABLE Output\". Xem Cài đặt › Micro điện thoại.",
                null, InfoBarSeverity.Warning);
    }

    // The camera bar's microphone: shown for phones that offer it, its tooltip says what happens.
    private void UpdateMicButton()
    {
        bool offered = PhoneOffersMic;
        MicButton.Visibility = offered ? Visibility.Visible : Visibility.Collapsed;
        if (!offered) return;
        MicButton.IsChecked = _settings.PhoneMic;
        MicIcon.Glyph = _settings.PhoneMic ? Icons.Mic : Icons.MicOff;
        string tip;
        bool problemShown = false;
        if (!_settings.PhoneMic) tip = "Gửi tiếng micro điện thoại sang máy tính";
        else
        {
            string target = PhoneAudioTarget(out string name);
            int state = Engine.PhoneAudioState(out string problem);
            problemShown = target.Length == 0 || problem.Length > 0;
            tip = target.Length == 0 ? "Chưa có nơi nhận tiếng: cài VB-Audio Virtual Cable (Cài đặt › Micro điện thoại)"
                : problem.Length > 0 ? problem
                : state == 1 ? $"Đang gửi tiếng điện thoại vào {name}. Trong ứng dụng gọi, chọn micro \"CABLE Output\"."
                : $"Tiếng điện thoại sẽ vào {name} khi điện thoại phát";
        }
        if (problemShown && MicWarnDot.Fill is null) MicWarnDot.Fill = new SolidColorBrush(StatusAmber);
        MicWarnDot.Visibility = problemShown ? Visibility.Visible : Visibility.Collapsed;
        // Only when it changes: this runs every status tick, and a new tooltip never gets to open.
        if (tip != _micTip)
        {
            _micTip = tip;
            ToolTipService.SetToolTip(MicButton, tip);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetHelpText(MicButton, tip);
        }
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(MicButton, _settings.PhoneMic ? "Tắt micro điện thoại" : "Bật micro điện thoại");
    }

    // ---- Settings › Micro điện thoại --------------------------------------------------------

    private bool _fillingOutputs;

    private void FillPhoneAudioOutputs()
    {
        _fillingOutputs = true;
        PhoneAudioCombo.Items.Clear();
        PhoneAudioCombo.Items.Add(new ComboBoxItem { Content = "Tự chọn (VB-Audio CABLE Input nếu có)", Tag = "" });
        int selected = 0;
        foreach (var (id, name) in AudioOutputs(fresh: true))
        {
            PhoneAudioCombo.Items.Add(new ComboBoxItem { Content = IsCable(name) ? name : name + " (loa / tai nghe)", Tag = id });
            if (id == _settings.PhoneAudioOutput) selected = PhoneAudioCombo.Items.Count - 1;
        }
        PhoneAudioCombo.SelectedIndex = selected;
        _fillingOutputs = false;
        bool cable = AudioOutputs().Any(o => IsCable(o.Name));
        PhoneAudioHint.Text = cable
            ? "Trong Discord, Zoom, Meet: chọn micro \"CABLE Output\". Nên đeo tai nghe để điện thoại không thu lại tiếng loa."
            : "Chưa có VB-Audio Virtual Cable: cài nó (miễn phí, cần khởi động lại máy), rồi chọn micro \"CABLE Output\" trong ứng dụng gọi.";
    }

    private void PhoneAudioCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || _fillingOutputs || PhoneAudioCombo.SelectedItem is not ComboBoxItem item) return;
        _settings.PhoneAudioOutput = (string)item.Tag;
        _settings.Save();
        ApplyPhoneAudio();
        UpdateMicButton();
    }

    private async void CableLink_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            await Windows.System.Launcher.LaunchUriAsync(new Uri(CableUrl));
        }
        catch (Exception)
        {
        }
    }
}
