using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32;

namespace CamStudio.Services;

// Updates from the GitHub releases (docs/updates.md):
// - looks 90 s after launch, then every 6 h (one small request; ETag, so mostly "304");
// - downloads the newer CamStudio-Setup-<version>.exe in the background and checks its size and
//   the SHA-256 that GitHub lists for it, refusing it otherwise;
// - installs only when asked (the app decides when: a click, or a manual launch with no camera in
//   use). The installer asks for admin rights itself (UAC) and opens the app again.
// Only the installed copy updates itself; a build run from elsewhere just checks.
public sealed class Updater
{
    public enum Phase { Idle, Checking, UpToDate, Available, Downloading, Ready, Failed }

    public sealed record Release(Version Version, string Tag, string PageUrl, string SetupName, long SetupSize, string SetupSha256, string SetupUrl);

    private const string DefaultFeed = "https://api.github.com/repos/2108win/cam-studio/releases/latest";
    private const string UninstallKey = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{6E3C8C2A-3F0B-4B8E-9C61-5A2D7F1E9B44}_is1";
    private static readonly TimeSpan FirstCheck = TimeSpan.FromSeconds(90);
    private static readonly TimeSpan Every = TimeSpan.FromHours(6);

    private readonly HttpClient _http;
    private readonly string _feed;
    private readonly bool _testFeed; // CAMSTUDIO_UPDATE_FEED on this PC (http://127.0.0.1…): for testing
    private readonly SemaphoreSlim _busy = new(1, 1);
    private Timer? _timer;
    private string? _etag;

    public Version Current { get; }
    public bool CanInstall { get; }        // running from the installed copy
    public Phase State { get; private set; } = Phase.Idle;
    public Release? Latest { get; private set; }
    public string? ReadyPath { get; private set; } // the verified installer
    public int Progress { get; private set; }       // 0..100 while downloading
    public string Error { get; private set; } = "";
    public DateTime LastCheck { get; private set; }
    public bool AutoDownload { get; set; } = true;  // off: a newer version is only announced (Available)

    /// Raised on a worker thread whenever State, Progress or Latest change.
    public event Action? Changed;

    public static string Folder => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CamStudio", "updates");

    public Updater()
    {
        var v = typeof(Updater).Assembly.GetName().Version ?? new Version(0, 0, 0);
        Current = new Version(v.Major, v.Minor, Math.Max(0, v.Build));
        string? feed = Environment.GetEnvironmentVariable("CAMSTUDIO_UPDATE_FEED");
        _testFeed = feed is not null && IsLoopback(feed);
        _feed = _testFeed ? feed! : DefaultFeed;
        CanInstall = IsInstalledCopy();
        _http = new HttpClient(new HttpClientHandler { AutomaticDecompression = DecompressionMethods.All }) { Timeout = Timeout.InfiniteTimeSpan };
        _http.DefaultRequestHeaders.UserAgent.ParseAdd($"CamStudio/{Current}");
    }

    /// soon: the user opened the app themselves, so look within seconds (an installer downloaded
    /// in an earlier run is then ready at once); otherwise after 90 s.
    public void Start(bool soon)
    {
        _timer ??= new Timer(_ => _ = CheckAsync(), null, soon ? TimeSpan.FromSeconds(3) : FirstCheck, Every);
    }

    public Task CheckNowAsync() => CheckAsync(download: AutoDownload);

    /// Checks, and downloads a newer installer even with AutoDownload off.
    public Task DownloadNowAsync() => CheckAsync(download: true);

    private Task CheckAsync() => CheckAsync(download: AutoDownload);

    private async Task CheckAsync(bool download)
    {
        if (!await _busy.WaitAsync(0)) return; // a check or download is already running
        try
        {
            Set(Phase.Checking);
            Release? release = await FetchLatestAsync();
            LastCheck = DateTime.Now;
            if (release is null && Latest is null) { Set(Phase.UpToDate); return; }
            if (release is not null) Latest = release;
            if (Latest is null || Latest.Version <= Current || Latest.SetupName.Length == 0) { Set(Phase.UpToDate); return; }
            string path = Path.Combine(Folder, Latest.SetupName);
            bool have = File.Exists(path) && await VerifyAsync(path, Latest);
            if (!have && !download) { Set(Phase.Available); return; }
            if (!have) await DownloadAsync(Latest, path);
            ReadyPath = path;
            CleanFolder(keep: path);
            Set(Phase.Ready);
        }
        catch (Exception e)
        {
            Error = e is HttpRequestException or TaskCanceledException or IOException
                ? "Không kết nối được máy chủ cập nhật" : e.Message;
            Set(Phase.Failed);
        }
        finally
        {
            _busy.Release();
        }
    }

    // The newest published release, or null when unchanged since the last look (304).
    private async Task<Release?> FetchLatestAsync()
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, _feed);
        request.Headers.Accept.ParseAdd("application/vnd.github+json");
        if (_etag is not null) request.Headers.TryAddWithoutValidation("If-None-Match", _etag);
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(30));
        using var response = await _http.SendAsync(request, cts.Token);
        if (response.StatusCode == HttpStatusCode.NotModified) return null;
        response.EnsureSuccessStatusCode();
        _etag = response.Headers.ETag?.ToString();
        using var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync(cts.Token));
        var root = doc.RootElement;
        if (root.TryGetProperty("draft", out var d) && d.GetBoolean()) return null;
        if (root.TryGetProperty("prerelease", out var p) && p.GetBoolean()) return null;
        string tag = root.GetProperty("tag_name").GetString() ?? "";
        if (!TryParseVersion(tag, out var version)) throw new InvalidDataException($"Phiên bản lạ: {tag}");
        string page = root.TryGetProperty("html_url", out var h) ? h.GetString() ?? "" : "";
        string wanted = $"CamStudio-Setup-{version}.exe";
        foreach (var asset in root.GetProperty("assets").EnumerateArray())
        {
            if (!string.Equals(asset.GetProperty("name").GetString(), wanted, StringComparison.OrdinalIgnoreCase)) continue;
            string digest = asset.TryGetProperty("digest", out var dg) ? dg.GetString() ?? "" : "";
            if (!digest.StartsWith("sha256:", StringComparison.OrdinalIgnoreCase) || digest.Length != 7 + 64)
                throw new InvalidDataException("Bản cập nhật không có mã kiểm tra SHA-256");
            string url = asset.GetProperty("browser_download_url").GetString() ?? "";
            if (!IsAllowedUrl(url)) throw new InvalidDataException("Địa chỉ tải bản cập nhật không hợp lệ");
            return new Release(version, tag, page, wanted, asset.GetProperty("size").GetInt64(), digest[7..].ToLowerInvariant(), url);
        }
        // A release without a Windows installer (e.g. phone only): nothing for this PC.
        return new Release(version, tag, page, "", 0, "", "");
    }

    private async Task DownloadAsync(Release release, string path)
    {
        Directory.CreateDirectory(Folder);
        string part = path + ".part";
        Progress = 0;
        Set(Phase.Downloading);
        using (var response = await _http.GetAsync(release.SetupUrl, HttpCompletionOption.ResponseHeadersRead))
        {
            response.EnsureSuccessStatusCode();
            if (response.RequestMessage?.RequestUri is { } final && !IsAllowedUrl(final.ToString()))
                throw new InvalidDataException("Địa chỉ tải bản cập nhật không hợp lệ");
            await using var input = await response.Content.ReadAsStreamAsync();
            await using var output = new FileStream(part, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16, useAsync: true);
            using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            var buffer = new byte[1 << 16];
            long done = 0;
            int read;
            while ((read = await input.ReadAsync(buffer)) > 0)
            {
                done += read;
                if (done > release.SetupSize) throw new InvalidDataException("Bản cập nhật lớn hơn khai báo");
                sha.AppendData(buffer, 0, read);
                await output.WriteAsync(buffer.AsMemory(0, read));
                int percent = (int)(done * 100 / Math.Max(1, release.SetupSize));
                if (percent != Progress) { Progress = percent; Changed?.Invoke(); }
            }
            string hash = Convert.ToHexString(sha.GetHashAndReset()).ToLowerInvariant();
            if (done != release.SetupSize || hash != release.SetupSha256)
            {
                output.Close();
                File.Delete(part);
                throw new InvalidDataException("Bản tải về bị hỏng (sai mã SHA-256), đã bỏ");
            }
        }
        File.Move(part, path, overwrite: true);
    }

    private static async Task<bool> VerifyAsync(string path, Release release)
    {
        try
        {
            if (new FileInfo(path).Length != release.SetupSize) return false;
            await using var f = File.OpenRead(path);
            string hash = Convert.ToHexString(await SHA256.HashDataAsync(f)).ToLowerInvariant();
            return hash == release.SetupSha256;
        }
        catch (IOException)
        {
            return false;
        }
    }

    /// Runs the verified installer (silent, with its own progress window). Inno Setup asks for admin
    /// rights itself, closes Cam Studio and opens it again ("tray": hidden in the tray).
    /// Returns the setup process, or null with Error set.
    public Process? Install(bool relaunchInTray)
    {
        if (!CanInstall) { Error = "Bản đang chạy không phải bản đã cài: không tự cập nhật"; return null; }
        if (State != Phase.Ready || ReadyPath is null || Latest is null) { Error = "Chưa có bản cập nhật"; return null; }
        // Checked again right before running: the file sits in a folder this user can write.
        if (!VerifyAsync(ReadyPath, Latest).GetAwaiter().GetResult()) { Error = "Bản tải về đã bị thay đổi, sẽ tải lại"; ReadyPath = null; Set(Phase.Failed); return null; }
        try
        {
            // No "runas" verb: Setup elevates itself, so it can open the app again as this user.
            return Process.Start(new ProcessStartInfo(ReadyPath,
                $"/SILENT /SUPPRESSMSGBOXES /NORESTART /RELAUNCH={(relaunchInTray ? "tray" : "show")}")
            { UseShellExecute = true });
        }
        catch (Exception e)
        {
            Error = e.Message;
            return null;
        }
    }

    private void Set(Phase phase)
    {
        State = phase;
        Changed?.Invoke();
    }

    private static void CleanFolder(string keep)
    {
        try
        {
            foreach (string f in Directory.EnumerateFiles(Folder).Where(f => !string.Equals(f, keep, StringComparison.OrdinalIgnoreCase)))
                File.Delete(f);
        }
        catch (Exception)
        {
            // An old installer still open: removed next time.
        }
    }

    public static bool TryParseVersion(string text, out Version version)
    {
        version = new Version(0, 0, 0);
        string t = text.Trim().TrimStart('v', 'V');
        if (t.Contains('-') || t.Contains('+')) return false; // pre-release builds are not offered
        if (!Version.TryParse(t, out var v) || v.Build < 0) return false;
        version = new Version(v.Major, v.Minor, v.Build);
        return true;
    }

    private bool IsAllowedUrl(string url) =>
        Uri.TryCreate(url, UriKind.Absolute, out var u) &&
        (u.Scheme == Uri.UriSchemeHttps || (_testFeed && u.Scheme == Uri.UriSchemeHttp && u.IsLoopback));

    private static bool IsLoopback(string url) =>
        Uri.TryCreate(url, UriKind.Absolute, out var u) && u.IsLoopback && (u.Scheme == Uri.UriSchemeHttp || u.Scheme == Uri.UriSchemeHttps);

    // The installer records where it put the app ({app}); the exe runs from {app}\App.
    private static bool IsInstalledCopy()
    {
        try
        {
            using var hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            using var key = hklm.OpenSubKey(UninstallKey);
            if (key?.GetValue("InstallLocation") is not string location || location.Length == 0) return false;
            string installed = Path.GetFullPath(Path.Combine(location, "App")).TrimEnd('\\');
            string running = Path.GetFullPath(AppContext.BaseDirectory).TrimEnd('\\');
            return string.Equals(installed, running, StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception)
        {
            return false;
        }
    }
}
