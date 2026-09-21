// src/gui/main_window.hpp
#pragma once
#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include "gui/agent_controller.hpp"

class QCheckBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QTextBrowser;
class QTimer;
class QWidget;

namespace voice_agent {
namespace gui {

class SettingsPanel;

// ========== 主窗口 ==========
// Tab0：对话（状态式布局）；Tab1：设置（模型选择/下载/切换）
// 对话页自上而下：
//   模型状态条（四类模型 文件/大小/后端/就绪）
//   语音控制行 + 流程阶段条（按住空格说 → VAD → ASR → LLM/工具 → TTS → 播放）
//   中部分栏：左=对话，右=实时回复 + 工具/记忆
//   底部：轮次耗时时间轴
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

    // 全局事件过滤器：用于"按住空格说话"（push-to-talk）
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onSendClicked();
    void onVoiceToggled(bool checked);
    void onSpeakClicked();
    void onTtsToggled(bool checked);
    void onVadToggled(bool checked);
    void onUserMessage_(const QString& text, bool fromVoice);
    void appendToken_(const QString& token);
    void appendAssistantFinal_(const QString& finalText);
    void onLogLine_(const QString& line);
    void onModelsApplied_(const ModelPaths& paths);
    void onLoadStage_(int step, int total, const QString& label);
    void onLoadFinished_(bool ok);
    void updateLoadLabel_();

    // 新面板
    void onModelStatus(const QVariantMap& models);
    void onStageTiming(const QString& stage, double ms);
    void onTurnTimeline(const QVariantMap& timeline);
    void onStateChanged_(const QString& state);

private:
    void buildUi_();
    void connectSignals_();
    void setControlsEnabled_(bool enabled);
    void resetLive_();
    void refreshTimelineTable_();
    QString stagePlainStyle_() const;
    QString stageDisplayName_(const QString& key) const;
    // 在对话区追加一条消息；仅在"贴底"模式下自动滚动到最新
    void appendChatBubble_(const QString& who, const QString& text,
                           const char* color, const char* icon);
    bool stickToBottom_ = true;   // 滚动跟随：用户手动滚到历史中间时暂停贴底
    // 按 VAD 开关刷新状态栏提示文案
    void updateVadHint_();

    // ===== 流程阶段条（支持跳过式展示）=====
    enum StageState {
        kStagePending = 0,   // 未开始（灰）
        kStageActive,        // 进行中（黄）
        kStageDone,          // 已完成（绿）
        kStageSkipped,       // 跳过（虚线灰）
    };
    void setPipelineStep_(int idx);                 // 线性推进：<idx 完成、idx 激活（不覆盖已跳过）
    void setPipelineState_(int idx, int state);     // 绝对设置某阶段状态
    void markStageSkipped_(int idx);                // 标记某阶段跳过
    void resetPipeline_();                          // 全部回到未开始
    void finalizePipelineFromStages_(const QVariantList& stages); // 轮次结束：未出现阶段一律标记跳过
    QVector<int> stageStates_;

    // 空格按下的按键记录，用于区分"按住"与"正常敲击空格"
    bool pttHolding_ = false;

    AgentController* controller_ = nullptr;
    SettingsPanel* settings_ = nullptr;
    QTabWidget* tabs_ = nullptr;

    QLabel* statusLabel_ = nullptr;
    QPushButton* voiceBtn_ = nullptr;
    QPushButton* speakBtn_ = nullptr;
    QCheckBox* ttsToggle_ = nullptr;
    QCheckBox* vadToggle_ = nullptr;
    QLineEdit* input_ = nullptr;
    QPushButton* sendBtn_ = nullptr;
    QTextBrowser* chat_ = nullptr;
    QPlainTextEdit* replyView_ = nullptr;   // 实时回复（流式，与工具调用分开）
    QPlainTextEdit* toolsView_ = nullptr;   // 工具 / 记忆 / 日志

    // 模型状态条（索引对应 VAD/ASR/TTS/LLM）
    QLabel* modelChips_[4] = {nullptr, nullptr, nullptr, nullptr};

    // 流程阶段点
    enum { kStageCount = 7 };
    QLabel* stageDots_[kStageCount] = {};
    int currentStageIdx_ = -1;

    // 轮次时间轴
    QTableWidget* timelineTable_ = nullptr;
    QLabel* timelineSummary_ = nullptr;
    QLabel* timelineHist_ = nullptr;   // 最近几轮汇总
    QHash<QString, double> liveStages_;
    int liveTools_ = 0;
    bool liveActive_ = false;
    int lastTurnIndex_ = 0;
    QStringList historyLines_;

    // 后台初始化进度显示
    QGroupBox* initBox_ = nullptr;
    QProgressBar* loadProgress_ = nullptr;
    QLabel* loadLabel_ = nullptr;
    QTimer* loadTimer_ = nullptr;
    QTimer* levelTimer_ = nullptr;   // 麦克风输入电平轮询
    QElapsedTimer loadElapsed_;
    QString loadStageName_;
    int loadStep_ = 0;
    int loadTotal_ = 0;
    bool loading_ = false;

    QString pendingAssistant_;   // 流式累积中的回答
};

}  // namespace voice_agent::gui
}  // namespace voice_agent