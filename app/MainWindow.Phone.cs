using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;
using CamStudio.Native;
using CamStudio.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CamStudio;

// The phone's manual controls (inspector, Camera tab): built when the tab shows or the lens
// changes, from what this phone and lens actually support (caps), with live values from "state".
public sealed partial class MainWindow
{
    private bool _phoneOpen;
    private TextBlock? _phoneReadout;
    private readonly Dictionary<string, object> _phoneSent = new(); // last values sent, so the panel reopens as left

    private static readonly Dictionary<string, string> Names = new()
    {
        ["continuous"] = "Tự động liên tục",
        ["auto"] = "Tự động",
        ["manual"] = "Thủ công",
        ["off"] = "Tắt",
        ["standard"] = "Tiêu chuẩn",
        ["optical"] = "Quang học (OIS)",
        ["cinematic"] = "Điện ảnh",
        ["cinematicExtended"] = "Điện ảnh mở rộng",
        ["previewOptimized"] = "Tối ưu xem trực tiếp",
        ["lowLatency"] = "Độ trễ thấp",
    };

    private static string Name(string key) => Names.TryGetValue(key, out var n) ? n : key;

    private PhoneCaps.Lens? CurrentLens() =>
        _phoneCaps is null || _format < 0 ? null : _phoneCaps.FindByLabel(_formats[_format].Format.LensText);

    private void SendPhone(string key, object value)
    {
        _phoneSent[key] = value;
        var msg = new JsonObject { ["type"] = "set", [key] = JsonValue.Create(value) };
        Engine.PhoneSend(msg.ToJsonString());
    }

    private T Sent<T>(string key, T fallback) => _phoneSent.TryGetValue(key, out var v) && v is T t ? t : fallback;

    private void BuildPhoneControls()
    {
        PhoneControls.Children.Clear();
        var lens = CurrentLens();
        if (lens is null)
        {
            PhoneControls.Children.Add(new TextBlock { Text = "Chưa có thông tin camera điện thoại.", TextWrapping = TextWrapping.Wrap });
            return;
        }

        // Lenses, zoom, flash and exposure bias are the sections above; these are the manual controls.

        // Focus
        if (lens.FocusModes.Count > 0)
        {
            string mode = Sent("focusMode", lens.FocusModes[0]);
            AddChoice("Lấy nét", lens.FocusModes, mode, m => { SendPhone("focusMode", m); BuildPhoneControls(); });
            if (mode == "manual")
                AddSlider("Khoảng lấy nét (gần ↔ xa)", 0, 1, 0.01, Sent("focus", 1.0), v => $"{v * 100:0}%", v => SendPhone("focus", v));
        }

        // Exposure
        if (lens.ExposureModes.Count > 0)
        {
            string mode = Sent("exposureMode", "auto");
            AddChoice("Phơi sáng", lens.ExposureModes, mode, m => { SendPhone("exposureMode", m); BuildPhoneControls(); });
            if (mode == "manual")
            {
                if (lens.Iso is { } iso)
                    AddSlider("ISO", iso.Min, iso.Max, 1, Sent("iso", Math.Clamp(400.0, iso.Min, iso.Max)), v => $"{v:0}", v => SendPhone("iso", Math.Round(v)));
                if (lens.ExposureMs is { } ms)
                {
                    // Never longer than one frame at the chosen frame rate.
                    double frameMs = _format >= 0 && _formats[_format].Format.Fps > 0 ? 1000 / _formats[_format].Format.Fps : ms.Max;
                    double max = Math.Min(ms.Max, frameMs);
                    AddSlider("Tốc độ màn trập", ms.Min, max, 0.01, Sent("exposureMs", Math.Min(10.0, max)), ShutterText, v => SendPhone("exposureMs", v));
                }
            }
        }

        // White balance
        if (lens.WhiteBalanceModes.Count > 0)
        {
            string mode = Sent("wbMode", "auto");
            AddChoice("Cân bằng trắng", lens.WhiteBalanceModes, mode, m => { SendPhone("wbMode", m); BuildPhoneControls(); });
            if (mode == "manual")
                AddSlider("Nhiệt độ màu", lens.Kelvin.Min, lens.Kelvin.Max, 50, Sent("wbKelvin", 5500.0), v => $"{v:0} K", v => SendPhone("wbKelvin", Math.Round(v)));
        }

        // Stabilization
        if (lens.Stabilization.Count > 1)
            AddChoice("Chống rung", lens.Stabilization, Sent("stabilization", "off"), m => SendPhone("stabilization", m));

        if (lens.LowLightBoost) AddToggle("Tăng sáng khi thiếu sáng", Sent("lowLightBoost", false), on => SendPhone("lowLightBoost", on));

        // Effects: "app" ones are toggles; "system" ones live in the OS panel.
        if (_phoneCaps!.Effects.TryGetValue("cinematic", out var cin) && cin == "app" && lens.Cinematic)
            AddToggle("Chế độ điện ảnh (xóa phông)", Sent("cinematic", false), on => SendPhone("cinematic", on));
        if (_phoneCaps.HasSystemEffects)
        {
            var names = _phoneCaps.Effects.Where(kv => kv.Value == "system").Select(kv => kv.Key switch
            {
                "portrait" => "Xóa phông",
                "studioLight" => "Ánh sáng studio",
                "centerStage" => "Center Stage",
                "backgroundReplacement" => "Thay nền",
                _ => kv.Key,
            });
            var button = new Button { Content = "Hiệu ứng trên điện thoại", HorizontalAlignment = HorizontalAlignment.Stretch, Margin = new Thickness(0, 8, 0, 0) };
            ToolTipService.SetToolTip(button, string.Join(", ", names) + " do hệ điều hành điều khiển: bật trong bảng mở ra trên điện thoại.");
            button.Click += (_, _) => Engine.PhoneSend("{\"type\":\"showSystemEffects\"}");
            PhoneControls.Children.Add(button);
        }

        _phoneReadout = new TextBlock { Style = Res<Style>("InspectorHintStyle"), Margin = new Thickness(0, 12, 0, 0) };
        PhoneControls.Children.Add(_phoneReadout);
        UpdatePhoneReadout();
    }

    // Live values from the phone's latest "state" (ISO, shutter, battery, heat).
    private void UpdatePhoneReadout()
    {
        if (_phoneReadout is not null) _phoneReadout.Text = PhoneReadoutText();
    }

    // "Pin 57%" (charging marked), for the source list; null when the phone did not say.
    private string? PhoneBattery()
    {
        string json = Engine.PhoneStateJson();
        if (string.IsNullOrEmpty(json)) return null;
        try
        {
            using var doc = JsonDocument.Parse(json);
            var s = doc.RootElement;
            if (!s.TryGetProperty("battery", out var bat) || bat.ValueKind != JsonValueKind.Number) return null;
            bool charging = s.TryGetProperty("charging", out var ch) && ch.ValueKind == JsonValueKind.True;
            return $"{(charging ? "Sạc" : "Pin")} {bat.GetDouble():0}%";
        }
        catch (JsonException)
        {
            return null;
        }
    }

    private string PhoneReadoutText()
    {
        string json = Engine.PhoneStateJson();
        if (string.IsNullOrEmpty(json)) return "";
        try
        {
            using var doc = JsonDocument.Parse(json);
            var s = doc.RootElement;
            var parts = new List<string>();
            if (s.TryGetProperty("iso", out var iso) && iso.ValueKind == JsonValueKind.Number) parts.Add($"ISO {iso.GetDouble():0}");
            if (s.TryGetProperty("exposureMs", out var ms) && ms.ValueKind == JsonValueKind.Number) parts.Add(ShutterText(ms.GetDouble()));
            if (s.TryGetProperty("zoom", out var zm) && zm.ValueKind == JsonValueKind.Number) parts.Add($"{zm.GetDouble():0.0}×");
            if (s.TryGetProperty("bitrateKbps", out var br) && br.ValueKind == JsonValueKind.Number) parts.Add($"{br.GetDouble() / 1000:0.0} Mbps");
            if (s.TryGetProperty("battery", out var bat) && bat.ValueKind == JsonValueKind.Number)
            {
                bool charging = s.TryGetProperty("charging", out var ch) && ch.ValueKind == JsonValueKind.True;
                parts.Add($"Pin {bat.GetDouble():0}%{(charging ? " (đang sạc)" : "")}");
            }
            if (s.TryGetProperty("thermal", out var th) && th.ValueKind == JsonValueKind.String)
            {
                parts.Add(th.GetString() switch
                {
                    "fair" => "Máy hơi ấm",
                    "serious" => "Máy nóng: nên giảm độ phân giải",
                    "critical" => "Máy quá nóng!",
                    _ => "Nhiệt độ bình thường",
                });
            }
            return string.Join("  ·  ", parts);
        }
        catch (JsonException)
        {
            return "";
        }
    }

    private static string ShutterText(double ms) =>
        ms >= 1000 ? $"{ms / 1000:0.#} s" : ms >= 100 ? $"{ms:0} ms" : $"1/{Math.Round(1000 / Math.Max(ms, 0.001)):0} s";

    // ---- small builders -----------------------------------------------------------

    private void AddSlider(string label, double min, double max, double step, double value, Func<double, string> format, Action<double> onChange)
    {
        var valueText = new TextBlock { Text = format(value), VerticalAlignment = VerticalAlignment.Center, MinWidth = 64, TextAlignment = TextAlignment.Right };
        var slider = new Slider { Minimum = min, Maximum = max, StepFrequency = step, Value = Math.Clamp(value, min, max), IsThumbToolTipEnabled = false };
        slider.ValueChanged += (_, e) =>
        {
            valueText.Text = format(e.NewValue);
            onChange(e.NewValue);
        };
        var row = new Grid { ColumnSpacing = 8 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.Children.Add(slider);
        Grid.SetColumn(valueText, 1);
        row.Children.Add(valueText);
        PhoneControls.Children.Add(new TextBlock { Text = label, Margin = new Thickness(0, 8, 0, 0) });
        PhoneControls.Children.Add(row);
    }

    private void AddChoice(string label, List<string> options, string current, Action<string> onChange)
    {
        var combo = new ComboBox { Header = label, HorizontalAlignment = HorizontalAlignment.Stretch, Margin = new Thickness(0, 8, 0, 0) };
        foreach (var o in options) combo.Items.Add(Name(o));
        combo.SelectedIndex = Math.Max(0, options.IndexOf(current));
        combo.SelectionChanged += (_, _) =>
        {
            if (combo.SelectedIndex >= 0) onChange(options[combo.SelectedIndex]);
        };
        PhoneControls.Children.Add(combo);
    }

    private void AddToggle(string label, bool on, Action<bool> onChange)
    {
        var toggle = new ToggleSwitch { Header = label, IsOn = on, Margin = new Thickness(0, 4, 0, 0) };
        toggle.Toggled += (_, _) => onChange(toggle.IsOn);
        PhoneControls.Children.Add(toggle);
    }

    // A style, not a brush looked up here: it follows the app's light/dark choice.
    private TextBlock Hint(string text) => new() { Text = text, Style = Res<Style>("InspectorHintStyle") };

    private static T Res<T>(string key) => (T)Application.Current.Resources[key];
}
