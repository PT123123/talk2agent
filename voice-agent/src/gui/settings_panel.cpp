// src/gui/settings_panel.cpp
#include "gui/settings_panel.hpp"
#include "gui/model_downloader.hpp"
#include "gui/model_catalog.hpp"
#include "gui/latency_panel.hpp"
#include "audio/audio_device.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

#include <QSignalBlocker>

namespace voice_agent {
namespace gui {

namespace {
// “当前使用”下拉里的本地模型项 → 存 relPath
constexpr int kLocalRole = Qt::UserRole + 1;
}

SettingsPanel::SettingsPanel(QWidget* parent) : QWidget(parent) {
    downloader_ = new ModelDownloader(this);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    innerTabs_ = new QTabWidget(this);
    buildModelTab_();
    buildLatencyTab_();
    buildBehaviorTab_();
    buildAudioTab_();
    root->addWidget(innerTabs_, 1);

    // 首次扫描 models/ 目录延后到首帧绘制之后，避免阻塞窗口显示
    QTimer::singleShot(0, this, [this] { refreshInstalledCombos_(); });

    // 下载结果 → 刷新“当前使用”，并跳过应用者重复步骤
    // 下载进度 → 实时更新对应行
    connect(downloader_, &ModelDownloader::progressChanged, this,
            [this](int percent, qint64 got, qint64 total) {
                if (currentRow_ < 0 || currentRow_ >= 4) return;
                Row& row = rows_[currentRow_];
                row.progress->setValue(percent);
                row.status->setText(QStringLiteral("下载中… %1% (")
                    .arg(percent) +
                    QStringLiteral("%1 / %2)")
                        .arg(QString::number(got / 1024 / 1024).append(" MB"),
                             (total > 0 ? QString::number(total / 1024 / 1024)
                                                  .append(" MB")
                                        : QStringLiteral("?"))));
            });
    connect(downloader_, &ModelDownloader::statusChanged, this,
            [this](const QString& s) {
                if (currentRow_ < 0 || currentRow_ >= 4) return;
                rows_[currentRow_].status->setText(s);
            });
    connect(downloader_, &ModelDownloader::downloaded, this,
            [this](const QString&) {
                if (currentRow_ >= 0 && currentRow_ < 4)
                    rows_[currentRow_].progress->setValue(100);
                currentRow_ = -1;
                refreshInstalledCombos_();
            });
    connect(downloader_, &ModelDownloader::downloadFailed, this,
            [this](const ModelEntry& e, const QString& err) {
                downloadFailed_(e.category, err);
                currentRow_ = -1;
            });
}

void SettingsPanel::buildModelTab_() {
    auto* page = new QWidget(this);
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(12, 12, 12, 12);

    auto* tip = new QLabel(QStringLiteral(
        "选择并下载免费开源小模型，然后“应用切换”到助手。所有文件保存在 models/ 目录下。"),
        page);
    tip->setWordWrap(true);
    tip->setStyleSheet(QStringLiteral("color:#9b9b9b;"));
    pageLayout->addWidget(tip);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(10);

    const int kCols = 4;
    for (int i = 0; i < 4; ++i) {
        ModelCategory cat = static_cast<ModelCategory>(i);
        Row& row = rows_[i];
        row.category = cat;

        auto* box = new QGroupBox(modelCategoryName(cat), page);
        auto* lay = new QGridLayout(box);
        lay->setContentsMargins(8, 8, 8, 8);

        auto* srcLbl = new QLabel(QStringLiteral("可下载"), box);
        row.downloadSrc = new QComboBox(box);
        fillDownloadSource_(row);

        row.downloadBtn = new QPushButton(QStringLiteral("下载"), box);
        row.progress = new QProgressBar(box);
        row.progress->setRange(0, 100);
        row.progress->setValue(0);
        row.status = new QLabel(QStringLiteral("待下载"), box);
        row.status->setWordWrap(true);

        auto* useLbl = new QLabel(QStringLiteral("当前使用"), box);
        row.installed = new QComboBox(box);
        row.installed->setSizeAdjustPolicy(QComboBox::AdjustToContents);

        lay->addWidget(srcLbl, 0, 0);
        lay->addWidget(row.downloadSrc, 0, 1);
        lay->addWidget(row.downloadBtn, 0, 2);
        lay->addWidget(row.progress, 0, 3);
        lay->addWidget(row.status, 1, 0, 1, 4);
        lay->addWidget(useLbl, 2, 0);
        lay->addWidget(row.installed, 2, 1, 1, 3);

        connect(row.downloadBtn, &QPushButton::clicked, this,
                [this, i] { startDownload_(i); });

        grid->addWidget(box, i, 0, 1, kCols);
    }
    pageLayout->addLayout(grid, 1);

    // 底部：当前激活摘要 + 应用按钮
    auto* bottom = new QHBoxLayout;
    activeSummary_ = new QLabel(QStringLiteral("当前模型：默认"), page);
    activeSummary_->setWordWrap(true);
    auto* applyBtn = new QPushButton(QStringLiteral("应用切换"), page);
    applyBtn->setStyleSheet(
        QStringLiteral("background:#1a73e8;color:#fff;padding:6px 18px;border-radius:4px;"));
    connect(applyBtn, &QPushButton::clicked, this, &SettingsPanel::applySelection);
    bottom->addWidget(activeSummary_, 1);
    bottom->addWidget(applyBtn, 0);
    pageLayout->addLayout(bottom);

    innerTabs_->addTab(page, QStringLiteral("模型管理"));
}

void SettingsPanel::buildLatencyTab_() {
    latency_ = new LatencyPanel(this);
    innerTabs_->addTab(latency_, QStringLiteral("延时调试"));
}

void SettingsPanel::buildBehaviorTab_() {
    auto* page = new QWidget(this);
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(12, 12, 12, 12);

    auto* hint = new QLabel(QStringLiteral(
        "交互设置：\n"
        "• 语音输入方式：VAD 自动切分，或按住空格/🎙 说话。\n"
        "• 打断门槛：Agent 说话时你开口，先把它的音量压低，"
        "说话持续超过门槛才真正停止（过滤咳嗽、咂嘴）。\n"
        "• 喇叭按钮：点击朗读最近一次回复。\n"
        "• 语音端点（VAD/ASR）采用本地模型，不联网，隐私优先。"),
        page);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#9b9b9b;"));
    lay->addWidget(hint);

    // ---- 语音输入方式：VAD 自动切分 / 手动按键 ----
    auto* inputBox = new QGroupBox(QStringLiteral("语音输入方式"), page);
    auto* ig = new QGridLayout(inputBox);
    ig->setContentsMargins(10, 10, 10, 10);

    inputVad_ = new QRadioButton(QStringLiteral(
        "VAD 自动切分"), inputBox);
    inputVad_->setToolTip(QStringLiteral(
        "麦克风常开，检测到你开始说话就自动开始识别，说完静音一小段后"
        "自动转写并提交。\n"
        "缺点：一直占着音频设备，环境噪声可能被当成人声；"
        "思考时容易被自己的键盘声触发。"));
    inputPtt_ = new QRadioButton(QStringLiteral(
        "手动按键（按住说话，默认）"), inputBox);
    inputPtt_->setToolTip(QStringLiteral(
        "只有按住空格或🎙 才开始录音，松开立刻转写。\n"
        "不会被环境噪声误触发，也不打断正在进行的思考。"));
    // 默认选中 PTT（与 agent.yaml 的 input_mode: ptt 一致；
    // 控制器初始化后会按配置回填，这里只管首帧）
    inputPtt_->setChecked(true);

    ig->addWidget(inputVad_, 0, 0, 1, 2);
    ig->addWidget(inputPtt_, 1, 0, 1, 2);

    auto* inputNote = new QLabel(QStringLiteral(
        "两种方式的区别只在“怎么开始录音”：打断时机与门槛规则两者一致。"
        "手动按键按下时按键本身就是明确意图，因此立即打断，不等门槛。"),
        inputBox);
    inputNote->setWordWrap(true);
    inputNote->setStyleSheet(QStringLiteral("color:#8a8a8a;"));
    ig->addWidget(inputNote, 2, 0, 1, 2);

    connect(inputVad_, &QRadioButton::toggled, this, [this](bool on) {
        if (on) onInputModeSelected_();
    });
    connect(inputPtt_, &QRadioButton::toggled, this, [this](bool on) {
        if (on) onInputModeSelected_();
    });

    lay->addWidget(inputBox);

    // ---- 打断门槛 ----
    auto* bargeBox = new QGroupBox(QStringLiteral("打断控制"), page);
    auto* bg = new QGridLayout(bargeBox);
    bg->setContentsMargins(10, 10, 10, 10);

    bargeThresholdOn_ = new QCheckBox(
        QStringLiteral("启用打断门槛（说话持续到阈值才停止 Agent）"), bargeBox);
    bargeThresholdOn_->setChecked(true);
    bargeThresholdOn_->setToolTip(QStringLiteral(
        "关闭后：你在 Agent 说话期间一开口就立即停止它 —— 响应最快，"
        "但咳嗽、清嗓子也会把回答掐掉。"));
    bg->addWidget(bargeThresholdOn_, 0, 0, 1, 3);

    bg->addWidget(new QLabel(QStringLiteral("最短语音"), bargeBox), 1, 0);
    bargeMinSpeechSpin_ = new QSpinBox(bargeBox);
    bargeMinSpeechSpin_->setRange(0, 2000);
    bargeMinSpeechSpin_->setValue(160);
    bargeMinSpeechSpin_->setSingleStep(20);
    bargeMinSpeechSpin_->setSuffix(QStringLiteral(" ms"));
    bargeMinSpeechSpin_->setToolTip(QStringLiteral(
        "开口后必须持续这么久才算插话。低于此时长视为噪声，不打断。\n"
        "中文偏短（120~200ms），英文可长一些（250~400ms）。"));
    bg->addWidget(bargeMinSpeechSpin_, 1, 1);

    bg->addWidget(new QLabel(QStringLiteral("确认期音量"), bargeBox), 2, 0);
    bargeDuckSlider_ = new QSlider(Qt::Horizontal, bargeBox);
    bargeDuckSlider_->setRange(0, 100);      // 0% ~ 100%
    bargeDuckSlider_->setValue(35);
    bargeDuckSlider_->setToolTip(QStringLiteral(
        "门槛等待期间把 Agent 音量压到多少，避免它盖住你说话。"));
    bg->addWidget(bargeDuckSlider_, 2, 1);
    bargeDuckValue_ = new QLabel(QStringLiteral("35%"), bargeBox);
    bargeDuckValue_->setMinimumWidth(48);
    bargeDuckValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bg->addWidget(bargeDuckValue_, 2, 2);

    connect(bargeDuckSlider_, &QSlider::valueChanged, this, [this](int v) {
        if (bargeDuckValue_)
            bargeDuckValue_->setText(QStringLiteral("%1%").arg(v));
    });

    auto* bargeApply = new QPushButton(QStringLiteral("应用打断设置"), bargeBox);
    bargeApply->setStyleSheet(QStringLiteral(
        "background:#1a73e8;color:#fff;padding:6px 16px;border-radius:4px;"));
    bg->addWidget(bargeApply, 3, 0, 1, 3);
    connect(bargeApply, &QPushButton::clicked, this, [this] {
        emit bargeInSettingsApplied(
            bargeThresholdOn_->isChecked(),
            bargeMinSpeechSpin_->value(),
            bargeDuckSlider_->value());
    });

    lay->addWidget(bargeBox);

    // ---- TTS 语音参数：语速 / 发音人 ----
    auto* ttsBox = new QGroupBox(QStringLiteral("语音播报（TTS）"), page);
    auto* tg = new QGridLayout(ttsBox);
    tg->setContentsMargins(10, 10, 10, 10);

    tg->addWidget(new QLabel(QStringLiteral("语速"), ttsBox), 0, 0);
    ttsSpeedSlider_ = new QSlider(Qt::Horizontal, ttsBox);
    ttsSpeedSlider_->setRange(50, 200);        // 0.50× ~ 2.00×
    ttsSpeedSlider_->setValue(100);
    ttsSpeedSlider_->setTickInterval(10);
    ttsSpeedSlider_->setTickPosition(QSlider::TicksBelow);
    tg->addWidget(ttsSpeedSlider_, 0, 1);
    ttsSpeedValue_ = new QLabel(QStringLiteral("1.00×"), ttsBox);
    ttsSpeedValue_->setMinimumWidth(56);
    ttsSpeedValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    tg->addWidget(ttsSpeedValue_, 0, 2);

    tg->addWidget(new QLabel(QStringLiteral("发音人"), ttsBox), 1, 0);
    ttsSpeakerSpin_ = new QSpinBox(ttsBox);
    ttsSpeakerSpin_->setRange(0, 200);
    ttsSpeakerSpin_->setValue(45);
    ttsSpeakerSpin_->setToolTip(QStringLiteral(
        "Kokoro 多语言发音人 ID（45=中文女声）。SAPI 用系统默认语音，此项无效。"));
    tg->addWidget(ttsSpeakerSpin_, 1, 1, 1, 2);

    auto* note = new QLabel(QStringLiteral(
        "• 语速：SAPI / Kokoro 均生效\n"
        "• 发音人：仅 Kokoro 模型生效，SAPI 使用系统默认中文语音"),
        ttsBox);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color:#8a8a8a;"));
    tg->addWidget(note, 2, 0, 1, 3);

    auto* ttsApply = new QPushButton(QStringLiteral("应用语音参数"), ttsBox);
    ttsApply->setStyleSheet(QStringLiteral(
        "background:#1a73e8;color:#fff;padding:6px 16px;border-radius:4px;"));
    tg->addWidget(ttsApply, 3, 0, 1, 3);

    connect(ttsSpeedSlider_, &QSlider::valueChanged, this, [this](int v) {
        if (ttsSpeedValue_)
            ttsSpeedValue_->setText(
                QStringLiteral("%1×").arg(v / 100.0, 0, 'f', 2));
    });
    connect(ttsApply, &QPushButton::clicked, this, [this] {
        emit ttsParamsApplied(ttsSpeedSlider_->value() / 100.0, 1.0,
                              ttsSpeakerSpin_->value());
    });

    lay->addWidget(ttsBox);
    lay->addStretch(1);

    innerTabs_->addTab(page, QStringLiteral("播报与交互"));
}

void SettingsPanel::buildAudioTab_() {
    auto* page = new QWidget(this);
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(12, 12, 12, 12);

    auto* tip = new QLabel(QStringLiteral(
        "麦克风调试：选择输入设备并点“应用”，然后对着麦克风说话观察电平变化。\n"
        "说话时电平仍低于 -40 dB，说明声音没有进入本机（设备选错或系统录音权限未开）。"),
        page);
    tip->setWordWrap(true);
    tip->setStyleSheet(QStringLiteral("color:#9b9b9b;"));
    lay->addWidget(tip);

    auto* devRow = new QHBoxLayout;
    devRow->addWidget(new QLabel(QStringLiteral("输入设备"), page));
    micCombo_ = new QComboBox(page);
    micCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    devRow->addWidget(micCombo_, 1);
    auto* refreshBtn = new QPushButton(QStringLiteral("刷新"), page);
    devRow->addWidget(refreshBtn, 0);
    auto* applyBtn = new QPushButton(QStringLiteral("应用"), page);
    applyBtn->setStyleSheet(QStringLiteral(
        "background:#1a73e8;color:#fff;padding:6px 16px;border-radius:4px;"));
    devRow->addWidget(applyBtn, 0);
    lay->addLayout(devRow);

    micStatus_ = new QLabel(page);
    micStatus_->setWordWrap(true);
    micStatus_->setStyleSheet(QStringLiteral("color:#9b9b9b;"));
    lay->addWidget(micStatus_);

    auto* levelRow = new QHBoxLayout;
    levelRow->addWidget(new QLabel(QStringLiteral("输入电平"), page));
    levelBar_ = new QProgressBar(page);
    levelBar_->setRange(0, 60);          // 0..60 映射 -60..0 dB
    levelBar_->setValue(0);
    levelBar_->setTextVisible(false);
    levelRow->addWidget(levelBar_, 1);
    levelLabel_ = new QLabel(QStringLiteral("-96 dB"), page);
    levelLabel_->setMinimumWidth(64);
    levelLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    levelRow->addWidget(levelLabel_, 0);
    lay->addLayout(levelRow);

    lay->addStretch(1);

    connect(refreshBtn, &QPushButton::clicked, this, &SettingsPanel::refreshAudioDevices_);
    connect(applyBtn, &QPushButton::clicked, this, [this] {
        emit audioDeviceApplied(micCombo_->currentData().toString());
    });

    // 设备枚举是磁盘/系统 IO：延后到首帧绘制之后，不阻塞界面加载
    QTimer::singleShot(0, this, [this] { refreshAudioDevices_(); });
    innerTabs_->addTab(page, QStringLiteral("麦克风调试"));
}

void SettingsPanel::refreshAudioDevices_() {
    micCombo_->clear();
    micCombo_->addItem(QStringLiteral("（系统默认）"), QString());

    AudioDeviceManager mgr;
    if (!mgr.initialize()) {
        micStatus_->setText(QStringLiteral("音频上下文初始化失败。"));
        return;
    }
    int defaultIdx = 0;
    const auto devices = mgr.list_input_devices();
    for (const auto& d : devices) {
        micCombo_->addItem(QString::fromStdString(d.name),
                           QString::fromStdString(d.name));
        if (d.is_default) defaultIdx = micCombo_->count() - 1;
    }
    micCombo_->setCurrentIndex(defaultIdx);
    micStatus_->setText(devices.empty()
        ? QStringLiteral("未检测到任何输入设备。")
        : QStringLiteral("共检测到 %1 个输入设备。").arg(devices.size()));
}

void SettingsPanel::setInputLevel(float db) {
    if (!levelBar_ || !levelLabel_) return;
    const float clamped = std::max(-60.0f, std::min(0.0f, db));
    levelBar_->setValue(static_cast<int>(clamped + 60.0f));
    levelLabel_->setText(QStringLiteral("%1 dB").arg(db, 0, 'f', 1));
    const QString color = db < -40.0f ? QStringLiteral("#6f6f6f")
                        : db < -20.0f ? QStringLiteral("#e0a020")
                                      : QStringLiteral("#6dd58c");
    levelBar_->setStyleSheet(
        QStringLiteral("QProgressBar::chunk{background:%1;}").arg(color));
}

void SettingsPanel::onInputModeSelected_() {
    if (!inputVad_ || !inputPtt_) return;
    // 两个 radio 互斥，被选中那个就是目标状态
    emit inputModeApplied(inputVad_->isChecked());
}

void SettingsPanel::setInputMode(bool vadEnabled) {
    if (!inputVad_ || !inputPtt_) return;
    // 用 QSignalBlocker 防止回填时又触发 inputModeApplied，
    // 否则会和外部状态互相打乒乓。
    const QSignalBlocker b1(inputVad_);
    const QSignalBlocker b2(inputPtt_);
    inputVad_->setChecked(vadEnabled);
    inputPtt_->setChecked(!vadEnabled);
}

SettingsPanel::~SettingsPanel() = default;

void SettingsPanel::fillDownloadSource_(const Row& row) {
    row.downloadSrc->clear();
    for (const auto& e : downloadableModels()) {
        if (e.category != row.category) continue;
        row.downloadSrc->addItem(
            QStringLiteral("%1（%2）").arg(e.name, e.sizeDisplay),
            e.id
        );
    }
}

void SettingsPanel::refreshInstalledCombos_() {
    for (auto& row : rows_) {
        const QString prev = row.installed->currentData(kLocalRole).toString();
        row.installed->clear();
        row.installed->addItem(QStringLiteral("（默认 %1）")
                                   .arg(modelCategoryName(row.category)));
        row.installed->setItemData(row.installed->count() - 1, QString(),
                                   kLocalRole);
        for (const auto& l : scanLocalModels()) {
            if (l.category != row.category) continue;
            const QString label = QStringLiteral("%1 (%2 MB)")
                .arg(l.name)
                .arg(l.sizeBytes / 1024 / 1024);
            row.installed->addItem(label);
            const int idx = row.installed->count() - 1;
            row.installed->setItemData(idx, l.relPath, kLocalRole);
        }
        // 恢复之前选择
        int idx = row.installed->findData(prev, kLocalRole);
        if (idx >= 0) row.installed->setCurrentIndex(idx);
    }
}

void SettingsPanel::startDownload_(int rowIndex) {
    const Row& row = rows_[rowIndex];
    const QString id = row.downloadSrc->currentData().toString();
    if (downloader_->isDownloading()) {
        row.status->setText(QStringLiteral("已有下载进行中，请稍候。"));
        return;
    }
    for (const auto& e : downloadableModels()) {
        if (e.id == id) {
            currentRow_ = rowIndex;
            row.progress->setValue(0);
            row.status->setText(QStringLiteral("准备就绪"));
            downloader_->start(e);
            return;
        }
    }
    row.status->setText(QStringLiteral("没有可下载的模型。"));
}

int SettingsPanel::activeRowForCategory_(ModelCategory cat) const {
    for (int i = 0; i < 4; ++i)
        if (rows_[i].category == cat) return i;
    return -1;
}

void SettingsPanel::downloadFailed_(ModelCategory cat, const QString& error) {
    const int i = activeRowForCategory_(cat);
    if (i < 0) return;
    rows_[i].status->setText(QStringLiteral("下载失败：%1").arg(error));
    rows_[i].progress->setValue(0);
}

void SettingsPanel::applySelection() {
    ModelPaths paths;
    paths.vad = rows_[0].installed->currentData(kLocalRole).toString();
    paths.asr = rows_[1].installed->currentData(kLocalRole).toString();
    paths.tts = rows_[2].installed->currentData(kLocalRole).toString();
    paths.llm = rows_[3].installed->currentData(kLocalRole).toString();

    // 摘要
    activeSummary_->setText(QStringLiteral(
        "待应用 → VAD:%1   ASR:%2   TTS:%3   LLM:%4")
        .arg(paths.vad.isEmpty() ? QStringLiteral("默认") : paths.vad,
             paths.asr.isEmpty() ? QStringLiteral("默认") : paths.asr,
             paths.tts.isEmpty() ? QStringLiteral("默认") : paths.tts,
             paths.llm.isEmpty() ? QStringLiteral("默认") : paths.llm));

    emit modelsApplied(paths);
}

void SettingsPanel::setActiveModels(const ModelPaths& paths) {
    activeSummary_->setText(QStringLiteral(
        "当前模型 → VAD:%1   ASR:%2   TTS:%3   LLM:%4")
        .arg(paths.vad.isEmpty() ? QStringLiteral("默认") : paths.vad,
             paths.asr.isEmpty() ? QStringLiteral("默认") : paths.asr,
             paths.tts.isEmpty() ? QStringLiteral("默认") : paths.tts,
             paths.llm.isEmpty() ? QStringLiteral("默认") : paths.llm));
}

}  // namespace voice_agent::gui
}  // namespace voice_agent