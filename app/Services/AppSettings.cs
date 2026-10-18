using System;
using System.Collections.Generic;
using Microsoft.Win32;

namespace CamStudio.Services;

// Persisted under HKCU\Software\CamStudio (same key the earlier Win32 app used).
public sealed class AppSettings
{
    private const string KeyPath = @"Software\CamStudio";

    /// Best-effort dump to %LOCALAPPDATA%\CamStudio (e.g. the phone's last caps), for troubleshooting.
    public static void WriteDiagnostic(string name, string text)
    {
        try
        {
            string dir = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CamStudio");
            System.IO.Directory.CreateDirectory(dir);
            System.IO.File.WriteAllText(System.IO.Path.Combine(dir, name), text);
        }
        catch (Exception)
        {
        }
    }

    public string Camera { get; set; } = "";
    public string CameraId { get; set; } = ""; // the engine id of the source the user picked last
    public uint LastWidth { get; set; }        // the camera's last size: published at launch, before any source
    public uint LastHeight { get; set; }
    public uint Width { get; set; }        // 0 = camera's native mode
    public uint Height { get; set; }
    public uint FpsMilli { get; set; }
    public int Rotation { get; set; }
    public bool Mirror { get; set; }
    public bool Flip { get; set; }
    public int Aspect { get; set; }
    public double Zoom { get; set; } = 1.0;
    public double CenterX { get; set; } = 0.5;
    public double CenterY { get; set; } = 0.5;
    public bool CloseToTray { get; set; } = true;
    public bool TrayHintShown { get; set; }
    public int Theme { get; set; }         // 0 system, 1 light, 2 dark
    public int Brightness { get; set; }    // -100 .. 100
    public int Contrast { get; set; }
    public int Saturation { get; set; }
    public int Temperature { get; set; }
    public int OutputSize { get; set; }    // "Kích thước camera ảo": 0 auto, 1 4K, 2 1080p, 3 720p
    // The mode last used per source (by its engine id): "lens|width|height|fpsMilli", 0 = native.
    public Dictionary<string, string> Modes { get; } = new();

    public static AppSettings Load()
    {
        var s = new AppSettings();
        using var key = Registry.CurrentUser.OpenSubKey(KeyPath);
        if (key is null) return s;

        int Int(string name, int fallback) => key.GetValue(name) is int v ? v : fallback;
        s.Camera = key.GetValue("Camera") as string ?? "";
        s.CameraId = key.GetValue("CameraId") as string ?? "";
        s.LastWidth = (uint)Int("LastWidth", 0);
        s.LastHeight = (uint)Int("LastHeight", 0);
        s.Width = (uint)Int("Width", 0);
        s.Height = (uint)Int("Height", 0);
        s.FpsMilli = (uint)Int("FpsMilli", 0);
        s.Rotation = Int("Rotation", 0) is var r && r % 90 == 0 ? ((r % 360) + 360) % 360 : 0;
        s.Mirror = Int("Mirror", 0) != 0;
        s.Flip = Int("Flip", 0) != 0;
        s.Aspect = Math.Clamp(Int("Aspect", 0), 0, 4);
        s.Zoom = Math.Clamp(Int("ZoomMilli", 1000) / 1000.0, 1.0, 4.0);
        s.CenterX = Math.Clamp(Int("CenterXMilli", 500) / 1000.0, 0, 1);
        s.CenterY = Math.Clamp(Int("CenterYMilli", 500) / 1000.0, 0, 1);
        s.CloseToTray = Int("CloseToTray", 1) != 0;
        s.TrayHintShown = Int("TrayHintShown", 0) != 0;
        s.Theme = Math.Clamp(Int("Theme", 0), 0, 2);
        s.Brightness = Math.Clamp(Int("Brightness", 0), -100, 100);
        s.Contrast = Math.Clamp(Int("Contrast", 0), -100, 100);
        s.Saturation = Math.Clamp(Int("Saturation", 0), -100, 100);
        s.Temperature = Math.Clamp(Int("Temperature", 0), -100, 100);
        s.OutputSize = Math.Clamp(Int("OutputSize", 0), 0, 3);
        using (var modes = key.OpenSubKey("Modes"))
        {
            if (modes is not null)
                foreach (string name in modes.GetValueNames())
                    if (modes.GetValue(name) is string v) s.Modes[name] = v;
        }
        return s;
    }

    public void Save()
    {
        using var key = Registry.CurrentUser.CreateSubKey(KeyPath);
        key.SetValue("Camera", Camera);
        key.SetValue("CameraId", CameraId);
        key.SetValue("LastWidth", (int)LastWidth, RegistryValueKind.DWord);
        key.SetValue("LastHeight", (int)LastHeight, RegistryValueKind.DWord);
        key.SetValue("Width", (int)Width, RegistryValueKind.DWord);
        key.SetValue("Height", (int)Height, RegistryValueKind.DWord);
        key.SetValue("FpsMilli", (int)FpsMilli, RegistryValueKind.DWord);
        key.SetValue("Rotation", Rotation, RegistryValueKind.DWord);
        key.SetValue("Mirror", Mirror ? 1 : 0, RegistryValueKind.DWord);
        key.SetValue("Flip", Flip ? 1 : 0, RegistryValueKind.DWord);
        key.SetValue("Aspect", Aspect, RegistryValueKind.DWord);
        key.SetValue("ZoomMilli", (int)Math.Round(Zoom * 1000), RegistryValueKind.DWord);
        key.SetValue("CenterXMilli", (int)Math.Round(CenterX * 1000), RegistryValueKind.DWord);
        key.SetValue("CenterYMilli", (int)Math.Round(CenterY * 1000), RegistryValueKind.DWord);
        key.SetValue("CloseToTray", CloseToTray ? 1 : 0, RegistryValueKind.DWord);
        key.SetValue("TrayHintShown", TrayHintShown ? 1 : 0, RegistryValueKind.DWord);
        key.SetValue("Theme", Theme, RegistryValueKind.DWord);
        key.SetValue("Brightness", Brightness, RegistryValueKind.DWord);
        key.SetValue("Contrast", Contrast, RegistryValueKind.DWord);
        key.SetValue("Saturation", Saturation, RegistryValueKind.DWord);
        key.SetValue("Temperature", Temperature, RegistryValueKind.DWord);
        key.SetValue("OutputSize", OutputSize, RegistryValueKind.DWord);
        using var modes = key.CreateSubKey("Modes");
        foreach (var (id, mode) in Modes) modes.SetValue(id, mode);
    }
}

// "Start with Windows" is our value under the HKCU Run key.
public static class Autostart
{
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "CamStudio";

    public static bool IsEnabled
    {
        get
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKey);
            return key?.GetValue(ValueName) is not null;
        }
    }

    public static void Set(bool enable)
    {
        using var key = Registry.CurrentUser.CreateSubKey(RunKey);
        if (enable)
        {
            // --tray: start hidden in the notification area and resume the camera.
            key.SetValue(ValueName, $"\"{Environment.ProcessPath}\" --tray");
        }
        else
        {
            key.DeleteValue(ValueName, throwOnMissingValue: false);
        }
    }
}
