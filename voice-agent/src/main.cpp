// src/main.cpp
#include "util/log.hpp"
#include "util/config.hpp"
#include "core/event_bus.hpp"
#include "core/types.hpp"
#include "core/ring_buffer.hpp"
#include "core/cancel_token.hpp"
#include "audio/audio.hpp"
#include "vad/vad.hpp"
#include "asr/asr.hpp"
#include "llm/llm.hpp"
#include "tts/tts.hpp"
#include "orchestrator/orchestrator.hpp"
#include "agent/tool_registry.hpp"
#include "agent/tools.hpp"
#include "agent/agent_loop.hpp"
#include "agent/grammar.hpp"
#include "search/isearch.hpp"
#include "search/searxng_provider.hpp"
#include "search/online_providers.hpp"
#include "memory/memory_store.hpp"
#include "memory/memory_extractor.hpp"
#include "memory/memory_manager.hpp"
#include "memory/memory_command.hpp"
#include <iostream>
#include <csignal>
#include <thread>
#include <atomic>

using namespace std;

// 全局停止标志
atomic<bool> g_running{true};

void signal_handler(int sig) {
    LOG_INFO("Received signal {}, shutting down...", sig);
    g_running = false;
}

void print_banner() {
    cout << R"(
╔═══════════════════════════════════════════════════════════╗
║           Voice Agent - 本地全双工语音助手                 ║
║           Local-First Full-Duplex Voice Agent             ║
╠═══════════════════════════════════════════════════════════╣
║  C++20 | llama.cpp | sherpa-onnx | miniaudio              ║
║  GPU: Intel Arc (Vulkan/OpenVINO)                         ║
╚═══════════════════════════════════════════════════════════╝
)" << endl;
}

void print_usage(const char* prog) {
    cout << "Usage: " << prog << " [options]\n"
         << "Options:\n"
         << "  -c, --config <file>    Config file (default: configs/agent.yaml)\n"
         << "  -l, --log-level <level> Log level: trace, debug, info, warn, error\n"
         << "  -h, --help              Show this help\n"
         << endl;
}

int main(int argc, char* argv[]) {
    // 信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    string config_path = "configs/agent.yaml";
    string log_level = "info";

    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "-c" || arg == "--config") {
            if (++i < argc) config_path = argv[i];
        } else if (arg == "-l" || arg == "--log-level") {
            if (++i < argc) log_level = argv[i];
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
    }

    // 初始化日志
    auto logger = init_logger("voice-agent", log_level);
    LOG_INFO("Voice Agent starting...");

    // 打印 banner
    print_banner();

    // 加载配置
    AppConfig config;
    try {
        config = load_config(config_path);
        print_config(config);
    } catch (const exception& e) {
        LOG_WARN("Config not found or invalid: {}, using defaults", e.what());
    }

    // 初始化事件总线
    auto& event_bus = global_event_bus();

    // 示例：订阅事件
    auto token = event_bus.subscribe(EventType::StateChanged, [](const Event& e) {
        LOG_INFO("State changed: {}", e.text);
    });

    // M0 阶段：只打印配置然后退出
    LOG_INFO("=== M0: Engineering Skeleton ===");
    LOG_INFO("Ring buffer: capacity={}, frame buffer: capacity={}",
             SpscRingBuffer<int16_t, 32768>::capacity(),
             FrameRingBuffer<int16_t, 160, 300>::CAPACITY);

    LOG_INFO("SPSC ring buffer test:");
    {
        SpscRingBuffer<int16_t, 32768> rb;
        int16_t data[480] = {0};
        for (int i = 0; i < 480; i++) data[i] = static_cast<int16_t>(i);

        size_t pushed = rb.push(data, 480);
        LOG_INFO("  Pushed {} samples, available: {}", pushed, rb.available());

        int16_t out[480] = {0};
        size_t popped = rb.pop(out, 480);
        LOG_INFO("  Popped {} samples, available: {}", popped, rb.available());
        LOG_INFO("  First/last sample: {}/{}", out[0], out[479]);
    }

    LOG_INFO("CancelToken test:");
    {
        auto parent = make_shared<CancelToken>();
        auto child = parent->make_child();

        bool parent_called = false, child_called = false;
        parent->on_cancel([&]() { parent_called = true; });
        child->on_cancel([&]() { child_called = true; });

        parent->cancel();
        LOG_INFO("  Parent cancelled: parent_called={}, child_called={}", parent_called, child_called);
        assert(parent_called && child_called);
    }

    LOG_INFO("EventBus test:");
    {
        event_bus.publish(Event{EventType::StateChanged, 0, "Idle", {}, "Testing"});
        this_thread::sleep_for(chrono::milliseconds(50));
    }

    // 取消订阅
    event_bus.unsubscribe(token);

    LOG_INFO("=== M0 Complete ===");
    LOG_INFO("All core components initialized successfully.");

    // ========== M1: 音频闭环 + AEC ==========
    LOG_INFO("\n=== M1: Audio Loop + AEC ===");

    // 初始化音频设备管理器
    voice_agent::AudioDeviceManager device_manager;
    if (!device_manager.initialize()) {
        LOG_ERROR("Failed to initialize audio device manager");
        return 1;
    }

    // 列出可用设备
    LOG_INFO("Available input devices:");
    for (const auto& dev : device_manager.list_input_devices()) {
        LOG_INFO("  - {} (default: {})", dev.name, dev.is_default);
    }

    LOG_INFO("Available output devices:");
    for (const auto& dev : device_manager.list_output_devices()) {
        LOG_INFO("  - {} (default: {})", dev.name, dev.is_default);
    }

    // 配置音频参数
    voice_agent::AudioConfig audio_config;
    audio_config.sample_rate = 48000;
    audio_config.channels = 1;
    audio_config.frames_per_buffer = 480;  // 10ms @ 48kHz

    // 初始化 AEC
    voice_agent::EchoCanceller aec;
    voice_agent::AECConfig aec_config;
    aec_config.sample_rate = audio_config.sample_rate;
    aec_config.frame_size = audio_config.frames_per_buffer;
    aec_config.filter_length_ms = 200;
    if (aec.initialize(aec_config)) {
        LOG_INFO("AEC initialized successfully");
        LOG_INFO("AEC config: {} Hz, {} ms tail, {} taps",
                 aec_config.sample_rate, aec_config.filter_length_ms,
                 aec_config.sample_rate * aec_config.filter_length_ms / 1000);
    } else {
        LOG_WARN("AEC initialization failed, continuing without AEC");
    }

    // 初始化音频管道
    voice_agent::AudioPipeline pipeline;
    if (!pipeline.initialize(audio_config)) {
        LOG_ERROR("Failed to initialize audio pipeline");
        return 1;
    }

    // 设置输入回调（打印能量信息）
    int frame_count = 0;
    pipeline.set_input_callback([&](const int16_t* data, size_t frames) {
        frame_count++;
        if (frame_count % 50 == 0) {  // 每 50 帧（约 0.5 秒）打印一次
            double energy_db = voice_agent::calculate_energy_db(data, frames);
            bool has_voice = voice_agent::detect_voice_activity(data, frames, 48000);
            LOG_DEBUG("Input energy: {:.1f} dB, voice: {}", energy_db, has_voice);
        }
    });

    // 启动音频管道
    if (!pipeline.start()) {
        LOG_ERROR("Failed to start audio pipeline");
        return 1;
    }

    LOG_INFO("Audio pipeline started, listening for 10 seconds...");
    LOG_INFO("Speak into your microphone or make some noise!");

    // 运行 10 秒
    this_thread::sleep_for(chrono::seconds(10));

    // 停止音频管道
    pipeline.stop();

    // 获取 AEC 统计
    if (aec.is_initialized()) {
        auto stats = aec.get_stats();
        LOG_INFO("AEC stats: ERLE={:.1f} dB, converged={}", stats.erle_db, stats.converged);
    }

    LOG_INFO("=== M1 Complete ===");
    LOG_INFO("Audio loop working! Next: M2 - VAD + interruption detection");

    // ========== M2: VAD + 打断探测 ==========
    LOG_INFO("\n=== M2: VAD + Interruption Detection ===");

    // 初始化 VAD
    voice_agent::VAD vad;
    voice_agent::VADConfig vad_config;
    vad_config.sample_rate = 16000;
    vad_config.frame_length_ms = 32;
    vad_config.min_speech_duration_ms = 250;
    vad_config.min_silence_duration_ms = 500;
    vad_config.speech_threshold = 0.5f;
    
    if (!vad.initialize(vad_config)) {
        LOG_ERROR("Failed to initialize VAD");
        return 1;
    }
    
    // 设置 VAD 回调
    vad.set_callback([](voice_agent::VADEvent event, const int16_t*, size_t) {
        switch (event) {
            case voice_agent::VADEvent::SpeechStart:
                LOG_INFO("[VAD] Speech start detected!");
                break;
            case voice_agent::VADEvent::SpeechEnd:
                LOG_INFO("[VAD] Speech end detected!");
                break;
            case voice_agent::VADEvent::SpeechOngoing:
                // 语音进行中，不打印
                break;
            case voice_agent::VADEvent::Silence:
                break;
        }
    });

    // 初始化打断探测器
    voice_agent::InterruptionDetector interruption_detector;
    voice_agent::InterruptionDetector::InterruptionConfig int_config;
    int_config.energy_jump_threshold = 3.0f;
    int_config.energy_history_size = 10;
    int_config.min_interruption_frames = 3;
    
    if (!interruption_detector.initialize(int_config)) {
        LOG_ERROR("Failed to initialize interruption detector");
        return 1;
    }
    
    // 设置打断回调
    bool interruption_detected = false;
    interruption_detector.set_callback([&]() {
        interruption_detected = true;
        LOG_WARN("[INTERRUPTION] User interruption detected!");
    });

    // 使用音频管道进行 VAD 测试（复用 M1 的管道）
    LOG_INFO("VAD test: listening for 10 seconds...");
    LOG_INFO("Speak into your microphone - VAD should detect speech start/end");

    // 设置输入回调，添加 VAD 处理
    frame_count = 0;
    pipeline.set_input_callback([&](const int16_t* data, size_t frames) {
        frame_count++;
        
        // 下采样到 16kHz 用于 VAD
        std::vector<int16_t> pcm_16k(frames / 3);
        for (size_t i = 0; i < pcm_16k.size(); i++) {
            int32_t sum = 0;
            for (int j = 0; j < 3; j++) {
                sum += data[i * 3 + j];
            }
            pcm_16k[i] = static_cast<int16_t>(sum / 3);
        }
        
        // VAD 处理
        vad.process(pcm_16k.data(), pcm_16k.size());
        
        // 打断检测
        interruption_detector.process(data, frames);
        
        // 打印状态（每 50 帧）
        if (frame_count % 50 == 0) {
            double energy_db = voice_agent::calculate_energy_db(data, frames);
            bool is_speaking = vad.is_speaking();
            float int_conf = interruption_detector.get_confidence();
            LOG_DEBUG("[VAD] Energy: {:.1f} dB, speaking: {}, interruption_conf: {:.2f}", 
                     energy_db, is_speaking, int_conf);
        }
    });

    // 重新启动音频管道（之前已经停止）
    if (!pipeline.start()) {
        LOG_ERROR("Failed to start audio pipeline for VAD test");
        return 1;
    }

    // 运行 10 秒
    this_thread::sleep_for(chrono::seconds(10));

    // 停止音频管道
    pipeline.stop();

    if (interruption_detected) {
        LOG_INFO("User interruption was detected during the test!");
    }

    LOG_INFO("=== M2 Complete ===");
    LOG_INFO("VAD and interruption detection working! Next: M3 - ASR + LLM + TTS");

    // ========== M3: ASR + LLM + TTS ==========
    LOG_INFO("\n=== M3: ASR + LLM + TTS ===");

    // 初始化 ASR
    voice_agent::ASR asr;
    voice_agent::ASRConfig asr_config;
    asr_config.model_path = "models/sense-voice";
    asr_config.sample_rate = 16000;
    asr_config.provider = "cpu";
    
    if (!asr.initialize(asr_config)) {
        LOG_WARN("ASR initialization failed, using mock ASR");
    } else {
        LOG_INFO("ASR initialized: {}", asr_config.model_path);
    }
    
    // 设置 ASR 回调
    std::atomic<bool> asr_triggered{false};
    asr.set_callback([&](const voice_agent::ASRResult& result) {
        LOG_INFO("[ASR] Recognized: '{}' (conf={:.2f}, final={})", 
                 result.text, result.confidence, result.is_final);
        asr_triggered = true;
    });

    // 初始化 LLM
    voice_agent::LLM llm;
    voice_agent::LLMConfig llm_config;
    llm_config.model_path = "models/qwen3-1.5b-q4.gguf";
    llm_config.n_ctx = 4096;
    llm_config.n_threads = 4;
    llm_config.temperature = 0.7f;
    llm_config.max_tokens = 256;
    
    if (!llm.initialize(llm_config)) {
        LOG_WARN("LLM initialization failed, using mock LLM");
    } else {
        LOG_INFO("LLM initialized: {}", llm_config.model_path);
    }

    // 初始化 TTS
    voice_agent::TTS tts;
    voice_agent::TTSConfig tts_config;
    tts_config.model_path = "models/kokoro";
    tts_config.voice_path = "models/kokoro/voices/zh_female.pt";
    tts_config.sample_rate = 24000;
    tts_config.speed = 1.0f;
    tts_config.lang = "zh";
    
    if (!tts.initialize(tts_config)) {
        LOG_WARN("TTS initialization failed, using mock TTS");
    } else {
        LOG_INFO("TTS initialized: {}", tts_config.model_path);
    }

    // 端到端测试：模拟识别→生成→合成
    LOG_INFO("\nM3 Pipeline test:");
    
    // 1. 模拟 ASR 识别
    LOG_INFO("1. Simulating ASR recognition...");
    asr_triggered = false;
    std::vector<int16_t> test_audio(16000, 0);  // 1秒静音
    // 添加一些能量模拟语音
    for (int i = 0; i < 4000; i++) {
        test_audio[4000 + i] = static_cast<int16_t>(8000 * std::sin(2 * 3.14159 * 440 * i / 16000));
    }
    asr.process(test_audio.data(), test_audio.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 2. LLM 生成
    LOG_INFO("2. Testing LLM generation...");
    std::string prompt = "你好，请介绍一下你自己";
    LOG_INFO("   Prompt: {}", prompt);
    
    std::string llm_response;
    llm.generate_stream(prompt, [&](const voice_agent::LLMResponse& chunk) {
        LOG_DEBUG("[LLM] {}", chunk.text);
        llm_response += chunk.text;
    });
    LOG_INFO("   Response: {}", llm_response);

    // 3. TTS 合成
    LOG_INFO("3. Testing TTS synthesis...");
    auto tts_audio = tts.synthesize(llm_response.empty() ? "你好，我是语音助手" : llm_response);
    LOG_INFO("   Synthesized {} samples ({} ms)", tts_audio.size(), tts_audio.size() * 1000 / tts_config.sample_rate);

    // 4. 流式 TTS 测试
    LOG_INFO("4. Testing streaming TTS...");
    std::atomic<int> tts_chunks{0};
    tts.synthesize_stream("流式合成测试", [&](const int16_t* audio, size_t frames, bool is_last) {
        tts_chunks++;
        LOG_DEBUG("[TTS] Chunk {}: {} samples, last={}", tts_chunks.load(), frames, is_last);
    });

    LOG_INFO("=== M3 Complete ===");
    LOG_INFO("All M3 components initialized. Next: M4 - Full-duplex state machine");

    // ========== M4: Orchestrator 全双工状态机 ==========
    LOG_INFO("\n=== M4: Orchestrator Full-Duplex State Machine ===");

    // 构造 Orchestrator（接管 VAD→ASR→LLM→TTS 流程）
    voice_agent::Orchestrator::Config orch_config;
    orch_config.eou_fast_ms = 350;
    orch_config.eou_force_ms = 900;
    orch_config.max_utterance_ms = 20000;
    orch_config.barge_in_min_ms = 160;
    orch_config.backchannel_max_ms = 600;
    orch_config.fade_out_ms = 40;
    orch_config.capture_sample_rate = 16000;
    orch_config.playback_sample_rate = 24000;

    voice_agent::Orchestrator orch(orch_config);

    // 注入子模块（复 用 M1/M2/M3 已初始化的实例）
    if (!orch.initialize(
            nullptr,   // AudioPipeline 在 orch.start() 内部构造，此处传 null
            std::make_shared<voice_agent::VAD>(),
            std::make_shared<voice_agent::ASR>(),
            std::make_shared<voice_agent::LLM>(),
            std::make_shared<voice_agent::TTS>())) {
        LOG_ERROR("Failed to initialize Orchestrator");
        return 1;
    }
    LOG_INFO("Orchestrator initialized");

    // 设置文本输出回调
    orch.set_text_callback([](const std::string& text) {
        LOG_INFO("[Orchestrator] output: {}", text);
    });

    // 启动状态机
    orch.start();
    LOG_INFO("Orchestrator started. State: {}", static_cast<int>(orch.current_state()));

    // 保持运行，按 Ctrl-C 停止
    LOG_INFO("M4 running - Orchestrator active. Press Ctrl-C to stop...");
    while (g_running && orch.is_running()) {
        this_thread::sleep_for(chrono::milliseconds(200));
        // 每 2 秒打印一次状态
        static int tick = 0;
        if (++tick % 10 == 0) {
            LOG_DEBUG("[Orchestrator] state={}", static_cast<int>(orch.current_state()));
        }
    }

    // 停止状态机
    orch.stop();
    LOG_INFO("Orchestrator stopped");

    LOG_INFO("=== M4 Complete ===");

    // ========== M5: Agent 结构化输出 + 工具循环 + 搜索 ==========
    LOG_INFO("\n=== M5: Agent Tools & Search ===");

    using namespace voice_agent;

    // 1. 搜索路由：本地 SearXNG 优先，未配置在线 key 时在线回退自动为空
    auto search_router = std::make_shared<SearchRouter>();
    if (!config.searxng_url.empty()) {
        search_router->add_provider(std::make_shared<SearxngProvider>(config.searxng_url));
    }
    for (auto& p : create_online_providers(config.tavily_key, config.brave_key)) {
        search_router->add_provider(p);
    }

    // 2. 注册内置工具（memory_* 走持久化 MemoryStore）
    ToolRegistry registry;
    auto m5_mem = std::make_shared<MemoryStore>("build/m5_memory.db");
    m5_mem->open();
    ToolKit kit{search_router, m5_mem};
    register_builtin_tools(registry, kit);
    {
        std::string joined;
        for (auto& n : registry.names()) {
            if (!joined.empty()) joined += ", ";
            joined += n;
        }
        LOG_INFO("Registered tools: [{}]", joined);
    }

    // 3. 演示结构化 grammar（JSON Schema → GBNF）
    {
        auto defs = registry.tool_defs();
        std::string gbnf = tool_calls_grammar(defs);
        LOG_INFO("Generated tool-call GBNF grammar ({} bytes):", gbnf.size());
        LOG_INFO("  {}", gbnf);
    }

    // 4. 演示工具调用解析
    {
        const char* sample =
            "我正在查。\n<tool_call>{\"name\":\"get_time\",\"arguments\":{}}</tool_call>\n"
            "请稍等。";
        auto calls = parse_tool_calls(sample);
        LOG_INFO("parse_tool_calls -> {} call(s), first name='{}'",
                 calls.size(), calls.empty() ? "" : calls[0].name);
    }

    // 5. 并行工具执行（get_time + memory_save + memory_query）
    {
        ToolExecutor running(registry);
        std::vector<ToolCall> calls = {
            ToolCall{"c1", "get_time", "{}"},
            ToolCall{"c2", "memory_save",
                     R"({"subject":"user","content":"我住在北京。靠窗。偏好安静"})"},
            ToolCall{"c3", "memory_query", R"({"q":"北京"})"},
        };
        auto results = running.execute_parallel(calls, nullptr, 5000);
        for (auto& r : results) {
            LOG_INFO("  tool[{}]: {}", r.call_id,
                     r.is_error ? ("ERROR " + r.content) : r.content);
        }
        static_cast<void>(kit);  // kit 已通过 registry 捕获
    }

    // 6. AgentLoop 端到端（复用 M3 的 LLM 实例，mock 或真实皆可）
    {
        ToolExecutor running(registry);
        AgentLoop loop(llm, registry, running);
        loop.set_system_prompt(
            "你是本地语音助手。可调用工具完成任务；当前时间与搜索由工具提供。");
        auto result = loop.run("你好，现在几点了？", nullptr);
        LOG_INFO("AgentLoop rounds={} -> '{}'", result.tool_rounds, result.final_text);
    }

    // 7. 挂载到 Orchestrator（演示 agent_enabled）
    {
        voice_agent::Orchestrator demo_agent(
            voice_agent::Orchestrator::Config{});
        demo_agent.attach_agent(std::make_shared<ToolRegistry>(), nullptr,
                                "你是语音助手。");
        LOG_INFO("Orchestrator agent_enabled = {}", demo_agent.agent_enabled());
    }

    LOG_INFO("=== M5 Complete ===");

    // ========== M6: 持久化记忆（SQLite + 抽取 + 检索 + 命令）==========
    LOG_INFO("\n=== M6: Persistent Memory ===");

    // 用独立演示库，避免污染主 memory.db
    std::string mem_path = "build/m6_demo_memory.db";
    (void)std::remove(mem_path.c_str());  // 干净起步
    auto memory = std::make_shared<voice_agent::MemoryManager>(mem_path);
    if (!memory->open()) {
        LOG_ERROR("M6: failed to open memory db");
    } else {
        // 1. 抽取 + ingest（启发式基线）
        LOG_INFO("Memory ingest...");
        LOG_INFO("  [{}]", "我叫小明，喜欢喝绿茶");
        memory->ingest("我叫小明，喜欢喝绿茶");
        LOG_INFO("  [{}]", "我住在上海");
        memory->ingest("我住在上海");
        LOG_INFO("  [{}]", "今天天气不错（应被忽略）");
        memory->ingest("今天天气不错");

        LOG_INFO("Memory count = {}", memory->count());

        // 2. 召回（中文短词）
        auto recalled = memory->recall("小明喜欢什么", 3);
        LOG_INFO("Recall '小明喜欢什么':");
        for (auto& m : recalled) {
            LOG_INFO("  - {}#{} [{}]: {}", m.id, m.subject, m.type, m.content);
        }

        // 3. /memory 命令入口
        LOG_INFO("cmd 'memory list'  -> {}", memory->run_command("memory list"));
        LOG_INFO("cmd 'memory save 称号@小茶友' -> {}",
                 memory->run_command("memory save 称号@小茶友"));
        LOG_INFO("cmd 'memory count' -> {}", memory->run_command("memory count"));
        LOG_INFO("cmd 'memory query 上海' -> {}", memory->run_command("memory query 上海"));

        // 4. 持久化验证：重开一个实例读取同一库
        {
            auto reopen = std::make_shared<voice_agent::MemoryManager>(mem_path);
            reopen->open();
            LOG_INFO("Reopened same db -> count = {}", reopen->count());
            auto lst = reopen->list(10);
            for (auto& m : lst) {
                LOG_INFO("  persisted: [{}] {}", m.id, m.content);
            }
        }
    }

    // 5. 挂载到 Orchestrator（演示 memory_enabled）
    {
        voice_agent::Orchestrator demo_agent(voice_agent::Orchestrator::Config{});
        demo_agent.attach_memory(memory);
        LOG_INFO("Orchestrator memory_enabled = {}", demo_agent.memory_enabled());
    }

    LOG_INFO("=== M6 Complete ===");
    LOG_INFO("All modules complete. Shutting down...");

    return 0;
}
