// src/gui/main_window.cpp
#include "gui/main_window.hpp"
#include "util/log.hpp"

#include <QtCore/QOperatingSystemVersion>
#include <QApplication>

// Windows Mica/亚克力效果
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <QColor>
#include <QCloseEvent>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QVariantList>
#include <QVBoxLayout>
#include <QDateTime>
#include <QWidget>

#include "gui/settings_panel.hpp"
#include "gui/latency_panel.hpp"
#include "gui/memory_panel.hpp"

namespace voice_agent {
namespace gui {

namespace {

QString escHtml(QString s) {
    return s.toHtmlEscaped();
}

// ===== 全局暗色主题（Chat 深色风格：深灰分层、圆角、微分割线）=====
// 施加在 central widget 上，级联到全部子控件；QMainWindow 自身在
// Windows 11 下保持透明以透出 Mica 背板。基础调色板在 main.cpp 设置。
QString appStylesheet() {
    return QStringLiteral(R"(
QWidget { font-family:'Segoe UI','Microsoft YaHei UI','Microsoft YaHei',sans-serif;
          font-size:13px; color:#ececec; }

/* ---- 侧栏 ---- */
#leftSidebar { background:#171717; border-right:1px solid #2a2a2a; }
#rightSidebar { background:#171717; border-left:1px solid #2a2a2a; }
QLabel#brand { font-size:15px; font-weight:600; }
QLabel#sectionLabel { color:#9b9b9b; font-size:11px; font-weight:bold; }
QToolButton#sectionHeader { color:#9b9b9b; font-size:11px; font-weight:bold;
                            text-align:left; padding:2px 4px; border:none; }
QListWidget { background:transparent; border:none; font-size:13px; }
QListWidget::item { border-radius:8px; padding:5px 8px; margin:1px 0; color:#d4d4d4; }
QListWidget::item:hover { background:#262626; }
QListWidget::item:selected { background:#333333; color:#ececec; }
QPushButton#newChatBtn { background:#2f2f2f; border:1px solid #3d3d3d;
                         border-radius:10px; padding:8px 10px; font-weight:600; }
QPushButton#newChatBtn:hover { background:#3a3a3a; }
QPushButton#sideNav { background:transparent; border:none; border-radius:8px;
                      padding:8px 10px; text-align:left; color:#d4d4d4; }
QPushButton#sideNav:hover { background:#262626; }

/* ---- 对话页 ---- */
#chatPage { background:#212121; }
#chatHeader { background:#212121; border-bottom:1px solid #2c2c2c; }
QLabel#pageTitle { font-size:15px; font-weight:600; }
#statusPill { padding:3px 12px; border-radius:12px; background:#22324d;
              color:#8ab4f8; font-weight:bold; }
QScrollArea#chatScroll { background:#212121; border:none; }
QWidget#chatHost { background:#212121; }
#emptyState { color:#9a9a9a; font-size:14px; padding-top:72px; }
QLabel#msgUser { background:#2f2f2f; border-radius:16px; padding:9px 14px; }
QLabel#msgAssistant { background:transparent; color:#ececec; }
QLabel#avatar { background:#10a37f; color:#ffffff; border-radius:12px;
                font-weight:bold; font-size:11px; }

/* ---- 输入卡 ---- */
#composer { background:#2f2f2f; border:1px solid #3d3d3d; border-radius:22px; }
QLineEdit#composerInput { background:transparent; border:none; font-size:14px; }
QPushButton#sendBtn { background:#ffffff; color:#0d0d0d; border:none;
                      border-radius:19px; font-size:16px; }
QPushButton#sendBtn:hover { background:#e0e0e0; }
QPushButton#sendBtn:pressed { background:#cccccc; }
QPushButton#sendBtn:disabled { background:#3d3d3d; color:#7a7a7a; }
QPushButton#iconBtn { background:transparent; border:none; border-radius:19px;
                      font-size:16px; color:#b4b4b4; }
QPushButton#iconBtn:hover { background:#3a3a3a; }
QPushButton#iconBtn:checked { background:#22324d; color:#8ab4f8; }
QToolButton#chipToggle { background:transparent; border:1px solid #3d3d3d;
                         border-radius:12px; padding:3px 10px; color:#b4b4b4; }
QToolButton#chipToggle:hover { background:#3a3a3a; }
QToolButton#chipToggle:checked { background:#22324d; border-color:#2f4a73; color:#8ab4f8; }
QLabel#composerHint { color:#6f6f6f; font-size:11px; }
QFrame#initBanner { background:#1c2a40; border:1px solid #2f4a73; border-radius:12px; }

/* ---- 右栏：模型/流程 chips、日志、耗时 ---- */
QTabWidget::pane { border:none; background:transparent; }
QTabBar::tab { background:transparent; color:#9b9b9b; padding:5px 12px;
               border:none; border-bottom:2px solid transparent; }
QTabBar::tab:selected { color:#ececec; border-bottom:2px solid #ececec; }
QTabBar::tab:hover { color:#ececec; }
QPlainTextEdit#logView { background:#1c1c1c; border:1px solid #2c2c2c;
                         border-radius:10px; font-size:12px; color:#d4d4d4; }
QTableWidget { background:transparent; border:none; font-size:12px; }
QTableWidget::item { padding:2px; }
QHeaderView::section { background:transparent; border:none;
                       border-bottom:1px solid #2c2c2c; padding:3px;
                       color:#9b9b9b; font-size:11px; }

/* ---- 通用 ---- */
QProgressBar { background:#3a3a3a; border:none; border-radius:4px; }
QProgressBar::chunk { background:#7aaafc; border-radius:4px; }
QToolButton { background:transparent; border:none; border-radius:8px;
              padding:3px 7px; color:#b4b4b4; }
QToolButton:hover { background:#2a2a2a; }
QMenu { background:#2a2a2a; border:1px solid #3d3d3d; }
QMenu::item { padding:5px 24px; border-radius:6px; }
QMenu::item:selected { background:#3a3a3a; }
QToolTip { background:#2a2a2a; color:#ececec; border:1px solid #3d3d3d;
           padding:4px; }
QScrollBar:vertical { background:transparent; width:8px; margin:2px; }
QScrollBar::handle:vertical { background:#3d3d3d; border-radius:4px; min-height:24px; }
QScrollBar::handle:vertical:hover { background:#4d4d4d; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
QScrollBar:horizontal { background:transparent; height:8px; margin:2px; }
QScrollBar::handle:horizontal { background:#3d3d3d; border-radius:4px; min-width:24px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width:0; }
)");
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

    // ===== Windows 深色标题栏 + Windows 11 Mica 微光效果 =====
    {
        QOperatingSystemVersion os = QOperatingSystemVersion::current();
        auto hwnd = reinterpret_cast<HWND>(winId());
        // 深色标题栏（Win10 1809+ / Win11 通用；失败只是回退浅色，无副作用）
        DWORD darkTitlebar = 1;
        DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/,
                              &darkTitlebar, sizeof(darkTitlebar));
        bool isWin11 = (os.majorVersion() > 10) ||
                       (os.majorVersion() == 10 && os.minorVersion() >= 0 &&
                        os.microVersion() >= 22000);
        if (isWin11) {
            // 方式一：DWMWA_SYSTEMBACKDROP_TYPE（Windows 11 22H2+，推荐）
            DWORD backdropType = 2; // 2 = Mica
            DwmSetWindowAttribute(hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/,
                                  &backdropType, sizeof(backdropType));
            // 方式二：DWMWA_USE_HOSTBACKDROPBRUSH（Windows 11 21H1+）
            DWORD useHostBackdrop = 1;
            DwmSetWindowAttribute(hwnd, 37 /*DWMWA_USE_HOSTBACKDROPBRUSH*/,
                                  &useHostBackdrop, sizeof(useHostBackdrop));
            // 将内容区扩展到整个窗口，覆写标题栏区
            MARGINS margins = {0, 0, 0, 0}; // 全0表示扩展整个客户区
            DwmExtendFrameIntoClientArea(hwnd, &margins);
            setStyleSheet("QMainWindow { background: transparent; }");
        }
        // 非 Windows 11：保持普通不透明窗口，不做处理
    }

    buildUi_();

    controller_ = new AgentController(this);
    connectSignals_();

    // 界面启动与重活分离：核心装配（config/模型/记忆/音频/状态机）在 AgentController
    // 的 worker 线程做，且延后到首帧绘制之后启动；窗口先立即出现，期间为加载态。
    // 任何磁盘 IO/设备枚举都不得出现在 GUI 线程的构造路径上。
    QTimer::singleShot(0, this, [this] {
        controller_->start();
        refreshMemoryList_();   // 启动后即拉取"共享记忆"分组内容
    });

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
                    pttKeyDownSeen_ = true;
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
                    pttKeyDownSeen_ = false;
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

// ========== PTT 兜底释放 ==========
// 按住空格后如果窗口失焦（Alt+Tab / 点别的程序 / 锁屏），key-up 事件
// 不会回到这个进程，pttHolding_ 与麦克风占用就都卡住了。
// 所以在窗口状态变化与关闭时都补一次幂等释放。

void MainWindow::releasePtt_() {
    if (!pttHolding_ && !pttKeyDownSeen_) return;
    pttHolding_ = false;
    pttKeyDownSeen_ = false;
    if (!controller_) return;

    // voiceBtn_ 在 VAD 模式下就是录音态高亮，必须一起取消，
    // 否则界面显示"正在录音"但实际已经不在录。
    if (controller_->vadEnabled()) {
        QSignalBlocker b(voiceBtn_);
        voiceBtn_->setChecked(false);
        controller_->stopVoice();
    } else {
        controller_->stopVoice();
    }
    updateVadHint_();
}

void MainWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::WindowDeactivate ||
        event->type() == QEvent::WindowStateChange) {
        releasePtt_();
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    releasePtt_();   // 别让麦克风被窗口关闭带着一起挂住
    QMainWindow::closeEvent(event);
}

void MainWindow::buildUi_() {
    auto* central = new QWidget(this);
    auto* rootLayout = new QHBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    buildLeftSidebar_();
    rootLayout->addWidget(leftSidebar_);

    // 中栏 + 右栏（可折叠流程/日志栏）
    centerRightSplit_ = new QSplitter(Qt::Horizontal, central);
    centerStack_ = new QStackedWidget(centerRightSplit_);
    settings_ = new SettingsPanel(this);
    buildChatPage_();
    centerStack_->addWidget(chatPage_);      // 0 = 主对话页
    centerStack_->addWidget(settings_);      // 1 = 设置页
    buildRightSidebar_();

    centerRightSplit_->addWidget(centerStack_);
    centerRightSplit_->addWidget(rightSidebar_);
    centerRightSplit_->setStretchFactor(0, 1);
    centerRightSplit_->setStretchFactor(1, 0);
    centerRightSplit_->setSizes({860, 340});
    centerRightSplit_->setHandleWidth(1);

    rootLayout->addWidget(centerRightSplit_, 1);
    central->setStyleSheet(appStylesheet());   // 浅色主题级联全部子控件
    setCentralWidget(central);
}

// ========== 左栏：对话 / 共享记忆 ==========
void MainWindow::buildLeftSidebar_() {
    leftSidebar_ = new QWidget(this);
    leftSidebar_->setObjectName("leftSidebar");
    leftSidebar_->setMinimumWidth(220);
    leftSidebar_->setMaximumWidth(300);
    auto* l = new QVBoxLayout(leftSidebar_);
    l->setContentsMargins(10, 14, 10, 12);
    l->setSpacing(8);

    auto* brand = new QLabel(QStringLiteral("Voice Agent"), leftSidebar_);
    brand->setObjectName("brand");
    l->addWidget(brand);

    newConvBtn_ = new QPushButton(QStringLiteral("＋ 新建对话"), leftSidebar_);
    newConvBtn_->setObjectName("newChatBtn");
    newConvBtn_->setToolTip(QStringLiteral("新建一个对话（当前核心为单会话上下文，先建好结构留作扩展）"));
    l->addWidget(newConvBtn_);

    // 对话分组
    auto* convHeader = new QToolButton(leftSidebar_);
    convHeader->setObjectName("sectionHeader");
    convHeader->setText(QStringLiteral("▾ 对话"));
    convHeader->setToolButtonStyle(Qt::ToolButtonTextOnly);
    convHeader->setCheckable(true);
    convHeader->setChecked(true);
    convHeader->setAutoRaise(true);
    l->addWidget(convHeader);
    convList_ = new QListWidget(leftSidebar_);
    convList_->setFrameShape(QFrame::NoFrame);
    QFont cf = convList_->font();
    cf.setPointSize(cf.pointSize() - 1);
    convList_->setFont(cf);
    l->addWidget(convList_, 3);
    onNewConversation_();   // 预置一个默认对话

    // 共享记忆分组
    auto* memHeader = new QToolButton(leftSidebar_);
    memHeader->setObjectName("sectionHeader");
    memHeader->setText(QStringLiteral("▾ 共享记忆"));
    memHeader->setToolButtonStyle(Qt::ToolButtonTextOnly);
    memHeader->setCheckable(true);
    memHeader->setChecked(true);
    memHeader->setAutoRaise(true);
    memHeader->setFont(cf);
    l->addWidget(memHeader);
    memList_ = new QListWidget(leftSidebar_);
    memList_->setFrameShape(QFrame::NoFrame);
    memList_->setFont(cf);
    l->addWidget(memList_, 4);

    connect(convHeader, &QToolButton::toggled, this, [this, convHeader](bool on) {
        convHeader->setText(on ? QStringLiteral("▾ 对话") : QStringLiteral("▸ 对话"));
        convList_->setVisible(on);
    });
    connect(memHeader, &QToolButton::toggled, this, [this, memHeader](bool on) {
        memHeader->setText(on ? QStringLiteral("▾ 共享记忆") : QStringLiteral("▸ 共享记忆"));
        memList_->setVisible(on);
    });

    l->addStretch(1);

    settingsNavBtn_ = new QPushButton(QStringLiteral("⚙  设置"), leftSidebar_);
    settingsNavBtn_->setObjectName("sideNav");
    settingsNavBtn_->setAutoDefault(false);
    settingsNavBtn_->setToolTip(QStringLiteral("打开 / 返回 设置页"));
    l->addWidget(settingsNavBtn_);
}

// ========== 中栏：主对话区（ChatGPT 式） ==========
void MainWindow::buildChatPage_() {
    chatPage_ = new QWidget(this);
    chatPage_->setObjectName("chatPage");
    auto* dlg = new QVBoxLayout(chatPage_);
    dlg->setContentsMargins(0, 0, 0, 0);
    dlg->setSpacing(0);

    // ---- 顶栏：左栏开关 + 会话标题 + 状态胶囊 + 右栏开关 ----
    auto* header = new QFrame(chatPage_);
    header->setObjectName("chatHeader");
    header->setFixedHeight(46);
    auto* hh = new QHBoxLayout(header);
    hh->setContentsMargins(12, 6, 12, 6);
    hh->setSpacing(10);
    leftToggleBtn_ = new QToolButton(header);
    leftToggleBtn_->setText(QStringLiteral("☰"));
    leftToggleBtn_->setToolTip(QStringLiteral("折叠 / 展开左侧栏"));
    leftToggleBtn_->setAutoRaise(true);
    hh->addWidget(leftToggleBtn_);
    pageTitle_ = new QLabel(QStringLiteral("对话"), header);
    pageTitle_->setObjectName("pageTitle");
    hh->addWidget(pageTitle_);
    hh->addStretch(1);
    statusLabel_ = new QLabel(QStringLiteral("正在加载…"), header);
    statusLabel_->setObjectName("statusPill");
    hh->addWidget(statusLabel_);
    rightToggleBtn_ = new QToolButton(header);
    rightToggleBtn_->setText(QStringLiteral("⊟"));
    rightToggleBtn_->setAutoRaise(true);
    rightToggleBtn_->setToolTip(QStringLiteral("展开 / 收起右侧流程与日志栏"));
    hh->addWidget(rightToggleBtn_);
    dlg->addWidget(header);

    // ---- 对话流：滚动区 + 气泡行 ----
    chatScroll_ = new QScrollArea(chatPage_);
    chatScroll_->setObjectName("chatScroll");
    chatScroll_->setWidgetResizable(true);
    chatScroll_->setFrameShape(QFrame::NoFrame);
    chatScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    chatHost_ = new QWidget;
    chatHost_->setObjectName("chatHost");
    chatLayout_ = new QVBoxLayout(chatHost_);
    chatLayout_->setContentsMargins(32, 20, 32, 12);
    chatLayout_->setSpacing(14);
    emptyState_ = new QLabel(QStringLiteral(
        "👋 你好，我是你的本地语音助手\n\n"
        "点击 🎙 开始语音对话，或直接在下方输入消息"), chatHost_);
    emptyState_->setObjectName("emptyState");
    emptyState_->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    emptyState_->setWordWrap(true);
    chatLayout_->addWidget(emptyState_);
    chatLayout_->addStretch(1);
    chatScroll_->setWidget(chatHost_);
    connect(chatScroll_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        auto* sb = chatScroll_->verticalScrollBar();
        stickToBottom_ = (sb->value() >= sb->maximum() - 4);
    });
    dlg->addWidget(chatScroll_, 1);

    // ---- 底部输入区：初始化横幅 + 圆角输入卡 ----
    auto* bottom = new QWidget(chatPage_);
    auto* bl = new QVBoxLayout(bottom);
    bl->setContentsMargins(28, 4, 28, 14);
    bl->setSpacing(8);

    initBox_ = new QFrame(bottom);
    initBox_->setObjectName("initBanner");
    auto* initLayout = new QHBoxLayout(initBox_);
    initLayout->setContentsMargins(14, 10, 14, 10);
    initLayout->setSpacing(12);
    loadLabel_ = new QLabel(QStringLiteral("正在准备…"), initBox_);
    loadLabel_->setWordWrap(true);
    loadLabel_->setStyleSheet(QStringLiteral("color:#8ab4f8;"));
    initLayout->addWidget(loadLabel_, 1);
    loadProgress_ = new QProgressBar(initBox_);
    loadProgress_->setRange(0, 100);
    loadProgress_->setValue(0);
    loadProgress_->setFixedWidth(180);
    initLayout->addWidget(loadProgress_);
    bl->addWidget(initBox_);

    auto* composer = new QFrame(bottom);
    composer->setObjectName("composer");
    auto* cv = new QVBoxLayout(composer);
    cv->setContentsMargins(16, 10, 10, 10);
    cv->setSpacing(6);
    input_ = new QLineEdit(composer);
    input_->setObjectName("composerInput");
    input_->setPlaceholderText(
        QStringLiteral("输入消息，回车发送（支持 /memory save|list|query|clear）…"));
    input_->setMinimumHeight(26);
    cv->addWidget(input_);

    auto* ctrlRow = new QHBoxLayout;
    ctrlRow->setSpacing(6);
    voiceBtn_ = new QPushButton(QStringLiteral("🎙"), composer);
    voiceBtn_->setObjectName("iconBtn");
    voiceBtn_->setCheckable(true);
    voiceBtn_->setFixedSize(38, 38);
    voiceBtn_->setToolTip(QStringLiteral("开始 / 停止语音（按住空格可即按即说）"));
    speakBtn_ = new QPushButton(QStringLiteral("🔊"), composer);
    speakBtn_->setObjectName("iconBtn");
    speakBtn_->setFixedSize(38, 38);
    speakBtn_->setToolTip(QStringLiteral("朗读最近一次回复"));
    ttsToggle_ = new QToolButton(composer);
    ttsToggle_->setObjectName("chipToggle");
    ttsToggle_->setText(QStringLiteral("播报"));
    ttsToggle_->setCheckable(true);
    ttsToggle_->setChecked(true);
    ttsToggle_->setToolTip(QStringLiteral("TTS 语音播报开关（关闭后仅显示文字）"));
    vadToggle_ = new QToolButton(composer);
    vadToggle_->setObjectName("chipToggle");
    vadToggle_->setText(QStringLiteral("VAD"));
    vadToggle_->setCheckable(true);
    // 初始为关（= 手动按键，与 agent.yaml 的 input_mode: ptt 一致）。
    // 控制器初始化完会按配置回填，这里只是让首帧不闪一下"开"。
    vadToggle_->setChecked(false);
    vadToggle_->setToolTip(QStringLiteral(
        "关闭（默认）：按住空格/🎙 说话，松开即转写。\n"
        "开启：麦克风常开，自动检测说话起止。\n"
        "完整设置在「设置 → 播报与交互 → 语音输入方式」。"));
    sendBtn_ = new QPushButton(QStringLiteral("➤"), composer);
    sendBtn_->setObjectName("sendBtn");
    sendBtn_->setFixedSize(38, 38);
    sendBtn_->setToolTip(QStringLiteral("发送"));
    ctrlRow->addWidget(voiceBtn_);
    ctrlRow->addWidget(speakBtn_);
    ctrlRow->addWidget(ttsToggle_);
    ctrlRow->addWidget(vadToggle_);
    ctrlRow->addStretch(1);
    ctrlRow->addWidget(sendBtn_);
    cv->addLayout(ctrlRow);
    bl->addWidget(composer);

    auto* hint = new QLabel(
        QStringLiteral("回车发送 · 按住空格说话 · 语音状态见顶部胶囊 · 流程与日志在右栏"), bottom);
    hint->setObjectName("composerHint");
    hint->setAlignment(Qt::AlignHCenter);
    bl->addWidget(hint);

    dlg->addWidget(bottom);
}

// ========== 右栏：模型状态 + 处理流程 + 日志/耗时标签页（可折叠） ==========
void MainWindow::buildRightSidebar_() {
    rightSidebar_ = new QWidget(this);
    rightSidebar_->setObjectName("rightSidebar");
    rightSidebar_->setMinimumWidth(300);
    rightSidebar_->setMaximumWidth(440);
    auto* r = new QVBoxLayout(rightSidebar_);
    r->setContentsMargins(12, 12, 12, 12);
    r->setSpacing(8);

    auto* mTitle = new QLabel(QStringLiteral("模型状态"), rightSidebar_);
    mTitle->setObjectName("sectionLabel");
    r->addWidget(mTitle);
    const char* cats[4] = {"VAD", "ASR", "TTS", "LLM"};
    for (int i = 0; i < 4; ++i) {
        modelChips_[i] = new QLabel(
            QStringLiteral("%1：—").arg(QLatin1String(cats[i])), rightSidebar_);
        modelChips_[i]->setWordWrap(true);
        modelChips_[i]->setStyleSheet(stagePlainStyle_());
        r->addWidget(modelChips_[i]);
    }

    // 处理流程：两行圆角 chips（采集 VAD ASR LLM / 工具 TTS 播放），常显
    auto* pTitle = new QLabel(QStringLiteral("处理流程"), rightSidebar_);
    pTitle->setObjectName("sectionLabel");
    r->addWidget(pTitle);
    auto* stageGrid = new QGridLayout;
    stageGrid->setContentsMargins(0, 0, 0, 0);
    stageGrid->setHorizontalSpacing(5);
    stageGrid->setVerticalSpacing(5);
    for (int i = 0; i < kStageCount; ++i) {
        stageDots_[i] = new QLabel(QString::fromUtf8(kStageLabels[i]), rightSidebar_);
        stageDots_[i]->setAlignment(Qt::AlignCenter);
        stageDots_[i]->setStyleSheet(stagePlainStyle_());
        stageGrid->addWidget(stageDots_[i], i / 4, i % 4);
    }
    r->addLayout(stageGrid);
    resetPipeline_();

    // 日志 / 耗时：标签页收纳
    rightTabs_ = new QTabWidget(rightSidebar_);
    rightTabs_->setDocumentMode(true);

    auto* logPage = new QWidget(rightTabs_);
    auto* logL = new QVBoxLayout(logPage);
    logL->setContentsMargins(0, 6, 0, 0);
    toolsView_ = new QPlainTextEdit(logPage);
    toolsView_->setObjectName("logView");
    toolsView_->setReadOnly(true);
    toolsView_->setFrameShape(QFrame::NoFrame);
    logL->addWidget(toolsView_);
    rightTabs_->addTab(logPage, QStringLiteral("日志"));

    auto* tlPage = new QWidget(rightTabs_);
    auto* tlLayout = new QVBoxLayout(tlPage);
    tlLayout->setContentsMargins(0, 6, 0, 0);
    tlLayout->setSpacing(6);
    timelineSummary_ = new QLabel(QStringLiteral("完成一轮对话后，这里显示各处理阶段耗时。"),
                                  tlPage);
    timelineSummary_->setWordWrap(true);
    timelineSummary_->setStyleSheet(QStringLiteral("color:#b4b4b4;"));
    timelineTable_ = new QTableWidget(tlPage);
    timelineTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    timelineTable_->setColumnCount(2);
    timelineTable_->setHorizontalHeaderLabels(
        {QStringLiteral("阶段"), QStringLiteral("耗时 (ms)")});
    timelineTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    timelineTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    timelineTable_->verticalHeader()->setVisible(false);
    timelineTable_->setShowGrid(false);
    timelineTable_->setMaximumHeight(170);
    timelineHist_ = new QLabel(tlPage);
    timelineHist_->setWordWrap(true);
    timelineHist_->setStyleSheet(QStringLiteral("color:#8a8a8a;font-size:11px;"));
    tlLayout->addWidget(timelineSummary_);
    tlLayout->addWidget(timelineTable_);
    tlLayout->addWidget(timelineHist_);
    tlLayout->addStretch(1);
    rightTabs_->addTab(tlPage, QStringLiteral("耗时"));

    // 内存：进程/系统占比 + 各模型占用条（常显的问题"谁在吃内存"放这里最直观）
    memPanel_ = new MemoryPanel(rightTabs_);
    rightTabs_->addTab(memPanel_, QStringLiteral("内存"));

    r->addWidget(rightTabs_, 1);

    // 每 2s 刷新一次内存面板：读一次进程计数器是微秒级开销，
    // 但没必要跟 levelTimer_ 那样 80ms 刷一次。
    memTimer_ = new QTimer(this);
    memTimer_->setInterval(2000);
    connect(memTimer_, &QTimer::timeout, this, [this] {
        if (memPanel_) memPanel_->refresh();
    });
    memTimer_->start();
}

void MainWindow::connectSignals_() {
    connect(sendBtn_, &QPushButton::clicked, this, &MainWindow::onSendClicked);
    connect(input_, &QLineEdit::returnPressed, this, &MainWindow::onSendClicked);
    connect(newConvBtn_, &QPushButton::clicked, this, &MainWindow::onNewConversation_);
    connect(convList_, &QListWidget::itemClicked, this, &MainWindow::onConversationClicked_);
    connect(memList_, &QListWidget::itemActivated, this, &MainWindow::onMemoryActivated_);
    connect(leftToggleBtn_, &QToolButton::clicked, this, &MainWindow::onToggleLeft_);
    connect(rightToggleBtn_, &QToolButton::clicked, this, &MainWindow::onToggleRight_);
    connect(settingsNavBtn_, &QPushButton::clicked, this, &MainWindow::onSettingsNav_);
    connect(voiceBtn_, &QPushButton::toggled, this, &MainWindow::onVoiceToggled);
    connect(speakBtn_, &QPushButton::clicked, this, &MainWindow::onSpeakClicked);
    connect(ttsToggle_, &QToolButton::toggled, this, &MainWindow::onTtsToggled);
    connect(vadToggle_, &QToolButton::toggled, this, &MainWindow::onVadToggled);
    // 控制器侧 VAD 状态变化（如后台初始化完成后按配置回填）→ 同步复选框
    // 与设置页单选按钮，三处 UI 保持同一个真相
    connect(controller_, &AgentController::vadEnabledChanged, this, [this](bool enabled) {
        QSignalBlocker b(vadToggle_);
        vadToggle_->setChecked(enabled);
        settings_->setInputMode(enabled);
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

    // 设置页 → TTS 语速/发音人（跨线程投递到 worker 处置并持久化）
    connect(settings_, &SettingsPanel::ttsParamsApplied, controller_,
            &AgentController::setTtsParams);

    // 设置页 → 语音输入方式（VAD 自动 / 手动按键）。
    // 走同一个 controller_->setVadEnabled，所以输入卡上的 VAD 芯片会跟着变，
    // 反过来在芯片上点也会同步回设置页 —— 单选按钮与芯片是同一个开关。
    connect(settings_, &SettingsPanel::inputModeApplied, this,
            [this](bool vadEnabled) {
                onVadToggled(vadEnabled);
            });

    // 设置页 → 打断门槛（最短语音时长 + 确认期音量）
    connect(settings_, &SettingsPanel::bargeInSettingsApplied, controller_,
            [this](bool enabled, int minMs, int duckPercent) {
                controller_->setBargeInSettings(enabled, minMs, duckPercent);
            });

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
    // 每轮完成 → 反馈记录 + 瓶颈判定 + 调整建议（仅记录，不自动应用）
    connect(controller_, &AgentController::turnTimeline,
            settings_->latencyPanel(), &LatencyPanel::onTurnFeedback);

    connect(controller_, &AgentController::userMessage, this, &MainWindow::onUserMessage_);
    connect(controller_, &AgentController::llmToken, this, &MainWindow::appendToken_);
    connect(controller_, &AgentController::llmComplete, this,
            &MainWindow::appendAssistantFinal_);
    connect(controller_, &AgentController::stateChanged, this, &MainWindow::onStateChanged_);
    connect(controller_, &AgentController::audioStarted, this, [this] {
        QSignalBlocker b(voiceBtn_);
        voiceBtn_->setChecked(true);   // 🎙 高亮录音态（不限触发来源）
        statusLabel_->setText(controller_->vadEnabled()
                                 ? QStringLiteral("监听中 · VAD 自动切分")
                                 : QStringLiteral("录音中 · 结束即转写"));
        setPipelineStep_(0);
    });
    connect(controller_, &AgentController::audioStopped, this, [this] {
        QSignalBlocker b(voiceBtn_);
        voiceBtn_->setChecked(false);
        updateVadHint_();
    });
    connect(controller_, &AgentController::voicePlaying, this,
            [this](bool playing) {
                speakBtn_->setEnabled(!playing);
                if (playing) setPipelineStep_(5);
            });
    // 工具调用与"回复"分开：跳转到工具阶段 + 记入工具区
    connect(controller_, &AgentController::toolCalled, this, [this](const QString& line) {
        setPipelineStep_(4);
        onLogLine_(line);
    });
    connect(controller_, &AgentController::memoryEvent, this, &MainWindow::onLogLine_);
    // 记忆事件 / 轮次结束 → 刷新"共享记忆"分组
    connect(controller_, &AgentController::memoryEvent, this, &MainWindow::refreshMemoryList_);
    connect(controller_, &AgentController::memoryList, this, &MainWindow::onMemoryList_);
    connect(controller_, &AgentController::logLine, this, &MainWindow::onLogLine_);
    connect(controller_, &AgentController::errorLine, this, [this](const QString& e) {
        toolsView_->appendHtml(QStringLiteral("<span style='color:#f28b82;'>%1</span>")
                            .arg(escHtml(e)));
    });

    // 新面板
    connect(controller_, &AgentController::modelStatus, this, &MainWindow::onModelStatus);
    connect(controller_, &AgentController::stageTiming, this, &MainWindow::onStageTiming);
    connect(controller_, &AgentController::turnTimeline, this, &MainWindow::onTurnTimeline);
    // 内存面板除了自己定时采样，还需要模型的占用数字（加载时才测得准），
    // 所以这条信号里捎带的 ram_bytes 必须喂给它。
    connect(controller_, &AgentController::modelStatus, this, [this](const QVariantMap& m) {
        if (memPanel_) memPanel_->onModelStatus(m);
    });

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
    // 芯片与设置页单选按钮是同一个开关，两边都要跟上
    settings_->setInputMode(checked);
    updateVadHint_();
}

void MainWindow::updateVadHint_() {
    if (!controller_ || !vadToggle_) return;
    const bool vad = controller_->vadEnabled();
    statusLabel_->setText(statusTextFor_(QStringLiteral("Idle")));
    vadToggle_->setToolTip(vad
        ? QStringLiteral(
            "当前：VAD 自动切分。\n"
            "麦克风常开，检测到你开始说话就自动识别，说完自动转写。\n"
            "Agent 播报时你开口即进入打断确认（超过门槛才真正停止）。")
        : QStringLiteral(
            "当前：手动按键。\n"
            "按住空格或 🎙 才开始录音，松开立刻转写。\n"
            "Agent 播报时按下立即打断（按键本身就是明确意图，不等门槛）。"));
}

// ========== 侧栏导航 / 对话 / 共享记忆 ==========

// 高亮当前激活对话，并同步主对话页标题
void MainWindow::refreshConversationBadges_() {
    for (int i = 0; i < convList_->count(); ++i) {
        auto* it = convList_->item(i);
        const bool active = (it == activeConvItem_);
        it->setForeground(active ? QColor(0x8a, 0xb4, 0xf8) : QColor(0xc9, 0xc9, 0xc9));
        QFont f = it->font();
        f.setBold(active);
        it->setFont(f);
    }
    if (pageTitle_ && activeConvItem_) pageTitle_->setText(activeConvItem_->text());
}

void MainWindow::onNewConversation_() {
    ++convCounter_;
    auto* item = new QListWidgetItem(QStringLiteral("对话 %1").arg(convCounter_), convList_);
    item->setData(Qt::UserRole, convCounter_);
    convList_->addItem(item);
    activeConvItem_ = item;
    activeConvIndex_ = convList_->row(item);
    convList_->setCurrentRow(activeConvIndex_);
    refreshConversationBadges_();
}

void MainWindow::onConversationClicked_(QListWidgetItem* item) {
    if (activeConvItem_ == item) {   // 点击当前对话：回到主对话页
        centerStack_->setCurrentIndex(0);
        return;
    }
    activeConvItem_ = item;
    activeConvIndex_ = convList_->row(item);
    convList_->setCurrentRow(activeConvIndex_);
    refreshConversationBadges_();
    centerStack_->setCurrentIndex(0);   // 切回主对话页
}

void MainWindow::onToggleLeft_() {
    leftSidebar_->setVisible(!leftSidebar_->isVisible());
}

void MainWindow::onToggleRight_() {
    rightSidebar_->setVisible(!rightSidebar_->isVisible());
}

void MainWindow::onSettingsNav_() {
    if (centerStack_->currentIndex() == 1) {
        centerStack_->setCurrentIndex(0);
        refreshConversationBadges_();   // 恢复当前对话标题
    } else {
        centerStack_->setCurrentIndex(1);
        if (pageTitle_) pageTitle_->setText(QStringLiteral("设置"));
    }
}

void MainWindow::refreshMemoryList_() {
    if (controller_) controller_->requestMemoryList();
}

void MainWindow::onMemoryList_(const QVariantList& items) {
    memList_->clear();
    if (items.isEmpty()) {
        auto* empty = new QListWidgetItem(QStringLiteral("（暂无记忆）"), memList_);
        empty->setForeground(QColor(0x73, 0x73, 0x73));
        empty->setFlags(empty->flags() & ~Qt::ItemIsSelectable);
        memList_->addItem(empty);
        return;
    }
    for (const auto& v : items) {
        const QVariantMap m = v.toMap();
        QString subject = m.value(QStringLiteral("subject")).toString().trimmed();
        QString content = m.value(QStringLiteral("content")).toString().simplified();
        if (subject.isEmpty()) subject = QStringLiteral("记忆");
        const qint64 ct = m.value(QStringLiteral("created_at")).toLongLong();
        QString firstLine = content.length() > 56 ? content.left(56) + QStringLiteral("…") : content;
        auto* it = new QListWidgetItem(
            QStringLiteral("%1\n%2").arg(subject, firstLine), memList_);
        it->setData(Qt::UserRole, m.value(QStringLiteral("id")));
        it->setData(Qt::UserRole + 1, content);
        it->setData(Qt::UserRole + 2, subject);
        it->setToolTip(QStringLiteral("【%1】\n%2\n\n%3")
                           .arg(subject,
                                content,
                                QDateTime::fromSecsSinceEpoch(ct)
                                    .toString(QStringLiteral("记录于 yyyy-MM-dd HH:mm"))));
        memList_->addItem(it);
    }
}

void MainWindow::onMemoryActivated_(QListWidgetItem* item) {
    const QString content = item->data(Qt::UserRole + 1).toString();
    const QString subject = item->data(Qt::UserRole + 2).toString();
    if (content.isEmpty()) return;
    QMessageBox::information(this, QStringLiteral("共享记忆 · %1").arg(subject),
                             content);
}

// ========== 对话流气泡 ==========

// 把消息行插入到流末尾（空态与 stretch 之前），并按需贴底
void MainWindow::insertChatRow_(QWidget* row) {
    emptyState_->setVisible(false);
    // 布局末尾固定为 [emptyState_, stretch]，消息插在它们之前
    chatLayout_->insertWidget(qMax(0, chatLayout_->count() - 2), row);
    scrollChatToBottom_();
}

void MainWindow::scrollChatToBottom_() {
    if (!stickToBottom_) return;
    // 气泡高度在布局事件后才稳定，贴底放到下一轮事件循环
    QTimer::singleShot(0, this, [this] {
        if (!stickToBottom_) return;
        auto* sb = chatScroll_->verticalScrollBar();
        sb->setValue(sb->maximum());
    });
}

// 用户消息：右侧灰底圆角气泡
void MainWindow::createUserRow_(const QString& text) {
    auto* row = new QWidget(chatHost_);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);
    auto* bubble = new QLabel(text, row);
    bubble->setObjectName("msgUser");
    bubble->setTextFormat(Qt::PlainText);
    bubble->setWordWrap(true);
    bubble->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bubble->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Minimum);
    const int maxW = qBound(320, chatScroll_->viewport()->width() * 7 / 10, 640);
    bubble->setMaximumWidth(maxW);
    h->addStretch(1);
    h->addWidget(bubble);
    insertChatRow_(row);
}

// 助手消息：左侧头像 + 全宽正文；返回正文标签供流式更新
QLabel* MainWindow::createAssistantRow_() {
    auto* row = new QWidget(chatHost_);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(10);
    auto* avatar = new QLabel(QStringLiteral("AI"), row);
    avatar->setObjectName("avatar");
    avatar->setFixedSize(24, 24);
    avatar->setAlignment(Qt::AlignCenter);
    auto* label = new QLabel(row);
    label->setObjectName("msgAssistant");
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    h->addWidget(avatar, 0, Qt::AlignTop);
    h->addWidget(label, 1);
    insertChatRow_(row);
    return label;
}

void MainWindow::appendChatBubble_(const QString& text, bool isUser) {
    if (isUser) {
        createUserRow_(text);
    } else {
        createAssistantRow_()->setText(text);
    }
}

void MainWindow::onUserMessage_(const QString& text, bool fromVoice) {
    resetLive_();
    appendChatBubble_(text, /*isUser=*/true);
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
    const bool first = pendingAssistant_.isEmpty();
    pendingAssistant_ += token;
    if (!streamingLabel_) streamingLabel_ = createAssistantRow_();
    streamingLabel_->setText(pendingAssistant_);
    scrollChatToBottom_();
    if (first) LOG_INFO("SA_LIVE first token arrived, len=%d", token.size());
}

void MainWindow::appendAssistantFinal_(const QString& finalText) {
    // 流式已在对话流里生成气泡：只需收尾；无流式时回退到最终文本
    const bool streamed = streamingLabel_ != nullptr;
    const QString text = pendingAssistant_.isEmpty() ? finalText : pendingAssistant_;
    streamingLabel_ = nullptr;
    pendingAssistant_.clear();
    if (streamed) {
        scrollChatToBottom_();
        return;
    }
    if (text.isEmpty()) return;
    appendChatBubble_(text, /*isUser=*/false);
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
    statusLabel_->setStyleSheet(QStringLiteral("padding:3px 12px;border-radius:12px;"
                                               "background:#22324d;color:#8ab4f8;"
                                               "font-weight:bold;"));
    setControlsEnabled_(false);
    if (!loadTimer_->isActive()) loadTimer_->start();
    updateLoadLabel_();
}

void MainWindow::onLoadFinished_(bool ok) {
    loading_ = false;
    loadTimer_->stop();
    initBox_->hide();
    statusLabel_->setText(ok ? QStringLiteral("待机 · 按住空格说话")
                             : QStringLiteral("初始化异常 · 详见右栏日志"));
    statusLabel_->setStyleSheet(ok
        ? QStringLiteral("padding:3px 12px;border-radius:12px;background:#22324d;"
                         "color:#8ab4f8;font-weight:bold;")
        : QStringLiteral("padding:3px 12px;border-radius:12px;background:#3d1f22;"
                         "color:#f28b82;font-weight:bold;"));
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

// ========== 流程阶段条（右栏两行 chips） ==========

QString MainWindow::stagePlainStyle_() const {
    return QStringLiteral("padding:3px 8px;border-radius:10px;font-size:11px;"
                          "background:#2a2a2a;color:#6f6f6f;font-weight:bold;");
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
            style = QStringLiteral("padding:3px 8px;border-radius:10px;font-size:11px;"
                                   "background:#3d2f14;color:#f0b26b;font-weight:bold;");
            break;
        case kStageDone:
            style = QStringLiteral("padding:3px 8px;border-radius:10px;font-size:11px;"
                                   "background:#1e2f24;color:#6dd58c;font-weight:bold;");
            break;
        case kStageSkipped:
            label += QStringLiteral("·跳过");
            style = QStringLiteral("padding:3px 8px;border-radius:10px;font-size:11px;"
                                   "border:1px dashed #3d3d3d;background:transparent;color:#5f5f5f;");
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
        const qint64 ram = x.value(QStringLiteral("ram_bytes")).toLongLong();
        const QString backend = x.value(QStringLiteral("backend")).toString();
        const QString provider = x.value(QStringLiteral("provider")).toString();
        const double loadMs = x.value(QStringLiteral("load_ms")).toDouble();
        const QString disp = x.value(QStringLiteral("disp_name")).toString();
        const bool realBackend = !backend.startsWith(QStringLiteral("Mock占位"));

        const QString sizeTxt = bytes <= 0 ? QStringLiteral("—")
            : (bytes >= 1024 * 1024
                   ? QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1)
                   : QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 0));
        // 运行时内存（加载前后私有内存增量），比磁盘大小更贴近真实占用
        const QString ramTxt = ram <= 0 ? QString()
            : (ram >= 1024 * 1024
                   ? QStringLiteral("内存 %1 MB").arg(ram / (1024.0 * 1024.0), 0, 'f', 0)
                   : QStringLiteral("内存 %1 KB").arg(ram / 1024.0, 0, 'f', 0));
        const QString loadTxt = loadMs <= 0 ? QString()
            : QStringLiteral(" · 加载 %1ms").arg(loadMs, 0, 'f', 1);

        const bool isVad = qstrcmp(keys[i], "VAD") == 0;

        QString status;          // 中文状态词
        QString baseStyle =
            QStringLiteral("padding:5px 10px;border-radius:10px;color:#ececec;");
        if (state == QLatin1String("loading")) {
            status = QStringLiteral("加载中");
            baseStyle += QStringLiteral("border:1px solid #6b5a2a;background:#352c14;");
        } else if (state == QLatin1String("ready") && !realBackend) {
            // 模块"就绪"了但真实模型文件缺失（Mock 占位不推理），明确提示
            status = QStringLiteral("缺失(Mock)");
            baseStyle += QStringLiteral("border:1px solid #6b3a2f;background:#34201b;");
        } else if (state == QLatin1String("ready")) {
            status = QStringLiteral("就绪");
            baseStyle += QStringLiteral("border:1px solid #2f4a38;background:#1e2f24;");
        } else if (isVad && !controller_->vadEnabled()) {
            // 刻意不加载 ≠ 没配好：说清楚这是省内存的结果，切一下就加载
            status = QStringLiteral("未加载·省内存");
            baseStyle += QStringLiteral("border:1px solid #2f4a38;background:#1b2620;color:#8ab4a0;");
        } else {
            status = QStringLiteral("未加载");
            baseStyle += QStringLiteral("border:1px solid #3d3d3d;background:#262626;color:#9b9b9b;");
        }

        QString chip = QStringLiteral("<b>%1</b> · %2")
                           .arg(QLatin1String(keys[i]), escHtml(status));
        if (!disp.isEmpty())
            chip += QStringLiteral(" · %1").arg(escHtml(disp));
        if (realBackend && !provider.isEmpty() && provider != QLatin1String("Mock"))
            chip += QStringLiteral(" · %1").arg(escHtml(provider));
        if (loadMs > 0 || bytes > 0 || !ramTxt.isEmpty()) {
            QString line = QStringLiteral("%1 · %2").arg(escHtml(file), sizeTxt);
            if (!ramTxt.isEmpty())
                line += QStringLiteral(" · %1").arg(ramTxt);
            chip += QStringLiteral("<br><span style='color:#9b9b9b;font-size:11px;'>%1%2</span>")
                        .arg(line, loadTxt);
        }
        modelChips_[i]->setTextFormat(Qt::RichText);
        modelChips_[i]->setText(chip);
        modelChips_[i]->setStyleSheet(baseStyle);
        modelChips_[i]->setToolTip(x.isEmpty()
            ? QStringLiteral("尚未上报状态")
            : QStringLiteral("后端：%1\n推理：%2\n型号：%3\n文件：%4\n"
                             "磁盘：%5\n运行时内存：%6\n加载耗时：%7 ms")
                  .arg(backend,
                       provider.isEmpty() ? QStringLiteral("—") : provider,
                       disp,
                       x.value(QStringLiteral("path")).toString(),
                       sizeTxt,
                       ram <= 0 ? QStringLiteral("未占用") : ramTxt,
                       QString::number(loadMs, 'f', 1)));
    }
}

// ========== 时间轴 ==========

QString MainWindow::statusTextFor_(const QString& state) const {
    // 状态机内部用的是英文枚举名，直接显示给用户等于把实现细节漏出去。
    // 这里翻成中文，并补上"这一轮发生过什么"的信息 ——
    // 光说"正在听你说"分不清是正常轮次开始，还是用户刚打断了 Agent。
    const bool vad = controller_ && controller_->vadEnabled();

    if (state == QLatin1String("Listening")) {
        if (turnWasInterrupted_) {
            // 打断路径：状态是 Interrupting → Listening 立刻切回来的
            return vad ? QStringLiteral("正在听你说（已打断当前回答）")
                       : QStringLiteral("正在录音（已打断当前回答）");
        }
        return vad ? QStringLiteral("正在听你说…")
                   : QStringLiteral("正在录音…松开即转写");
    }
    if (state == QLatin1String("EouPending"))
        return QStringLiteral("正在判断是否说完…");
    if (state == QLatin1String("Thinking"))
        return QStringLiteral("正在思考…");
    if (state == QLatin1String("Speaking"))
        return controller_ && controller_->ttsEnabled()
                   ? QStringLiteral("正在播报 · 按住空格可打断")
                   : QStringLiteral("正在生成（播报已关闭）");
    if (state == QLatin1String("Interrupting"))
        return QStringLiteral("已停止回答");
    if (state == QLatin1String("Idle"))
        return controller_ && controller_->vadEnabled()
                   ? QStringLiteral("待机 · 按住空格可打断")
                   : QStringLiteral("待机 · 按住空格说话");
    return state;   // 未知状态：原样透传，不猜
}

void MainWindow::onStateChanged_(const QString& state) {
    if (state == QLatin1String("Interrupting")) {
        turnWasInterrupted_ = true;
    } else if (state == QLatin1String("Thinking")) {
        // 新的回答轮次开始，标记复位
        turnWasInterrupted_ = false;
    } else if (state == QLatin1String("Idle")) {
        // 回到空闲说明这一轮彻底结束了，下次打断不该再显示"已打断"
        turnWasInterrupted_ = false;
    }

    statusLabel_->setText(statusTextFor_(state));
    const bool busy = (state == QStringLiteral("Thinking") ||
                       state == QStringLiteral("Speaking"));
    statusLabel_->setStyleSheet(busy
        ? QStringLiteral("padding:3px 12px;border-radius:12px;background:#3d2f14;"
                         "color:#f0b26b;font-weight:bold;")
        : QStringLiteral("padding:3px 12px;border-radius:12px;background:#22324d;"
                         "color:#8ab4f8;font-weight:bold;"));

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
        it->setForeground(QColor(0x73, 0x73, 0x73));
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