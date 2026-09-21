// src/gui/main_window.cpp
#include "gui/main_window.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTimer>
#include <QVariantList>
#include <QVBoxLayout>
#include <QWidget>

#include "gui/settings_panel.hpp"
#include "gui/latency_panel.hpp"

namespace voice_agent {
namespace gui {

namespace {

QString escHtml(QString s) {
    return s.toHtmlEscaped();
}

// 流程阶段点标签
const char* const kStageLabels[] = {
    "采集", "VAD", "ASR", "LLM", "工具", "TTS", "播放",
};

// 时间轴阶段展示名
QString stageNameCn(const QString& key) {
    if (key == QLatin1String("speech")) return QStringLiteral("用户语音");
    if (key == QLatin1String("asr")) return QStringLiteral("ASR 转写");
    if (key == QLatin1String("llm_first")) return QStringLiteral("LLM 首token");
    if (key == QLatin1String("llm_generate")) return QStringLiteral("LLM 生成");
    if (key == QLatin1String("tools")) return QStringLiteral("工具执行");
    if (key == QLatin1String("tts")) return QStringLiteral("TTS 合成");
    return key;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Voice Agent — 本地全双工语音助手"));
    resize(1180, 760);

    buildUi_();

    controller_ = new AgentController(this);
    connectSignals_();
    controller_->start();

    // 初始即处于加载态：禁用交互，等待首个进度信号
    setControlsEnabled_(false);
    loading_ = true;
    loadTimer_ = new QTimer(this);
    loadTimer_->setInterval(1000);
    connect(loadTimer_, &QTimer::timeout, this, &MainWindow::updateLoadLabel_);
    loadElapsed_.start();
    loadTimer_->start();

    // 全局监听空格，实现"按住说话"
    qApp->installEventFilter(this);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        auto* ke = static_cast<QKeyEvent*>(event);
        // 空格 + 无修饰键 → 对讲机式按说；屏蔽 IME 组合期间的按键
        if (ke->key() == Qt::Key_Space && ke->modifiers() == Qt::NoModifier) {
            if (event->type() == QEvent::KeyPress) {
                // 忽略按住时的自动重复事件
                if (ke->isAutoRepeat()) return true;
                if (!loading_ && !pttHolding_ && controller_) {
                    if (controller_->vadEnabled() && controller_->isListening()) {
                        // VAD 模式：监听中按空格不触发（保持原行为）
                        return false;
                    }
                    pttHolding_ = true;
                    if (controller_->vadEnabled()) {
                        voiceBtn_->setChecked(true);   // 触发 onVoiceToggled(true) → startVoice
                    } else {
                        controller_->startVoice();     // ASR 接管模式：按下 = 开始录音
                    }
                }
                return true;   // 消费掉空格，避免在输入框打出空格
            } else if (event->type() == QEvent::KeyRelease) {
                if (ke->isAutoRepeat()) return true;
                if (pttHolding_) {
                    pttHolding_ = false;
                    if (controller_->vadEnabled()) {
                        voiceBtn_->setChecked(false);  // 触发 onVoiceToggled(false) → stopVoice
                    } else {
                        controller_->stopVoice();      // ASR 接管模式：松开 = 结束录音并转写
                    }
                }
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::buildUi_() {
    auto* dialogTab = new QWidget(this);
    auto* dlg = new QVBoxLayout(dialogTab);
    dlg->setContentsMargins(8, 8, 8, 8);
    dlg->setSpacing(6);

    // ---------- 后台初始化（模型加载完成后隐藏） ----------
    initBox_ = new QGroupBox(QStringLiteral("初始化"), dialogTab);
    auto* initLayout = new QVBoxLayout(initBox_);
    loadLabel_ = new QLabel(QStringLiteral("正在准备…"), initBox_);
    loadLabel_->setWordWrap(true);
    loadLabel_->setStyleSheet(QStringLiteral("color:#1a73e8;"));
    loadProgress_ = new QProgressBar(initBox_);
    loadProgress_->setRange(0, 100);
    loadProgress_->setValue(0);
    initLayout->addWidget(loadLabel_);
    initLayout->addWidget(loadProgress_);
    dlg->addWidget(initBox_);

    // ---------- 模型状态条 ----------
    auto* modelRow = new QHBoxLayout;
    modelRow->setSpacing(8);
    auto* mTitle = new QLabel(QStringLiteral("模型"), dialogTab);
    mTitle->setStyleSheet(QStringLiteral("font-weight:bold;color:#444;"));
    modelRow->addWidget(mTitle);
    const char* cats[4] = {"VAD", "ASR", "TTS", "LLM"};
    for (int i = 0; i < 4; ++i) {
        modelChips_[i] = new QLabel(QStringLiteral("%1：—").arg(QLatin1String(cats[i])),
                                    dialogTab);
        modelChips_[i]->setStyleSheet(
            QStringLiteral("padding:2px 10px;border:1px solid #ccc;border-radius:10px;"
                           "background:#f5f6f7;color:#666;"));
        modelRow->addWidget(modelChips_[i]);
    }
    modelRow->addStretch(1);
    dlg->addLayout(modelRow);

    // ---------- 语音控制 + TTS 开关 + 状态 ----------
    auto* ctrlBox = new QGroupBox(QStringLiteral("语音控制"), dialogTab);
    auto* ctrl = new QHBoxLayout(ctrlBox);
    voiceBtn_ = new QPushButton(QStringLiteral("开始语音"), ctrlBox);
    voiceBtn_->setCheckable(true);
    voiceBtn_->setMinimumWidth(96);
    speakBtn_ = new QPushButton(QStringLiteral("喇叭"), ctrlBox);
    speakBtn_->setCheckable(false);
    speakBtn_->setToolTip(QStringLiteral("朗读最近一次回复"));
    ttsToggle_ = new QCheckBox(QStringLiteral("语音播报"), ctrlBox);
    ttsToggle_->setChecked(true);
    ttsToggle_->setToolTip(QStringLiteral("关闭后仅停止合成/播放声音，文字回复照常"));
    vadToggle_ = new QCheckBox(QStringLiteral("VAD 端点检测"), ctrlBox);
    vadToggle_->setChecked(true);
    vadToggle_->setToolTip(QStringLiteral("关闭后改为按住说话/点击按钮录音，结束即转写（ASR 直接接管）"));
    statusLabel_ = new QLabel(QStringLiteral("待机（按住空格说话）"), ctrlBox);
    QFont sf = statusLabel_->font();
    sf.setBold(true);
    sf.setPointSize(sf.pointSize() + 1);
    statusLabel_->setFont(sf);
    statusLabel_->setStyleSheet(QStringLiteral("color:#1a73e8;"));
    ctrl->addWidget(voiceBtn_);
    ctrl->addWidget(speakBtn_);
    ctrl->addWidget(ttsToggle_);
    ctrl->addWidget(vadToggle_);
    ctrl->addWidget(statusLabel_, 1);
    dlg->addWidget(ctrlBox);

    // ---------- 处理流程条 ----------
    auto* stageBox = new QGroupBox(QStringLiteral("处理流程"), dialogTab);
    auto* stageRow = new QHBoxLayout(stageBox);
    stageRow->setContentsMargins(8, 4, 8, 4);
    stageRow->setSpacing(2);
    for (int i = 0; i < kStageCount; ++i) {
        if (i > 0) {
            auto* arrow = new QLabel(QStringLiteral("→"), stageBox);
            arrow->setStyleSheet(QStringLiteral("color:#aaa;"));
            stageRow->addWidget(arrow);
        }
        stageDots_[i] = new QLabel(QString::fromUtf8(kStageLabels[i]), stageBox);
        stageDots_[i]->setAlignment(Qt::AlignCenter);
        stageDots_[i]->setStyleSheet(stagePlainStyle_());
        stageRow->addWidget(stageDots_[i], 1);
    }
    resetPipeline_();   // 全部置灰
    dlg->addWidget(stageBox);

    // ---------- 中部：对话 | 实时回复 + 工具区 ----------
    auto* splitter = new QSplitter(Qt::Horizontal, dialogTab);

    // 左：对话 + 输入
    auto* left = new QWidget(dialogTab);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    chat_ = new QTextBrowser(left);
    chat_->setReadOnly(true);
    chat_->document()->setDefaultStyleSheet(
        "body{font-family:Segoe UI, Microsoft YaHei;}");
    // 滚动跟随：滚到最底 = 贴底模式（新消息自动跳转）；
    // 手动滚到历史中间 = 暂停贴底（新消息不再强制跳转）
    connect(chat_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        auto* sb = chat_->verticalScrollBar();
        stickToBottom_ = (sb->value() >= sb->maximum() - 4);
    });
    leftLayout->addWidget(new QLabel(QStringLiteral("对话"), left), 0);
    auto* inputRow = new QHBoxLayout;
    input_ = new QLineEdit(left);
    input_->setPlaceholderText(QStringLiteral("输入消息，回车发送（支持 /memory save|list|query|clear）…"));
    sendBtn_ = new QPushButton(QStringLiteral("发送"), left);
    leftLayout->addWidget(chat_, 1);
    inputRow->addWidget(input_, 1);
    inputRow->addWidget(sendBtn_, 0);
    leftLayout->addLayout(inputRow);

    // 右：实时回复 + 工具/记忆/日志（与回复分开）
    auto* right = new QWidget(dialogTab);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    auto* replyBox = new QGroupBox(QStringLiteral("实时回复（流式）"), right);
    auto* replyLayout = new QVBoxLayout(replyBox);
    replyView_ = new QPlainTextEdit(replyBox);
    replyView_->setReadOnly(true);
    replyView_->setMaximumHeight(150);
    replyLayout->addWidget(replyView_);
    rightLayout->addWidget(replyBox, 1);

    auto* toolsBox = new QGroupBox(QStringLiteral("工具 / 记忆 / 日志"), right);
    auto* toolsLayout = new QVBoxLayout(toolsBox);
    toolsView_ = new QPlainTextEdit(toolsBox);
    toolsView_->setReadOnly(true);
    toolsView_->setMaximumHeight(200);
    toolsLayout->addWidget(toolsView_);
    rightLayout->addWidget(toolsBox, 2);

    right->setMinimumWidth(360);
    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({680, 460});
    dlg->addWidget(splitter, 1);

    // ---------- 轮次时间轴 ----------
    auto* timelineBox = new QGroupBox(QStringLiteral("轮次耗时"), dialogTab);
    auto* tlLayout = new QVBoxLayout(timelineBox);
    timelineSummary_ = new QLabel(QStringLiteral("完成一轮对话后，这里显示各处理阶段耗时。"),
                                  timelineBox);
    timelineSummary_->setWordWrap(true);
    timelineSummary_->setStyleSheet(QStringLiteral("color:#444;"));
    timelineTable_ = new QTableWidget(timelineBox);
    timelineTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    timelineTable_->setColumnCount(2);
    timelineTable_->setHorizontalHeaderLabels(
        {QStringLiteral("阶段"), QStringLiteral("耗时 (ms)")});
    timelineTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    timelineTable_->horizontalHeader()->setSectionResizeMode(1,
        QHeaderView::ResizeToContents);
    timelineTable_->setMaximumHeight(150);
    timelineHist_ = new QLabel(timelineBox);
    timelineHist_->setWordWrap(true);
    timelineHist_->setStyleSheet(QStringLiteral("color:#888;"));
    tlLayout->addWidget(timelineSummary_);
    tlLayout->addWidget(timelineTable_);
    tlLayout->addWidget(timelineHist_);
    dlg->addWidget(timelineBox);

    // 设置页
    settings_ = new SettingsPanel(this);

    // 顶层 Tab：对话 / 设置
    tabs_ = new QTabWidget(this);
    tabs_->addTab(dialogTab, QStringLiteral("对话"));
    tabs_->addTab(settings_, QStringLiteral("设置"));
    setCentralWidget(tabs_);
}

void MainWindow::connectSignals_() {
    connect(sendBtn_, &QPushButton::clicked, this, &MainWindow::onSendClicked);
    connect(input_, &QLineEdit::returnPressed, this, &MainWindow::onSendClicked);
    connect(voiceBtn_, &QPushButton::toggled, this, &MainWindow::onVoiceToggled);
    connect(speakBtn_, &QPushButton::clicked, this, &MainWindow::onSpeakClicked);
    connect(ttsToggle_, &QCheckBox::toggled, this, &MainWindow::onTtsToggled);
    connect(vadToggle_, &QCheckBox::toggled, this, &MainWindow::onVadToggled);
    // 控制器侧 VAD 状态变化（如后台初始化完成后按配置回填）→ 同步复选框
    connect(controller_, &AgentController::vadEnabledChanged, this, [this](bool enabled) {
        QSignalBlocker b(vadToggle_);
        vadToggle_->setChecked(enabled);
        updateVadHint_();
    });

    // 设置页 → 模型切换
    connect(settings_, &SettingsPanel::modelsApplied, this,
            &MainWindow::onModelsApplied_);
    connect(controller_, &AgentController::modelsChanged, settings_,
            &SettingsPanel::setActiveModels);

    // 设置页 → 麦克风设备切换
    connect(settings_, &SettingsPanel::audioDeviceApplied, controller_,
            &AgentController::setAudioDevice);

    // 麦克风输入电平 → 调试面板（80ms 轮询 atomic，开销可忽略）
    levelTimer_ = new QTimer(this);
    levelTimer_->setInterval(80);
    connect(levelTimer_, &QTimer::timeout, this, [this] {
        settings_->setInputLevel(controller_->inputLevelDb());
    });
    levelTimer_->start();

    // 延时调试面板：定时采集 → 实时刷新
    connect(settings_->latencyPanel(), &LatencyPanel::refreshRequested,
            controller_, &AgentController::refreshLatencies);
    connect(controller_, &AgentController::latencySnapshot,
            settings_->latencyPanel(), &LatencyPanel::onLatencySnapshot);

    connect(controller_, &AgentController::userMessage, this, &MainWindow::onUserMessage_);
    connect(controller_, &AgentController::llmToken, this, &MainWindow::appendToken_);
    connect(controller_, &AgentController::llmComplete, this,
            &MainWindow::appendAssistantFinal_);
    connect(controller_, &AgentController::stateChanged, this, &MainWindow::onStateChanged_);
    connect(controller_, &AgentController::audioStarted, this, [this] {
        voiceBtn_->setText(QStringLiteral("停止语音"));
        statusLabel_->setText(controller_->vadEnabled()
                                 ? QStringLiteral("语音监听中（VAD 自动切分）")
                                 : QStringLiteral("录音中（结束即转写）"));
        setPipelineStep_(0);
    });
    connect(controller_, &AgentController::audioStopped, this, [this] {
        voiceBtn_->setText(QStringLiteral("开始语音"));
        updateVadHint_();
    });
    connect(controller_, &AgentController::voicePlaying, this,
            [this](bool playing) {
                speakBtn_->setEnabled(!playing);
                speakBtn_->setText(playing ? QStringLiteral("播放中…")
                                           : QStringLiteral("喇叭"));
                if (playing) setPipelineStep_(5);
            });
    // 工具调用与"回复"分开：跳转到工具阶段 + 记入工具区
    connect(controller_, &AgentController::toolCalled, this, [this](const QString& line) {
        setPipelineStep_(4);
        onLogLine_(line);
    });
    connect(controller_, &AgentController::memoryEvent, this, &MainWindow::onLogLine_);
    connect(controller_, &AgentController::logLine, this, &MainWindow::onLogLine_);
    connect(controller_, &AgentController::errorLine, this, [this](const QString& e) {
        toolsView_->appendHtml(QStringLiteral("<span style='color:#c00;'>%1</span>")
                            .arg(escHtml(e)));
    });

    // 新面板
    connect(controller_, &AgentController::modelStatus, this, &MainWindow::onModelStatus);
    connect(controller_, &AgentController::stageTiming, this, &MainWindow::onStageTiming);
    connect(controller_, &AgentController::turnTimeline, this, &MainWindow::onTurnTimeline);

    // 后台初始化/切换模型进度 → 进度条、状态栏、按钮可用性
    connect(controller_, &AgentController::loadStageChanged,
            this, &MainWindow::onLoadStage_);
    connect(controller_, &AgentController::loadFinished,
            this, &MainWindow::onLoadFinished_);
}

void MainWindow::onSendClicked() {
    const QString text = input_->text().trimmed();
    if (text.isEmpty()) return;
    input_->clear();
    controller_->sendText(text);
}

void MainWindow::onVoiceToggled(bool checked) {
    if (checked) {
        controller_->startVoice();
    } else {
        controller_->stopVoice();
    }
}

void MainWindow::onSpeakClicked() {
    controller_->playResponse();
}

void MainWindow::onTtsToggled(bool checked) {
    controller_->setTtsEnabled(checked);
}

void MainWindow::onVadToggled(bool checked) {
    controller_->setVadEnabled(checked);
    updateVadHint_();
}

void MainWindow::updateVadHint_() {
    if (controller_ && vadToggle_) {
        const bool vad = controller_->vadEnabled();
        statusLabel_->setText(vad ? QStringLiteral("待机（按住空格说话，VAD 自动切分）")
                                  : QStringLiteral("待机（VAD 关闭：按住说话，结束即转写）"));
    }
}

// 在对话区追加一条带样式的消息；仅"贴底"模式下自动滚动到最新
void MainWindow::appendChatBubble_(const QString& who, const QString& text,
                                   const char* color, const char* icon) {
    const QString body = escHtml(text).replace(QLatin1Char('\n'),
                                               QStringLiteral("<br>"));
    chat_->append(QStringLiteral(
        "<p style='margin:2px 0;'>"
        "<span style='color:%1;font-weight:bold;'>%2 %3</span></p>"
        "<p style='margin:0 0 10px 0;padding:8px 10px;border-radius:6px;background:%4;color:#1a1a1a;'>%5</p>")
        .arg(QString::fromUtf8(color),
             QString::fromUtf8(icon),
             escHtml(who),
             QString::fromUtf8(color),
             body));
    // 用户手动滚到历史中间时不强制跳回；滚回底部后恢复跟随
    if (stickToBottom_) {
        auto* sb = chat_->verticalScrollBar();
        sb->setValue(sb->maximum());
    }
}

void MainWindow::onUserMessage_(const QString& text, bool fromVoice) {
    resetLive_();
    appendChatBubble_(QStringLiteral("我"), text, "#1a73e8", "●");
    if (fromVoice) {
        // 语音轮：采集/VAD/ASR 已由 stageTiming 实时点亮，直接推进到 LLM
        setPipelineStep_(3);
    } else {
        // 文本轮：采集/VAD/ASR 不参与，明确标记跳过
        for (int i = 0; i < 3; ++i) markStageSkipped_(i);
        setPipelineStep_(3);
    }
}

void MainWindow::appendToken_(const QString& token) {
    pendingAssistant_ += token;
    replyView_->setPlainText(pendingAssistant_);
    replyView_->moveCursor(QTextCursor::End);
    replyView_->ensureCursorVisible();
}

void MainWindow::appendAssistantFinal_(const QString& finalText) {
    // 若已有流式累积，以累积文本提交；否则回退到最终文本
    const QString text = pendingAssistant_.isEmpty() ? finalText : pendingAssistant_;
    if (text.isEmpty()) return;
    appendChatBubble_(QStringLiteral("助手"), text, "#0f7b0f", "◆");
    pendingAssistant_.clear();
    replyView_->clear();
}

void MainWindow::onModelsApplied_(const ModelPaths& paths) {
    controller_->setModels(paths);
}

void MainWindow::onLogLine_(const QString& line) {
    toolsView_->appendPlainText(line);
    toolsView_->moveCursor(QTextCursor::End);
}

void MainWindow::onLoadStage_(int step, int total, const QString& label) {
    loading_ = true;
    loadStep_ = step;
    loadTotal_ = total;
    loadStageName_ = label;
    loadElapsed_.restart();
    loadProgress_->setRange(0, total);
    loadProgress_->setValue(step);
    initBox_->show();
    statusLabel_->setText(QStringLiteral("正在加载…"));
    setControlsEnabled_(false);
    if (!loadTimer_->isActive()) loadTimer_->start();
    updateLoadLabel_();
}

void MainWindow::onLoadFinished_(bool ok) {
    loading_ = false;
    loadTimer_->stop();
    initBox_->hide();
    statusLabel_->setText(ok ? QStringLiteral("待机（按住空格说话）")
                             : QStringLiteral("初始化异常（详见日志）"));
    statusLabel_->setStyleSheet(QStringLiteral("color:#1a73e8;"));
    setControlsEnabled_(true);
}

void MainWindow::updateLoadLabel_() {
    if (!loading_) return;
    const qint64 secs = loadElapsed_.elapsed() / 1000;
    loadLabel_->setText(QStringLiteral("正在加载：%1\n步骤 %2/%3（已用时 %4 秒）")
                            .arg(loadStageName_)
                            .arg(loadStep_)
                            .arg(loadTotal_)
                            .arg(secs));
}

void MainWindow::setControlsEnabled_(bool enabled) {
    voiceBtn_->setEnabled(enabled);
    speakBtn_->setEnabled(enabled);
    sendBtn_->setEnabled(enabled);
}

// ========== 流程阶段条 ==========

QString MainWindow::stagePlainStyle_() const {
    return QStringLiteral("padding:2px 8px;border-radius:10px;"
                          "background:#ececec;color:#bbb;font-weight:bold;");
}

void MainWindow::resetPipeline_() {
    currentStageIdx_ = -1;
    stageStates_.resize(kStageCount);
    stageStates_.fill(kStagePending);
    for (int i = 0; i < kStageCount; ++i) setPipelineState_(i, kStagePending);
}

void MainWindow::setPipelineState_(int idx, int state) {
    if (idx < 0 || idx >= kStageCount) return;
    stageStates_[idx] = state;

    QString label = QString::fromUtf8(kStageLabels[idx]);
    QString style;
    switch (state) {
        case kStageActive:
            style = QStringLiteral("padding:2px 8px;border-radius:10px;"
                                   "background:#fde293;color:#b06000;font-weight:bold;");
            break;
        case kStageDone:
            style = QStringLiteral("padding:2px 8px;border-radius:10px;"
                                   "background:#e6f4ea;color:#188038;font-weight:bold;");
            break;
        case kStageSkipped:
            label += QStringLiteral("·跳过");
            style = QStringLiteral("padding:2px 8px;border-radius:10px;border:1px dashed #ccc;"
                                   "background:#fafafa;color:#bbb;");
            break;
        default:
            style = stagePlainStyle_();
            break;
    }
    stageDots_[idx]->setText(label);
    stageDots_[idx]->setStyleSheet(style);
}

void MainWindow::markStageSkipped_(int idx) {
    setPipelineState_(idx, kStageSkipped);
}

// 线性推进：i<idx 完成、i==idx 激活；已跳过的阶段保持跳过（跳过优先于完成显示）
void MainWindow::setPipelineStep_(int idx) {
    currentStageIdx_ = idx;
    for (int i = 0; i < kStageCount; ++i) {
        if (idx < 0) {
            setPipelineState_(i, kStagePending);
        } else if (i < idx) {
            if (stageStates_[i] == kStagePending) setPipelineState_(i, kStageDone);
        } else if (i == idx) {
            setPipelineState_(i, kStageActive);
        }
        // i > idx：保持 pending / skipped
    }
}

// 轮次结束时按时间轴重建：实际执行过的阶段标记完成，未出现的阶段标记跳过
void MainWindow::finalizePipelineFromStages_(const QVariantList& stages) {
    bool hasSpeech = false, hasAsr = false, hasLlm = false;
    bool hasTools = false, hasTts = false;
    for (const auto& s : stages) {
        const QString n = s.toMap().value(QStringLiteral("name")).toString();
        if (n == QLatin1String("speech")) hasSpeech = true;
        else if (n == QLatin1String("asr")) hasAsr = true;
        else if (n == QLatin1String("llm_first") || n == QLatin1String("llm_generate"))
            hasLlm = true;
        else if (n == QLatin1String("tools")) hasTools = true;
        else if (n == QLatin1String("tts")) hasTts = true;
    }
    setPipelineState_(0, hasSpeech ? kStageDone : kStageSkipped);
    setPipelineState_(1, hasSpeech ? kStageDone : kStageSkipped);   // VAD 随语音
    setPipelineState_(2, hasAsr ? kStageDone : kStageSkipped);
    setPipelineState_(3, hasLlm ? kStageDone : kStageSkipped);
    setPipelineState_(4, hasTools ? kStageDone : kStageSkipped);
    setPipelineState_(5, hasTts ? kStageDone : kStageSkipped);
    setPipelineState_(6, hasTts ? kStageDone : kStageSkipped);      // 播放随 TTS
}

// ========== 模型状态条 ==========

void MainWindow::onModelStatus(const QVariantMap& models) {
    const char* keys[4] = {"VAD", "ASR", "TTS", "LLM"};
    for (int i = 0; i < 4; ++i) {
        const QVariantMap x = models.value(QLatin1String(keys[i])).toMap();
        const QString state = x.value(QStringLiteral("state")).toString();
        const QString file = QFileInfo(x.value(QStringLiteral("path")).toString())
                                 .fileName();
        const qint64 bytes = x.value(QStringLiteral("size_bytes")).toLongLong();
        const QString backend = x.value(QStringLiteral("backend")).toString();
        const QString provider = x.value(QStringLiteral("provider")).toString();
        const double loadMs = x.value(QStringLiteral("load_ms")).toDouble();
        const QString disp = x.value(QStringLiteral("disp_name")).toString();
        const bool realBackend = !backend.startsWith(QStringLiteral("Mock占位"));

        const QString sizeTxt = bytes <= 0 ? QStringLiteral("—")
            : (bytes >= 1024 * 1024
                   ? QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1)
                   : QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 0));
        const QString loadTxt = loadMs <= 0 ? QString()
            : QStringLiteral(" · 加载 %1ms").arg(loadMs, 0, 'f', 1);

        QString status;          // 中文状态词
        QString style;
        if (state == QLatin1String("loading")) {
            status = QStringLiteral("加载中");
            style = QStringLiteral("padding:2px 10px;border:1px solid #f0c674;"
                                   "border-radius:10px;background:#fdf3d8;color:#9a6b00;");
        } else if (state == QLatin1String("ready") && !realBackend) {
            // 模块"就绪"了但真实模型文件缺失（Mock 占位不推理），明确提示
            status = QStringLiteral("缺失(Mock)");
            style = QStringLiteral("padding:2px 10px;border:1px solid #e0b0a0;"
                                   "border-radius:10px;background:#fbece7;color:#a33;");
        } else if (state == QLatin1String("ready")) {
            status = QStringLiteral("就绪");
            style = QStringLiteral("padding:2px 10px;border:1px solid #cce8d4;"
                                   "border-radius:10px;background:#e6f4ea;color:#188038;");
        } else {
            status = QStringLiteral("未加载");
            style = QStringLiteral("padding:2px 10px;border:1px dashed #ccc;"
                                   "border-radius:10px;background:#f5f6f7;color:#999;");
        }

        QString chip = QStringLiteral("%1：%2")
                           .arg(QLatin1String(keys[i]), status);
        if (!disp.isEmpty())
            chip += QStringLiteral(" · %1").arg(disp);
        if (realBackend && !provider.isEmpty() && provider != QLatin1String("Mock"))
            chip += QStringLiteral(" · %1").arg(provider);
        if (loadMs > 0 || bytes > 0)
            chip += QStringLiteral(" · %1 · %2%3").arg(file, sizeTxt, loadTxt);
        modelChips_[i]->setText(chip);
        modelChips_[i]->setStyleSheet(style);
        modelChips_[i]->setToolTip(x.isEmpty()
            ? QStringLiteral("尚未上报状态")
            : QStringLiteral("后端：%1\n推理：%2\n型号：%3\n文件：%4\n占用：%5\n加载耗时：%6 ms")
                  .arg(backend,
                       provider.isEmpty() ? QStringLiteral("—") : provider,
                       disp,
                       x.value(QStringLiteral("path")).toString(),
                       sizeTxt,
                       QString::number(loadMs, 'f', 1)));
    }
}

// ========== 时间轴 ==========

void MainWindow::onStateChanged_(const QString& state) {
    statusLabel_->setText(state);
    statusLabel_->setStyleSheet(
        state == QStringLiteral("Thinking") || state == QStringLiteral("Speaking")
            ? QStringLiteral("color:#e37400;")
            : QStringLiteral("color:#1a73e8;"));

    if (state == QLatin1String("Listening")) {
        resetLive_();
        resetPipeline_();          // 新一轮语音：所有阶段回到未开始
        setPipelineStep_(0);
    } else if (state == QLatin1String("Thinking")) {
        setPipelineStep_(3);
    } else if (state == QLatin1String("Speaking")) {
        setPipelineStep_(5);
    }
}

void MainWindow::resetLive_() {
    liveStages_.clear();
    liveTools_ = 0;
    liveActive_ = true;
    refreshTimelineTable_();
}

QString MainWindow::stageDisplayName_(const QString& key) const {
    return stageNameCn(key);
}

void MainWindow::onStageTiming(const QString& stage, double ms) {
    liveActive_ = true;
    liveStages_[stage] = liveStages_.value(stage) + ms;
    if (stage == QLatin1String("speech")) {
        setPipelineStep_(0);
        setPipelineState_(1, kStageDone);   // VAD 在语音结束前已实时完成
    } else if (stage == QLatin1String("asr")) setPipelineStep_(2);
    else if (stage == QLatin1String("llm_first") ||
             stage == QLatin1String("llm_generate")) setPipelineStep_(3);
    else if (stage == QLatin1String("tools")) setPipelineStep_(4);
    else if (stage == QLatin1String("tts")) setPipelineStep_(5);
    refreshTimelineTable_();
}

void MainWindow::refreshTimelineTable_() {
    const char* order[] = {"speech", "asr", "llm_first",
                           "llm_generate", "tools", "tts"};

    double total = 0.0;
    for (const char* k : order)
        total += liveStages_.value(QLatin1String(k));

    timelineTable_->setRowCount(0);
    int row = 0;
    for (const char* k : order) {
        const QString key = QLatin1String(k);
        if (!liveStages_.contains(key)) continue;
        timelineTable_->insertRow(row);
        timelineTable_->setItem(row, 0, new QTableWidgetItem(stageDisplayName_(key)));
        timelineTable_->setItem(row, 1,
            new QTableWidgetItem(QStringLiteral("%1 ms").arg(liveStages_.value(key), 0, 'f', 0)));
        ++row;
    }

    if (row == 0) {
        timelineTable_->insertRow(0);
        auto* it = new QTableWidgetItem(liveActive_
            ? QStringLiteral("本轮处理中…")
            : QStringLiteral("等待一轮对话"));
        it->setForeground(QColor(0x99, 0x99, 0x99));
        timelineTable_->setItem(0, 0, it);
        timelineTable_->setItem(0, 1, new QTableWidgetItem(QString()));
        timelineSummary_->setText(liveActive_ ? QStringLiteral("本轮处理中…")
                                              : QStringLiteral("完成一轮对话后，这里显示各处理阶段耗时。"));
        return;
    }

    // 合计行
    timelineTable_->insertRow(row);
    auto* t0 = new QTableWidgetItem(QStringLiteral("合计"));
    QFont bf = t0->font();
    bf.setBold(true);
    t0->setFont(bf);
    timelineTable_->setItem(row, 0, t0);
    timelineTable_->setItem(row, 1,
        new QTableWidgetItem(QStringLiteral("%1 ms").arg(total, 0, 'f', 0)));

    timelineSummary_->setText(liveActive_
        ? QStringLiteral("本轮处理中…（工具 %1 次）").arg(liveTools_)
        : QStringLiteral("最近一轮 · 工具 %1 次 · 各阶段耗时合计 %2 ms")
              .arg(liveTools_).arg(total, 0, 'f', 0));
}

void MainWindow::onTurnTimeline(const QVariantMap& timeline) {
    // 以最终结果重建时间轴数据
    liveStages_.clear();
    const QVariantList stages = timeline.value(QStringLiteral("stages")).toList();
    for (const auto& s : stages) {
        const QVariantMap m = s.toMap();
        liveStages_[m.value(QStringLiteral("name")).toString()] =
            m.value(QStringLiteral("ms")).toDouble();
    }
    // 流程条：实际执行的阶段 → 完成；未执行（如无工具/无TTS/文本跳过采集）→ 跳过
    finalizePipelineFromStages_(stages);
    liveTools_ = timeline.value(QStringLiteral("tools")).toInt();
    liveActive_ = false;
    refreshTimelineTable_();

    const int idx = timeline.value(QStringLiteral("index")).toInt();
    const QString mode = timeline.value(QStringLiteral("mode")).toString();
    const QString user = timeline.value(QStringLiteral("user")).toString();
    const double total = timeline.value(QStringLiteral("total_ms")).toDouble();
    lastTurnIndex_ = idx;

    timelineSummary_->setText(QStringLiteral("第 %1 轮 · %2 · 「%3」 · 工具 %4 次 · 合计 %5 ms")
        .arg(idx)
        .arg(mode == QLatin1String("voice") ? QStringLiteral("语音") : QStringLiteral("文本"))
        .arg(!user.isEmpty() ? user : QStringLiteral("—"))
        .arg(liveTools_)
        .arg(total, 0, 'f', 0));

    historyLines_.prepend(QStringLiteral("第 %1 轮[%2] 工具 %3 · 合计 %4 ms")
                              .arg(idx)
                              .arg(mode == QLatin1String("voice")
                                       ? QStringLiteral("语音") : QStringLiteral("文本"))
                              .arg(liveTools_)
                              .arg(total, 0, 'f', 0));
    while (historyLines_.size() > 8) historyLines_.removeLast();
    timelineHist_->setText(QStringLiteral("最近：\n") + historyLines_.join(QLatin1Char('\n')));
}

// ========== 工具/记忆 与回复分离辅助 ==========

}  // namespace voice_agent::gui
}  // namespace voice_agent