using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace CamStudio.Services;

// System-wide shortcuts (RegisterHotKey): they work while Cam Studio is hidden in the tray or
// another app (Discord, Zoom) is in front. A message-only window on the UI thread receives
// WM_HOTKEY, so the handlers run on the UI thread.
public sealed unsafe partial class GlobalHotkeys : IDisposable
{
    public const uint ModAlt = 0x1, ModControl = 0x2, ModShift = 0x4, ModWin = 0x8;
    private const uint ModNoRepeat = 0x4000;
    private const uint WM_HOTKEY = 0x0312;

    public event Action<int>? Pressed;               // the id given to Register
    private readonly WndProcDelegate _wndProc;       // kept alive for the window's lifetime
    private readonly nint _hwnd;
    private readonly HashSet<int> _registered = new();

    public GlobalHotkeys()
    {
        _wndProc = WndProc;
        nint inst = GetModuleHandleW(null);
        fixed (char* cls = "CamStudioHotkeyWindow")
        {
            var wc = new WNDCLASSEXW
            {
                cbSize = (uint)sizeof(WNDCLASSEXW),
                lpfnWndProc = Marshal.GetFunctionPointerForDelegate(_wndProc),
                hInstance = inst,
                lpszClassName = cls,
            };
            RegisterClassExW(&wc);
            _hwnd = CreateWindowExW(0, cls, cls, 0, 0, 0, 0, 0, -3 /* HWND_MESSAGE */, 0, inst, 0);
        }
    }

    // A packed shortcut: modifiers in the high 16 bits, the virtual key in the low ones; 0 = none.
    public static int Pack(uint modifiers, uint key) => (int)((modifiers << 16) | (key & 0xFFFF));
    public static uint Modifiers(int packed) => (uint)packed >> 16;
    public static uint Key(int packed) => (uint)packed & 0xFFFF;

    // False when another app already owns this combination.
    public bool Register(int id, int packed)
    {
        Unregister(id);
        if (packed == 0 || _hwnd == 0) return true;
        if (RegisterHotKey(_hwnd, id, Modifiers(packed) | ModNoRepeat, Key(packed)) == 0) return false;
        _registered.Add(id);
        return true;
    }

    public void Unregister(int id)
    {
        if (_registered.Remove(id)) UnregisterHotKey(_hwnd, id);
    }

    public void UnregisterAll()
    {
        foreach (int id in _registered) UnregisterHotKey(_hwnd, id);
        _registered.Clear();
    }

    // "Ctrl + Alt + P" (the key's name in the current keyboard layout).
    public static string Describe(int packed)
    {
        if (packed == 0) return "Không dùng";
        uint m = Modifiers(packed), vk = Key(packed);
        var parts = new List<string>();
        if ((m & ModControl) != 0) parts.Add("Ctrl");
        if ((m & ModAlt) != 0) parts.Add("Alt");
        if ((m & ModShift) != 0) parts.Add("Shift");
        if ((m & ModWin) != 0) parts.Add("Win");
        parts.Add(KeyName(vk));
        return string.Join(" + ", parts);
    }

    private static string KeyName(uint vk)
    {
        if (vk is >= 0x30 and <= 0x39 or >= 0x41 and <= 0x5A) return ((char)vk).ToString();
        if (vk is >= 0x70 and <= 0x87) return "F" + (vk - 0x6F);
        uint scan = MapVirtualKeyW(vk, 0 /* MAPVK_VK_TO_VSC */);
        char* name = stackalloc char[64];
        int n = GetKeyNameTextW((int)(scan << 16), name, 64);
        return n > 0 ? new string(name, 0, n) : $"0x{vk:X2}";
    }

    public void Dispose()
    {
        UnregisterAll();
        if (_hwnd != 0) DestroyWindow(_hwnd);
    }

    private nint WndProc(nint hwnd, uint msg, nint wParam, nint lParam)
    {
        if (msg == WM_HOTKEY)
        {
            Pressed?.Invoke((int)wParam);
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
    [LibraryImport("user32.dll")] private static partial int RegisterHotKey(nint hwnd, int id, uint modifiers, uint vk);
    [LibraryImport("user32.dll")] private static partial int UnregisterHotKey(nint hwnd, int id);
    [LibraryImport("user32.dll")] private static partial uint MapVirtualKeyW(uint code, uint type);
    [LibraryImport("user32.dll")] private static partial int GetKeyNameTextW(int lParam, char* buffer, int size);
    [LibraryImport("kernel32.dll")] private static partial nint GetModuleHandleW(char* name);
}
