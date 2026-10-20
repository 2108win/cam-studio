using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;

namespace CamStudio.Services;

// The phone's "caps" message (docs/protocol.md section 5), as far as the
// UI needs it to build controls. Unknown fields are ignored.
public sealed class PhoneCaps
{
    public sealed record Range(double Min, double Max);

    public sealed class Lens
    {
        public string Id = "";
        public string Label = "";
        public string Facing = "back";
        public string Kind = "wide";
        public double FocalLength35;      // 0 when unknown
        public Range? Zoom;
        public List<string> FocusModes = new();
        public List<string> ExposureModes = new();
        public Range? Iso;
        public Range? ExposureMs;
        public Range? Bias;
        public List<string> WhiteBalanceModes = new();
        public Range Kelvin = new(2000, 10000);
        public bool Torch;
        public List<string> Stabilization = new();
        public bool LowLightBoost;
        public bool Cinematic = true;     // this lens can do caps.effects.cinematic (absent = yes)
    }

    public string Os = "";
    public List<Lens> Lenses = new();
    public Dictionary<string, string> Effects = new(); // name -> app | system | none

    public Lens? FindByLabel(string label) => Lenses.FirstOrDefault(l => l.Label == label) ?? Lenses.FirstOrDefault();

    /// Short chip text like a phone camera app: "0.6×", "1×", "2×", "Macro".
    public string ChipText(Lens lens)
    {
        if (lens.Kind == "macro") return "Macro";
        if (lens.Kind == "virtual") return "Tự động";
        var main = Lenses.FirstOrDefault(l => l.Facing == lens.Facing && l.Kind is "wide" or "front" or "trueDepth" && l.FocalLength35 > 0);
        if (main is not null && lens.FocalLength35 > 0)
        {
            double ratio = lens.FocalLength35 / main.FocalLength35;
            return (Math.Abs(ratio - Math.Round(ratio)) < 0.05 ? Math.Round(ratio).ToString("0", CultureInfo.InvariantCulture) : ratio.ToString("0.#", CultureInfo.InvariantCulture)) + "×";
        }
        return lens.Kind switch { "ultraWide" => "0.5×", "telephoto" => "2×", _ => "1×" };
    }

    public bool HasSystemEffects => Effects.Values.Any(v => v == "system");
    public bool HasAudio;   // caps.audio: the phone can send its microphone (docs/protocol.md 5.1)

    public static PhoneCaps? Parse(string json)
    {
        if (string.IsNullOrEmpty(json)) return null;
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            var caps = new PhoneCaps();
            if (root.TryGetProperty("device", out var device)) caps.Os = Str(device, "os");
            JsonElement topControls = root.TryGetProperty("controls", out var tc) ? tc : default;

            if (root.TryGetProperty("lenses", out var lenses))
            {
                foreach (var l in lenses.EnumerateArray())
                {
                    var lens = new Lens
                    {
                        Id = Str(l, "id"),
                        Label = Str(l, "label"),
                        Facing = Str(l, "facing"),
                        Kind = Str(l, "kind"),
                        FocalLength35 = l.TryGetProperty("focalLength35mm", out var f) && f.ValueKind == JsonValueKind.Number ? f.GetDouble() : 0,
                        Zoom = RangeOf(l, "zoom", "min", "max"),
                        Cinematic = !(l.TryGetProperty("cinematic", out var cin) && cin.ValueKind == JsonValueKind.False),
                    };
                    // Per-lens controls win over the shared ones (protocol section 5).
                    var controls = l.TryGetProperty("controls", out var lc) ? lc : topControls;
                    if (controls.ValueKind == JsonValueKind.Object) ReadControls(controls, lens);
                    caps.Lenses.Add(lens);
                }
            }
            caps.HasAudio = root.TryGetProperty("audio", out var audio) && audio.ValueKind == JsonValueKind.Object;
            if (root.TryGetProperty("effects", out var effects) && effects.ValueKind == JsonValueKind.Object)
            {
                foreach (var e in effects.EnumerateObject())
                    if (e.Value.ValueKind == JsonValueKind.String) caps.Effects[e.Name] = e.Value.GetString()!;
            }
            return caps;
        }
        catch (JsonException)
        {
            return null;
        }
    }

    private static void ReadControls(JsonElement c, Lens lens)
    {
        if (c.TryGetProperty("focus", out var focus)) lens.FocusModes = Strings(focus, "modes");
        if (c.TryGetProperty("exposure", out var exp))
        {
            lens.ExposureModes = Strings(exp, "modes");
            lens.Iso = RangeOf(exp, null, "isoMin", "isoMax");
            lens.ExposureMs = RangeOf(exp, null, "msMin", "msMax");
            lens.Bias = RangeOf(exp, null, "biasMin", "biasMax");
        }
        if (c.TryGetProperty("whiteBalance", out var wb))
        {
            lens.WhiteBalanceModes = Strings(wb, "modes");
            lens.Kelvin = RangeOf(wb, null, "kelvinMin", "kelvinMax") ?? lens.Kelvin;
        }
        lens.Torch = c.TryGetProperty("torch", out var t) && t.ValueKind == JsonValueKind.True;
        if (c.TryGetProperty("stabilization", out var s) && s.ValueKind == JsonValueKind.Array)
            lens.Stabilization = s.EnumerateArray().Where(x => x.ValueKind == JsonValueKind.String).Select(x => x.GetString()!).ToList();
        lens.LowLightBoost = c.TryGetProperty("lowLightBoost", out var ll) && ll.ValueKind == JsonValueKind.True;
    }

    private static string Str(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString()! : "";

    private static List<string> Strings(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Array
            ? v.EnumerateArray().Where(x => x.ValueKind == JsonValueKind.String).Select(x => x.GetString()!).ToList()
            : new List<string>();

    private static Range? RangeOf(JsonElement e, string? child, string min, string max)
    {
        if (child is not null && !e.TryGetProperty(child, out e)) return null;
        if (e.ValueKind != JsonValueKind.Object) return null;
        if (e.TryGetProperty(min, out var a) && a.ValueKind == JsonValueKind.Number &&
            e.TryGetProperty(max, out var b) && b.ValueKind == JsonValueKind.Number && b.GetDouble() > a.GetDouble())
            return new Range(a.GetDouble(), b.GetDouble());
        return null;
    }
}
