// src/gui/agent_controller.hpp
#pragma once
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "gui/model_catalog.hpp"
#include "audio/audio_pipeline.hpp"

namespace voice_agent {
class Orchestrator;   // 前置声明（buildOrchestrator_ 返回 shared_ptr 所需）

namespace gui {

// ========== 业务控制器 ==========
// 在独立工作线程中装配并驱动核心模块（Orchestrator / AgentLoop / Memory / 搜索），
// 通过跨线程 Qt 信号把结果安全地投递到 GUI 线程。
//
// 职责：
//   - 文本对话：路由到 AgentLoop（LLM 产物走工具循环），支持 /memory 命令
//   - 语音控制：启动/停止 Orchestrator 全双工状态机（当前模型为 mock，框架就绪）
//   - 状态/ASR/工具/记忆等实时信息回传给界面
class AgentController final : public QObject {
    Q_OBJECT

public:
    explicit AgentController(QObject* parent = nullptr);
    ~AgentController() override;

    // 启动工作线程并装配核心（调用后即可接收 startVoice/stopVoice/sendText）
    void start();

    // 语音控制
    void startVoice();
    void stopVoice();

    // 发送一条用户文本（异步，立即返回）
    void sendText(const QString& text);

    // 朗读最近一次回复（TTS 合成并经音频输出播放，异步返回）
    void playResponse();

    // 采集各模型最近一次推理耗时并发射 latencySnapshot（调试面板轮询用）
    void refreshLatencies();

    // 切换模型：在 worker 线程内重建模型模块并重新挂载 Orchestrator。
    // paths 中空串表示沿用当前（默认）配置。
    void setModels(const ModelPaths& paths);

    // 开关语音播报（TTS 合成/播放），关闭仅停声、文字照常
    void setTtsEnabled(bool enabled);

    // 开关 VAD 端点检测：开启（默认）= VAD 自动切分；关闭 = ASR 直接接管，
    // 语音轮改为"按住说话/点击按钮录音，结束即转写"
    void setVadEnabled(bool enabled);

    // 调节 TTS 语音参数：语速倍率 speed(0.25~2.0)、发音人 id(Kokoro)。
    // 对后续合成即时生效，并持久化到 agent.yaml。async 在线程内处理。
    void setTtsParams(double speed, double pitch, int speakerId);

    // 当前 VAD 开关状态
    bool vadEnabled() const { return vad_enabled_.load(); }

    // 切换麦克风输入设备（空串 = 系统默认设备），异步在 worker 线程重建音频管道
    void setAudioDevice(const QString& deviceName);

    // 请求异步读取记忆库最近条目（供"共享记忆"侧栏展示）。结果经 memoryList 信号回传。
    void requestMemoryList();

    // 当前麦克风输入电平（dBFS，供调试面板轮询；未采集时返回 -96）
    float inputLevelDb() const;

    // 当前是否处于"监听中"状态
    bool isListening() const { return listening_.load(); }

signals:
    void stateChanged(const QString& state);   // 状态机当前状态
    void audioStarted();                        // 语音监听已开始
    void audioStopped();                        // 语音监听已停止
    void userMessage(const QString& text, bool fromVoice); // 用户一条消息（回显到对话区，fromVoice=语音转写）
    void llmToken(const QString& token);        // LLM 流式增量 token
    void llmComplete(const QString& text);      // 一段回答完成
    void toolCalled(const QString& info);       // 工具调用日志
    void memoryEvent(const QString& info);      // 记忆事件
    void logLine(const QString& line);          // 普通日志
    void voicePlaying(bool playing);            // 正在用 TTS 播报回复
    void errorLine(const QString& line);        // 错误提示
    void modelsChanged(const ModelPaths& paths); // 模型切换已应用
    void latencySnapshot(const QVariantMap& stages); // 各模型推理延时快照
    void loadStageChanged(int step, int total, const QString& label); // 后台初始化进度
    void loadFinished(bool ok);                 // 初始化结束（ok=false 表示失败但进程保持运行）
    void modelStatus(const QVariantMap& models); // 四类模型当前文件/大小/后端/就绪
    void stageTiming(const QString& stage, double ms); // 单轮某阶段实时耗时（毫秒）
    void turnTimeline(const QVariantMap& timeline);     // 一轮完成：各阶段耗时 + 汇总
    void memoryList(const QVariantList& items);         // 记忆库最近条目（供"共享记忆"侧栏）
    void ttsEnabledChanged(bool enabled);       // TTS 开关状态变化
    void vadEnabledChanged(bool enabled);       // VAD 开关状态变化

private:
    enum class TaskType { StartVoice, StopVoice, SendText, SetModels, PlayResponse, RefreshLat, SetTts, SetTtsParams, SetVad, SetAudioDevice, ListMemory, Quit };
    struct Task {
        TaskType type{TaskType::Quit};
        QString text;
        ModelPaths models;
        bool flag{true};
        double d0{1.0};   // 语速倍率（SetTtsParams）
        double d1{1.0};   // 音调倍率（保留）
        int ival{45};     // 发音人 ID（SetTtsParams）
    };

    void threadMain_();
    void initCore_();
    void handleTask_(const Task& task);
    void handleVoiceStart_();
    void handleVoiceStop_();
    void handleText_(const QString& text);
    void handleSetModels_(const ModelPaths& paths);
    void handleSetAudioDevice_(const QString& deviceName);
    void handlePlayResponse_();
    void handleRefreshLat_();
    void handleSetTts_(bool enabled);
    void handleSetTtsParams_(double speed, double pitch, int speakerId);
    void handleSetVad_(bool enabled);
    void handleListMemory_();
    void emitText_(const std::string& text);   // llmComplete + 记录最近回答
    void persistConfig_();

    // 音频管道：按当前配置（含设备名）创建并初始化；失败时回退默认设备
    std::shared_ptr<AudioPipeline> createAudioPipeline_();
    // 用给定模型/记忆/回调重新装配 Orchestrator（模型切换或音频设备切换后调用）
    std::shared_ptr<Orchestrator> buildOrchestrator_(std::shared_ptr<AudioPipeline> audio);

    // 单轮确认计时（全部在 worker 线程调用）
    void clearTurn_();
    void onStage_(const std::string& stage, double ms);
    void onToolEvent_(const std::string& line);
    void finishVoiceTurn_();                     // 语音轮次结束（Speaking 触发）
    void emitTurnTimeline_();                    // 汇总当前轮各阶段并发射 turnTimeline

    // 模型状态机：标记某模型进入"加载中"/"就绪"，驱动 modelStatus 信号
    void seedModelInfo_();
    void markModelLoading_(const std::string& key, const std::string& path);
    void markModelReady_(const std::string& key, double load_ms);
    void emitModelStatus_();

    void emitState_(int state_int);

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> listening_{false};
    std::atomic<bool> vad_enabled_{true};   // VAD 端点检测开关（默认开启）
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Task> queue_;

    // 核心对象（pimpl：在 worker 线程创建/使用）
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace voice_agent::gui
}  // namespace voice_agent