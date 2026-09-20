# CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(voice-agent VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 构建选项
option(BUILD_TESTS "Build tests" ON)
option(ENABLE_CUDA "Enable CUDA support" OFF)
option(ENABLE_VULKAN "Enable Vulkan for AMD/Intel GPU" OFF)
option(ENABLE_OPENVINO "Enable OpenVINO for Intel GPU" OFF)

# 编译器选项
if(MSVC)
    add_compile_options(/W3 /utf-8)
    # 忽略某些 MSVC 警告
    add_compile_options(/wd4251 /wd4275)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    # 启用更多警告
    add_compile_options(-Wconversion -Wshadow)
endif()

# ========== 第三方库 ==========

# FetchContent 或 find_package 方式管理依赖
include(FetchContent)

# spdlog（头文件库，直接包含）
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

# miniaudio（单头文件音频库）- 使用本地下载的头文件
# miniaudio.h 已经下载到 third_party/miniaudio.h

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
# 编译 amalgamation 时静音海量第三方警告
if(MSVC)
    target_compile_options(sqlite3_static PRIVATE /w)
else()
    target_compile_options(sqlite3_static PRIVATE -w)
endif()
find_package(Threads REQUIRED)

# onnxruntime（用于 VAD/ASR 模型推理）
# Windows 上使用系统安装的 onnxruntime
find_path(ONNXRUNTIME_INCLUDE_DIR NAMES onnxruntime_cxx_api.h PATHS 
    "C:/onnxruntime/include"
    "C:/Program Files/onnxruntime/include"
    "$ENV{CONDA_PREFIX}/Library/include"
)
find_library(ONNXRUNTIME_LIB NAMES onnxruntime PATHS
    "C:/onnxruntime/lib"
    "C:/Program Files/onnxruntime/lib"
    "$ENV{CONDA_PREFIX}/Library/lib"
)
if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIB)
    message(STATUS "Found ONNX Runtime: ${ONNXRUNTIME_INCLUDE_DIR}, ${ONNXRUNTIME_LIB}")
    add_compile_definitions(USE_ONNXRUNTIME)
else()
    message(STATUS "ONNX Runtime not found - VAD/ASR will use placeholder")
endif()

# ========== CUDA 配置（可选）==========
if(ENABLE_CUDA)
    find_package(CUDAToolkit REQUIRED)
    add_compile_definitions(USE_CUDA)
    include_directories(${CUDAToolkit_INCLUDE_DIRS})
endif()

# ========== Vulkan 配置（可选，AMD/Intel GPU）==========
if(ENABLE_VULKAN)
    find_package(Vulkan REQUIRED)
    add_compile_definitions(USE_VULKAN)
    include_directories(${Vulkan_INCLUDE_DIRS})
endif()

# ========== OpenVINO 配置（可选，Intel GPU）==========
if(ENABLE_OPENVINO)
    find_package(OpenVINO REQUIRED)
    add_compile_definitions(USE_OPENVINO)
endif()

# ========== 源文件组织 ==========

# 核心模块
set(CORE_SOURCES
    src/core/event_bus.cpp
)

# 工具模块
set(UTIL_SOURCES
    src/util/http.cpp
)

# 音频模块（M1: miniaudio + AEC）
set(AUDIO_SOURCES
    src/audio/audio_device.cpp
    src/audio/audio_pipeline.cpp
    src/audio/echo_cancellation.cpp
)

# VAD 模块（M2: VAD + 打断探测）
set(VAD_SOURCES
    src/vad/vad.cpp
)

# ASR 模块（M3: SenseVoice）
set(ASR_SOURCES
    src/asr/asr.cpp
)

# TTS 模块（M3: Kokoro）
set(TTS_SOURCES
    src/tts/tts.cpp
)

# LLM 模块（M3: llama.cpp）
set(LLM_SOURCES
    src/llm/llm.cpp
)

# Agent 模块（M5: 结构化输出 + 工具循环）
set(AGENT_SOURCES
    src/agent/grammar.cpp
    src/agent/tool_registry.cpp
    src/agent/tools.cpp
    src/agent/agent_loop.cpp
)

# Orchestrator 模块（M4: 全双工状态机）
set(ORCHESTRATOR_SOURCES
    src/orchestrator/orchestrator.cpp
    src/orchestrator/eou_detector.cpp
    src/orchestrator/smart_turn.cpp
    src/orchestrator/audio_router.cpp
)

# 搜索模块（M5: SearXNG 本地 + 在线回退）
set(SEARCH_SOURCES
    src/search/isearch.cpp
    src/search/searxng_provider.cpp
    src/search/online_providers.cpp
)

# Memory 模块（M6: 持久化 + 检索）
set(MEMORY_SOURCES
    src/memory/text_features.cpp
    src/memory/memory_store.cpp
    src/memory/memory_extractor.cpp
    src/memory/memory_retriever.cpp
    src/memory/memory_manager.cpp
    src/memory/memory_command.cpp
)

# 主程序
add_executable(voice-agent
    src/main.cpp
    ${CORE_SOURCES}
    ${UTIL_SOURCES}
    ${AUDIO_SOURCES}
    ${VAD_SOURCES}
    ${ASR_SOURCES}
    ${TTS_SOURCES}
    ${LLM_SOURCES}
    ${ORCHESTRATOR_SOURCES}
    ${AGENT_SOURCES}
    ${SEARCH_SOURCES}
    ${MEMORY_SOURCES}
)

target_include_directories(voice-agent PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/third_party
    ${spdlog_SOURCE_DIR}/include
    ${nlohmann_json_SOURCE_DIR}/include
)

# 链接库
target_link_libraries(voice-agent PRIVATE
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    sqlite3_static
    Threads::Threads
)
if(ONNXRUNTIME_LIB)
    target_link_libraries(voice-agent PRIVATE ${ONNXRUNTIME_LIB})
    target_include_directories(voice-agent PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
endif()

# Windows: 链接 ws2_32 用于网络
if(WIN32)
    target_link_libraries(voice-agent PRIVATE ws2_32)
endif()

# ========== 测试 ==========
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ========== 安装 ==========
install(TARGETS voice-agent RUNTIME DESTINATION bin)
