cmake_minimum_required(VERSION 3.20)
project(RobloxShadeHost VERSION 0.3.5 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Third-party files are downloaded once at configure time instead of being vendored.
function(fetch_file url hash destination)
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
fetch_file("${RESHADE_HEADERS_URL}/reshade.hpp" cf3cd150920ea310b4b1e2e4f11ab8d7221a9fc18755f4891a026563311b524c "${RESHADE_INCLUDE}/reshade.hpp")
fetch_file("${RESHADE_HEADERS_URL}/reshade_api.hpp" 8d83ec8a620c48da7a20d975bd27b3345b24d3f0d5b78ae4c4fe633d61b804eb "${RESHADE_INCLUDE}/reshade_api.hpp")
fetch_file("${RESHADE_HEADERS_URL}/reshade_api_device.hpp" a716c44b6fbb064dbbb0826687728f21aa71ed45e13564e4dc1cdf36da5bdfa0 "${RESHADE_INCLUDE}/reshade_api_device.hpp")
fetch_file("${RESHADE_HEADERS_URL}/reshade_api_format.hpp" 68caa17fef712043e810ab88770c15af009aa3c7693e5c115ddc324f2d0809a7 "${RESHADE_INCLUDE}/reshade_api_format.hpp")
fetch_file("${RESHADE_HEADERS_URL}/reshade_api_pipeline.hpp" 5bda8e07260c2c60f0fd18a78b11782aed8f69e5d24b5757874614caafdf416f "${RESHADE_INCLUDE}/reshade_api_pipeline.hpp")
fetch_file("${RESHADE_HEADERS_URL}/reshade_api_resource.hpp" 32cdafe6d783813f3ac72ad43e426638d8d0fef56bcb558fad218a14cfb75ee3 "${RESHADE_INCLUDE}/reshade_api_resource.hpp")
fetch_file("${RESHADE_HEADERS_URL}/reshade_events.hpp" 2717f56e2bd89af6d708c9c316353d6581c5a41d4526629fadba51d13be57a89 "${RESHADE_INCLUDE}/reshade_events.hpp")
fetch_file("${RESHADE_HEADERS_URL}/reshade_overlay.hpp" ed914b12fbc41ed038287b0163c0bc934772afcff2c22392044c6008adc5ae79 "${RESHADE_INCLUDE}/reshade_overlay.hpp")

set(ONNXRUNTIME_INCLUDE "${CMAKE_BINARY_DIR}/onnxruntime/include")
set(ONNXRUNTIME_HEADERS_URL "https://raw.githubusercontent.com/microsoft/onnxruntime/v1.24.4/include/onnxruntime/core/session")
fetch_file("${ONNXRUNTIME_HEADERS_URL}/onnxruntime_c_api.h" 9ed0d7054a4e74249467365b25b415d36f51a44a6349e2a994a1812e4723d1e2 "${ONNXRUNTIME_INCLUDE}/onnxruntime_c_api.h")
fetch_file("${ONNXRUNTIME_HEADERS_URL}/onnxruntime_ep_c_api.h" e94ac3490697bdb107844fa9d403952ee7136134132a057eae5c1207b57ce352 "${ONNXRUNTIME_INCLUDE}/onnxruntime_ep_c_api.h")
fetch_file("https://raw.githubusercontent.com/microsoft/onnxruntime/v1.24.4/include/onnxruntime/core/providers/dml/dml_provider_factory.h" 893ddb645545bddad8c3f24bb768e97651c9ce45ec22076b435439889e4228c8 "${ONNXRUNTIME_INCLUDE}/dml_provider_factory.h")

add_executable(RobloxShadeHost
    src/RobloxShadeHost.rc
    src/main.cpp
    src/addon.cpp
    src/capture.cpp
    src/config.cpp
    src/log.cpp
    src/depth/depth.cpp
    src/depth/depth_model.cpp
    src/overlay.cpp
    src/roblox_window.cpp
    src/setup_check.cpp)
target_include_directories(RobloxShadeHost PRIVATE "${RESHADE_INCLUDE}" "${ONNXRUNTIME_INCLUDE}")
target_compile_definitions(RobloxShadeHost PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(RobloxShadeHost PRIVATE ROBLOX_SHADE_HOST_VERSION="${PROJECT_VERSION}")
target_compile_options(RobloxShadeHost PRIVATE /W4 /permissive- /EHsc /bigobj)
target_link_libraries(RobloxShadeHost PRIVATE d3d11 d3d12 d3dcompiler dxgi dwmapi windowsapp gdi32 version)

# Setup draws its window with Dear ImGui and unpacks effect packages with miniz.
set(IMGUI_DIR "${CMAKE_BINARY_DIR}/imgui")
set(IMGUI_URL "https://raw.githubusercontent.com/ocornut/imgui/v1.92.9b")
fetch_file("${IMGUI_URL}/imgui.h" 0d8db1045db01d908853adfd26ae07c5bc5ab4789d4515f6ea34234a69ade0ca "${IMGUI_DIR}/imgui.h")
fetch_file("${IMGUI_URL}/imgui.cpp" 5a1e5128c0305f50b6556a77f71fde35836505883b74c1d6ae44723243f1e8de "${IMGUI_DIR}/imgui.cpp")
fetch_file("${IMGUI_URL}/imgui_internal.h" efba9bccc971cc49ec3da3168968ba412c7ea89745c104a7f5c004c8349bc71f "${IMGUI_DIR}/imgui_internal.h")
fetch_file("${IMGUI_URL}/imgui_draw.cpp" 83d30419a8e06a5f0a8692ee6de186f7ecfddecdc33769e76244837e2975b400 "${IMGUI_DIR}/imgui_draw.cpp")
fetch_file("${IMGUI_URL}/imgui_tables.cpp" b5deabe5b569ab712c11b6556562a646bf88327320fc60a451071b2a36498d72 "${IMGUI_DIR}/imgui_tables.cpp")
fetch_file("${IMGUI_URL}/imgui_widgets.cpp" a8a0b2f65caa5711467d9e3ca4028fded48329ea65e11ec92a968293dc2aa231 "${IMGUI_DIR}/imgui_widgets.cpp")
fetch_file("${IMGUI_URL}/imconfig.h" 5755e1b8d6ab0d7811a9d7cac0f509878b51fc5ee77e09bd0f235bcb414971e7 "${IMGUI_DIR}/imconfig.h")
fetch_file("${IMGUI_URL}/imstb_rectpack.h" 889b396795202d1457560a797a7242e96f6f132d4b88ca2d69be58bf05e1771f "${IMGUI_DIR}/imstb_rectpack.h")
fetch_file("${IMGUI_URL}/imstb_textedit.h" a985f5fa0ed97353d493b497961e9eef52082edcd045cf6954b69990ec9d0741 "${IMGUI_DIR}/imstb_textedit.h")
fetch_file("${IMGUI_URL}/imstb_truetype.h" c51a0f7e7ea760f2366bd3752635ec58e21fccfec4a832501639990ba6ce0528 "${IMGUI_DIR}/imstb_truetype.h")
fetch_file("${IMGUI_URL}/backends/imgui_impl_win32.h" 2736595b8de1e56871661d5d9b96deec567d35521746fbe2ade8f0543acdfd2b "${IMGUI_DIR}/backends/imgui_impl_win32.h")
fetch_file("${IMGUI_URL}/backends/imgui_impl_win32.cpp" 6a602ce28470bb209636c40633156e63a530acd49f137819d837d80688d52df6 "${IMGUI_DIR}/backends/imgui_impl_win32.cpp")
fetch_file("${IMGUI_URL}/backends/imgui_impl_dx11.h" fb8a314a6a2904dcc5bfeb938a169389e170f233ad367d33f1ce5c7bde586a7c "${IMGUI_DIR}/backends/imgui_impl_dx11.h")
fetch_file("${IMGUI_URL}/backends/imgui_impl_dx11.cpp" 749d12b01011f504284497db21cb52b3c6f73c59612e7115f46f41e0c4ea1c49 "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp")

set(MINIZ_DIR "${CMAKE_BINARY_DIR}/miniz")
if(NOT EXISTS "${MINIZ_DIR}/miniz.c")
    fetch_file("https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip"
        f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a "${CMAKE_BINARY_DIR}/miniz-3.1.2.zip")
    file(ARCHIVE_EXTRACT INPUT "${CMAKE_BINARY_DIR}/miniz-3.1.2.zip" DESTINATION "${MINIZ_DIR}" PATTERNS miniz.c miniz.h)
endif()

add_library(setup_libraries STATIC EXCLUDE_FROM_ALL
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
    "${MINIZ_DIR}/miniz.c")
target_include_directories(setup_libraries PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends" "${MINIZ_DIR}")
target_compile_definitions(setup_libraries PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME)

# Built on request, like the Inno Setup installer it replaces: cmake --build build --config Release --target installer
add_executable(installer WIN32 EXCLUDE_FROM_ALL
    installer/main.cpp
    installer/install.cpp
    installer/net.cpp
    installer/setup.rc)
# setup.rc embeds RobloxShadeHost.exe, found through the include path.
target_include_directories(installer PRIVATE "$<TARGET_FILE_DIR:RobloxShadeHost>")
target_compile_definitions(installer PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(installer PRIVATE ROBLOX_SHADE_HOST_VERSION="${PROJECT_VERSION}")
target_compile_options(installer PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(installer PRIVATE setup_libraries d3d11 dxgi d3dcompiler dwmapi windowscodecs winhttp bcrypt imm32 delayimp)
# Loaded from System32 at startup instead, see wWinMain.
target_link_options(installer PRIVATE /DELAYLOAD:d3d11.dll)
add_dependencies(installer RobloxShadeHost)
# The generator expression keeps multi-config generators from adding a Release folder.
set_target_properties(installer PROPERTIES
    OUTPUT_NAME "RobloxShadeHost-Setup-${PROJECT_VERSION}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/installer$<0:>")

include(CTest)
if(BUILD_TESTING)
    add_executable(hotkey_tests tests/hotkey_tests.cpp)
    target_compile_definitions(hotkey_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(hotkey_tests PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME hotkey_tests COMMAND hotkey_tests)
endif()
