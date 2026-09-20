// src/util/config.hpp
#pragma once
#include "../core/types.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>

using json = nlohmann::json;

// ========== 配置管理 ==========

inline AppConfig load_config(const std::string& path) {
    AppConfig cfg;

    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Failed to open config: " + path);
    }

    json j;
    f >> j;

    // 音频配置
    if (j.contains("audio")) {
        auto& a = j["audio"];
        cfg.audio_device = a.value("device", 0);
        cfg.audio_sample_rate = a.value("sample_rate", 48000);
        cfg.audio_channels = a.value("channels", 1);
        cfg.audio_buffer_ms = a.value("buffer_ms", 10);
    }

    // VAD 参数
    if (j.contains("vad")) {
        auto& v = j["vad"];
        cfg.vad_threshold = v.value("threshold", 0.5f);
        cfg.vad_min_speech_ms = v.value("min_speech_ms", 220);
        cfg.vad_min_silence_ms = v.value("min_silence_ms", 300);
    }

    // 打断参数
    if (j.contains("barge_in")) {
        auto& b = j["barge_in"];
        cfg.barge_in_min_ms = b.value("min_ms", 160);
        cfg.backchannel_max_ms = b.value("backchannel_max_ms", 600);
        cfg.coherence_max = b.value("coherence_max", 0.6f);
        cfg.fade_out_ms = b.value("fade_out_ms", 40);
    }

    // EOU 参数
    if (j.contains("eou")) {
        auto& e = j["eou"];
        cfg.eou_fast_ms = e.value("fast_ms", 350);
        cfg.eou_force_ms = e.value("force_ms", 900);
        cfg.max_utterance_ms = e.value("max_utterance_ms", 20000);
    }

    // 模型路径
    cfg.vad_model = j.value("vad_model", "models/ten-vad.onnx");
    cfg.asr_model = j.value("asr_model", "models/sensevoice");
    cfg.tts_model = j.value("tts_model", "models/kokoro");
    cfg.llm_model = j.value("llm_model", "models/qwen3-4b-q4_k_m.gguf");

    // 搜索
    if (j.contains("search")) {
        auto& s = j["search"];
        cfg.searxng_url = s.value("searxng_url", "http://localhost:8080");
        cfg.tavily_key = s.value("tavily_key", "");
        cfg.brave_key = s.value("brave_key", "");
    }

    // 日志
    cfg.log_level = j.value("log_level", "info");
    cfg.log_file = j.value("log_file", "");

    return cfg;
}

// 打印配置（用于调试）
inline void print_config(const AppConfig& cfg) {
    LOG_INFO("=== Configuration ===");
    LOG_INFO("Audio: device={}, rate={}, channels={}, buffer={}ms",
             cfg.audio_device, cfg.audio_sample_rate, cfg.audio_channels, cfg.audio_buffer_ms);
    LOG_INFO("VAD: threshold={}, min_speech={}ms, min_silence={}ms",
             cfg.vad_threshold, cfg.vad_min_speech_ms, cfg.vad_min_silence_ms);
    LOG_INFO("Barge-in: min={}ms, backchannel_max={}ms, fade_out={}ms",
             cfg.barge_in_min_ms, cfg.backchannel_max_ms, cfg.fade_out_ms);
    LOG_INFO("EOU: fast={}ms, force={}ms, max_utterance={}ms",
             cfg.eou_fast_ms, cfg.eou_force_ms, cfg.max_utterance_ms);
    LOG_INFO("Models: VAD={}, ASR={}, TTS={}, LLM={}",
             cfg.vad_model, cfg.asr_model, cfg.tts_model, cfg.llm_model);
    LOG_INFO("Search: searxng={}", cfg.searxng_url);
}
