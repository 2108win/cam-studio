using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography.X509Certificates;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32;
using Windows.Devices.Enumeration;

namespace CamStudio.Services;

// What some phones need on the PC that Windows lacks, and how the app gets it for the person in
// one click instead of a hunt for downloads:
// - an iPhone on a cable: Apple's device service (usbmuxd on 127.0.0.1:27015). It comes with the
//   free "Apple Devices" app from the Microsoft Store (or with iTunes). winget installs it; the
//   person then accepts Apple's licence in Apple Devices, once. "Trust" is not needed.
// - the phone's microphone in a call: VB-Audio's VB-CABLE (donationware, www.vb-cable.com). The
//   official package is downloaded, its signature checked, and its own setup opened: the person
//   clicks "Install Driver" (the package's licence asks that it is not folded into another
//   setup without the author's consent, so it is not run silently).
internal static class AddOns
{
    public const string AppleDevicesProductId = "9NP83LWLPZ9K";
    private const string AppleDevicesFamily = "AppleInc.AppleDevices_nzyj5cx40ttqa";
    public const string VbCableSite = "https://vb-audio.com/Cable/";
    private const string VbCableFallback = "https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip";
    private static readonly Regex VbCablePack = new(@"https://download\.vb-audio\.com/Download_CABLE/VBCABLE_Driver_Pack(\d+)\.zip");

    // ---- iPhone on a cable ----------------------------------------------------------------

    // Apple's device service answers (Apple Devices running, or iTunes' service).
    public static async Task<bool> AppleUsbReadyAsync()
    {
        using var client = new TcpClient();
        try
        {
            await client.ConnectAsync(IPAddress.Loopback, 27015).WaitAsync(TimeSpan.FromMilliseconds(500));
            return client.Connected;
        }
        catch (Exception)
        {
            return false;
        }
    }

    public static bool AppleDevicesInstalled()
    {
        try
        {
            if (new Windows.Management.Deployment.PackageManager().FindPackagesForUser("", AppleDevicesFamily).Any()) return true;
        }
        catch (Exception)
        {
        }
        using var service = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\Apple Mobile Device Service");
        return service is not null; // iTunes
    }

    // An iPhone or iPad on a USB cable (Apple's vendor id, product ids 0x12xx: not an Apple
    // keyboard or mouse), seen by Windows whether or not Apple's service runs.
    public static async Task<bool> IPhonePluggedAsync()
    {
        try
        {
            var found = await DeviceInformation.FindAllAsync(
                "System.Devices.DeviceInstanceId:~<\"USB\\VID_05AC&PID_12\" AND System.Devices.Present:=System.StructuredQueryType.Boolean#True",
                null, DeviceInformationKind.Device);
            return found.Count > 0;
        }
        catch (Exception)
        {
            return false;
        }
    }

    // winget, hidden, for this user only (no UAC). Null when it worked, else why not.
    public static async Task<string?> InstallAppleDevicesAsync()
    {
        try
        {
            var psi = new ProcessStartInfo("winget.exe",
                $"install --id {AppleDevicesProductId} --source msstore --accept-package-agreements --accept-source-agreements --silent --disable-interactivity")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            using var p = Process.Start(psi);
            if (p is null) return "winget không chạy được";
            var output = p.StandardOutput.ReadToEndAsync();
            _ = p.StandardError.ReadToEndAsync();
            using var timeout = new CancellationTokenSource(TimeSpan.FromMinutes(10));
            try
            {
                await p.WaitForExitAsync(timeout.Token);
            }
            catch (OperationCanceledException)
            {
                try { p.Kill(true); } catch (Exception) { }
                return "Microsoft Store không trả lời";
            }
            if (p.ExitCode == 0 || AppleDevicesInstalled()) return null;
            string last = (await output).Split('\n').Select(l => l.Trim()).LastOrDefault(l => l.Length > 0) ?? "";
            return last.Length > 0 ? last : $"winget lỗi 0x{p.ExitCode:X8}";
        }
        catch (Win32Exception)
        {
            return "Máy chưa có winget";
        }
        catch (Exception e)
        {
            return e.Message;
        }
    }

    // Opening Apple Devices starts Apple's device process (and, the first time, shows Apple's
    // licence). After that it starts with Windows by itself.
    public static void OpenAppleDevices() => Shell("explorer.exe", $"shell:AppsFolder\\{AppleDevicesFamily}!App");

    public static void OpenAppleDevicesStorePage() => Shell($"ms-windows-store://pdp/?ProductId={AppleDevicesProductId}");

    // ---- VB-CABLE ----------------------------------------------------------------------------

    public static bool IsCable(string outputName) =>
        outputName.Contains("CABLE Input", StringComparison.OrdinalIgnoreCase) || outputName.Contains("VB-Audio", StringComparison.OrdinalIgnoreCase);

    // Downloads VB-Audio's package (the newest the VB-CABLE page links to), checks that the setup
    // is signed by its author, opens it elevated and waits for it to close. Null when the setup
    // ran; the caller then looks for the new "CABLE Input".
    public static async Task<string?> InstallVbCableAsync(IProgress<string> progress)
    {
        string dir = Path.Combine(Path.GetTempPath(), "CamStudio", "vbcable");
        try
        {
            if (Directory.Exists(dir)) Directory.Delete(dir, recursive: true);
            Directory.CreateDirectory(dir);
            using var http = new HttpClient { Timeout = TimeSpan.FromMinutes(3) };
            http.DefaultRequestHeaders.UserAgent.ParseAdd("CamStudio");

            progress.Report("Đang tải VB-CABLE từ vb-audio.com…");
            string url = VbCableFallback;
            try
            {
                string page = await http.GetStringAsync(VbCableSite);
                var newest = VbCablePack.Matches(page).OrderByDescending(m => int.Parse(m.Groups[1].Value)).FirstOrDefault();
                if (newest is not null) url = newest.Value;
            }
            catch (Exception)
            {
                // the page moved or is down: the known package
            }
            string zip = Path.Combine(dir, "VBCABLE.zip");
            await File.WriteAllBytesAsync(zip, await http.GetByteArrayAsync(url));
            string folder = Path.Combine(dir, "package");
            ZipFile.ExtractToDirectory(zip, folder);
            string setup = Path.Combine(folder, "VBCABLE_Setup_x64.exe");
            if (!File.Exists(setup)) return "Gói VB-CABLE tải về không có trình cài";
            if (!SignedByVbAudio(setup)) return "Trình cài VB-CABLE tải về không có chữ ký của VB-Audio: không mở";

            progress.Report("Trong cửa sổ VB-CABLE, bấm \"Install Driver\", rồi đóng nó khi cài xong.");
            using var p = Process.Start(new ProcessStartInfo(setup) { UseShellExecute = true, Verb = "runas", WorkingDirectory = folder });
            if (p is null) return "Không mở được trình cài VB-CABLE";
            await p.WaitForExitAsync();
            return null;
        }
        catch (Win32Exception e) when (e.NativeErrorCode == 1223)
        {
            return "Đã không cho quyền quản trị: VB-CABLE chưa được cài";
        }
        catch (Exception e)
        {
            return e.Message;
        }
    }

    // A valid Authenticode signature whose signer is VB-Audio's author (EV certificate of
    // "BUREL VINCENT Entrepreneur individuel").
    private static bool SignedByVbAudio(string path)
    {
        if (!AuthenticodeValid(path)) return false;
        try
        {
#pragma warning disable SYSLIB0057 // the signer of a signed file: no newer API reads it
            using var cert = X509Certificate.CreateFromSignedFile(path);
#pragma warning restore SYSLIB0057
            return cert.Subject.Contains("BUREL VINCENT", StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception)
        {
            return false;
        }
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WintrustFileInfo
    {
        public uint cbStruct;
        public string pcwszFilePath;
        public IntPtr hFile;
        public IntPtr pgKnownSubject;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WintrustData
    {
        public uint cbStruct;
        public IntPtr pPolicyCallbackData;
        public IntPtr pSIPClientData;
        public uint dwUIChoice;
        public uint fdwRevocationChecks;
        public uint dwUnionChoice;
        public IntPtr pFile;
        public uint dwStateAction;
        public IntPtr hWVTStateData;
        public IntPtr pwszURLReference;
        public uint dwProvFlags;
        public uint dwUIContext;
        public IntPtr pSignatureSettings;
    }

    [DllImport("wintrust.dll", CharSet = CharSet.Unicode)]
    private static extern int WinVerifyTrust(IntPtr hwnd, ref Guid action, ref WintrustData data);

    private static bool AuthenticodeValid(string path)
    {
        var file = new WintrustFileInfo { cbStruct = (uint)Marshal.SizeOf<WintrustFileInfo>(), pcwszFilePath = path };
        IntPtr pFile = Marshal.AllocHGlobal(Marshal.SizeOf<WintrustFileInfo>());
        try
        {
            Marshal.StructureToPtr(file, pFile, false);
            var data = new WintrustData
            {
                cbStruct = (uint)Marshal.SizeOf<WintrustData>(),
                dwUIChoice = 2,          // WTD_UI_NONE
                fdwRevocationChecks = 0, // WTD_REVOKE_NONE
                dwUnionChoice = 1,       // WTD_CHOICE_FILE
                pFile = pFile,
                dwStateAction = 0,       // WTD_STATEACTION_IGNORE
            };
            var action = new Guid("00AAC56B-CD44-11d0-8CC2-00C04FC295EE"); // WINTRUST_ACTION_GENERIC_VERIFY_V2
            return WinVerifyTrust(IntPtr.Zero, ref action, ref data) == 0;
        }
        finally
        {
            Marshal.DestroyStructure<WintrustFileInfo>(pFile);
            Marshal.FreeHGlobal(pFile);
        }
    }

    private static void Shell(string file, string? args = null)
    {
        try
        {
            Process.Start(new ProcessStartInfo(file, args ?? "") { UseShellExecute = true })?.Dispose();
        }
        catch (Exception)
        {
        }
    }
}
