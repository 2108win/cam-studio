cmake_minimum_required(VERSION 3.21)
project(CamStudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
add_compile_options(/W4 /permissive- /utf-8 /EHsc)

# Virtual camera media source, loaded by the Windows Frame Server service.
add_library(CamStudioVCam SHARED
    src/vcam/dllmain.cpp
    src/vcam/Activate.cpp
    src/vcam/MediaSource.cpp
    src/vcam/MediaStream.cpp
    src/vcam/FrameReader.cpp
    src/vcam/vcam.def)
target_link_libraries(CamStudioVCam PRIVATE mfplat mfuuid mf mfsensorgroup propsys advapi32 runtimeobject)

# Windows 10 virtual camera: a DirectShow source filter that reads the same frames from the
# host's local channel (docs/windows10.md). Built for x64 and (scripts\build.ps1, build-x86) x86,
# since a 32-bit app loads a 32-bit filter.
add_library(CamStudioDShow SHARED
    src/dshow/dllmain.cpp
    src/dshow/SourceFilter.cpp
    src/vcam/FrameReader.cpp
    src/dshow/dshow.def)
target_link_libraries(CamStudioDShow PRIVATE mfplat strmiids ole32 oleaut32 advapi32 uuid runtimeobject)

# Camera engine used by the WinUI app (app/): capture -> orient/crop/zoom/framing ->
# shared memory, and the virtual camera registration. Flat C API.
add_library(CamStudioEngine SHARED
    src/engine/EngineApi.cpp
    src/engine/CaptureEngine.cpp
    src/engine/VideoDecoder.cpp
    src/engine/PhoneLink.cpp
    src/engine/Connector.cpp
    src/engine/ScreenViewer.cpp
    src/engine/WindowSource.cpp
    src/engine/FaceDetect.cpp
    src/engine/StillPicture.cpp
    src/engine/Recorder.cpp
    src/engine/Segmenter.cpp
    src/engine/Discovery.cpp
    src/engine/UsbTransport.cpp
    src/engine/Crypto.cpp)
# mfsensorgroup (MFCreateVirtualCamera, Windows 11) is loaded at run time: see VirtualCameraApi().
target_link_libraries(CamStudioEngine PRIVATE mfplat mfuuid mf mfreadwrite ole32 ws2_32 dnsapi bcrypt crypt32 cfgmgr32
    windowsapp d3d11 dxgi dwmapi d2d1 dwrite windowscodecs)
# C++/WinRT is used in WindowSource.cpp (Windows.Graphics.Capture), FaceDetect.cpp (FaceTracker) and
# Segmenter.cpp (Windows ML) only.
set_source_files_properties(src/engine/WindowSource.cpp src/engine/FaceDetect.cpp src/engine/Segmenter.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")

# Stand-in phone for testing the PC side of the protocol without a device.
add_executable(camstudio-fake-phone tools/fake_phone.cpp src/engine/Crypto.cpp)
target_link_libraries(camstudio-fake-phone PRIVATE mfplat mfuuid ole32 ws2_32 bcrypt crypt32)

# Phones on a USB cable: lists them (adb, usbmuxd) and exchanges "hello" through the tunnel.
add_executable(camstudio-usb-check tools/usb_check.cpp src/engine/UsbTransport.cpp)
target_link_libraries(camstudio-usb-check PRIVATE ws2_32 cfgmgr32)

# Picture code: the virtual camera size rule, fit with black bars, BGRA <-> NV12 (also builds on a Mac).
add_executable(camstudio-frame-check tools/frame_check.cpp)

# Automatic framing: the shot for some faces, easing, the zoom limit (also builds on a Mac).
add_executable(camstudio-framing-check tools/framing_check.cpp)

# Protocol crypto against docs/protocol-vectors.json.
add_executable(camstudio-protocol-test src/verify/protocol_test.cpp src/engine/Crypto.cpp)

# End-to-end check of resolution and live frames.
add_executable(camstudio-verify src/verify/main.cpp)
target_link_libraries(camstudio-verify PRIVATE mfplat mfuuid mf mfreadwrite ole32)

# Opens the virtual camera through DirectShow (ksproxy) like Discord, and keeps
# holding it when it goes away: reproduces consumers that never let go.
add_executable(camstudio-dshow-consumer tools/dshow_consumer.cpp)
target_link_libraries(camstudio-dshow-consumer PRIVATE strmiids ole32 oleaut32)
