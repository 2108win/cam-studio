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
    public int Standby;       // virtual camera up, source closed until an app opens it (or the preview shows)
    public uint PictureWidth; // the picture inside the output (smaller when fitted with black bars)
    public uint PictureHeight;
    public uint SourceId;     // which Start/Switch the frame counts belong to
    public int NoSource;      // the camera is up with no source (cs_switch_none): apps see "no signal"
    public fixed char Error[256];

    public readonly string ErrorText
    {
        get
        {
            fixed (char* p = Error) return new string(p);
        }
    }
}

// Mirrors CsBackground: the "Nền" effect.
[StructLayout(LayoutKind.Sequential)]
public struct CsBackground
{
    public int Available; // 1 works (or not tried yet), 0 Windows ML / the model missing, -1 gave up
    public int Active;    // blending frames now
    public double InferMs;
}

// Mirrors CsFraming: what "Tự căn khung" shows now.
[StructLayout(LayoutKind.Sequential)]
public struct CsFraming
{
    public double Zoom;       // the view shown now (1 .. MaxZoom)
    public double CenterX;    // 0 .. 1 of the oriented frame, as CsImage
    public double CenterY;
    public double MaxZoom;    // how far framing may zoom into this picture
    public int Active;        // framing moves the picture now
    public int Faces;         // people in the last look
    public int Available;     // 1 face detection works, 0 Windows has none, -1 it kept failing
    public int CameraSource;  // the source is a camera (screens and windows are never framed)
}

// Mirrors CsLinkState: a phone's background connection, for the camera menu.
public enum LinkState
{
    Idle = 0,
    Connected = 1,    // webcams always report this
    Connecting = 2,
    NeedsPairing = 3,
    Busy = 4,         // another PC has the phone
    Unreachable = 5,
    Elsewhere = 6,    // the same phone is connected another way (Wi‑Fi / cable)
}

public enum EngineState
{
    Stopped = 0,
    Starting = 1,
    Running = 2,
    Failed = 3,
    Waiting = 4, // source lost: the virtual camera stays up while the engine reconnects
}

internal static unsafe partial class Engine
{
    private const string Dll = "CamStudioEngine.dll";

    [LibraryImport(Dll)] public static partial int cs_init();
    [LibraryImport(Dll)] public static partial int cs_init_delayed(int delayMs);
    [LibraryImport(Dll)] public static partial int cs_switch_none(int width, int height);
    [LibraryImport(Dll)] public static partial void cs_shutdown();
    [LibraryImport(Dll)] public static partial int cs_refresh_cameras();
    [LibraryImport(Dll)] public static partial int cs_camera_name(int camera, char* buffer, int capacity);
    [LibraryImport(Dll)] public static partial int cs_camera_kind(int camera);
    [LibraryImport(Dll)] public static partial int cs_camera_id(int camera, char* buffer, int capacity);
    [LibraryImport(Dll)] public static partial int cs_camera_state(int camera, char* error, int capacity);
    [LibraryImport(Dll)] public static partial uint cs_sources_generation();
    [LibraryImport(Dll)] public static partial int cs_load_formats(int camera);
    [LibraryImport(Dll)] public static partial int cs_get_format(int index, CsFormat* format);
    [LibraryImport(Dll)] public static partial int cs_last_error(char* buffer, int capacity);
    [LibraryImport(Dll)] public static partial int cs_pairing_code(char* buffer, int capacity);
    [LibraryImport(Dll)] public static partial void cs_pairing_answer(int accept);
    [LibraryImport(Dll, StringMarshalling = StringMarshalling.Utf16)] public static partial void cs_add_phone(string host, int port);
    public const int PhoneManual = 1, PhonePaired = 2; // cs_phone_flags
    [LibraryImport(Dll)] public static partial int cs_phone_flags(int camera);
    [LibraryImport(Dll)] public static partial void cs_forget_phone(int camera, int what);
    [LibraryImport(Dll, StringMarshalling = StringMarshalling.Utf16)] public static partial void cs_set_pause_picture(int kind, string text, string imagePath);
    [LibraryImport(Dll, StringMarshalling = StringMarshalling.Utf16)] public static partial void cs_set_background(int mode, int strength, string imagePath, string modelPath);
    [LibraryImport(Dll)] public static partial void cs_get_background(out CsBackground state);
    [LibraryImport(Dll, StringMarshalling = StringMarshalling.Utf16)] public static partial int cs_snapshot(string path);
    [LibraryImport(Dll, StringMarshalling = StringMarshalling.Utf16)] public static partial int cs_record_start(string path, int microphone);
    [LibraryImport(Dll)] public static partial int cs_record_stop();
    [LibraryImport(Dll)] public static partial long cs_record_status(int* hasAudio, char* problem, int capacity);

    // ms recorded so far (0: not recording); why there is no sound, if so.
    public static long RecordStatus(out bool hasAudio, out string problem)
    {
        int audio = 0;
        char* buffer = stackalloc char[256];
        long ms = cs_record_status(&audio, buffer, 256);
        hasAudio = audio != 0;
        problem = new string(buffer);
        return ms;
    }
    [LibraryImport(Dll)] public static partial int cs_start(int camera, int format, CsImage* image);
    [LibraryImport(Dll)] public static partial int cs_switch(int camera, int format, CsImage* image);
    [LibraryImport(Dll)] public static partial void cs_set_output_size(int mode);
    [LibraryImport(Dll)] public static partial void cs_stop();
    [LibraryImport(Dll)] public static partial int cs_set_image(CsImage* image);
    [LibraryImport(Dll)] public static partial void cs_get_status(CsStatus* status);
    [LibraryImport(Dll)] public static partial void cs_set_auto_frame(int enabled);
    [LibraryImport(Dll)] public static partial void cs_get_framing(CsFraming* framing);
    [LibraryImport(Dll)] public static partial void cs_set_preview_enabled(int enabled);
    [LibraryImport(Dll)] public static partial void cs_set_preview_size(int maxWidth, int maxHeight);
    [LibraryImport(Dll)] public static partial int cs_copy_preview(uint* pixels, int capacity, uint* width, uint* height, ulong* frameId);
    [LibraryImport(Dll)] public static partial int cs_phone_caps(byte* buffer, int capacity);
    [LibraryImport(Dll)] public static partial int cs_phone_state(byte* buffer, int capacity);
    [LibraryImport(Dll)] public static partial int cs_phone_send(byte* json);
    [LibraryImport(Dll)] public static partial int cs_viewer_open(int camera);
    [LibraryImport(Dll)] public static partial void cs_viewer_set_size(int viewer, int maxWidth, int maxHeight);
    [LibraryImport(Dll)] public static partial int cs_viewer_copy(int viewer, uint* pixels, int capacity, uint* width, uint* height, ulong* frameId);
    [LibraryImport(Dll)] public static partial int cs_viewer_state(int viewer, uint* screenWidth, uint* screenHeight, char* error, int capacity);
    [LibraryImport(Dll)] public static partial void cs_viewer_close(int viewer);
    [LibraryImport(Dll)] public static partial int cs_list_windows();
    [LibraryImport(Dll)] public static partial int cs_window_title(int index, char* buffer, int capacity);
    [LibraryImport(Dll)] public static partial long cs_window_handle(int index);
    [LibraryImport(Dll)] public static partial int cs_switch_window(long handle, CsImage* image);

    // Phone camera (1) or a phone's shared screen (2): both connect over the network or a cable.
    public static bool IsPhone(int camera) => cs_camera_kind(camera) is 1 or 2;
    public static bool IsScreen(int camera) => cs_camera_kind(camera) == 2;

    public static string CameraName(int camera)
    {
        char* buf = stackalloc char[512];
        return Text(buf, cs_camera_name(camera, buf, 512));
    }

    public static string CameraId(int camera)
    {
        int needed = cs_camera_id(camera, null, 0);
        if (needed <= 0) return string.Empty;
        char[] buf = new char[needed + 1];
        fixed (char* p = buf) cs_camera_id(camera, p, buf.Length);
        return new string(buf, 0, needed);
    }

    public static LinkState CameraState(int camera, out string error)
    {
        char* buf = stackalloc char[512];
        int state = cs_camera_state(camera, buf, 512);
        error = new string(buf);
        return state < 0 ? LinkState.Idle : (LinkState)state;
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

    // Both return the new source's id (> 0, see CsStatus.SourceId) or an HRESULT (< 0).
    public static int Start(int camera, int format, CsImage image) => cs_start(camera, format, &image);
    public static int Switch(int camera, int format, CsImage image) => cs_switch(camera, format, &image);
    public static int SwitchWindow(long handle, CsImage image) => cs_switch_window(handle, &image);

    // Windows on this PC that can be the camera's source.
    public static System.Collections.Generic.List<(long Handle, string Title)> ListWindows()
    {
        var list = new System.Collections.Generic.List<(long, string)>();
        int count = cs_list_windows();
        char* buf = stackalloc char[512];
        for (int i = 0; i < count; i++)
        {
            long handle = cs_window_handle(i);
            if (handle != 0) list.Add((handle, Text(buf, cs_window_title(i, buf, 512))));
        }
        return list;
    }

    public static bool SetImage(CsImage image) => cs_set_image(&image) != 0;

    public static CsFraming GetFraming()
    {
        CsFraming f;
        cs_get_framing(&f);
        return f;
    }

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
