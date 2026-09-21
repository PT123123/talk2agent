# tests/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

# 运行时 DLL 搜索路径：llama/ggml（${CMAKE_BINARY_DIR}/bin）、spdlog、onnxruntime 等
# 由 FetchContent 生成的 DLL 不在系统 PATH 中，ctest 直接运行 exe 会 0xc0000135。
if(WIN32)
    # 多个目录用转义分号连接（ENVIRONMENT_MODIFICATION 中分号是列表分隔符）
    set(_RUNTIME_DLL_DIRS
        "${CMAKE_BINARY_DIR}/bin\\;${CMAKE_BINARY_DIR}/_deps/spdlog-build")
    set(_TEST_ENV_MOD "PATH=path_list_append:${_RUNTIME_DLL_DIRS}")
    # 追加 PATH 的辅助宏：所有链接 voice_agent_core 的测试都调用它
    function(voice_agent_test_env _test_name)
        set_tests_properties(${_test_name} PROPERTIES
            ENVIRONMENT_MODIFICATION "${_TEST_ENV_MOD}")
    endfunction()
    # 把所有运行时 DLL（sherpa-onnx C API / onnxruntime）复制到测试 exe 旁边。
    # voice_agent_core 静态库会把 onnxruntime/sherpa 的导入表带进每个测试 exe，
    # 只给部分测试复制会导致其余测试 0xc0000135。
    if(SHERPA_C_API_LIB)
        function(voice_agent_test_copy_rt _test_name)
            add_custom_command(TARGET ${_test_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SHERPA_ROOT}/lib/sherpa-onnx-c-api.dll"
                    "$<TARGET_FILE_DIR:${_test_name}>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SHERPA_ROOT}/lib/onnxruntime.dll"
                    "$<TARGET_FILE_DIR:${_test_name}>"
                COMMENT "Copying runtime DLLs next to test")
        endfunction()
    endif()
endif()

# 单元测试（ring buffer 等）
add_executable(test_ring_buffer
    test_ring_buffer.cpp
    ${CMAKE_SOURCE_DIR}/src/core/ring_buffer.hpp
)
target_include_directories(test_ring_buffer PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_ring_buffer PRIVATE spdlog::spdlog)
add_test(NAME test_ring_buffer COMMAND test_ring_buffer)

# AEC 测试（后续）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_aec.cpp)
    add_executable(test_aec test_aec.cpp)
    target_include_directories(test_aec PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_aec PRIVATE spdlog::spdlog)
    add_test(NAME test_aec COMMAND test_aec)
endif()

# VAD 测试（后续）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_vad_bargein.cpp)
    add_executable(test_vad_bargein test_vad_bargein.cpp)
    target_include_directories(test_vad_bargein PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_vad_bargein PRIVATE spdlog::spdlog)
    add_test(NAME test_vad_bargein COMMAND test_vad_bargein)
endif()

# M5 Agent 测试
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_agent.cpp)
    add_executable(test_agent test_agent.cpp)
    target_link_libraries(test_agent PRIVATE voice_agent_core)
    add_test(NAME test_agent COMMAND test_agent)
    voice_agent_test_env(test_agent)
    voice_agent_test_copy_rt(test_agent)
endif()

# M6 Memory 测试
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_memory.cpp)
    add_executable(test_memory test_memory.cpp)
    target_link_libraries(test_memory PRIVATE voice_agent_core)
    add_test(NAME test_memory COMMAND test_memory)
    voice_agent_test_env(test_memory)
    voice_agent_test_copy_rt(test_memory)
endif()

# 临时 probe：隔离 validate_tool_args_json
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_probe.cpp)
    add_executable(test_probe test_probe.cpp)
    target_link_libraries(test_probe PRIVATE voice_agent_core)
endif()

# 端到端语音链路：Silero VAD → Whisper tiny ONNX（真实语音→文本）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_vad_asr.cpp AND ONNXRUNTIME_LIB)
    add_executable(test_vad_asr test_vad_asr.cpp)
    target_link_libraries(test_vad_asr PRIVATE voice_agent_core)
    add_test(NAME test_vad_asr COMMAND test_vad_asr)
    voice_agent_test_env(test_vad_asr)
    voice_agent_test_copy_rt(test_vad_asr)
    set_tests_properties(test_vad_asr PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_custom_command(TARGET test_vad_asr POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SHERPA_ROOT}/lib/onnxruntime.dll"
            "$<TARGET_FILE_DIR:test_vad_asr>"
        COMMENT "Copying onnxruntime.dll next to test")
endif()

# Whisper ONNX 冒烟测试（加载真实模型 + 一次完整推理）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_whisper.cpp AND ONNXRUNTIME_LIB)
    add_executable(test_whisper test_whisper.cpp)
    target_link_libraries(test_whisper PRIVATE voice_agent_core)
    add_test(NAME test_whisper COMMAND test_whisper "models/asr/whisper-tiny")
    voice_agent_test_env(test_whisper)
    voice_agent_test_copy_rt(test_whisper)
    set_tests_properties(test_whisper PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    # 把正确的 ORT DLL 放到测试 exe 旁边，避免被 System32 里的旧版 onnxruntime.dll 抢先加载
    add_custom_command(TARGET test_whisper POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SHERPA_ROOT}/lib/onnxruntime.dll"
            "$<TARGET_FILE_DIR:test_whisper>"
        COMMENT "Copying onnxruntime.dll next to test")
endif()

# GPU 探针：隔离验证 VAD/ASR/TTS 的 DirectML 加载（定位 GPU 卡死点）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_gpu_probe.cpp AND SHERPA_C_API_LIB)
    add_executable(test_gpu_probe test_gpu_probe.cpp)
    target_link_libraries(test_gpu_probe PRIVATE voice_agent_core)
    add_test(NAME test_gpu_probe COMMAND test_gpu_probe)
    voice_agent_test_env(test_gpu_probe)
    voice_agent_test_copy_rt(test_gpu_probe)
    set_tests_properties(test_gpu_probe PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    # DirectML.dll 依赖系统内置 1.15.x；仅当 build 根目录存在时才拷贝（不拷贝旧版 1.13）
    if(EXISTS "${CMAKE_BINARY_DIR}/DirectML.dll")
        add_custom_command(TARGET test_gpu_probe POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/DirectML.dll"
                "$<TARGET_FILE_DIR:test_gpu_probe>"
            COMMENT "Copying DirectML.dll next to test")
    endif()
endif()

# Kokoro TTS 冒烟测试（加载真实模型 + 一次完整中文合成）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_kokoro.cpp AND SHERPA_C_API_LIB)
    add_executable(test_kokoro test_kokoro.cpp)
    target_link_libraries(test_kokoro PRIVATE voice_agent_core)
    add_test(NAME test_kokoro COMMAND test_kokoro "models/tts/kokoro-multi-lang-v1_0")
    voice_agent_test_env(test_kokoro)
    voice_agent_test_copy_rt(test_kokoro)
    set_tests_properties(test_kokoro PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_custom_command(TARGET test_kokoro POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SHERPA_ROOT}/lib/onnxruntime.dll"
            "$<TARGET_FILE_DIR:test_kokoro>"
        COMMENT "Copying onnxruntime.dll next to test")
endif()
