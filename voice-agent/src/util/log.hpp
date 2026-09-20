// src/util/log.hpp
#pragma once
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <memory>
#include <string>

// ========== 日志工具 ==========
// M0: 使用同步 logger，后续 M2 阶段引入异步队列

inline std::shared_ptr<spdlog::logger> init_logger(
    const std::string& name = "voice-agent",
    const std::string& level = "info",
    const std::string& log_file = ""
) {
    auto logger = spdlog::get(name);
    if (logger) return logger;

    std::vector<spdlog::sink_ptr> sinks;

    // 控制台 sink
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::from_str(level));
    console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
    sinks.push_back(console_sink);

    // 文件 sink（可选）
    if (!log_file.empty()) {
        try {
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_file, 10 * 1024 * 1024, 3);
            file_sink->set_level(spdlog::level::from_str(level));
            sinks.push_back(file_sink);
        } catch (const spdlog::spdlog_ex& ex) {
            spdlog::warn("Failed to create file sink: {}", ex.what());
        }
    }

    // 创建 logger
    auto logger_ptr = std::make_shared<spdlog::logger>(name, begin(sinks), end(sinks));
    logger_ptr->set_level(spdlog::level::from_str(level));
    spdlog::register_logger(logger_ptr);

    return logger_ptr;
}

// 便捷宏
#define LOG_TRACE(...)    spdlog::trace(__VA_ARGS__)
#define LOG_DEBUG(...)   spdlog::debug(__VA_ARGS__)
#define LOG_INFO(...)    spdlog::info(__VA_ARGS__)
#define LOG_WARN(...)    spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...)   spdlog::error(__VA_ARGS__)
#define LOG_CRITICAL(...) spdlog::critical(__VA_ARGS__)

// 音频专用 logger（异步）
inline std::shared_ptr<spdlog::logger> audio_logger() {
    static auto logger = init_logger("audio", "info");
    return logger;
}
