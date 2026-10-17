using System;
using Microsoft.UI.Xaml;
using Microsoft.Windows.AppLifecycle;

namespace CamStudio;

public partial class App : Application
{
    private MainWindow? _window;

    public App()
    {
        InitializeComponent();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        // Command line: --device <index|name> selects a camera and starts it;
        // --tray starts hidden and resumes the last camera (autostart).
        string? device = null;
        bool tray = false;
        string[] argv = Environment.GetCommandLineArgs();
        for (int i = 1; i < argv.Length; i++)
        {
            if (argv[i] == "--device" && i + 1 < argv.Length) device = argv[++i];
            else if (argv[i] == "--tray") tray = true;
        }

        _window = new MainWindow(new MainWindow.LaunchOptions(device, tray));
        if (!tray) _window.Activate();

        // A second launch redirects here: bring the window forward.
        AppInstance.GetCurrent().Activated += (_, _) =>
            _window.DispatcherQueue.TryEnqueue(_window.ShowFromTray);
    }
}
