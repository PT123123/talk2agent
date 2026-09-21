// src/gui/agent_controller.cpp
#include "gui/agent_controller.hpp"

// 核心装配
#include "core/event_bus.hpp"
#include "util/config.hpp"
#include "util/log.hpp"
#include "orchestrator/orchestrator.hpp"
#include "agent/tool_registry.hpp"
#include "agent/agent_loop.hpp"
#include "agent/tools.hpp"
#include "search/searxng_provider.hpp"
#include "search/online_providers.hpp"
#include "memory/memory_manager.hpp"
#include "memory/memory_command.hpp"
#include "tts/speaker.hpp"
#include "tts/streaming_speaker.hpp"

#include <QObject>
#include <QFileInfo>
#include <QString>
#include <QVariantList>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <system_error>
#include <vector>
#include <nlohmann/json.hpp>

namespace voice_agent {
namespace gui {

namespace {
QString to_q(const std::string& s) { return QString::fromStdString(s); }

// LLM 全量层数下放 GPU（Vulkan/CUDA 后端可用时；后端缺失时 llama.cpp 自动回退 CPU）
inline constexpr int kLLMGpuLayers = 999;

// 把模型文件名提取成可读型号名（去路径/扩展名/量化后缀），如 qwen3-4b-q4_k_m → qwen3-4b
QString chipModelName(const std::string& file) {
    std::string f = file;
    auto ext = f.rfind('.');
    if (ext != std::string::npos) f = f.substr(0, ext);
    static const char* const kQ[] = {"-q4_k_m", "-q4_0", "-q8_0", "-q5_k_m", "-q6_k"};
    for (auto* sq : kQ) {
        auto p = f.find(sq);
        if (p != std::string::npos) f = f.substr(0, p);
    }
    return to_q(f);
}

// 把处理阶段映射成右侧日志区的中文明细（首次到达该阶段时提示一次）
QString stageLogLine(const std::string& stage, double ms) {
    if (stage == "speech") return QStringLiteral("采集：用户语音段结束（%1 ms）").arg(ms, 0, 'f', 1);
    if (stage == "asr")
        return QStringLiteral("ASR 录音转文字模型前向推理完成（%1 ms）").arg(ms, 0, 'f', 1);
    if (stage == "llm_first") return QStringLiteral("LLM 首token 到达（%1 ms）").arg(ms, 0, 'f', 1);
    if (stage == "llm_generate") return QStringLiteral("LLM 生成完成（%1 ms）").arg(ms, 0, 'f', 1);
    if (stage == "tools") return QStringLiteral("工具执行完成（%1 ms）").arg(ms, 0, 'f', 1);
    if (stage == "tts") return QStringLiteral("TTS 合成完成（%1 ms）").arg(ms, 0, 'f', 1);
    return QStringLiteral("阶段 %1 完成（%2 ms）").arg(to_q(stage), QString::number(ms, 'f', 1));
}
const char* kSystemPrompt =
    "你是本地语音助手，本地优先全双工。"
    "可调用工具完成任务（取时间、联网搜索、读写记忆）；"
    "上下文中的 [相关记忆] 是此前会话记住的事实，回答时优先参考。";

// 后台初始化阶段（供界面显示“正在加载到哪一步”）
const char* const kInitStages[] = {
    "读取配置",
    "初始化搜索路由",
    "打开记忆库",
    "注册工具",
    "加载 LLM 模型",
    "加载 VAD 模型",
    "加载 ASR 模型",
    "加载 TTS 模型",
    "装配语音编排器",
};
constexpr int kInitStageCount() {
    return static_cast<int>(sizeof(kInitStages) / sizeof(kInitStages[0]));
}

// 切换模型时的重载阶段（LLM 起）
const char* const kSwitchStages[] = {
    "加载 LLM 模型",
    "加载 VAD 模型",
    "加载 ASR 模型",
    "加载 TTS 模型",
    "装配语音编排器",
};
constexpr int kSwitchStageCount() {
    return static_cast<int>(sizeof(kSwitchStages) / sizeof(kSwitchStages[0]));
}

// 把 paths 中的空段回退为配置默认值，并拼成 “models/<subdir>/file”
std::string resolve_model(const QString& sel, const std::string& def,
                          const std::string& prefix) {
    if (!sel.isEmpty()) return "models/" + sel.toStdString();
    if (def.rfind("models/", 0) == 0) return def;
    return prefix + "/" + def;
}

// 探测 TTS 模型目录类型："kokoro"（含 voices.bin）/ "piper"（含 espeak-ng-data + onnx）/ 空
std::string detect_tts_engine(const std::string& dir) {
    if (dir.empty()) return {};
    std::error_code ec;
    if (std::filesystem::exists(dir + "/voices.bin", ec)) return "kokoro";
    if (std::filesystem::exists(dir + "/espeak-ng-data", ec)) return "piper";
    return {};
}

// 在 Piper 目录内定位 <voice>.onnx（优先 model.onnx，其次任一 *.onnx）
std::string find_piper_onnx(const std::string& dir) {
    if (dir.empty()) return {};
    std::error_code ec;
    if (std::filesystem::exists(dir + "/model.onnx", ec))
        return dir + "/model.onnx";
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == ".onnx")
            return e.path().string();
    }
    return {};
}

// 按 TTS 模型目录自动构建配置（Kokoro / Piper 目录均可识别）；
// 目录不可识别时返回空 engine，由调用方回退到配置的 tts_engine。
TTSConfig tts_config_for_dir(const std::string& dir, const AppConfig& cfg) {
    TTSConfig tc{};
    tc.speed = cfg.tts_speed;
    tc.pitch = cfg.tts_pitch;
    tc.speaker_id = cfg.tts_speaker_id;
    const std::string eng = detect_tts_engine(dir);
    if (eng == "piper") {
        // Piper(VITS)：<voice>.onnx + tokens.txt + espeak-ng-data
        tc.engine = "piper";
        tc.model_path = find_piper_onnx(dir);
        tc.tokens_path = dir + "/tokens.txt";
        tc.data_dir = dir + "/espeak-ng-data";
    } else if (eng == "kokoro") {
        // Kokoro：model.onnx + voices.bin + tokens.txt + espeak-ng-data
        tc.engine = "kokoro";
        tc.model_path = dir + "/model.onnx";
        tc.voice_path = dir + "/voices.bin";
        tc.tokens_path = dir + "/tokens.txt";
        tc.data_dir = dir + "/espeak-ng-data";
        tc.lexicon = dir + "/lexicon-zh.txt";
    }
    return tc;
}

// 计算文件或目录占用总字节数（目录递归求和；路径不存在/不可用返回 0）
std::uintmax_t path_total_bytes(const std::string& p) {
    std::error_code ec;
    if (p.empty()) return 0;
    const std::filesystem::path fp(p);
    if (std::filesystem::is_regular_file(fp, ec)) return std::filesystem::file_size(fp, ec);
    if (!std::filesystem::is_directory(fp, ec)) return 0;
    std::uintmax_t total = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(
             fp, std::filesystem::directory_options::none, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        std::error_code fe;
        if (it->is_regular_file(fe)) total += it->file_size(fe);
    }
    return total;
}
}  // namespace

// ========== 核心对象（工作线程内使用）==========
struct AgentController::Impl {
    AppConfig cfg;
    std::shared_ptr<SearchRouter> router;
    std::shared_ptr<MemoryManager> memory;
    std::shared_ptr<ToolRegistry> registry;
    std::shared_ptr<LLM> llm;
    std::shared_ptr<VAD> vad;
    std::shared_ptr<ASR> asr;
    std::shared_ptr<TTS> tts;
    std::shared_ptr<AudioPipeline> audio;   // 麦克风采集 + 扬声器播放管道
    std::string audio_device_name;          // 当前输入设备名（空 = 系统默认）
    std::shared_ptr<Orchestrator> orch;
    uint64_t event_token{0};
    std::string base_prompt{kSystemPrompt};
    std::string lastResponse;          // 最近一次回答（供"语音播报"按钮朗读）
    std::shared_ptr<TTSSpeaker> speaker;
    std::shared_ptr<StreamingSpeaker> stream_speaker;   // 文本链路流式播报（边生成边播）
    ModelPaths activeModels;   // 当前实际启用的模型（相对 models/ 或空）
    bool tts_enabled{true};

    // 模型状态机信息（key ∈ {LLM,VAD,ASR,TTS}）
    struct ModelInfo {
        std::string state{"idle"};   // "idle" / "loading" / "ready"
        std::string path;            // 实际模型路径（可能为空）
        double load_ms{0.0};         // 最近一次加载耗时
    };
    std::map<std::string, ModelInfo> models;

    // 单轮统计（仅在 worker 线程访问）
    std::map<std::string, double> turn_stages;
    int turn_tool_calls{0};
    int turn_index{0};
    bool turn_active{false};
    std::string turn_mode;      // "text" / "voice"
    std::string turn_user;
};

AgentController::AgentController(QObject* parent) : QObject(parent) {}

AgentController::~AgentController() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (impl_ && impl_->event_token) {
            global_event_bus().unsubscribe(impl_->event_token);
        }
    }
    if (running_) {
        Task quit{TaskType::Quit, {}};
        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push_back(std::move(quit));
        }
        cv_.notify_all();
        running_ = false;
        if (worker_.joinable()) worker_.join();
    }
}

void AgentController::start() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (running_) return;
    running_ = true;
    worker_ = std::thread(&AgentController::threadMain_, this);
}

void AgentController::startVoice() {
    Task t{TaskType::StartVoice, {}};
    std::lock_guard<std::mutex> lk(mtx_);
    queue_.push_back(std::move(t));
    cv_.notify_all();
}

void AgentController::stopVoice() {
    Task t{TaskType::StopVoice, {}};
    std::lock_guard<std::mutex> lk(mtx_);
    queue_.push_back(std::move(t));
    cv_.notify_all();
}

void AgentController::sendText(const QString& text) {
    Task t{TaskType::SendText, text};
    std::lock_guard<std::mutex> lk(mtx_);
    queue_.push_back(std::move(t));
    cv_.notify_all();
}

void AgentController::playResponse() {
    Task t{TaskType::PlayResponse, {}};
    std::lock_guard<std::mutex> lk(mtx_);
    queue_.push_back(std::move(t));
    cv_.notify_all();
}

void AgentController::refreshLatencies() {
    Task t{TaskType::RefreshLat, {}};
    std::lock_guard<std::mutex> lk(mtx_);
    queue_.push_back(std::move(t));
    cv_.notify_all();
}

void AgentController::setModels(const ModelPaths& paths) {
    Task t;
    t.type = TaskType::SetModels;
    t.models = paths;
    std::lock_guard<std::mutex> lk(mtx_);
    queue_.push_back(std::move(t));
    cv_.notify_all();
}

void AgentController::setTtsEnabled(bool enabled) {
    Task t;
    t.type = TaskType::SetTts;
    t.flag = enabled;
    std::lock_guard<std::mutex> lk(mtx_);
    queue_.push_back(std::move(t));
    cv_.notify_all();
}

void AgentController::setVadEnabled(bool enabled) {
    Task t;
    t.type = TaskType::SetVad;
    t.flag = enabled;
    std::lock_guard<std::mutex> lk(mtx_);
    queue_.push_back(std::move(t));
    cv_.notify_all();
}

void AgentController::setTtsParams(double speed, double pitch, int speakerId) {
    Task t;
    t.type = TaskType::SetTtsParams;
    t.d0 = speed;
    t.d1 = pitch;
    t.ival = speakerId;
    std::lock_guard<std::mutex> lk(mtx_);
    queue_.push_back(std::move(t));
    cv_.notify_all();
}

void AgentController::setAudioDevice(const QString& deviceName) {
    Task t;
    t.type = TaskType::SetAudioDevice;
    t.text = deviceName;
    std::lock_guard<std::mutex> lk(mtx_);
    queue_.push_back(std::move(t));
    cv_.notify_all();
}

void AgentController::requestMemoryList() {
    Task t{TaskType::ListMemory, {}};
    std::lock_guard<std::mutex> lk(mtx_);
    queue_.push_back(std::move(t));
    cv_.notify_all();
}

float AgentController::inputLevelDb() const {
    if (!impl_ || !impl_->audio) return -96.0f;
    return impl_->audio->last_input_level_db();
}

void AgentController::threadMain_() {
    // 初始化整体包 try/catch：模型过大导致内存不足（bad_alloc）等异常
    // 绝不能从工作线程逃逸，否则 std::terminate 会把整个进程（含 GUI）干掉。
    try {
        initCore_();
        emit logLine("核心模块就绪，等待指令。");
        emit loadFinished(true);
    } catch (const std::exception& e) {
        emit errorLine(QStringLiteral("初始化失败：%1").arg(to_q(e.what())));
        emit loadFinished(false);
    } catch (...) {
        emit errorLine(QStringLiteral("初始化失败（未知错误）。"));
        emit loadFinished(false);
    }

    while (running_) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return !running_ || !queue_.empty(); });
            if (queue_.empty()) continue;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        if (task.type == TaskType::Quit) break;
        try {
            handleTask_(task);
        } catch (const std::exception& e) {
            emit errorLine(QStringLiteral("内部错误：%1").arg(to_q(e.what())));
        }
    }
}

void AgentController::initCore_() {
    auto impl = std::make_shared<Impl>();
    impl_ = impl;   // 提前挂到成员，确保模型状态机信号可即时上报
    seedModelInfo_();
    const int total = kInitStageCount();
    int step = 0;

    // 配置（缺失时回退默认值）
    emit loadStageChanged(++step, total, kInitStages[step - 1]);
    try {
        impl->cfg = load_config("configs/agent.yaml");
    } catch (...) {
        emit logLine("configs/agent.yaml 未找到，使用默认配置。");
    }

    // 搜索路由：本地 SearXNG 优先，在线 Provider 桩
    emit loadStageChanged(++step, total, kInitStages[step - 1]);
    impl->router = std::make_shared<SearchRouter>();
    if (!impl->cfg.searxng_url.empty()) {
        impl->router->add_provider(
            std::make_shared<SearxngProvider>(impl->cfg.searxng_url));
    }
    for (auto& p : create_online_providers(impl->cfg.tavily_key, impl->cfg.brave_key)) {
        impl->router->add_provider(p);
    }

    // 持久化记忆
    emit loadStageChanged(++step, total, kInitStages[step - 1]);
    std::filesystem::create_directories("data");
    impl->memory = std::make_shared<MemoryManager>("data/voice_agent.db");
    if (!impl->memory->open()) {
        emit errorLine("记忆库打开失败。");
    } else {
        emit logLine(QStringLiteral("记忆库已加载：%1 条").arg(impl->memory->count()));
    }

    // 工具注册（get_time / web_search / memory_save / memory_query）
    emit loadStageChanged(++step, total, kInitStages[step - 1]);
    impl->registry = std::make_shared<ToolRegistry>();
    ToolKit kit{impl->router, impl->memory->store()};
    register_builtin_tools(*impl->registry, kit);
    {
        std::string joined;
        for (auto& n : impl->registry->names()) {
            if (!joined.empty()) joined += ", ";
            joined += n;
        }
        emit logLine(QStringLiteral("已注册工具：[%1]").arg(to_q(joined)));
    }

    // 模型模块：先创建 Mock 占位，SEH 防护加载真实模型；
    // 访问违规（0xc0000005）被 __except 拦截并回退 Mock，保证进程存活。
    impl->llm = std::make_shared<LLM>();
    impl->vad = std::make_shared<VAD>();
    impl->asr = std::make_shared<ASR>();
    impl->tts = std::make_shared<TTS>();
    // initModelsWork_ 内部独立 try/catch，单模失败不影响其余四个
    initModelsGuarded_(impl.get(), step, total);

    // Orchestrator 全双工状态机：真实麦克风采集 → VAD → ASR → LLM/工具 → TTS → 播放
    emit loadStageChanged(++step, total, kInitStages[step - 1]);
    impl->audio = createAudioPipeline_();
    impl->orch = buildOrchestrator_(impl->audio);

    // 订阅状态事件 → 界面（并驱动语音轮次的开始/结束计时）
    impl->event_token = global_event_bus().subscribe(
        EventType::StateChanged, [this](const Event& e) {
            emit stateChanged(to_q(e.text.empty() ? "未知状态" : e.text));
            if (!impl_) return;
            if (e.text == "Listening" && !impl_->turn_active) {
                impl_->turn_active = true;
                impl_->turn_mode = "voice";
                clearTurn_();
            } else if (e.text == "Speaking" && impl_->turn_active &&
                       impl_->turn_mode == "voice") {
                finishVoiceTurn_();
            }
        });

    // 汇报本地已扫描到的模型
    emit logLine("本地模型：请在“设置”页查看/下载/切换。");
    emitModelStatus_();

    // 说明默认 LLM 型号与真实后端加载情况
    {
        const std::string llmPath = impl->cfg.llm_model;
        QFileInfo lfi(QString::fromStdString(llmPath));
        const QString name = chipModelName(lfi.fileName().toStdString());
        const bool real = impl->llm && impl->llm->real_backend();
        emit logLine(QStringLiteral("默认 LLM 型号：%1（文件 %2）")
                         .arg(name.isEmpty() ? QStringLiteral("未知") : name,
                              QString::fromStdString(llmPath)));
        emit logLine(real
            ? QStringLiteral("LLM 真实后端已就绪（llama.cpp）。")
            : QStringLiteral("注意：LLM 模型文件缺失，已回退到 Mock 占位，当前不会产生真实的模型推理。"
                             "请在“设置”页下载或把 GGUF 放到 %1。")
                  .arg(QString::fromStdString(llmPath)));
    }
}

// ========== SEH 就地防护（拦截 0xc0000005 等硬件异常）==========
// MSVC SEH：__try / __except；E06D 对应 C++ throw
// C2712 约束：含 __try 的函数不得有带析构的局部对象（智能指针/容器等）。
// 策略：Impl 以原始指针传入；__try 内只调用成员方法，不触发隐式析构链。

int AgentController::initModelsGuarded_(Impl* impl, int& step, int total) {
    __try {
        initModelsWork_(impl);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        emit errorLine(QStringLiteral("模型加载触发访问违规（0x%1），已回退 Mock 模式，界面保持运行。")
                           .arg(GetExceptionCode(), 0, 16));
        return static_cast<int>(GetExceptionCode());
    }
}

void AgentController::initModelsWork_(Impl* impl) {
    // 每个模块独立 try/catch：单个失败不影响其余四个
    try {
        emit loadStageChanged(5, 9, kInitStages[4]);   // "加载 LLM 模型"
        auto t0 = std::chrono::steady_clock::now();
        markModelLoading_("LLM", impl->cfg.llm_model);
        impl->llm->initialize(LLMConfig{impl->cfg.llm_model, kLLMGpuLayers});
        markModelReady_("LLM", std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - t0).count());
    } catch (const std::exception& e) {
        emit logLine(QStringLiteral("LLM 加载失败：%1（回退 Mock）").arg(to_q(e.what())));
    }

    try {
        emit loadStageChanged(6, 9, kInitStages[5]);   // "加载 VAD 模型"
        auto t0 = std::chrono::steady_clock::now();
        markModelLoading_("VAD", impl->cfg.vad_model);
        impl->vad->initialize(VADConfig{});
        impl->vad->set_model_path(impl->cfg.vad_model,
                                  impl->cfg.vad_model.empty()
                                      ? std::string()
                                      : impl->cfg.vad_model);
        markModelReady_("VAD", std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count());
    } catch (const std::exception& e) {
        emit logLine(QStringLiteral("VAD 加载失败：%1（回退 Mock）").arg(to_q(e.what())));
    }

    try {
        emit loadStageChanged(7, 9, kInitStages[6]);   // "加载 ASR 模型"
        auto t0 = std::chrono::steady_clock::now();
        markModelLoading_("ASR", impl->cfg.asr_model);
        impl->asr->initialize(ASRConfig{impl->cfg.asr_model});
        markModelReady_("ASR", std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count());
        emit logLine(QStringLiteral("ASR后端：%1")
                         .arg(impl->asr->uses_real_backend()
                                  ? QStringLiteral("Whisper(真实)")
                                  : QStringLiteral("Mock(占位)")));
    } catch (const std::exception& e) {
        emit logLine(QStringLiteral("ASR 加载失败：%1（回退 Mock）").arg(to_q(e.what())));
    }

    try {
        emit loadStageChanged(8, 9, kInitStages[7]);   // "加载 TTS 模型"
        TTSConfig tc = tts_config_for_dir(impl->cfg.tts_model, impl->cfg);
        if (tc.engine.empty()) tc.engine = impl->cfg.tts_engine;
        auto t0 = std::chrono::steady_clock::now();
        markModelLoading_("TTS", impl->cfg.tts_model);
        impl->tts->initialize(tc);
        markModelReady_("TTS", std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count());
        emit logLine(QStringLiteral("TTS后端：%1").arg(to_q(impl->tts->provider_label())));
    } catch (const std::exception& e) {
        emit logLine(QStringLiteral("TTS 加载失败：%1（回退 Mock）").arg(to_q(e.what())));
    }
}

int AgentController::applyModelsGuarded_(Impl* impl, const ModelPaths& paths) {
    __try {
        applyModelsWork_(impl, paths);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        emit errorLine(QStringLiteral("模型切换触发访问违规（0x%1），保持原有模型。")
                           .arg(GetExceptionCode(), 0, 16));
        return static_cast<int>(GetExceptionCode());
    }
}

void AgentController::applyModelsWork_(Impl* impl, const ModelPaths& paths) {
    const std::string defVad = impl->cfg.vad_model;
    const std::string defAsr = impl->cfg.asr_model;
    const std::string defTts = impl->cfg.tts_model;
    const std::string defLlm = impl->cfg.llm_model;

    const std::string vadPath = resolve_model(paths.vad, defVad, "models/vad");
    const std::string asrPath = resolve_model(paths.asr, defAsr, "models/asr");
    const std::string ttsPath = resolve_model(paths.tts, defTts, "models/tts");
    const std::string llmPath = resolve_model(paths.llm, defLlm, "models/llm");

    emit logLine(QStringLiteral("正在切换模型：VAD=%1  ASR=%2  TTS=%3  LLM=%4")
                     .arg(to_q(vadPath), to_q(asrPath), to_q(ttsPath), to_q(llmPath)));

    int step = 0;
    const int total = kSwitchStageCount();

    try {
        emit loadStageChanged(++step, total, kSwitchStages[step - 1]);
        auto t0 = std::chrono::steady_clock::now();
        markModelLoading_("LLM", llmPath);
        impl->llm->initialize(LLMConfig{llmPath, kLLMGpuLayers});
        markModelReady_("LLM", std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count());
    } catch (const std::exception& e) {
        emit errorLine(QStringLiteral("LLM 切换失败：%1").arg(to_q(e.what())));
    }

    try {
        emit loadStageChanged(++step, total, kSwitchStages[step - 1]);
        auto t0 = std::chrono::steady_clock::now();
        markModelLoading_("VAD", vadPath);
        impl->vad->initialize(VADConfig{});
        impl->vad->set_model_path(vadPath, vadPath);
        markModelReady_("VAD", std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count());
    } catch (const std::exception& e) {
        emit errorLine(QStringLiteral("VAD 切换失败：%1").arg(to_q(e.what())));
    }

    try {
        emit loadStageChanged(++step, total, kSwitchStages[step - 1]);
        auto t0 = std::chrono::steady_clock::now();
        markModelLoading_("ASR", asrPath);
        impl->asr->initialize(ASRConfig{asrPath});
        markModelReady_("ASR", std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count());
        emit logLine(QStringLiteral("ASR后端：%1")
                         .arg(impl->asr->uses_real_backend()
                                  ? QStringLiteral("Whisper(真实)")
                                  : QStringLiteral("Mock(占位)")));
    } catch (const std::exception& e) {
        emit errorLine(QStringLiteral("ASR 切换失败：%1").arg(to_q(e.what())));
    }

    try {
        emit loadStageChanged(++step, total, kSwitchStages[step - 1]);
        TTSConfig tc = tts_config_for_dir(ttsPath, impl->cfg);
        if (tc.engine.empty()) tc.engine = impl->cfg.tts_engine;
        auto t0 = std::chrono::steady_clock::now();
        markModelLoading_("TTS", ttsPath);
        impl->tts->initialize(tc);
        markModelReady_("TTS", std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count());
        emit logLine(QStringLiteral("TTS后端：%1").arg(to_q(impl->tts->provider_label())));
    } catch (const std::exception& e) {
        emit errorLine(QStringLiteral("TTS 切换失败：%1").arg(to_q(e.what())));
    }

    emit loadStageChanged(++step, total, kSwitchStages[step - 1]);

    // 旧 StreamingSpeaker / TTSSpeaker 持有旧 TTS 指针，必须先停后清
    if (impl->stream_speaker) { impl->stream_speaker->stop(); impl->stream_speaker.reset(); }
    if (impl->speaker) { impl->speaker->stop(); impl->speaker.reset(); }

    // 更新配置并持久化
    if (!vadPath.empty()) impl->cfg.vad_model = vadPath;
    if (!asrPath.empty()) impl->cfg.asr_model = asrPath;
    if (!ttsPath.empty()) impl->cfg.tts_model = ttsPath;
    if (!llmPath.empty()) impl->cfg.llm_model = llmPath;
    impl->activeModels = paths;
    persistConfig_();

    emit logLine("模型切换完成。");
    emit modelsChanged(paths);
    emitModelStatus_();
}

void AgentController::handleTask_(const Task& task) {
    switch (task.type) {
        case TaskType::StartVoice: handleVoiceStart_(); break;
        case TaskType::StopVoice:  handleVoiceStop_();  break;
        case TaskType::SendText:   handleText_(task.text); break;
        case TaskType::SetModels:  handleSetModels_(task.models); break;
        case TaskType::SetAudioDevice: handleSetAudioDevice_(task.text); break;
        case TaskType::PlayResponse: handlePlayResponse_(); break;
        case TaskType::RefreshLat:  handleRefreshLat_(); break;
        case TaskType::SetTts:      handleSetTts_(task.flag); break;
        case TaskType::SetTtsParams: handleSetTtsParams_(task.d0, task.d1, task.ival); break;
        case TaskType::SetVad:      handleSetVad_(task.flag); break;
        case TaskType::ListMemory: handleListMemory_(); break;
        default: break;
    }
}

void AgentController::handleVoiceStart_() {
    if (!impl_ || !impl_->orch) return;
    // 进入语音对讲前，先中断正在进行的文本链路播报（流式播报 / 手动"语音播报"按钮），
    // 保证从各方开麦都能真正停声（重置后 handleText_ 会在下一轮按需重建）。
    if (impl_->stream_speaker) {
        impl_->stream_speaker->stop();
        impl_->stream_speaker.reset();
    }
    if (impl_->speaker) {
        impl_->speaker->stop();
        impl_->speaker.reset();
    }
    if (vad_enabled_.load()) {
        // VAD 模式：启动全双工监听，由 VAD 自动切分语音段
        if (listening_) return;
        impl_->orch->start();
        listening_ = true;
        emit logLine("语音监听已启动（VAD 自动切分）。");
        emit audioStarted();
    } else {
        // ASR 接管模式：会话保持常开，每次按下对讲 = 开始录音
        if (!listening_) {
            impl_->orch->start();
            listening_ = true;
            emit logLine("VAD 已关闭：按住说话/点击按钮录音，结束即转写（ASR 直接接管）。");
            emit audioStarted();
        }
        impl_->orch->begin_capture();
    }
}

void AgentController::handleVoiceStop_() {
    if (!impl_ || !impl_->orch || !listening_) return;
    if (vad_enabled_.load()) {
        impl_->orch->stop();
        listening_ = false;
        emit logLine("语音监听已停止。");
        emit audioStopped();
    } else {
        // ASR 接管模式：松开 = 结束录音并转写；会话保持，便于连续对讲
        impl_->orch->end_capture();
        emit logLine("录音结束，正在转写…");
        emit audioStopped();
    }
}

void AgentController::handleRefreshLat_() {
    if (!impl_) return;
    QVariantMap m;
    if (impl_->vad) m[QStringLiteral("VAD 单帧推理")] = impl_->vad->last_inference_ms.load();
    if (impl_->asr) m[QStringLiteral("ASR 整段转写")] = impl_->asr->last_inference_ms.load();
    if (impl_->llm) {
        m[QStringLiteral("LLM 首token")] = impl_->llm->last_first_token_ms();
        m[QStringLiteral("LLM 总生成")] = impl_->llm->last_generate_ms();
    }
    if (impl_->tts) m[QStringLiteral("TTS 合成")] = impl_->tts->last_synthesize_ms.load();
    emit latencySnapshot(m);
}

void AgentController::handleListMemory_() {
    if (!impl_ || !impl_->memory) {
        emit memoryList(QVariantList{});
        return;
    }
    QVariantList out;
    const auto items = impl_->memory->store()->list(200, 0);
    for (const auto& m : items) {
        QVariantMap x;
        x[QStringLiteral("id")] = static_cast<qlonglong>(m.id);
        x[QStringLiteral("subject")] = to_q(m.subject);
        x[QStringLiteral("content")] = to_q(m.content);
        x[QStringLiteral("created_at")] = static_cast<qlonglong>(m.created_at);
        out.append(x);
    }
    emit memoryList(out);
}

void AgentController::emitText_(const std::string& text) {
    if (!impl_) return;
    impl_->lastResponse = text;   // 供"语音播报"按钮朗读
    emit llmComplete(to_q(text));
}

// ========== 单轮耗时统计（worker 线程） ==========

void AgentController::clearTurn_() {
    if (!impl_) return;
    impl_->turn_stages.clear();
    impl_->turn_tool_calls = 0;
}

void AgentController::onStage_(const std::string& stage, double ms) {
    if (!impl_) return;
    // 每轮每个阶段首次到达时，在右侧日志区给出中文明细，确认该阶段已实际执行
    if (impl_->turn_stages.find(stage) == impl_->turn_stages.end()) {
        emit logLine(stageLogLine(stage, ms));
    }
    impl_->turn_stages[stage] += ms;
    emit stageTiming(to_q(stage), ms);
}

void AgentController::onToolEvent_(const std::string& line) {
    if (!impl_) return;
    ++impl_->turn_tool_calls;
    emit toolCalled(to_q(line));
}

void AgentController::finishVoiceTurn_() {
    if (!impl_ || !impl_->turn_active) return;
    emitTurnTimeline_();
    impl_->turn_active = false;
}

void AgentController::emitTurnTimeline_() {
    if (!impl_ || impl_->turn_stages.empty()) return;

    // 固定展示顺序，便于阅读
    const char* const kOrder[] = {"speech", "asr", "llm_first",
                                  "llm_generate", "tools", "tts"};
    QVariantList stages;
    double total = 0.0;
    for (const char* key : kOrder) {
        auto it = impl_->turn_stages.find(key);
        if (it == impl_->turn_stages.end()) continue;
        QVariantMap s;
        s[QStringLiteral("name")] = to_q(it->first);
        s[QStringLiteral("ms")] = it->second;
        stages.append(s);
        total += it->second;
    }

    QVariantMap m;
    m[QStringLiteral("index")] = ++impl_->turn_index;
    m[QStringLiteral("mode")] = to_q(impl_->turn_mode);
    m[QStringLiteral("user")] = to_q(impl_->turn_user);
    m[QStringLiteral("tools")] = impl_->turn_tool_calls;
    m[QStringLiteral("total_ms")] = total;
    m[QStringLiteral("stages")] = stages;
    emit turnTimeline(m);

    impl_->turn_stages.clear();
    impl_->turn_tool_calls = 0;
}

void AgentController::seedModelInfo_() {
    if (!impl_) return;
    for (const char* k : {"LLM", "VAD", "ASR", "TTS"}) {
        impl_->models[std::string(k)] = Impl::ModelInfo{};
    }
    emitModelStatus_();   // 初始即上报"未加载"状态
}

void AgentController::markModelLoading_(const std::string& key,
                                        const std::string& path) {
    if (!impl_) return;
    auto& info = impl_->models[key];
    info.state = "loading";
    info.path = path;
    emitModelStatus_();
}

void AgentController::markModelReady_(const std::string& key, double load_ms) {
    if (!impl_) return;
    auto& info = impl_->models[key];
    info.state = "ready";
    info.load_ms = load_ms;
    emitModelStatus_();
}

void AgentController::emitModelStatus_() {
    if (!impl_) return;

    // 各模型的实际后端/文件名（按当前模块状态动态解析）
    auto backend_of = [&](const std::string& key) -> std::pair<bool, std::string> {
        if (key == "LLM")
            return {impl_->llm && impl_->llm->real_backend(),
                    std::string("llama.cpp")};
        if (key == "VAD")
            return {!impl_->cfg.vad_model.empty() && impl_->vad != nullptr,
                    std::string("Silero")};
        if (key == "ASR")
            return {impl_->asr && impl_->asr->uses_real_backend(),
                    impl_->asr->backend_name()};
        if (impl_->tts) {
            const std::string eng = impl_->tts->engine_name();
            return {impl_->tts->uses_real_backend(),
                    eng == "simple"   ? std::string("系统语音(SAPI)")
                    : eng == "piper"  ? std::string("Piper")
                    : eng == "kokoro" ? std::string("Kokoro")
                                      : eng};
        }
        return {false, "Mock"};
    };

    // 生效推理后端（Vulkan GPU / DirectML GPU / CPU / Mock）
    auto provider_of = [&](const std::string& key) -> std::string {
        if (key == "LLM")
            return impl_->llm ? impl_->llm->provider_label() : std::string("Mock");
        if (key == "VAD")
            return impl_->vad ? impl_->vad->provider_label() : std::string("Mock");
        if (key == "ASR")
            return impl_->asr ? impl_->asr->provider_label() : std::string("Mock");
        return impl_->tts ? impl_->tts->provider_label() : std::string("Mock");
    };

    QVariantMap m;
    for (const char* ck : {"LLM", "VAD", "ASR", "TTS"}) {
        const std::string key(ck);
        const auto it = impl_->models.find(key);
        if (it == impl_->models.end()) continue;
        const auto& info = it->second;

        auto [real, backendName] = backend_of(key);
        const std::uintmax_t bytes = path_total_bytes(info.path);
        QFileInfo fi(QString::fromStdString(info.path));

        QVariantMap x;
        x[QStringLiteral("state")] = to_q(info.state);
        x[QStringLiteral("path")] = to_q(info.path);
        x[QStringLiteral("file")] = fi.fileName();
        x[QStringLiteral("disp_name")] = chipModelName(fi.fileName().toStdString());
        x[QStringLiteral("size_bytes")] = static_cast<qint64>(bytes);
        x[QStringLiteral("backend")] = real
            ? to_q(backendName) : QStringLiteral("Mock占位");
        x[QStringLiteral("provider")] = to_q(provider_of(key));
        x[QStringLiteral("load_ms")] = info.load_ms;
        m[QString::fromUtf8(ck)] = x;
    }
    emit modelStatus(m);
}

void AgentController::handlePlayResponse_() {
    if (!impl_ || !impl_->tts) return;
    if (!impl_->tts_enabled) {
        emit logLine(QStringLiteral("语音播报已关闭，请先在右侧打开 TTS 开关。"));
        return;
    }
    if (impl_->lastResponse.empty()) {
        emit logLine(QStringLiteral("还没有可朗读的回复，请先进行对话。"));
        return;
    }
    if (!impl_->speaker) impl_->speaker = std::make_shared<TTSSpeaker>(impl_->tts.get());
    emit voicePlaying(true);
    emit logLine(QStringLiteral("正在朗读最近回复…"));
    bool ok = impl_->speaker->play(impl_->lastResponse);
    emit voicePlaying(false);
    emit logLine(ok ? QStringLiteral("朗读完成。") : QStringLiteral("朗读失败或已中断。"));
}

void AgentController::handleSetTts_(bool enabled) {
    if (!impl_) return;
    impl_->tts_enabled = enabled;
    if (impl_->orch) impl_->orch->set_tts_enabled(enabled);
    if (!enabled && impl_->stream_speaker) {
        impl_->stream_speaker->stop();   // 关闭播报：立即中断正在进行的流式合成/播放
        impl_->stream_speaker.reset();
    }
    emit ttsEnabledChanged(enabled);
    emit logLine(enabled ? QStringLiteral("语音播报已开启。")
                         : QStringLiteral("语音播报已关闭（文字回复正常）。"));
}

void AgentController::handleSetTtsParams_(double speed, double pitch, int speakerId) {
    if (!impl_) return;
    if (speed < 0.25) speed = 0.25;
    if (speed > 2.0) speed = 2.0;
    if (speakerId < 0) speakerId = 0;

    impl_->cfg.tts_speed = static_cast<float>(speed);
    impl_->cfg.tts_pitch = static_cast<float>(pitch);
    impl_->cfg.tts_speaker_id = speakerId;

    // 热更 TTS 实例（对后续合成立即生效；Kokoro 发音人 + 语速，SAPI 语速）
    if (impl_->tts) {
        impl_->tts->set_speed(static_cast<float>(speed));
        impl_->tts->set_speaker_id(speakerId);
    }

    persistConfig_();
    emit logLine(QStringLiteral("TTS 语音参数已更新：语速 %1 ×，发音人 #%2。")
                     .arg(speed)
                     .arg(speakerId));
}

void AgentController::handleSetVad_(bool enabled) {
    vad_enabled_.store(enabled);
    if (impl_ && impl_->orch) impl_->orch->set_vad_enabled(enabled);
    emit vadEnabledChanged(enabled);
    emit logLine(enabled ? QStringLiteral("VAD 端点检测已开启（自动切分语音段）。")
                         : QStringLiteral("VAD 已关闭：改为按住说话/点击按钮录音，ASR 直接接管。"));
}

void AgentController::persistConfig_() {
    if (!impl_) return;
    try {
        const char* path = "configs/agent.yaml";
        std::ifstream in(path);
        std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        // 仅当文件存在时改写模型路径条目；TTS 语音参数缺失则追加
        bool changed = false;
        auto set_scalar = [&](const std::string& key, const std::string& val) {
            const std::string pat = key + ":";
            auto pos = text.find(pat);
            if (pos == std::string::npos) {
                text += "\n" + pat + " " + val;   // 缺失则该追加一行
                changed = true;
                return;
            }
            auto lineEnd = text.find('\n', pos);
            if (lineEnd == std::string::npos) lineEnd = text.size();
            std::string line = text.substr(pos, lineEnd - pos);
            const std::string newLine = pat + " " + val;
            if (line != newLine) {
                text.replace(pos, lineEnd - pos, newLine);
                changed = true;
            }
        };
        std::map<std::string, std::string> keys = {
            {"vad_model", impl_->cfg.vad_model},
            {"asr_model", impl_->cfg.asr_model},
            {"tts_model", impl_->cfg.tts_model},
            {"llm_model", impl_->cfg.llm_model},
            {"tts_engine", impl_->cfg.tts_engine},
        };
        for (auto& [key, val] : keys) set_scalar(key, val);
        set_scalar("tts_speed", std::to_string(impl_->cfg.tts_speed));
        set_scalar("tts_pitch", std::to_string(impl_->cfg.tts_pitch));
        set_scalar("tts_speaker_id", std::to_string(impl_->cfg.tts_speaker_id));
        if (changed) {
            std::ofstream out(path, std::ios::trunc);
            out << text;
            emit logLine("已保存配置到 configs/agent.yaml");
        }
    } catch (const std::exception& e) {
        emit logLine(QStringLiteral("保存配置失败：%1").arg(to_q(e.what())));
    }
}

void AgentController::handleSetModels_(const ModelPaths& paths) {
    if (!impl_) return;
    if (listening_) {
        impl_->orch->stop();
        listening_ = false;
        emit audioStopped();
    }
    // applyModelsWork_ 在 impl_ 上重建模型（in-place）；旧模型指针在 move 前始终有效。
    // SEH 拦截切换期间的访问违规，保证 GUI 进程不崩溃，旧模型继续服务。
    applyModelsGuarded_(impl_.get(), paths);
    emit loadFinished(true);   // 旧模型仍可用，界面恢复可操作
}

std::shared_ptr<AudioPipeline> AgentController::createAudioPipeline_() {
    auto audio = std::make_shared<AudioPipeline>();
    AudioConfig ac{};
    ac.sample_rate = impl_->cfg.audio_sample_rate;
    ac.channels = impl_->cfg.audio_channels;
    ac.frames_per_buffer =
        std::max(1, impl_->cfg.audio_sample_rate * impl_->cfg.audio_buffer_ms / 1000);
    ac.input_device = impl_->audio_device_name;
    // 输出设备按 TTS 采样率（24kHz）独立配采样率，否则 48kHz 设备播放 24kHz
    // TTS 音频会变速（2 倍速）；须在 initialize() 之前设置。
    if (impl_->tts) audio->set_output_rate(impl_->tts->config().sample_rate);
    if (!audio->initialize(ac)) {
        emit errorLine("麦克风/扬声器初始化失败，语音输入将不可用（可先检查系统录音权限）。");
    } else {
        emit logLine(QStringLiteral("音频管道就绪：%1 Hz · %2 ch · 输入设备「%3」")
                         .arg(ac.sample_rate)
                         .arg(ac.channels)
                         .arg(impl_->audio_device_name.empty()
                                  ? QStringLiteral("系统默认")
                                  : to_q(impl_->audio_device_name)));
    }
    return audio;
}

std::shared_ptr<Orchestrator> AgentController::buildOrchestrator_(
    std::shared_ptr<AudioPipeline> audio) {
    auto orch = std::make_shared<Orchestrator>(Orchestrator::Config{});
    orch->initialize(std::move(audio), impl_->vad, impl_->asr, impl_->llm, impl_->tts);
    orch->attach_agent(impl_->registry, impl_->router, kSystemPrompt);
    orch->attach_memory(impl_->memory);
    orch->set_tts_enabled(impl_->tts_enabled);
    orch->set_vad_enabled(vad_enabled_.load());
    orch->set_text_callback([this](const std::string& text) {
        emitText_(text);
    });
    // 流式增量 token → 实时回复面板（完整回答由 text_callback 每轮一次上报）
    orch->set_token_callback([this](const std::string& text) {
        emit llmToken(to_q(text));
    });
    // ASR 转写完成的整句文本 → 回显到对话区（fromVoice=true）
    orch->set_user_text_callback([this](const std::string& text) {
        if (!text.empty()) emit userMessage(to_q(text), true);
    });
    orch->set_trace_callback([this](const std::string& stage, double ms) {
        onStage_(stage, ms);
    });
    orch->set_tool_callback([this](const std::string& line) {
        onToolEvent_(line);
    });
    return orch;
}

void AgentController::handleSetAudioDevice_(const QString& deviceName) {
    if (!impl_) return;
    const std::string name = deviceName.toStdString();
    if (name == impl_->audio_device_name) {
        emit logLine(QStringLiteral("输入设备未变化（仍为「%1」）。")
                         .arg(name.empty() ? QStringLiteral("系统默认") : deviceName));
        return;
    }

    // 停掉正在进行的语音（旧管道与旧状态机将被替换）
    if (listening_) {
        impl_->orch->stop();
        listening_ = false;
        emit audioStopped();
    }

    impl_->audio_device_name = name;
    impl_->audio = createAudioPipeline_();
    impl_->orch = buildOrchestrator_(impl_->audio);

    emit logLine(QStringLiteral("麦克风已切换：%1")
                     .arg(name.empty() ? QStringLiteral("系统默认") : deviceName));
}

void AgentController::handleText_(const QString& text) {
    if (!impl_) return;
    const std::string t = text.trimmed().toStdString();
    if (t.empty()) return;

    // /memory 命令：直接执行，不走 LLM
    if (impl_->memory && is_memory_command(t)) {
        std::string reply = impl_->memory->run_command(t);
        emit userMessage(text, false);
        emit memoryEvent(to_q(reply));
        emit llmComplete(to_q(reply));
        return;
    }

    emit userMessage(text, false);
    impl_->turn_active = true;
    impl_->turn_mode = "text";
    impl_->turn_user = t;
    clearTurn_();

    // 召回相关记忆，注入 system prompt
    std::string sys = impl_->base_prompt;
    auto recalled = impl_->memory ? impl_->memory->recall(t, 3)
                                  : std::vector<MemoryItem>{};
    if (!recalled.empty()) {
        sys += "\n\n[相关记忆]";
        for (auto& m : recalled) sys += "\n- " + m.content;
    }

    // 单轮 Agent 循环（生成→工具执行→回填→再生成）
    ToolExecutor executor(*impl_->registry);
    AgentLoop loop(*impl_->llm, *impl_->registry, executor);
    loop.set_system_prompt(sys);
    // 工具意图门控：与语音路径一致，仅当用户明确要求（搜索/记忆/时间）时才放行
    loop.set_enabled_tools(detect_tool_intent(t));

    // 文本链路流式播报：LLM 边生成，边按句喂给异步合成线程立即播放，
    // 不再等整段合成完才开口（大幅压缩作答延迟）。
    if (impl_->tts_enabled && impl_->tts && !impl_->stream_speaker) {
        impl_->stream_speaker = std::make_shared<StreamingSpeaker>(impl_->tts.get());
    }

    loop.set_token_sink([this](const std::string& tok) {
        emit llmToken(to_q(tok));
        if (impl_->stream_speaker) {
            impl_->stream_speaker->push_text(tok);
        }
    });
    loop.set_trace([this](const std::string& s, double ms) { onStage_(s, ms); });
    loop.set_tool_report([this](const std::string& l) { onToolEvent_(l); });

    auto res = loop.run(t, nullptr);
    // 生成结束：收尾喂入残留文本并等待播完
    if (impl_->stream_speaker) impl_->stream_speaker->flush();

    emitTurnTimeline_();
    impl_->turn_active = false;
    emitText_(res.final_text);

    // 等待流式播报把已合成的句子播完（异步合成线程在后台推进，不阻塞 GUI）
    if (impl_->stream_speaker) impl_->stream_speaker->wait_done();

    // 记忆写入现在只发生在"明确指示"时：
    //   - 用户主动执行 /memory save
    //   - 模型在用户要求"记住…"时调用 memory_save 工具
    // 不再每轮对话后自动 ingest，避免高频、无意地污染长期记忆。
}

}  // namespace voice_agent::gui
}  // namespace voice_agent