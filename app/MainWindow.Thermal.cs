using System;
using System.Text.Json;
using CamStudio.Native;
using Microsoft.UI.Xaml.Controls;

namespace CamStudio;

// A hot phone (its "state" says thermal serious / critical): before iOS or Android stops the
// camera in the middle of a call, ask for less: 1080p30, then 720p30, on the same lens. Once per
// level and source; the saved mode is left alone, so the next time starts as chosen.
public sealed partial class MainWindow
{
    private int _thermalActed;      // the level already acted on for this source (2 serious, 3 critical)
    private string? _thermalSource;
    private bool _thermalShown;     // the bar shows a heat warning (closed when the source changes)

    // Every status tick, also hidden in the tray (that is when calls run long).
    private void CheckPhoneHeat()
    {
        if (_selectedId != _thermalSource)
        {
            _thermalSource = _selectedId;
            _thermalActed = 0;
            if (_thermalShown) SavedBar.IsOpen = false; // about the phone before
            _thermalShown = false;
        }
        if (!_isPhone || _format < 0 || _format >= _formats.Count || _paused) return;
        int level = 0;
        try
        {
            string json = Engine.PhoneStateJson();
            if (string.IsNullOrEmpty(json)) return;
            using var doc = JsonDocument.Parse(json);
            if (doc.RootElement.TryGetProperty("thermal", out var th) && th.ValueKind == JsonValueKind.String)
                level = th.GetString() switch { "serious" => 2, "critical" => 3, _ => 0 };
        }
        catch (JsonException)
        {
            return;
        }
        if (level <= _thermalActed) return;
        _thermalActed = level;

        var current = _formats[_format].Format;
        uint maxSide = level >= 3 ? 720u : 1080u; // the short side, so portrait counts too
        static uint Short(CsFormat f) => Math.Min(f.Width, f.Height);
        if (Short(current) <= maxSide && current.Fps <= 30.5) return; // already light enough
        int pick = -1;
        for (int i = 0; i < _formats.Count; i++)
        {
            var f = _formats[i].Format;
            if (f.LensText != current.LensText || Short(f) > maxSide || f.Fps > 30.5) continue;
            if (pick < 0 || (ulong)f.Width * f.Height > (ulong)_formats[pick].Format.Width * _formats[pick].Format.Height ||
                ((ulong)f.Width * f.Height == (ulong)_formats[pick].Format.Width * _formats[pick].Format.Height && f.Fps > _formats[pick].Format.Fps))
                pick = i;
        }
        if (pick < 0) return;
        SelectFormat(pick);
        StartCapture(); // the camera stays; an app on it keeps its size (fitted)
        var chosen = _formats[pick].Format;
        string mode = $"{Math.Min(chosen.Width, chosen.Height)}p{chosen.Fps:0}";
        ShowSaved(level >= 3 ? "Điện thoại quá nóng" : "Điện thoại đang nóng",
            $"Đã giảm xuống {mode} để camera không bị ngắt. Khi máy nguội, chọn lại chế độ ở trên.", null, InfoBarSeverity.Warning);
        _thermalShown = true;
        HotkeyFeedback(level >= 3 ? "Điện thoại quá nóng" : "Điện thoại đang nóng", $"Cam Studio đã giảm xuống {mode}.");
    }
}
