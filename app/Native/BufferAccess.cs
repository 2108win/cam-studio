using System;
using System.Runtime.InteropServices;
using Windows.Storage.Streams;

namespace CamStudio.Native;

// Raw pointer to an IBuffer's bytes (IBufferByteAccess), so preview frames
// are copied by the engine straight into the WriteableBitmap: no managed
// array and no extra copy per frame.
internal static unsafe class BufferAccess
{
    private static readonly Guid IID_IBufferByteAccess = new("905a0fef-bc53-11df-8c49-001e4fc686da");

    public static byte* GetPointer(IBuffer buffer)
    {
        nint unknown = WinRT.MarshalInterface<IBuffer>.FromManaged(buffer);
        try
        {
            Marshal.ThrowExceptionForHR(Marshal.QueryInterface(unknown, in IID_IBufferByteAccess, out nint access));
            try
            {
                // IBufferByteAccess::Buffer is vtable slot 3 (after IUnknown).
                var buffer_ = (delegate* unmanaged[Stdcall]<nint, byte**, int>)(*(void***)access)[3];
                byte* data;
                Marshal.ThrowExceptionForHR(buffer_(access, &data));
                return data;
            }
            finally
            {
                Marshal.Release(access);
            }
        }
        finally
        {
            Marshal.Release(unknown);
        }
    }
}
