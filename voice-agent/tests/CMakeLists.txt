# tests/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)

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

# 供测试复用的 memory 模块源
set(MEMORY_TEST_SOURCES
    ${CMAKE_SOURCE_DIR}/src/memory/text_features.cpp
    ${CMAKE_SOURCE_DIR}/src/memory/memory_store.cpp
    ${CMAKE_SOURCE_DIR}/src/memory/memory_extractor.cpp
    ${CMAKE_SOURCE_DIR}/src/memory/memory_retriever.cpp
    ${CMAKE_SOURCE_DIR}/src/memory/memory_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/memory/memory_command.cpp
)

# M5 Agent 测试
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_agent.cpp)
    add_executable(test_agent
        test_agent.cpp
        ${CMAKE_SOURCE_DIR}/src/agent/grammar.cpp
        ${CMAKE_SOURCE_DIR}/src/agent/tool_registry.cpp
        ${CMAKE_SOURCE_DIR}/src/agent/tools.cpp
        ${CMAKE_SOURCE_DIR}/src/agent/agent_loop.cpp
        ${CMAKE_SOURCE_DIR}/src/llm/llm.cpp
        ${CMAKE_SOURCE_DIR}/src/search/isearch.cpp
        ${CMAKE_SOURCE_DIR}/src/search/searxng_provider.cpp
        ${CMAKE_SOURCE_DIR}/src/search/online_providers.cpp
        ${CMAKE_SOURCE_DIR}/src/util/http.cpp
        ${MEMORY_TEST_SOURCES}
    )
    target_include_directories(test_agent PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
    target_link_libraries(test_agent PRIVATE spdlog::spdlog nlohmann_json::nlohmann_json sqlite3_static Threads::Threads)
    if(WIN32)
        target_link_libraries(test_agent PRIVATE ws2_32)
    endif()
    add_test(NAME test_agent COMMAND test_agent)
endif()

# M6 Memory 测试
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_memory.cpp)
    add_executable(test_memory
        test_memory.cpp
        ${MEMORY_TEST_SOURCES}
    )
    target_include_directories(test_memory PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party)
    target_link_libraries(test_memory PRIVATE spdlog::spdlog nlohmann_json::nlohmann_json sqlite3_static Threads::Threads)
    if(WIN32)
        target_link_libraries(test_memory PRIVATE ws2_32)
    endif()
    add_test(NAME test_memory COMMAND test_memory)
endif()
