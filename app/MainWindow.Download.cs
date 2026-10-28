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

// Getting the phone app: one QR code per platform. Android: the release page (the APK is there).
// iPhone: the public TestFlight link (the same one as on winlax.id.vn/camstudio).
public sealed partial class MainWindow
{
    private const string AndroidAppUrl = "https://github.com/2108win/cam-studio/releases/latest";
    private const string IphoneAppUrl = "https://testflight.apple.com/join/h3NKXEjD";

    // The code as one vector path (no seams between modules at any scale) on white, with the
    // 4-module quiet zone scanners need.
    private static FrameworkElement QrImage(string text, double size, string name)
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
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(box, name);
        return box;
    }

    private void SetupPhoneAppCode()
    {
        PlaceholderQrAndroid.Child = QrImage(AndroidAppUrl, 96, "Mã QR tải app Cam Studio cho Android");
        PlaceholderQrIphone.Child = QrImage(IphoneAppUrl, 96, "Mã QR tải app Cam Studio cho iPhone");
    }

    // One platform in the dialog: its code, what it needs, how to install, and the link for this PC's browser.
    private static FrameworkElement PhoneAppColumn(string platform, string requirement, string steps, string url, string link)
    {
        var column = new StackPanel { Spacing = 8 };
        var code = QrImage(url, 160, $"Mã QR tải app Cam Studio cho {platform}");
        code.HorizontalAlignment = HorizontalAlignment.Left;
        column.Children.Add(code);
        column.Children.Add(new StackPanel
        {
            Children =
            {
                new TextBlock { Text = platform, Style = Res<Style>("BodyStrongTextBlockStyle") },
                new TextBlock
                {
                    Text = requirement,
                    Style = Res<Style>("CaptionTextBlockStyle"),
                    Foreground = Res<Brush>("TextFillColorSecondaryBrush"),
                },
            },
        });
        column.Children.Add(new TextBlock { Text = steps, TextWrapping = TextWrapping.Wrap });
        column.Children.Add(new HyperlinkButton { Content = link, NavigateUri = new Uri(url), Padding = new Thickness(0) });
        return column;
    }

    private async void PhoneApp_Click(object sender, RoutedEventArgs e)
    {
        var codes = new Grid { ColumnSpacing = 24 };
        codes.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        codes.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        var android = PhoneAppColumn("Android", "Android 8.0 trở lên",
            "Quét mã bằng camera điện thoại, tải file CamStudio-…-android.apk rồi mở để cài.",
            AndroidAppUrl, "Mở trang tải");
        var iphone = PhoneAppColumn("iPhone", "iOS 17 trở lên, qua TestFlight",
            "Cài app TestFlight của Apple từ App Store, rồi quét mã và bấm tham gia để cài.",
            IphoneAppUrl, "Mở link TestFlight");
        Grid.SetColumn(iphone, 1);
        codes.Children.Add(android);
        codes.Children.Add(iphone);

        var content = new StackPanel { Spacing = 16, Width = 400 };
        content.Children.Add(codes);
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
            CloseButtonText = "Đóng",
            DefaultButton = ContentDialogButton.Close,
            RequestedTheme = RootGrid.ActualTheme,
        };
        try
        {
            await dialog.ShowAsync();
        }
        catch (Exception)
        {
            // another dialog is open
        }
    }
}
