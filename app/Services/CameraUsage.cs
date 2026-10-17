using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using Microsoft.Win32;

namespace CamStudio.Services;

// Apps that Windows currently records as using a camera (the same data that
// drives the privacy indicator). An entry is "in use" while its
// LastUsedTimeStop is 0. This covers our virtual camera as well as physical
// ones, so we leave out Cam Studio itself (it holds the physical camera).
public static class CameraUsage
{
    private const string Root = @"Software\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\webcam";

    public static List<string> AppsInUse()
    {
        var result = new List<string>();
        using var root = Registry.CurrentUser.OpenSubKey(Root);
        if (root is null) return result;

        foreach (string name in root.GetSubKeyNames())
        {
            if (name == "NonPackaged")
            {
                using var np = root.OpenSubKey(name);
                if (np is null) continue;
                foreach (string exeKey in np.GetSubKeyNames())
                {
                    using var k = np.OpenSubKey(exeKey);
                    if (k is null || !InUse(k)) continue;
                    string path = exeKey.Replace('#', '\\');
                    if (string.Equals(path, Environment.ProcessPath, StringComparison.OrdinalIgnoreCase)) continue;
                    result.Add(DescribeExe(path));
                }
            }
            else
            {
                using var k = root.OpenSubKey(name);
                if (k is null || !InUse(k)) continue;
                // Package family name, e.g. Microsoft.WindowsCamera_8wekyb3d8bbwe.
                string friendly = name.Split('_')[0];
                result.Add(friendly.StartsWith("Microsoft.", StringComparison.Ordinal) ? friendly[10..] : friendly);
            }
        }
        return result;
    }

    private static bool InUse(RegistryKey k) =>
        k.GetValue("LastUsedTimeStart") is long start && start > 0 &&
        k.GetValue("LastUsedTimeStop") is long stop && stop == 0;

    private static string DescribeExe(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                string? desc = FileVersionInfo.GetVersionInfo(path).FileDescription;
                if (!string.IsNullOrWhiteSpace(desc)) return desc.Trim();
            }
        }
        catch (Exception)
        {
            // Unreadable version info: fall back to the file name.
        }
        return Path.GetFileNameWithoutExtension(path);
    }
}
