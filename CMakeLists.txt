cmake_minimum_required(VERSION 3.20)
project(RobloxShadeHost VERSION 0.3.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Third-party headers are downloaded once at configure time instead of being vendored.
function(fetch_header url hash destination)
    if(NOT EXISTS "${destination}")
        file(DOWNLOAD "${url}" "${destination}" EXPECTED_HASH "SHA256=${hash}" STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE "${destination}")
            list(GET status 1 message)
            message(FATAL_ERROR "Could not download ${url}: ${message}")
        endif()
    endif()
endfunction()

set(RESHADE_INCLUDE "${CMAKE_BINARY_DIR}/reshade/include")
set(RESHADE_HEADERS_URL "https://raw.githubusercontent.com/crosire/reshade/v6.8.0/include")
fetch_header("${RESHADE_HEADERS_URL}/reshade.hpp" cf3cd150920ea310b4b1e2e4f11ab8d7221a9fc18755f4891a026563311b524c "${RESHADE_INCLUDE}/reshade.hpp")
fetch_header("${RESHADE_HEADERS_URL}/reshade_api.hpp" 8d83ec8a620c48da7a20d975bd27b3345b24d3f0d5b78ae4c4fe633d61b804eb "${RESHADE_INCLUDE}/reshade_api.hpp")
fetch_header("${RESHADE_HEADERS_URL}/reshade_api_device.hpp" a716c44b6fbb064dbbb0826687728f21aa71ed45e13564e4dc1cdf36da5bdfa0 "${RESHADE_INCLUDE}/reshade_api_device.hpp")
fetch_header("${RESHADE_HEADERS_URL}/reshade_api_format.hpp" 68caa17fef712043e810ab88770c15af009aa3c7693e5c115ddc324f2d0809a7 "${RESHADE_INCLUDE}/reshade_api_format.hpp")
fetch_header("${RESHADE_HEADERS_URL}/reshade_api_pipeline.hpp" 5bda8e07260c2c60f0fd18a78b11782aed8f69e5d24b5757874614caafdf416f "${RESHADE_INCLUDE}/reshade_api_pipeline.hpp")
fetch_header("${RESHADE_HEADERS_URL}/reshade_api_resource.hpp" 32cdafe6d783813f3ac72ad43e426638d8d0fef56bcb558fad218a14cfb75ee3 "${RESHADE_INCLUDE}/reshade_api_resource.hpp")
fetch_header("${RESHADE_HEADERS_URL}/reshade_events.hpp" 2717f56e2bd89af6d708c9c316353d6581c5a41d4526629fadba51d13be57a89 "${RESHADE_INCLUDE}/reshade_events.hpp")
fetch_header("${RESHADE_HEADERS_URL}/reshade_overlay.hpp" ed914b12fbc41ed038287b0163c0bc934772afcff2c22392044c6008adc5ae79 "${RESHADE_INCLUDE}/reshade_overlay.hpp")

set(ONNXRUNTIME_INCLUDE "${CMAKE_BINARY_DIR}/onnxruntime/include")
set(ONNXRUNTIME_HEADERS_URL "https://raw.githubusercontent.com/microsoft/onnxruntime/v1.24.4/include/onnxruntime/core/session")
fetch_header("${ONNXRUNTIME_HEADERS_URL}/onnxruntime_c_api.h" 9ed0d7054a4e74249467365b25b415d36f51a44a6349e2a994a1812e4723d1e2 "${ONNXRUNTIME_INCLUDE}/onnxruntime_c_api.h")
fetch_header("${ONNXRUNTIME_HEADERS_URL}/onnxruntime_ep_c_api.h" e94ac3490697bdb107844fa9d403952ee7136134132a057eae5c1207b57ce352 "${ONNXRUNTIME_INCLUDE}/onnxruntime_ep_c_api.h")
fetch_header("https://raw.githubusercontent.com/microsoft/onnxruntime/v1.24.4/include/onnxruntime/core/providers/dml/dml_provider_factory.h" 893ddb645545bddad8c3f24bb768e97651c9ce45ec22076b435439889e4228c8 "${ONNXRUNTIME_INCLUDE}/dml_provider_factory.h")

add_executable(RobloxShadeHost
    src/main.cpp
    src/capture.cpp
    src/config.cpp
    src/depth/depth.cpp
    src/depth/depth_model.cpp
    src/overlay.cpp
    src/roblox_window.cpp)
target_include_directories(RobloxShadeHost PRIVATE "${RESHADE_INCLUDE}" "${ONNXRUNTIME_INCLUDE}")
target_compile_definitions(RobloxShadeHost PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(RobloxShadeHost PRIVATE ROBLOX_SHADE_HOST_VERSION="${PROJECT_VERSION}")
target_compile_options(RobloxShadeHost PRIVATE /W4 /permissive- /EHsc /bigobj)
target_link_libraries(RobloxShadeHost PRIVATE d3d11 d3d12 d3dcompiler dxgi dwmapi windowsapp gdi32)

find_program(INNO_SETUP_COMPILER ISCC
    HINTS "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6" "C:/Program Files (x86)/Inno Setup 6")
if(INNO_SETUP_COMPILER)
    add_custom_target(installer
        COMMAND "${INNO_SETUP_COMPILER}" /Q
            "/DHostExe=$<TARGET_FILE:RobloxShadeHost>"
            "/DAppVersion=${PROJECT_VERSION}"
            "/O${CMAKE_BINARY_DIR}/installer"
            "${CMAKE_CURRENT_SOURCE_DIR}/installer/RobloxShadeHost.iss"
        DEPENDS RobloxShadeHost
        VERBATIM)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(hotkey_tests tests/hotkey_tests.cpp)
    target_compile_definitions(hotkey_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(hotkey_tests PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME hotkey_tests COMMAND hotkey_tests)
endif()
