using System;
using System.Collections.Generic;
using CamStudio.Native;
using CamStudio.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CamStudio;

// Updates (Services/Updater.cs, docs/updates.md). When to install, so it never surprises:
// - never while an app uses a camera (the installer restarts the Windows camera service, which
//   drops every camera app, not only Cam Studio's);
// - by itself only right after the user opened Cam Studio themselves, with an installer already
//   downloaded (Windows then asks for admin rights: the user is there to answer). Started with
//   Windows (--tray): no prompt at boot, only the tray balloon and menu item;
// - otherwise when the user clicks "Cập nhật ngay" (bar, settings, tray menu).
public sealed partial class MainWindow
{
    private const string MakerUrl = "https://winlax.id.vn";
    private readonly Updater _updater = new();
    private DateTime _launchedAt = DateTime.UtcNow;
    private bool _manualLaunch;
    private bool _installing;
    private string _balloonShownFor = "";

    // updated: opened again by the installer after an update (--updated): not a manual launch, so
    // no automatic install in this session (a release whose tag is newer than its setup would loop).
    private void SetupUpdater(bool tray, bool updated)
    {
        _manualLaunch = !tray && !updated;
        _launchedAt = DateTime.UtcNow;
        _updater.AutoDownload = _settings.AutoUpdate;
        _updater.Changed += () => DispatcherQueue.TryEnqueue(OnUpdateChanged);
        UpdateBar.CloseButtonClick += (_, _) => _autoCancelled = true;
        VersionText.Text = $"Phiên bản {_updater.Current}" + (_updater.CanInstall ? "" : " (bản chạy thử: không tự cài)");
        bool was = _loading;
        _loading = true;
        AutoUpdateToggle.IsOn = _settings.AutoUpdate;
        _loading = was;
        OnUpdateChanged();
        _updater.Start(soon: _manualLaunch);
    }

    private void OnUpdateChanged()
    {
        var r = _updater.Latest;
        UpdateStatusText.Text = _updater.State switch
        {
            Updater.Phase.Checking => "Đang kiểm tra…",
            Updater.Phase.UpToDate => $"Đang dùng bản mới nhất ({_updater.LastCheck:HH:mm})",
            Updater.Phase.Available => $"Có bản mới {r?.Version}",
            Updater.Phase.Downloading => $"Đang tải bản {r?.Version}… {_updater.Progress}%",
            Updater.Phase.Ready => $"Bản {r?.Version} đã tải xong, sẵn sàng cài",
            Updater.Phase.Failed => _updater.Error,
            _ => "",
        };
        bool offer = _updater.State is Updater.Phase.Ready or Updater.Phase.Available;
        UpdateButton.Content = offer ? $"Cập nhật lên {r?.Version}" : "Kiểm tra cập nhật";
        UpdateButton.IsEnabled = _updater.State is not (Updater.Phase.Checking or Updater.Phase.Downloading) && !_installing;
        if (!offer || r is null) return;

        string version = r.Version.ToString();
        if (!UpdateBar.IsOpen && !_installing)
        {
            UpdateBar.Severity = InfoBarSeverity.Informational;
            UpdateBar.Title = $"Có bản mới {version}";
            UpdateBar.Message = _updater.State == Updater.Phase.Ready
                ? "Đã tải sẵn. Cài mất khoảng một phút, Windows sẽ hỏi quyền."
                : "Bấm Cập nhật để tải và cài.";
            UpdateBar.IsOpen = true;
        }
        if (!IsOnScreen && _balloonShownFor != version)
        {
            _balloonShownFor = version;
            _tray.ShowBalloon($"Cam Studio {version} đã sẵn sàng", "Chuột phải biểu tượng ở khay → Cập nhật.");
        }
        // By itself: just opened by the user, the installer is already here, no camera in use.
        if (_updater.State == Updater.Phase.Ready && _settings.AutoUpdate && _manualLaunch &&
            _settings.AutoInstalledVersion != version &&
            DateTime.UtcNow - _launchedAt < TimeSpan.FromMinutes(2) && CamerasInUse().Count == 0)
        {
            _manualLaunch = false; // once
            AutoInstallSoon(r.Version);
        }
    }

    // 15 s of notice: the user may have opened Cam Studio to join a call (closing the bar puts the
    // update off), and apps that had the camera open come back to it in that time.
    private async void AutoInstallSoon(Version version)
    {
        _autoCancelled = false;
        UpdateBar.Severity = InfoBarSeverity.Informational;
        UpdateBar.Title = $"Sẽ cập nhật lên {version} sau 15 giây";
        UpdateBar.Message = "Windows sẽ hỏi quyền, Cam Studio đóng rồi tự mở lại (khoảng một phút). Đóng thông báo này nếu cần camera ngay.";
        UpdateBar.IsOpen = true;
        await System.Threading.Tasks.Task.Delay(TimeSpan.FromSeconds(15));
        if (_autoCancelled || _installing || _updater.State != Updater.Phase.Ready || !_settings.AutoUpdate) return;
        if (CamerasInUse().Count > 0)
        {
            UpdateBar.Title = $"Có bản mới {version}";
            UpdateBar.Message = "Đã tải sẵn. Camera đang được dùng nên chưa cài: bấm Cập nhật ngay khi xong, hoặc để lần sau mở Cam Studio.";
            return;
        }
        // Never by itself twice for the same version (a click still installs it).
        _settings.AutoInstalledVersion = version.ToString();
        _settings.Save();
        await InstallUpdateAsync(confirmed: true);
    }

    private bool _autoCancelled;

    // Apps using a camera right now (ours, through the virtual camera, or any other).
    private List<string> CamerasInUse()
    {
        var apps = CameraUsage.RunningAppsInUse();
        if (apps.Count == 0 && Engine.GetStatus().ConsumerActive != 0) apps.Add("một ứng dụng");
        if (_recording) apps.Add("Cam Studio (đang ghi hình)");
        return apps;
    }

    private async System.Threading.Tasks.Task InstallUpdateAsync(bool confirmed)
    {
        if (_installing) return;
        if (_updater.State is Updater.Phase.Available or Updater.Phase.Failed or Updater.Phase.UpToDate or Updater.Phase.Idle)
        {
            await _updater.DownloadNowAsync();
            if (_updater.State != Updater.Phase.Ready) return; // the status line says why
        }
        if (_updater.State != Updater.Phase.Ready) return;
        if (!_updater.CanInstall)
        {
            ShowUpdateMessage("Bản chạy thử không tự cài", $"Bộ cài {_updater.Latest?.Version} đã tải và kiểm tra xong: {_updater.ReadyPath}", InfoBarSeverity.Warning);
            return;
        }
        var users = CamerasInUse();
        if (users.Count > 0 && !confirmed) confirmed = await ConfirmAsync(users);
        else if (users.Count == 0) confirmed = true;
        if (!confirmed) return;

        _installing = true;
        OnUpdateChanged();
        var setup = await _updater.InstallAsync(relaunchInTray: !IsOnScreen);
        if (setup is null)
        {
            _installing = false;
            ShowUpdateMessage("Không cập nhật được", _updater.Error, InfoBarSeverity.Error);
            OnUpdateChanged();
            return;
        }
        ShowUpdateMessage($"Đang cập nhật lên {_updater.Latest?.Version}…",
            "Bấm Có khi Windows hỏi quyền. Cam Studio sẽ tự đóng và mở lại.", InfoBarSeverity.Informational);
        // Setup closes this app. Still here when it ends: the user said no, or it failed.
        await setup.WaitForExitAsync();
        int code = setup.ExitCode;
        setup.Dispose();
        _installing = false;
        ShowUpdateMessage("Chưa cập nhật", code == 0
            ? "Bộ cài đã chạy xong nhưng Cam Studio chưa được thay. Hãy thử lại."
            : "Đã hủy, hoặc Windows không cho quyền. Bấm Cập nhật để thử lại.", InfoBarSeverity.Warning);
        OnUpdateChanged();
    }

    private async System.Threading.Tasks.Task<bool> ConfirmAsync(List<string> users)
    {
        if (!IsOnScreen) ShowFromTray();
        var dialog = new ContentDialog
        {
            XamlRoot = RootGrid.XamlRoot,
            Title = "Cập nhật lúc camera đang được dùng?",
            Content = new TextBlock
            {
                Text = $"{string.Join(", ", users)} đang dùng camera. Khi cài, camera trong các ứng dụng đó sẽ mất hình khoảng một phút.",
                TextWrapping = TextWrapping.Wrap,
            },
            PrimaryButtonText = "Cập nhật ngay",
            CloseButtonText = "Để sau",
            DefaultButton = ContentDialogButton.Close,
        };
        return await dialog.ShowAsync() == ContentDialogResult.Primary;
    }

    private void ShowUpdateMessage(string title, string message, InfoBarSeverity severity)
    {
        UpdateBar.Title = title;
        UpdateBar.Message = message;
        UpdateBar.Severity = severity;
        UpdateBar.IsOpen = true;
    }

    private IEnumerable<TrayIcon.MenuItem> TrayUpdateItems()
    {
        if (_updater.State is Updater.Phase.Ready or Updater.Phase.Available && _updater.Latest is { } r && !_installing)
            yield return new TrayIcon.MenuItem($"Cập nhật lên {r.Version}", () => DispatcherQueue.TryEnqueue(() => _ = InstallUpdateAsync(confirmed: false)));
    }

    private void UpdateNow_Click(object sender, RoutedEventArgs e) => _ = InstallUpdateAsync(confirmed: false);

    private void UpdateButton_Click(object sender, RoutedEventArgs e)
    {
        if (_updater.State is Updater.Phase.Ready or Updater.Phase.Available) _ = InstallUpdateAsync(confirmed: false);
        else _ = _updater.CheckNowAsync();
    }

    private void AutoUpdateToggle_Toggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _settings.AutoUpdate = AutoUpdateToggle.IsOn;
        _settings.Save();
        _updater.AutoDownload = _settings.AutoUpdate;
    }

    // "by Win Lã" under the version in Settings; opened the same way as the other links.
    private async void MakerLink_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            await Windows.System.Launcher.LaunchUriAsync(new Uri(MakerUrl));
        }
        catch (Exception)
        {
        }
    }
}
