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

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QSplitter;
class QStackedWidget;
class QTabWidget;
class QTableWidget;
class QFrame;
class QTimer;
class QToolButton;
class QVBoxLayout;
class QWidget;

namespace voice_agent {
namespace gui {

class SettingsPanel;
class MemoryPanel;

// ========== 主窗口 ==========
// ChatGPT 式三栏布局：
//   左栏（对话/共享记忆侧栏）：新建对话、对话历史列表、共享记忆分组
//   中栏（主对话区）：顶栏（侧栏开关 + 会话标题 + 状态胶囊）+ 气泡消息流 +
//                    底部圆角输入卡（语音/朗读/播报/VAD/发送），默认页
//   右栏（可折叠流程侧栏）：模型状态 + 处理流程条 + 日志/耗时标签页
// 流程、日志等诊断信息全部收进右栏，中栏只保留对话本身。
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

    // 全局事件过滤器：用于"按住空格说话"（push-to-talk）
    bool eventFilter(QObject* watched, QEvent* event) override;

protected:
    // 窗口失焦 / 最小化 / 关闭时兜底释放 PTT。
    // 按住空格后按 Alt+Tab、锁屏或切窗口，key-up 收不到 —— pttHolding_
    // 会卡在 true，麦克风一直占着不放。
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

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
    // 侧栏导航 / 对话 / 记忆
    void onNewConversation_();
    void onConversationClicked_(QListWidgetItem* item);
    void onToggleLeft_();
    void onToggleRight_();
    void onSettingsNav_();
    void onMemoryList_(const QVariantList& items);
    void onMemoryActivated_(QListWidgetItem* item);

    // 新面板
    void onModelStatus(const QVariantMap& models);
    void onStageTiming(const QString& stage, double ms);
    void onTurnTimeline(const QVariantMap& timeline);
    void onStateChanged_(const QString& state);

private:
    void buildUi_();
    void buildLeftSidebar_();
    void buildChatPage_();
    void buildRightSidebar_();
    void connectSignals_();
    void setControlsEnabled_(bool enabled);
    void resetLive_();
    void refreshTimelineTable_();
    QString stagePlainStyle_() const;
    QString stageDisplayName_(const QString& key) const;
    void refreshConversationBadges_();  // 高亮当前激活对话
    void refreshMemoryList_();          // 触发控制器读取记忆 → onMemoryList_
    // 在对话流中追加一条消息并按需贴底滚动
    void appendChatBubble_(const QString& text, bool isUser);
    // 新建一行"助手"消息（头像 + 正文），返回正文标签（流式更新用）
    QLabel* createAssistantRow_();
    // 新建一行"用户"消息（右侧灰气泡）
    void createUserRow_(const QString& text);
    // 把消息行插入到流末尾（占位的 stretch/空态之前），并按需贴底
    void insertChatRow_(QWidget* row);
    void scrollChatToBottom_();
    bool stickToBottom_ = true;   // 滚动跟随：用户手动滚到历史中间时暂停贴底
    // 按 VAD 开关刷新状态栏提示文案
    void updateVadHint_();

    // 幂等释放 PTT：窗口失焦/关闭时 key-up 收不到，必须在这里补一次 stop。
    // 无脑可重复调用 —— pttHolding_ 为 false 时直接返回。
    void releasePtt_();
    // PTT 按住说话的界面提示：对话区遮罩（脉冲红点）+ 输入卡录音态红边。
    // showPttOverlay_ 在按下空格的一瞬间调用，不等工作线程确认。
    void showPttOverlay_();
    void hidePttOverlay_();
    void setComposerRecording_(bool on);
    // 把状态机英文状态翻成用户能看懂的中文，并按当前上下文补充说明
    // （例如"正在听你说（已打断当前回答）"）。
    QString statusTextFor_(const QString& state) const;

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
    // 本轮回答是否被用户打断过（用于状态栏文案："已打断当前回答"）
    bool turnWasInterrupted_ = false;
    // 空格按键是否被按下过（用于失焦时判断要不要补 stop）
    bool pttKeyDownSeen_ = false;

    // PTT 按住提示：对话区遮罩 + 脉冲红点；输入卡容器（录音态红边）
    QFrame* pttOverlay_ = nullptr;
    QLabel* pttDot_ = nullptr;
    QTimer* pttPulseTimer_ = nullptr;
    bool pttPulseOn_ = false;
    QFrame* composerFrame_ = nullptr;

    AgentController* controller_ = nullptr;
    SettingsPanel* settings_ = nullptr;

    // ===== 三栏布局 =====
    QWidget* leftSidebar_ = nullptr;    // 左栏：对话 / 共享记忆
    QWidget* rightSidebar_ = nullptr;   // 右栏：模型/流程/日志（可折叠）
    QSplitter* centerRightSplit_ = nullptr;
    QStackedWidget* centerStack_ = nullptr;   // 0=对话页 1=设置页
    QWidget* chatPage_ = nullptr;
    QToolButton* leftToggleBtn_ = nullptr;    // 左栏折叠开关（对话页顶栏）
    QToolButton* rightToggleBtn_ = nullptr;   // 右栏折叠开关（对话页顶栏）
    QPushButton* newConvBtn_ = nullptr;
    QPushButton* settingsNavBtn_ = nullptr;
    QListWidget* convList_ = nullptr;
    QListWidget* memList_ = nullptr;
    QLabel* pageTitle_ = nullptr;
    int convCounter_ = 0;
    QListWidgetItem* activeConvItem_ = nullptr;
    int activeConvIndex_ = -1;

    QLabel* statusLabel_ = nullptr;   // 顶栏状态胶囊（待机/监听/思考/播放）
    QPushButton* voiceBtn_ = nullptr;      // 🎙 语音按钮（可勾选 = 录音中）
    QPushButton* speakBtn_ = nullptr;      // 🔊 朗读最近回复
    QToolButton* ttsToggle_ = nullptr;     // 语音播报开关（输入卡内）
    QToolButton* vadToggle_ = nullptr;     // VAD 端点检测开关（输入卡内）
    QLineEdit* input_ = nullptr;
    QPushButton* sendBtn_ = nullptr;

    // ===== 对话流（滚动区 + 气泡行）=====
    QScrollArea* chatScroll_ = nullptr;
    QWidget* chatHost_ = nullptr;
    QVBoxLayout* chatLayout_ = nullptr;   // 消息行依次插入，末尾为空态+stretch
    QLabel* emptyState_ = nullptr;        // 无消息时的欢迎占位
    QLabel* streamingLabel_ = nullptr;    // 正在流式生成的助手正文
    QPlainTextEdit* toolsView_ = nullptr; // 工具 / 记忆 / 日志（右栏标签页）

    // 模型状态条（索引对应 VAD/ASR/TTS/LLM）
    QLabel* modelChips_[4] = {nullptr, nullptr, nullptr, nullptr};

    // 流程阶段点（右栏"处理流程"区，2 行网格）
    enum { kStageCount = 7 };
    QLabel* stageDots_[kStageCount] = {};
    int currentStageIdx_ = -1;

    // 轮次时间轴（右栏"耗时"标签页）
    QTabWidget* rightTabs_ = nullptr;
    QTableWidget* timelineTable_ = nullptr;
    QLabel* timelineSummary_ = nullptr;
    QLabel* timelineHist_ = nullptr;   // 最近几轮汇总
    MemoryPanel* memPanel_ = nullptr;   // 右栏"内存"页
    QTimer* memTimer_ = nullptr;        // 内存面板刷新节拍（2s）
    QHash<QString, double> liveStages_;
    int liveTools_ = 0;
    bool liveActive_ = false;
    int lastTurnIndex_ = 0;
    QStringList historyLines_;

    // 后台初始化进度显示（对话流上方的圆角横幅）
    QFrame* initBox_ = nullptr;
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
    int nextTokenLogLen_ = 256;  // 流式进度日志的下一档长度（每 256 字符打一行）
};

}  // namespace voice_agent::gui
}  // namespace voice_agent