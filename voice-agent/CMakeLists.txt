# CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(voice-agent VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ========== 构建选项 ==========
option(BUILD_TESTS "Build tests" ON)
option(BUILD_GUI "Build Qt6 GUI" ON)
option(USE_LLAMACPP "Enable llama.cpp real LLM backend" ON)
option(ENABLE_CUDA "Enable CUDA support" OFF)
option(ENABLE_VULKAN "Enable Vulkan for AMD/Intel GPU" OFF)
option(ENABLE_OPENVINO "Enable OpenVINO for Intel GPU" OFF)

# ========== Qt 探测（graphical 界面，MSVC kit）==========
# 若未通过 -DCMAKE_PREFIX_PATH 指定 Qt，自动探测默认安装目录
if(NOT CMAKE_PREFIX_PATH)
    find_program(_QMAKE_EXECUTABLE NAMES qmake6 qmake)
    if(NOT _QMAKE_EXECUTABLE AND EXISTS "C:/Qt")
        file(GLOB _QT_VER_DIRS LIST_DIRECTORIES true "C:/Qt/[56].*")
        list(SORT _QT_VER_DIRS ORDER DESCENDING)
        foreach(_d ${_QT_VER_DIRS})
            file(GLOB _QT_KITS LIST_DIRECTORIES true "${_d}/msvc*/")
            list(SORT _QT_KITS ORDER DESCENDING)
            if(_QT_KITS)
                list(GET _QT_KITS 0 _qt_root)
                message(STATUS "Autodetected Qt: ${_qt_root}")
                list(APPEND CMAKE_PREFIX_PATH "${_qt_root}")
                break()
            endif()
        endforeach()
    endif()
endif()

# ========== 编译器选项 ==========
if(MSVC)
    add_compile_options(/W3 /utf-8)
    add_compile_options(/wd4251 /wd4275)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    add_compile_options(-Wconversion -Wshadow)
endif()

# ========== 第三方库 ==========
include(FetchContent)

# spdlog（头文件库）— 强制静态，避免 Windows 默认共享库导致运行时 DLL 缺失
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.12.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(spdlog)

# nlohmann/json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# sqlite3（amalgamation，本地 third_party）— 编译时启用 FTS5
add_library(sqlite3_static STATIC ${CMAKE_SOURCE_DIR}/third_party/sqlite3/sqlite3.c)
target_include_directories(sqlite3_static PUBLIC ${CMAKE_SOURCE_DIR}/third_party/sqlite3)
set_target_properties(sqlite3_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(sqlite3_static PRIVATE
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_FTS5_PARENTHESIS
    SQLITE_ENABLE_COLUMN_METADATA
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DQS=0
)
if(MSVC)
    target_compile_options(sqlite3_static PRIVATE /w)
else()
    target_compile_options(sqlite3_static PRIVATE -w)
endif()
find_package(Threads REQUIRED)

# onnxruntime（用于 VAD/ASR 模型推理）。优先用 DirectML 版（GPU），其次 CPU 版。
# 注意：sherpa-onnx 捆绑 onnxruntime 1.28.2，但 DirectML EP 只有 ORT 1.24.x 发布版可用，
# 因此 ASR/TTS 需使用与 DirectML 1.24.4 配套的自编译 sherpa-onnx（见 third_party/sherpa-onnx-src）。
set(ORT_DML_ROOT "${CMAKE_SOURCE_DIR}/third_party/onnxruntime-win-x64-directml-1.24.4")
set(ORT_CPU_ROOT "${CMAKE_SOURCE_DIR}/third_party/onnxruntime-win-x64-1.28.0")
# DirectML 版存在时强制使用（find_* 会缓存旧结果，不会自动切换到新路径）
if(EXISTS "${ORT_DML_ROOT}/runtimes/win-x64/native/onnxruntime.lib")
    set(ONNXRUNTIME_INCLUDE_DIR "${ORT_DML_ROOT}/build/native/include" CACHE FILEPATH "ONNX Runtime include dir" FORCE)
    set(ONNXRUNTIME_LIB "${ORT_DML_ROOT}/runtimes/win-x64/native/onnxruntime.lib" CACHE FILEPATH "ONNX Runtime lib" FORCE)
else()
    find_path(ONNXRUNTIME_INCLUDE_DIR NAMES onnxruntime_cxx_api.h PATHS
        "${ORT_CPU_ROOT}/include"
        "C:/onnxruntime/include"
        "C:/Program Files/onnxruntime/include"
        "$ENV{CONDA_PREFIX}/Library/include"
    )
    find_library(ONNXRUNTIME_LIB NAMES onnxruntime PATHS
        "${ORT_CPU_ROOT}/lib"
        "C:/onnxruntime/lib"
        "C:/Program Files/onnxruntime/lib"
        "$ENV{CONDA_PREFIX}/Library/lib"
    )
endif()
if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIB)
    message(STATUS "Found ONNX Runtime: ${ONNXRUNTIME_INCLUDE_DIR}, ${ONNXRUNTIME_LIB}")
    add_compile_definitions(USE_ONNXRUNTIME)
    # 记录 ORT DLL 目录，供 target 定义后复制
    get_filename_component(ORT_DLL_DIR "${ONNXRUNTIME_LIB}" DIRECTORY)
else()
    message(STATUS "ONNX Runtime not found - VAD/ASR will use placeholder")
endif()

# sherpa-onnx（TTS：Kokoro 等离线模型）。优先使用自编译 DirectML 版（GPU 推理），
# 否则回退预编译 CPU 版。DML 版链接的是同一份 DirectML 版 ORT 1.24.4，避免 onnxruntime.dll 版本冲突。
set(SHERPA_DML_ROOT "${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx-dml")
if(EXISTS "${SHERPA_DML_ROOT}/lib/sherpa-onnx-c-api.lib")
    set(SHERPA_ROOT "${SHERPA_DML_ROOT}")
    # find_* 会缓存旧路径，DML 版存在时强制指向它
    set(SHERPA_INCLUDE_DIR "${SHERPA_DML_ROOT}/include" CACHE PATH "sherpa-onnx include dir" FORCE)
    set(SHERPA_C_API_LIB "${SHERPA_DML_ROOT}/lib/sherpa-onnx-c-api.lib" CACHE FILEPATH "sherpa-onnx C API lib" FORCE)
    message(STATUS "sherpa-onnx: using DirectML build at ${SHERPA_DML_ROOT}")
else()
    set(SHERPA_ROOT "${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx-win-x64")
    find_path(SHERPA_INCLUDE_DIR NAMES sherpa-onnx/c-api/c-api.h PATHS
        "${SHERPA_ROOT}/include")
    find_library(SHERPA_C_API_LIB NAMES sherpa-onnx-c-api PATHS
        "${SHERPA_ROOT}/lib")
endif()
if(SHERPA_INCLUDE_DIR AND SHERPA_C_API_LIB)
    message(STATUS "Found sherpa-onnx C API: ${SHERPA_INCLUDE_DIR}")
    add_compile_definitions(USE_SHERPAONNX)
else()
    message(STATUS "sherpa-onnx not found - TTS will use placeholder")
endif()

# ========== GPU 后端（可选；须在 llama.cpp 配置前设置缓存变量）==========
# Vulkan：适配 NVIDIA/AMD/Intel Arc 等显卡。Windows 依赖 Vulkan SDK（vulkan-1.lib + glslc）。
if(ENABLE_VULKAN)
    # 环境变量未注入时自动探测 C:\VulkanSDK\<version>
    if(NOT DEFINED ENV{VULKAN_SDK})
        file(GLOB _VULKAN_SDK_DIRS LIST_DIRECTORIES true "C:/VulkanSDK/*")
        list(SORT _VULKAN_SDK_DIRS ORDER DESCENDING)
        if(_VULKAN_SDK_DIRS)
            list(GET _VULKAN_SDK_DIRS 0 _VULKAN_SDK)
            set(ENV{VULKAN_SDK} "${_VULKAN_SDK}")
            message(STATUS "Vulkan SDK auto-detected: ${_VULKAN_SDK}")
        endif()
    endif()
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        message(STATUS "Vulkan found: ${Vulkan_INCLUDE_DIRS}")
        add_compile_definitions(USE_VULKAN)
        include_directories(${Vulkan_INCLUDE_DIRS})
        set(GGML_VULKAN ON CACHE BOOL "" FORCE)
        if(DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/Bin/glslc.exe")
            set(VULKAN_SHADERS_GEN_TOOLCHAIN "$ENV{VULKAN_SDK}/Bin/glslc.exe" CACHE FILEPATH "" FORCE)
            message(STATUS "Vulkan shader compiler: ${VULKAN_SHADERS_GEN_TOOLCHAIN}")
        endif()
    else()
        message(WARNING "ENABLE_VULKAN=ON 但未找到 Vulkan SDK - 推理将回退 CPU")
    endif()
endif()

# llama.cpp（真实 LLM 后端）。仅编译静态库（不含示例/服务/测试）。
if(USE_LLAMACPP)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)   # llama.cpp 用通用选项，非 MinGW 默认共享
    set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)   # 关掉针对编译机的指令集，求得可移植
    set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
    set(GGML_CPU_ALL_VARIANTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        llama
        GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
        GIT_TAG v0.4.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(llama)
    # MSVC 默认把 __cplusplus 报告为 199711（除非加 /Zc:__cplusplus），
    # 会导致 llama.cpp 内部按 C++17 处理 u8 字面量而产生 char8_t 类型不匹配。
    if(MSVC)
        foreach(_llama_tgt llama ggml ggml-base ggml-cpu)
            if(TARGET ${_llama_tgt})
                target_compile_options(${_llama_tgt} PRIVATE /Zc:__cplusplus)
                # ggml-cpu 用 _WIN32_WINNT>=0x0602 判断进入线程功率节流分支，
                # 但 SDK 只在 >=0x0A00 时定义 THREAD_POWER_THROTTLING_STATE，需对齐。
                if(${_llama_tgt} STREQUAL "ggml-cpu")
                    target_compile_definitions(${_llama_tgt} PRIVATE _WIN32_WINNT=0x0A00)
                endif()
            endif()
        endforeach()
    endif()
    message(STATUS "llama.cpp enabled: backends supported")
    add_compile_definitions(USE_LLAMACPP)
endif()

# ========== CUDA / OpenVINO（可选）==========
if(ENABLE_CUDA)
    find_package(CUDAToolkit REQUIRED)
    add_compile_definitions(USE_CUDA)
    include_directories(${CUDAToolkit_INCLUDE_DIRS})
    set(GGML_CUDA ON CACHE BOOL "" FORCE)
endif()
if(ENABLE_OPENVINO)
    find_package(OpenVINO REQUIRED)
    add_compile_definitions(USE_OPENVINO)
    set(GGML_OPENVINO ON CACHE BOOL "" FORCE)
endif()

# ========== 源文件组织 ==========
set(CORE_SOURCES
    src/core/event_bus.cpp
    src/util/http.cpp
    src/util/gpu.cpp
    src/audio/audio_device.cpp
    src/audio/audio_pipeline.cpp
    src/audio/echo_cancellation.cpp
    src/vad/vad.cpp
    src/asr/asr.cpp
    src/asr/sherpa_asr.cpp
    src/asr/whisper_frontend.cpp
    src/asr/whisper_onnx.cpp
    src/tts/tts.cpp
    src/tts/speaker.cpp
    src/llm/llm.cpp
    src/agent/grammar.cpp
    src/agent/tool_registry.cpp
    src/agent/tools.cpp
    src/agent/agent_loop.cpp
    src/orchestrator/orchestrator.cpp
    src/orchestrator/eou_detector.cpp
    src/orchestrator/smart_turn.cpp
    src/orchestrator/audio_router.cpp
    src/search/isearch.cpp
    src/search/searxng_provider.cpp
    src/search/online_providers.cpp
    src/memory/text_features.cpp
    src/memory/memory_store.cpp
    src/memory/memory_extractor.cpp
    src/memory/memory_retriever.cpp
    src/memory/memory_manager.cpp
    src/memory/memory_command.cpp
)

# ========== 核心静态库（无 GUI 依赖，供 CLI/GUI/测试复用）==========
add_library(voice_agent_core STATIC ${CORE_SOURCES})
target_include_directories(voice_agent_core PUBLIC
    src
    ${CMAKE_SOURCE_DIR}/third_party
    ${spdlog_SOURCE_DIR}/include
    ${nlohmann_json_SOURCE_DIR}/include
)
target_link_libraries(voice_agent_core PUBLIC
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    sqlite3_static
    Threads::Threads
)
if(ONNXRUNTIME_LIB)
    target_link_libraries(voice_agent_core PUBLIC ${ONNXRUNTIME_LIB})
    target_include_directories(voice_agent_core PUBLIC ${ONNXRUNTIME_INCLUDE_DIR})
    # 将 ORT DLL 复制到可执行输出目录，便于直接运行。
    # 优先拷贝与链接库同源的 onnxruntime.dll（DML 版时即 1.24.4），避免 CPU/DML 混用导致 DML EP 缺失。
    if(EXISTS "${ORT_DLL_DIR}/onnxruntime.dll")
        set(_ORT_RT_DLL "${ORT_DLL_DIR}/onnxruntime.dll")
    elseif(EXISTS "${SHERPA_ROOT}/lib/onnxruntime.dll")
        set(_ORT_RT_DLL "${SHERPA_ROOT}/lib/onnxruntime.dll")
    else()
        set(_ORT_RT_DLL "")
    endif()
    if(_ORT_RT_DLL)
        add_custom_command(TARGET voice_agent_core POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_ORT_RT_DLL}"
                "${CMAKE_BINARY_DIR}"
            COMMENT "Copying onnxruntime.dll to build dir")
    endif()
    # 注意：不拷贝捆绑的 DirectML.dll（1.13.0 过旧，与 ORT 1.24.4 DML EP 配合异常），
    # 依赖 Windows 11 系统内置 DirectML 1.15.x（System32），版本更新且已验证可用。
    if(FALSE)
    if(EXISTS "${SHERPA_ROOT}/lib/DirectML.dll")
        add_custom_command(TARGET voice_agent_core POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SHERPA_ROOT}/lib/DirectML.dll"
                "${CMAKE_BINARY_DIR}"
            COMMENT "Copying DirectML.dll to build dir")
    endif()
    endif()
    if(EXISTS "${SHERPA_ROOT}/lib/onnxruntime_providers_shared.dll")
        add_custom_command(TARGET voice_agent_core POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SHERPA_ROOT}/lib/onnxruntime_providers_shared.dll"
                "${CMAKE_BINARY_DIR}"
            COMMENT "Copying onnxruntime_providers_shared.dll to build dir")
    endif()
endif()
if(SHERPA_C_API_LIB)
    target_link_libraries(voice_agent_core PUBLIC ${SHERPA_C_API_LIB})
    target_include_directories(voice_agent_core PUBLIC ${SHERPA_INCLUDE_DIR})
    # sherpa-onnx-c-api.dll 及其依赖放到输出目录
    add_custom_command(TARGET voice_agent_core POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SHERPA_ROOT}/lib/sherpa-onnx-c-api.dll"
            "${CMAKE_BINARY_DIR}"
        COMMENT "Copying sherpa-onnx-c-api.dll to build dir")
endif()
if(TARGET llama)
    target_link_libraries(voice_agent_core PUBLIC llama)
    target_include_directories(voice_agent_core PUBLIC
        ${llama_SOURCE_DIR}/include
        ${llama_SOURCE_DIR}/ggml/include)
endif()
if(WIN32)
    target_link_libraries(voice_agent_core PRIVATE ws2_32 dxgi)
endif()

# ========== Qt6 GUI ==========
if(BUILD_GUI)
    find_package(Qt6 QUIET COMPONENTS Widgets Core Network)
    if(Qt6_FOUND)
        message(STATUS "Qt6 found: ${Qt6_VERSION} (${Qt6Widgets_DIR})")
        set(GUI_SOURCES
            src/main.cpp
            src/gui/main_window.cpp
            src/gui/agent_controller.cpp
            src/gui/model_catalog.cpp
            src/gui/model_downloader.cpp
            src/gui/settings_panel.cpp
            src/gui/latency_panel.cpp
            src/gui/main_window.hpp
            src/gui/agent_controller.hpp
            src/gui/model_catalog.hpp
            src/gui/model_downloader.hpp
            src/gui/settings_panel.hpp
            src/gui/latency_panel.hpp
        )
        add_executable(voice-agent WIN32 ${GUI_SOURCES})
        target_include_directories(voice-agent PRIVATE src ${CMAKE_SOURCE_DIR}/third_party)
        target_link_libraries(voice-agent PRIVATE voice_agent_core Qt6::Widgets Qt6::Core Qt6::Network)
        set_target_properties(voice-agent PROPERTIES
            AUTOMOC ON
            WIN32_EXECUTABLE TRUE
        )
        # Windows: 部署 Qt DLL（Debug/Release）
        get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
        if(_qmake)
            get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
            set(WINDEPLOYQT "${_qt_bin}/windeployqt.exe")
        endif()
    else()
        message(WARNING "Qt6 not found - GUI disabled, core library only. "
                        "Set -DCMAKE_PREFIX_PATH=<Qt>/<version>/msvc<x>_64")
    endif()
endif()

# ========== 测试 ==========
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ========== 安装 ==========
install(TARGETS voice_agent_core ARCHIVE DESTINATION lib)
if(TARGET voice-agent)
    install(TARGETS voice-agent RUNTIME DESTINATION bin)
endif()