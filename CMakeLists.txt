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

# Camera engine used by the WinUI app (app/): capture -> orient/crop/zoom ->
# shared memory, and the virtual camera registration. Flat C API.
add_library(CamStudioEngine SHARED
    src/engine/EngineApi.cpp
    src/engine/CaptureEngine.cpp
    src/engine/VideoDecoder.cpp
    src/engine/PhoneLink.cpp
    src/engine/Discovery.cpp
    src/engine/Crypto.cpp)
target_link_libraries(CamStudioEngine PRIVATE mfplat mfuuid mf mfreadwrite mfsensorgroup ole32 ws2_32 dnsapi bcrypt crypt32)

# Stand-in phone for testing the PC side of the protocol without a device.
add_executable(camstudio-fake-phone tools/fake_phone.cpp src/engine/Crypto.cpp)
target_link_libraries(camstudio-fake-phone PRIVATE mfplat mfuuid ole32 ws2_32 bcrypt crypt32)

# Protocol crypto against docs/protocol-vectors.json.
add_executable(camstudio-protocol-test src/verify/protocol_test.cpp src/engine/Crypto.cpp)

# End-to-end check of resolution and live frames.
add_executable(camstudio-verify src/verify/main.cpp)
target_link_libraries(camstudio-verify PRIVATE mfplat mfuuid mf mfreadwrite ole32)

# Opens the virtual camera through DirectShow (ksproxy) like Discord, and keeps
# holding it when it goes away: reproduces consumers that never let go.
add_executable(camstudio-dshow-consumer tools/dshow_consumer.cpp)
target_link_libraries(camstudio-dshow-consumer PRIVATE strmiids ole32 oleaut32)
