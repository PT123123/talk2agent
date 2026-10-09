# tests/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

# 诊断：ENABLE_ASAN 时测试目标与 voice_agent_core 保持同样的插桩（否则 LNK2038）
if(ENABLE_ASAN)
    add_compile_options(/fsanitize=address)
    add_link_options(/INFERASANLIBS /INCREMENTAL:NO)
endif()

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

# R0 Conversation Runtime 测试（Task 生命周期 / WorkingContext / Policy / Intent）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_runtime.cpp)
    add_executable(test_runtime test_runtime.cpp)
    target_link_libraries(test_runtime PRIVATE voice_agent_core)
    add_test(NAME test_runtime COMMAND test_runtime)
    voice_agent_test_env(test_runtime)
    voice_agent_test_copy_rt(test_runtime)
endif()

# R1 轮次准入测试（意图 / Policy / 换话题 / supersede 触发）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_turn_admission.cpp)
    add_executable(test_turn_admission test_turn_admission.cpp)
    target_link_libraries(test_turn_admission PRIVATE voice_agent_core)
    add_test(NAME test_turn_admission COMMAND test_turn_admission)
    voice_agent_test_env(test_turn_admission)
    voice_agent_test_copy_rt(test_turn_admission)
endif()

# R3 Fast Response + BackgroundCache 测试
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_fast_response.cpp)
    add_executable(test_fast_response test_fast_response.cpp)
    target_link_libraries(test_fast_response PRIVATE voice_agent_core)
    add_test(NAME test_fast_response COMMAND test_fast_response)
    voice_agent_test_env(test_fast_response)
    voice_agent_test_copy_rt(test_fast_response)
endif()

# R4 Model Router 测试（分档参数 / 降级 / 统计）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_model_router.cpp)
    add_executable(test_model_router test_model_router.cpp)
    target_link_libraries(test_model_router PRIVATE voice_agent_core)
    add_test(NAME test_model_router COMMAND test_model_router)
    voice_agent_test_env(test_model_router)
    voice_agent_test_copy_rt(test_model_router)
endif()

# R9 业界对标测试（打断截断 / 语义轮次判定 / 播放进度）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_industry_alignment.cpp)
    add_executable(test_industry_alignment test_industry_alignment.cpp)
    target_link_libraries(test_industry_alignment PRIVATE voice_agent_core)
    add_test(NAME test_industry_alignment COMMAND test_industry_alignment)
    voice_agent_test_env(test_industry_alignment)
    voice_agent_test_copy_rt(test_industry_alignment)
endif()

# R8 长跑稳定性测试（内存红线 / 线程泄漏 / 容器容量）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_long_run.cpp)
    add_executable(test_long_run test_long_run.cpp)
    target_link_libraries(test_long_run PRIVATE voice_agent_core)
    # 读进程工作集需要 psapi
    target_link_libraries(test_long_run PRIVATE psapi)
    add_test(NAME test_long_run COMMAND test_long_run)
    voice_agent_test_env(test_long_run)
    voice_agent_test_copy_rt(test_long_run)
    set_tests_properties(test_long_run PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        TIMEOUT 300)
endif()

# R6 Prosody / ResponsePlan 测试
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_prosody.cpp)
    add_executable(test_prosody test_prosody.cpp)
    target_link_libraries(test_prosody PRIVATE voice_agent_core)
    add_test(NAME test_prosody COMMAND test_prosody)
    voice_agent_test_env(test_prosody)
    voice_agent_test_copy_rt(test_prosody)
endif()

# R7 远程 LLM 测试（OpenAI 兼容协议 / SSE 解析 / 路由降级；不联网）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_remote_llm.cpp)
    add_executable(test_remote_llm test_remote_llm.cpp)
    target_link_libraries(test_remote_llm PRIVATE voice_agent_core)
    add_test(NAME test_remote_llm COMMAND test_remote_llm)
    voice_agent_test_env(test_remote_llm)
    voice_agent_test_copy_rt(test_remote_llm)
endif()

# §24 TTS 适配器：Qwen3-TTS / Chatterbox 的韵律→引擎控制映射（纯逻辑，不依赖模型）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_tts_adapters.cpp)
    add_executable(test_tts_adapters test_tts_adapters.cpp)
    target_link_libraries(test_tts_adapters PRIVATE voice_agent_core)
    add_test(NAME test_tts_adapters COMMAND test_tts_adapters)
    voice_agent_test_env(test_tts_adapters)
    voice_agent_test_copy_rt(test_tts_adapters)
endif()

# §24 补完：TTS bridge 客户端（WAV 解析 / JSON 协议 / health 降级判定 / controls 贯通）
# 全部离线：不起网络、不起 Python 进程、不加载模型。
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_tts_bridge.cpp)
    add_executable(test_tts_bridge test_tts_bridge.cpp)
    target_link_libraries(test_tts_bridge PRIVATE voice_agent_core)
    add_test(NAME test_tts_bridge COMMAND test_tts_bridge)
    voice_agent_test_env(test_tts_bridge)
    voice_agent_test_copy_rt(test_tts_bridge)
endif()

# ZipVoice 引擎端到端（真实模型）。模型缺失时 SKIP 而非 FAIL——
# 换机/CI 跑不过这关是正常的，但静默通过会掩盖问题，所以会打印 SKIP 原因。
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_zipvoice_engine.cpp)
    add_executable(test_zipvoice_engine test_zipvoice_engine.cpp)
    target_link_libraries(test_zipvoice_engine PRIVATE voice_agent_core)
    add_test(NAME test_zipvoice_engine COMMAND test_zipvoice_engine)
    voice_agent_test_env(test_zipvoice_engine)
    voice_agent_test_copy_rt(test_zipvoice_engine)
endif()

# 临时 probe：隔离 validate_tool_args_json
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_probe.cpp)
    add_executable(test_probe test_probe.cpp)
    target_link_libraries(test_probe PRIVATE voice_agent_core)
endif()

# 简单 TTS（SAPI）冒烟测试：初始化 + 朗读（人工可听到出声）。非自动 ctest。
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_sapi.cpp)
    add_executable(test_sapi test_sapi.cpp)
    target_link_libraries(test_sapi PRIVATE voice_agent_core)
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

# 打断控制测试（AudioRouter 淡出/duck/播放进度 + barge_in 配置解析）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_barge_in_control.cpp)
    add_executable(test_barge_in_control test_barge_in_control.cpp)
    target_link_libraries(test_barge_in_control PRIVATE voice_agent_core)
    add_test(NAME test_barge_in_control COMMAND test_barge_in_control)
    voice_agent_test_env(test_barge_in_control)
    voice_agent_test_copy_rt(test_barge_in_control)
endif()

# 内存预算测试（VAD 懒加载状态 + input_mode 门控 + mem_probe/format_bytes）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_mem_budget.cpp)
    add_executable(test_mem_budget test_mem_budget.cpp)
    target_link_libraries(test_mem_budget PRIVATE voice_agent_core)
    add_test(NAME test_mem_budget COMMAND test_mem_budget)
    voice_agent_test_env(test_mem_budget)
    voice_agent_test_copy_rt(test_mem_budget)
endif()
