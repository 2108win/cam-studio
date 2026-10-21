using System;
using System.Linq;
using System.Threading.Tasks;
using CamStudio.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CamStudio;

// "Thành phần bổ sung": what a phone needs on this PC that Windows lacks (AddOns.cs), offered when
// it matters (an iPhone plugged in, the phone's microphone turned on) and in Settings, and set up
// with one click. Nothing is installed without that click.
public sealed partial class MainWindow
{
    private enum AddOn { None, Apple, Cable }

    private AddOn _addOnShown;
    private bool _addOnBusy, _addOnChecking, _appleBarDismissed;
    private DateTime _addOnCheckedAt = DateTime.MinValue;

    // Status tick, window on screen: every 10 s, an iPhone on the cable that Apple's service does
    // not see yet gets the offer (once per run if the person closes it).
    private async void CheckAddOns()
    {
        if (_addOnChecking || _addOnBusy || DateTime.UtcNow - _addOnCheckedAt < TimeSpan.FromSeconds(10)) return;
        _addOnChecking = true;
        _addOnCheckedAt = DateTime.UtcNow;
        try
        {
            if (await AddOns.AppleUsbReadyAsync())
            {
                if (_addOnShown == AddOn.Apple && !_addOnBusy && AddOnBar.Severity != InfoBarSeverity.Success) AddOnBar.IsOpen = false;
                return;
            }
            if (_appleBarDismissed || _addOnShown != AddOn.None || !await AddOns.IPhonePluggedAsync()) return;
            ShowAppleAddOn();
        }
        finally
        {
            _addOnChecking = false;
        }
    }

    private void ShowAppleAddOn()
    {
        bool installed = AddOns.AppleDevicesInstalled();
        ShowAddOn(AddOn.Apple, InfoBarSeverity.Informational, "iPhone đang cắm cáp",
            installed
                ? "Để dùng iPhone qua cáp, cần bật Apple Devices (của Apple, đã có trên máy)."
                : "Để dùng iPhone qua cáp, Windows cần app Apple Devices của Apple (miễn phí, Microsoft Store). Cam Studio cài giúp bạn.",
            installed ? "Bật" : "Cài tự động");
    }

    private void ShowCableAddOn() =>
        ShowAddOn(AddOn.Cable, InfoBarSeverity.Informational, "Cần VB-CABLE cho micro điện thoại",
            "Tiếng điện thoại vào cuộc gọi qua VB-CABLE của VB-Audio (www.vb-cable.com): miễn phí, ai thấy hữu ích có thể ủng hộ tác giả. " +
            "Cam Studio tải bản chính thức và mở trình cài, bạn chỉ bấm \"Install Driver\".",
            "Cài VB-CABLE");

    private void ShowAddOn(AddOn kind, InfoBarSeverity severity, string title, string message, string? button)
    {
        _addOnShown = kind;
        AddOnBar.Severity = severity;
        AddOnBar.Title = title;
        AddOnBar.Message = message;
        AddOnButton.Content = button;
        AddOnButton.Visibility = button is null ? Visibility.Collapsed : Visibility.Visible;
        AddOnProgress.Visibility = _addOnBusy ? Visibility.Visible : Visibility.Collapsed;
        AddOnBar.IsOpen = true;
    }

    private void AddOnBar_Closed(InfoBar sender, InfoBarClosedEventArgs args)
    {
        if (_addOnShown == AddOn.Apple && !_addOnBusy && sender.Severity != InfoBarSeverity.Success) _appleBarDismissed = true;
        _addOnShown = AddOn.None;
    }

    private void AddOnButton_Click(object sender, RoutedEventArgs e)
    {
        if (_addOnShown == AddOn.Apple) _ = SetUpAppleAsync();
        else if (_addOnShown == AddOn.Cable) _ = SetUpCableAsync();
    }

    // Apple Devices from the Store (winget), opened once: its process is Apple's usbmuxd, and it
    // starts only after the person accepted Apple's licence there.
    private async Task SetUpAppleAsync()
    {
        if (_addOnBusy) return;
        _addOnBusy = true;
        try
        {
            if (!AddOns.AppleDevicesInstalled())
            {
                ShowAddOn(AddOn.Apple, InfoBarSeverity.Informational, "Đang cài Apple Devices…",
                    "Từ Microsoft Store, mất khoảng một phút. Vẫn dùng Cam Studio bình thường trong lúc chờ.", null);
                string? error = await AddOns.InstallAppleDevicesAsync();
                if (error is not null)
                {
                    AddOns.OpenAppleDevicesStorePage();
                    _addOnBusy = false;
                    ShowAddOn(AddOn.Apple, InfoBarSeverity.Warning, "Chưa cài tự động được Apple Devices",
                        $"{error}. Microsoft Store đã mở trang Apple Devices: bấm Nhận, rồi quay lại đây bấm Thử lại.", "Thử lại");
                    return;
                }
            }
            AddOns.OpenAppleDevices();
            ShowAddOn(AddOn.Apple, InfoBarSeverity.Informational, "Còn một bước trong Apple Devices",
                "Nếu Apple Devices hiện điều khoản của Apple, bấm Agree. Không cần bấm Trust. Cam Studio tự nhận iPhone khi xong.", null);
            var until = DateTime.UtcNow.AddMinutes(10);
            while (DateTime.UtcNow < until)
            {
                if (await AddOns.AppleUsbReadyAsync())
                {
                    _addOnBusy = false;
                    ShowAddOn(AddOn.Apple, InfoBarSeverity.Success, "iPhone dùng được qua cáp",
                        "Có thể đóng cửa sổ Apple Devices. iPhone hiện trong danh sách với chip USB.", null);
                    RefreshCameraList();
                    return;
                }
                await Task.Delay(2000);
            }
            _addOnBusy = false;
            ShowAddOn(AddOn.Apple, InfoBarSeverity.Warning, "Apple Devices chưa sẵn sàng",
                "Mở Apple Devices, bấm Agree nếu nó hỏi, rồi bấm Thử lại.", "Thử lại");
        }
        finally
        {
            _addOnBusy = false;
            AddOnProgress.Visibility = Visibility.Collapsed;
        }
    }

    // VB-Audio's own setup, opened for the person (UAC): it needs one click on "Install Driver".
    private async Task SetUpCableAsync()
    {
        if (_addOnBusy) return;
        _addOnBusy = true;
        try
        {
            var progress = new Progress<string>(text => ShowAddOn(AddOn.Cable, InfoBarSeverity.Informational, "Đang cài VB-CABLE", text, null));
            ShowAddOn(AddOn.Cable, InfoBarSeverity.Informational, "Đang cài VB-CABLE", "Đang chuẩn bị…", null);
            string? error = await AddOns.InstallVbCableAsync(progress);
            if (error is not null)
            {
                _addOnBusy = false;
                ShowAddOn(AddOn.Cable, InfoBarSeverity.Warning, "Chưa cài được VB-CABLE", error, "Thử lại");
                return;
            }
            // Windows adds "CABLE Input" a moment after the driver; sometimes only after a restart.
            for (int i = 0; i < 15; i++)
            {
                if (AudioOutputs(fresh: true).Any(o => AddOns.IsCable(o.Name)))
                {
                    _addOnBusy = false;
                    ApplyPhoneAudio();
                    UpdateMicButton();
                    ShowAddOn(AddOn.Cable, InfoBarSeverity.Success, "VB-CABLE đã sẵn sàng",
                        "Trong Discord, Zoom hay Meet, chọn micro \"CABLE Output\". Nên đeo tai nghe để điện thoại không thu lại tiếng loa.", null);
                    return;
                }
                await Task.Delay(1000);
            }
            _addOnBusy = false;
            ShowAddOn(AddOn.Cable, InfoBarSeverity.Warning, "Khởi động lại máy để cài xong VB-CABLE",
                "Trình cài đã chạy nhưng Windows chưa hiện thiết bị \"CABLE Input\". Khởi động lại máy là xong.", null);
        }
        finally
        {
            _addOnBusy = false;
            AddOnProgress.Visibility = Visibility.Collapsed;
        }
    }

    // ---- Settings › Thành phần bổ sung ---------------------------------------------------------

    private async void FillAddOnRows()
    {
        bool cable = AudioOutputs(fresh: true).Any(o => AddOns.IsCable(o.Name));
        CableAddOnState.Text = cable ? "Đã có VB-CABLE: tiếng điện thoại vào micro \"CABLE Output\"" : "Chưa có VB-CABLE";
        CableAddOnButton.Visibility = cable ? Visibility.Collapsed : Visibility.Visible;

        bool installed = AddOns.AppleDevicesInstalled();
        AppleAddOnState.Text = installed ? "Đang kiểm tra…" : "Cần Apple Devices của Apple (miễn phí)";
        AppleAddOnButton.Content = installed ? "Bật" : "Cài";
        AppleAddOnButton.Visibility = Visibility.Visible;
        if (!installed) return;
        bool ready = await AddOns.AppleUsbReadyAsync();
        AppleAddOnState.Text = ready ? "Sẵn sàng (Apple Devices đang chạy)" : "Apple Devices đã cài nhưng chưa chạy";
        AppleAddOnButton.Visibility = ready ? Visibility.Collapsed : Visibility.Visible;
    }

    private void AppleAddOnButton_Click(object sender, RoutedEventArgs e)
    {
        SettingsDialog.Hide();
        _appleBarDismissed = false;
        _ = SetUpAppleAsync();
    }

    private void CableAddOnButton_Click(object sender, RoutedEventArgs e)
    {
        SettingsDialog.Hide();
        _ = SetUpCableAsync();
    }
}
