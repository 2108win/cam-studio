using System;
using System.Runtime.InteropServices;

namespace CamStudio.Services;

// The engine posts Message to this message-only window (on the UI thread) when a preview frame is
// ready, so the preview is drawn as frames arrive instead of on a timer (cs_set_notify).
public sealed unsafe partial class EngineSignal : IDisposable
{
    public const uint Message = 0x8000 + 2; // WM_APP + 2

    public event Action? Signaled;
    private readonly WndProcDelegate _wndProc; // kept alive for the window's lifetime
    public nint Hwnd { get; }

    public EngineSignal()
    {
        _wndProc = WndProc;
        nint inst = GetModuleHandleW(null);
        fixed (char* cls = "CamStudioEngineSignal")
        {
            var wc = new WNDCLASSEXW
            {
                cbSize = (uint)sizeof(WNDCLASSEXW),
                lpfnWndProc = Marshal.GetFunctionPointerForDelegate(_wndProc),
                hInstance = inst,
                lpszClassName = cls,
            };
            RegisterClassExW(&wc);
            Hwnd = CreateWindowExW(0, cls, cls, 0, 0, 0, 0, 0, -3 /* HWND_MESSAGE */, 0, inst, 0);
        }
    }

    public void Dispose()
    {
        if (Hwnd != 0) DestroyWindow(Hwnd);
    }

    private nint WndProc(nint hwnd, uint msg, nint wParam, nint lParam)
    {
        if (msg == Message)
        {
            Signaled?.Invoke();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate nint WndProcDelegate(nint hwnd, uint msg, nint wParam, nint lParam);

    [StructLayout(LayoutKind.Sequential)]
    private struct WNDCLASSEXW
    {
        public uint cbSize, style;
        public nint lpfnWndProc;
        public int cbClsExtra, cbWndExtra;
        public nint hInstance, hIcon, hCursor, hbrBackground;
        public char* lpszMenuName, lpszClassName;
        public nint hIconSm;
    }

    [LibraryImport("user32.dll")] private static partial ushort RegisterClassExW(WNDCLASSEXW* wc);
    [LibraryImport("user32.dll")] private static partial nint CreateWindowExW(uint exStyle, char* cls, char* name, uint style, int x, int y, int w, int h, nint parent, nint menu, nint inst, nint param);
    [LibraryImport("user32.dll")] private static partial int DestroyWindow(nint hwnd);
    [LibraryImport("user32.dll")] private static partial nint DefWindowProcW(nint hwnd, uint msg, nint wParam, nint lParam);
    [LibraryImport("kernel32.dll")] private static partial nint GetModuleHandleW(char* name);
}
