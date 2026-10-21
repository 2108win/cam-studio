using System.Collections.Generic;
using System.Linq;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Windows.UI;
using Windows.UI.ViewManagement;

namespace CamStudio.Services;

// "Màu nhấn" (Settings › Chung): Windows' own accent, or one of a few of ours. WinUI's controls
// draw with the SystemAccentColor ramp (dark theme: Light2, light theme: Dark1), so at launch a
// choice sets that ramp in the app's resources and "Theo Windows" leaves it alone. Red stays for
// "an app uses the camera", green / amber for the connection dots: no accent here is one of those.
internal static class AccentColors
{
    public static readonly string[] Names = { "Theo Windows", "Xanh dương", "Tím", "Xanh ngọc" };

    private static readonly string[] Keys =
    {
        "SystemAccentColor", "SystemAccentColorLight1", "SystemAccentColorLight2", "SystemAccentColorLight3",
        "SystemAccentColorDark1", "SystemAccentColorDark2", "SystemAccentColorDark3",
    };

    // Base, Light1..3, Dark1..3.
    private static readonly uint[][] Ramps =
    {
        new uint[0],
        new uint[] { 0xFF2F6FEB, 0xFF3D7FF5, 0xFF4A8CFF, 0xFF8DB6FF, 0xFF1F5FD6, 0xFF1A4FB0, 0xFF123E8F },
        new uint[] { 0xFF7C5CFA, 0xFF8C70FF, 0xFFA08AFF, 0xFFC7B9FF, 0xFF6A4AE6, 0xFF5638C2, 0xFF42289A },
        new uint[] { 0xFF0F9D8F, 0xFF19B3A3, 0xFF2CC9B8, 0xFF8BE6DC, 0xFF0B857A, 0xFF086B62, 0xFF05524B },
    };

    private static Color[] _current = System.Array.Empty<Color>();

    // At launch, before any control reads the accent.
    public static void Apply(ResourceDictionary resources, int choice)
    {
        if (choice > 0 && choice < Ramps.Length)
        {
            for (int i = 0; i < Keys.Length; i++) resources[Keys[i]] = ToColor(Ramps[choice][i]);
        }
        else
        {
            foreach (string key in Keys)
                if (resources.ContainsKey(key)) resources.Remove(key); // Remove throws on a missing key
        }
        _current = Ramp(choice);
    }

    // While the app runs: WinUI's controls already hold their accent brushes, so those brushes are
    // given the new colours (every control using one follows at once).
    public static void Change(ResourceDictionary resources, int choice)
    {
        Color[] from = _current;
        Apply(resources, choice);
        var map = new Dictionary<Color, Color>();
        for (int i = 0; i < from.Length && i < _current.Length; i++)
            if (!from[i].Equals(_current[i])) map.TryAdd(from[i], _current[i]);
        if (map.Count > 0) Recolor(resources, map, new HashSet<ResourceDictionary>());
    }

    private static void Recolor(ResourceDictionary d, Dictionary<Color, Color> map, HashSet<ResourceDictionary> seen)
    {
        if (!seen.Add(d)) return;
        foreach (var entry in d)
            if (entry.Value is SolidColorBrush brush && map.TryGetValue(brush.Color, out Color to)) brush.Color = to;
        foreach (var merged in d.MergedDictionaries) Recolor(merged, map, seen);
        foreach (var theme in d.ThemeDictionaries.Values)
            if (theme is ResourceDictionary td) Recolor(td, map, seen);
    }

    private static Color[] Ramp(int choice)
    {
        if (choice > 0 && choice < Ramps.Length) return Ramps[choice].Select(ToColor).ToArray();
        var ui = new UISettings();
        return new[]
        {
            UIColorType.Accent, UIColorType.AccentLight1, UIColorType.AccentLight2, UIColorType.AccentLight3,
            UIColorType.AccentDark1, UIColorType.AccentDark2, UIColorType.AccentDark3,
        }.Select(ui.GetColorValue).ToArray();
    }

    private static Color ToColor(uint c) => Color.FromArgb((byte)(c >> 24), (byte)(c >> 16), (byte)(c >> 8), (byte)c);
}
