using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.Windows.AppLifecycle;

namespace CamStudio;

// Custom entry point (DISABLE_XAML_GENERATED_MAIN) so only one Cam Studio
// runs: a second launch hands its activation to the first one and exits.
public static partial class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        WinRT.ComWrappersSupport.InitializeComWrappers();
        WaitForPrevious(args);

        var main = AppInstance.FindOrRegisterForKey("CamStudio.Main");
        if (!main.IsCurrent)
        {
            RedirectActivation(main);
            return 0;
        }

        Application.Start(p =>
        {
            var context = new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread());
            SynchronizationContext.SetSynchronizationContext(context);
            new App();
        });
        return 0;
    }

    // "--after <pid>" (Khởi động lại): the old Cam Studio is still closing. Wait for it, so this one
    // becomes the instance instead of handing its activation to the old one. One that hangs is
    // ended: that is what the restart is for.
    private static void WaitForPrevious(string[] args)
    {
        int at = Array.IndexOf(args, "--after");
        if (at < 0 || at + 1 >= args.Length || !int.TryParse(args[at + 1], out int pid)) return;
        try
        {
            using var old = System.Diagnostics.Process.GetProcessById(pid);
            if (!string.Equals(old.MainModule?.FileName, Environment.ProcessPath, StringComparison.OrdinalIgnoreCase)) return;
            if (old.WaitForExit(10000)) return;
            old.Kill();
            old.WaitForExit(3000);
        }
        catch (Exception)
        {
            // Already gone, or not ours to touch.
        }
    }

    // Redirect on a worker thread while this STA thread keeps pumping COM,
    // as the Windows App SDK docs recommend, to avoid a deadlock.
    private static void RedirectActivation(AppInstance target)
    {
        var args = AppInstance.GetCurrent().GetActivatedEventArgs();
        nint done = CreateEventW(0, 1, 0, null);
        Task.Run(() =>
        {
            target.RedirectActivationToAsync(args).AsTask().Wait();
            SetEvent(done);
        });
        CoWaitForMultipleObjects(0, 0xFFFFFFFF, 1, [done], out _);
        CloseHandle(done);
    }

    [LibraryImport("kernel32.dll", StringMarshalling = StringMarshalling.Utf16)]
    private static partial nint CreateEventW(nint attributes, int manualReset, int initialState, string? name);

    [LibraryImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool SetEvent(nint handle);

    [LibraryImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool CloseHandle(nint handle);

    [LibraryImport("ole32.dll")]
    private static partial int CoWaitForMultipleObjects(uint flags, uint timeout, uint count, nint[] handles, out uint index);
}
