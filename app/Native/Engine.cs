using System;
using System.Runtime.InteropServices;
using System.Text;

namespace CamStudio.Native;

// Mirrors src/engine/EngineApi.h. Keep the layouts in sync.

[StructLayout(LayoutKind.Sequential)]
public unsafe struct CsFormat
{
    public uint Width;
    public uint Height;
    public uint FpsNum;
    public uint FpsDen;
    public uint Fourcc;
    public int IsNative;
    public int Lens;          // phones: 1 back, 2 front, 3 external; 0 webcams
    public fixed char LensLabel[64];

    public readonly double Fps => FpsDen == 0 ? 0 : (double)FpsNum / FpsDen;

    public readonly string LensText
    {
        get
        {
            fixed (char* p = LensLabel) return new string(p);
        }
    }
}

[StructLayout(LayoutKind.Sequential)]
public struct CsImage
{
    public int Rotation;
    public int Mirror;
    public int Flip;
    public int Aspect;
    public double Zoom;
    public double CenterX;
    public double CenterY;
    public int Brightness;   // -100 .. 100
    public int Contrast;
    public int Saturation;
    public int Temperature;  // + warmer, - cooler
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct CsStatus
{
    public int State;
    public uint Width;
    public uint Height;
    public uint CameraWidth;
    public uint CameraHeight;
    public uint CameraFourcc;
    public double Fps;
    public double ProcessMs;
    public ulong FramesCaptured;
    public ulong FramesDropped;
    public int SharedMemory;
    public int ConsumerActive;
    public int SourceRotation;
    public fixed char Error[256];

    public readonly string ErrorText
    {
        get
        {
            fixed (char* p = Error) return new string(p);
        }
    }
}

public enum EngineState
{
    Stopped = 0,
    Starting = 1,
    Running = 2,
    Failed = 3,
}

internal static unsafe partial class Engine
{
    private const string Dll = "CamStudioEngine.dll";

    [LibraryImport(Dll)] public static partial int cs_init();
    [LibraryImport(Dll)] public static partial void cs_shutdown();
    [LibraryImport(Dll)] public static partial int cs_refresh_cameras();
    [LibraryImport(Dll)] public static partial int cs_camera_name(int camera, char* buffer, int capacity);
    [LibraryImport(Dll)] public static partial int cs_camera_kind(int camera);
    [LibraryImport(Dll)] public static partial int cs_load_formats(int camera);
    [LibraryImport(Dll)] public static partial int cs_get_format(int index, CsFormat* format);
    [LibraryImport(Dll)] public static partial int cs_last_error(char* buffer, int capacity);
    [LibraryImport(Dll)] public static partial int cs_pairing_code(char* buffer, int capacity);
    [LibraryImport(Dll)] public static partial void cs_pairing_answer(int accept);
    [LibraryImport(Dll, StringMarshalling = StringMarshalling.Utf16)] public static partial void cs_add_phone(string host, int port);
    [LibraryImport(Dll)] public static partial int cs_start(int camera, int format, CsImage* image);
    [LibraryImport(Dll)] public static partial void cs_stop();
    [LibraryImport(Dll)] public static partial int cs_set_image(CsImage* image);
    [LibraryImport(Dll)] public static partial void cs_get_status(CsStatus* status);
    [LibraryImport(Dll)] public static partial void cs_set_preview_enabled(int enabled);
    [LibraryImport(Dll)] public static partial void cs_set_preview_size(int maxWidth, int maxHeight);
    [LibraryImport(Dll)] public static partial int cs_copy_preview(uint* pixels, int capacity, uint* width, uint* height, ulong* frameId);
    [LibraryImport(Dll)] public static partial int cs_phone_caps(byte* buffer, int capacity);
    [LibraryImport(Dll)] public static partial int cs_phone_state(byte* buffer, int capacity);
    [LibraryImport(Dll)] public static partial int cs_phone_send(byte* json);

    public static bool IsPhone(int camera) => cs_camera_kind(camera) == 1;

    public static string CameraName(int camera)
    {
        char* buf = stackalloc char[512];
        return Text(buf, cs_camera_name(camera, buf, 512));
    }

    public static string LastError()
    {
        char* buf = stackalloc char[512];
        return Text(buf, cs_last_error(buf, 512));
    }

    public static string PairingCode()
    {
        char* buf = stackalloc char[64];
        return Text(buf, cs_pairing_code(buf, 64));
    }

    private static string Text(char* buf, int length) => length <= 0 ? string.Empty : new string(buf, 0, Math.Min(length, 511));

    public static string PhoneCapsJson() => ReadUtf8(&cs_phone_caps);
    public static string PhoneStateJson() => ReadUtf8(&cs_phone_state);

    private static string ReadUtf8(delegate*<byte*, int, int> read)
    {
        int needed = read(null, 0);
        if (needed <= 1) return string.Empty;
        byte[] buf = new byte[needed];
        fixed (byte* p = buf)
        {
            read(p, buf.Length);
            return Encoding.UTF8.GetString(buf, 0, needed - 1);
        }
    }

    public static bool PhoneSend(string json)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(json + "\0");
        fixed (byte* p = bytes) return cs_phone_send(p) >= 0;
    }

    public static CsFormat GetFormat(int index)
    {
        CsFormat f;
        cs_get_format(index, &f);
        return f;
    }

    public static int Start(int camera, int format, CsImage image) => cs_start(camera, format, &image);

    public static bool SetImage(CsImage image) => cs_set_image(&image) != 0;

    public static CsStatus GetStatus()
    {
        CsStatus s;
        cs_get_status(&s);
        return s;
    }

    public static string FourccText(uint fourcc)
    {
        Span<char> c = stackalloc char[4];
        for (int i = 0; i < 4; i++)
        {
            char ch = (char)((fourcc >> (8 * i)) & 0xFF);
            c[i] = char.IsAsciiLetterOrDigit(ch) ? ch : '?';
        }
        return new string(c).TrimEnd('?');
    }
}
