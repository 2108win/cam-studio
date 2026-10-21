using System;
using CamStudio.Services;
using Microsoft.UI.Xaml;
using Microsoft.Windows.AppLifecycle;

namespace CamStudio;

public partial class App : Application
{
    private MainWindow? _window;

    public App()
    {
        // The last crash, for whoever looks into it: %LOCALAPPDATA%\CamStudio\crash.txt.
        UnhandledException += (_, e) =>
        {
            try
            {
                string dir = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CamStudio");
                System.IO.Directory.CreateDirectory(dir);
                System.IO.File.WriteAllText(System.IO.Path.Combine(dir, "crash.txt"), $"{DateTime.Now:O}\n{e.Message}\n{e.Exception}\n");
            }
            catch (Exception)
            {
            }
        };
        InitializeComponent();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        // Command line: --device <index|name> selects a camera and starts it;
        // --tray starts hidden and resumes the last camera (autostart);
        // --updated: opened again by the installer after an update;
        // --after <pid>: "Khởi động lại" (Program.Main waited for that process).
        string? device = null;
        bool tray = false, updated = false, restarted = false;
        string[] argv = Environment.GetCommandLineArgs();
        for (int i = 1; i < argv.Length; i++)
        {
            if (argv[i] == "--device" && i + 1 < argv.Length) device = argv[++i];
            else if (argv[i] == "--tray") tray = true;
            else if (argv[i] == "--updated") updated = true;
            else if (argv[i] == "--after") restarted = true;
        }

        // Settings › Màu nhấn, before any control reads the accent (Resources is not ready in the
        // constructor: E_UNEXPECTED).
        AccentColors.Apply(Resources, AppSettings.Load().Accent);
        _window = new MainWindow(new MainWindow.LaunchOptions(device, tray, updated, restarted));
        if (!tray) _window.Activate();

        // A second launch redirects here: bring the window forward.
        AppInstance.GetCurrent().Activated += (_, _) =>
            _window.DispatcherQueue.TryEnqueue(_window.ShowFromTray);
    }
}
