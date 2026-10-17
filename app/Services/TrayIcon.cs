using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace CamStudio.Services;

// Notification-area icon. WinUI 3 has no tray API, so this owns a hidden
// Win32 window that receives the icon's callbacks.
public sealed unsafe partial class TrayIcon : IDisposable
{
    public sealed record MenuItem(string Text, Action OnClick, bool Checked = false, bool IsDefault = false);

    public event Action? Activated;                 // click / double-click
    public Func<IReadOnlyList<MenuItem?>>? BuildMenu; // null entry = separator

    private const int WM_APP_TRAY = 0x8000 + 1;
    private const uint IconId = 1;
    private readonly WndProcDelegate _wndProc;      // kept alive for the window's lifetime
    private readonly nint _hwnd;
    private readonly nint _icon;
    private readonly uint _taskbarCreated;
    private string _tip = "Cam Studio";
    private bool _added;

    public TrayIcon(string iconPath)
    {
        _wndProc = WndProc;
        nint inst = GetModuleHandleW(null);
        fixed (char* cls = "CamStudioTrayWindow")
        {
            var wc = new WNDCLASSEXW
            {
                cbSize = (uint)sizeof(WNDCLASSEXW),
                lpfnWndProc = Marshal.GetFunctionPointerForDelegate(_wndProc),
                hInstance = inst,
                lpszClassName = cls,
            };
            RegisterClassExW(&wc);
            _hwnd = CreateWindowExW(0, cls, cls, 0, 0, 0, 0, 0, 0, 0, inst, 0);
        }
        int size = GetSystemMetrics(49 /* SM_CXSMICON */);
        fixed (char* p = iconPath) _icon = LoadImageW(0, p, 1 /* IMAGE_ICON */, size, size, 0x10 /* LR_LOADFROMFILE */);
        fixed (char* p = "TaskbarCreated") _taskbarCreated = RegisterWindowMessageW(p);
        Add();
    }

    public void SetTooltip(string text)
    {
        if (text == _tip) return;
        _tip = text;
        var nid = NewData();
        nid.uFlags = NIF_TIP | NIF_SHOWTIP;
        CopyString(text, nid.szTip, 128);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    public void ShowBalloon(string title, string text)
    {
        var nid = NewData();
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = 1; // NIIF_INFO
        CopyString(title, nid.szInfoTitle, 64);
        CopyString(text, nid.szInfo, 256);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    public void Dispose()
    {
        if (_added)
        {
            var nid = NewData();
            Shell_NotifyIconW(NIM_DELETE, &nid);
            _added = false;
        }
        if (_icon != 0) DestroyIcon(_icon);
        if (_hwnd != 0) DestroyWindow(_hwnd);
    }

    private void Add()
    {
        var nid = NewData();
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        nid.uCallbackMessage = WM_APP_TRAY;
        nid.hIcon = _icon;
        CopyString(_tip, nid.szTip, 128);
        if (Shell_NotifyIconW(NIM_ADD, &nid) != 0)
        {
            nid.uVersion = 4; // NOTIFYICON_VERSION_4
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
            _added = true;
        }
    }

    private nint WndProc(nint hwnd, uint msg, nint wParam, nint lParam)
    {
        if (msg == _taskbarCreated && _taskbarCreated != 0)
        {
            _added = false; // Explorer restarted
            Add();
            return 0;
        }
        if (msg == WM_APP_TRAY)
        {
            switch ((uint)(lParam & 0xFFFF))
            {
                case 0x0400: // NIN_SELECT
                case 0x0401: // NIN_KEYSELECT
                case 0x0203: // WM_LBUTTONDBLCLK
                    Activated?.Invoke();
                    break;
                case 0x007B: // WM_CONTEXTMENU; anchor point in wParam
                    ShowMenu((short)(wParam & 0xFFFF), (short)((wParam >> 16) & 0xFFFF));
                    break;
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    private void ShowMenu(int x, int y)
    {
        var items = BuildMenu?.Invoke();
        if (items is null) return;

        nint menu = CreatePopupMenu();
        for (int i = 0; i < items.Count; i++)
        {
            var item = items[i];
            if (item is null)
            {
                AppendMenuW(menu, 0x800 /* MF_SEPARATOR */, 0, null);
                continue;
            }
            fixed (char* text = item.Text)
                AppendMenuW(menu, item.Checked ? 0x8u /* MF_CHECKED */ : 0u, (nuint)(i + 1), text);
            if (item.IsDefault) SetMenuDefaultItem(menu, (uint)(i + 1), 0);
        }

        // Required so the menu closes when the user clicks elsewhere.
        SetForegroundWindow(_hwnd);
        int cmd = TrackPopupMenuEx(menu, 0x0100 /* TPM_RETURNCMD */ | 0x0002 /* TPM_RIGHTBUTTON */ | 0x0020 /* TPM_BOTTOMALIGN */ | 0x0008 /* TPM_RIGHTALIGN */,
            x, y, _hwnd, 0);
        DestroyMenu(menu);
        if (cmd > 0 && items[cmd - 1] is { } chosen) chosen.OnClick();
    }

    private NOTIFYICONDATAW NewData() => new()
    {
        cbSize = (uint)sizeof(NOTIFYICONDATAW),
        hWnd = _hwnd,
        uID = IconId,
    };

    private static void CopyString(string s, char* dst, int capacity)
    {
        int n = Math.Min(s.Length, capacity - 1);
        for (int i = 0; i < n; i++) dst[i] = s[i];
        dst[n] = '\0';
    }

    // ---- Win32 ----------------------------------------------------------------

    private const uint NIM_ADD = 0, NIM_MODIFY = 1, NIM_DELETE = 2, NIM_SETVERSION = 4;
    private const uint NIF_MESSAGE = 0x1, NIF_ICON = 0x2, NIF_TIP = 0x4, NIF_INFO = 0x10, NIF_SHOWTIP = 0x80;

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

    [StructLayout(LayoutKind.Sequential)]
    private struct NOTIFYICONDATAW
    {
        public uint cbSize;
        public nint hWnd;
        public uint uID, uFlags, uCallbackMessage;
        public nint hIcon;
        public fixed char szTip[128];
        public uint dwState, dwStateMask;
        public fixed char szInfo[256];
        public uint uVersion;
        public fixed char szInfoTitle[64];
        public uint dwInfoFlags;
        public Guid guidItem;
        public nint hBalloonIcon;
    }

    [LibraryImport("kernel32.dll", StringMarshalling = StringMarshalling.Utf16)] private static partial nint GetModuleHandleW(string? name);
    [LibraryImport("user32.dll")] private static partial ushort RegisterClassExW(WNDCLASSEXW* wc);
    [LibraryImport("user32.dll")] private static partial nint CreateWindowExW(uint exStyle, char* cls, char* name, uint style, int x, int y, int w, int h, nint parent, nint menu, nint inst, nint param);
    [LibraryImport("user32.dll")] private static partial nint DefWindowProcW(nint hwnd, uint msg, nint wParam, nint lParam);
    [LibraryImport("user32.dll")] private static partial int DestroyWindow(nint hwnd);
    [LibraryImport("user32.dll")] private static partial uint RegisterWindowMessageW(char* name);
    [LibraryImport("user32.dll")] private static partial nint LoadImageW(nint inst, char* name, uint type, int cx, int cy, uint flags);
    [LibraryImport("user32.dll")] private static partial int DestroyIcon(nint icon);
    [LibraryImport("user32.dll")] private static partial int GetSystemMetrics(int index);
    [LibraryImport("user32.dll")] private static partial nint CreatePopupMenu();
    [LibraryImport("user32.dll")] private static partial int AppendMenuW(nint menu, uint flags, nuint id, char* text);
    [LibraryImport("user32.dll")] private static partial int SetMenuDefaultItem(nint menu, uint item, uint byPos);
    [LibraryImport("user32.dll")] private static partial int TrackPopupMenuEx(nint menu, uint flags, int x, int y, nint hwnd, nint tpm);
    [LibraryImport("user32.dll")] private static partial int DestroyMenu(nint menu);
    [LibraryImport("user32.dll")] private static partial int SetForegroundWindow(nint hwnd);
    [LibraryImport("shell32.dll")] private static partial int Shell_NotifyIconW(uint msg, NOTIFYICONDATAW* data);
}
