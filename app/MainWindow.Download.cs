using System;
using System.Text;
using CamStudio.Services;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;

namespace CamStudio;

// Getting the phone app: a QR code of the release page (the Android APK is there). The iPhone app
// is on TestFlight with no public link yet, so the dialog says so instead of a code.
public sealed partial class MainWindow
{
    private const string PhoneAppUrl = "https://github.com/2108win/cam-studio/releases/latest";

    // The code as one vector path (no seams between modules at any scale) on white, with the
    // 4-module quiet zone scanners need.
    private static FrameworkElement QrImage(string text, double size)
    {
        bool[,] q = QrCode.Encode(text);
        int n = q.GetLength(0);
        var data = new StringBuilder();
        for (int r = 0; r < n; r++)
            for (int c = 0; c < n; c++)
            {
                if (!q[r, c]) continue;
                int run = 1;
                while (c + run < n && q[r, c + run]) run++;
                data.Append($"M{c + 4},{r + 4}h{run}v1h{-run}z");
                c += run - 1;
            }
        var path = new Path
        {
            Data = (Geometry)XamlBindingHelper.ConvertValue(typeof(Geometry), data.ToString()),
            Fill = new SolidColorBrush(Colors.Black),
            Width = n + 8,
            Height = n + 8,
        };
        var canvas = new Grid { Width = n + 8, Height = n + 8, Background = new SolidColorBrush(Colors.White), Children = { path } };
        var box = new Border
        {
            Width = size,
            Height = size,
            CornerRadius = new CornerRadius(10),
            Background = new SolidColorBrush(Colors.White),
            Child = new Viewbox { Stretch = Stretch.Uniform, Child = canvas },
        };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(box, "Mã QR trang tải app Cam Studio cho điện thoại");
        return box;
    }

    private void SetupPhoneAppCode()
    {
        PlaceholderQr.Child = QrImage(PhoneAppUrl, 112);
    }

    private async void PhoneApp_Click(object sender, RoutedEventArgs e)
    {
        var content = new StackPanel { Spacing = 16, Width = 380 };
        content.Children.Add(new Grid { Children = { QrImage(PhoneAppUrl, 200) }, HorizontalAlignment = HorizontalAlignment.Center });
        content.Children.Add(new TextBlock
        {
            Text = "Android: quét mã bằng camera điện thoại, tải file CamStudio-…-android.apk rồi mở để cài.",
            TextWrapping = TextWrapping.Wrap,
        });
        content.Children.Add(new TextBlock
        {
            Text = "iPhone: app đang thử nghiệm qua TestFlight, chưa có đường tải công khai.",
            TextWrapping = TextWrapping.Wrap,
            Foreground = Res<Brush>("TextFillColorSecondaryBrush"),
        });
        content.Children.Add(new TextBlock
        {
            Text = "Mở app trên điện thoại, cùng mạng Wi‑Fi với máy tính (hoặc cắm cáp USB): điện thoại tự hiện ở cột bên trái.",
            TextWrapping = TextWrapping.Wrap,
            Style = Res<Style>("CaptionTextBlockStyle"),
            Foreground = Res<Brush>("TextFillColorSecondaryBrush"),
        });
        var dialog = new ContentDialog
        {
            XamlRoot = RootGrid.XamlRoot,
            Title = "Cài app Cam Studio trên điện thoại",
            Content = content,
            PrimaryButtonText = "Mở trang tải",
            CloseButtonText = "Đóng",
            DefaultButton = ContentDialogButton.Close,
            RequestedTheme = RootGrid.ActualTheme,
        };
        try
        {
            if (await dialog.ShowAsync() == ContentDialogResult.Primary)
                await Windows.System.Launcher.LaunchUriAsync(new Uri(PhoneAppUrl));
        }
        catch (Exception)
        {
            // another dialog is open
        }
    }
}
