// src/gui/settings_panel.cpp
#include "gui/settings_panel.hpp"
#include "gui/model_downloader.hpp"
#include "gui/model_catalog.hpp"
#include "gui/latency_panel.hpp"
#include "audio/audio_device.hpp"

#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

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
    tip->setStyleSheet(QStringLiteral("color:#666;"));
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
        "• 按住空格说话（push-to-talk）：按住空格开始监听，松开即停止。\n"
        "• 喇叭按钮：点击朗读最近一次回复。\n"
        "• 语音端点（VAD/ASR）采用本地模型，不联网，隐私优先。"),
        page);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#555;"));
    lay->addWidget(hint);
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
    tip->setStyleSheet(QStringLiteral("color:#666;"));
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
    micStatus_->setStyleSheet(QStringLiteral("color:#555;"));
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

    refreshAudioDevices_();
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
    const QString color = db < -40.0f ? QStringLiteral("#999")
                        : db < -20.0f ? QStringLiteral("#e0a020")
                                      : QStringLiteral("#188038");
    levelBar_->setStyleSheet(
        QStringLiteral("QProgressBar::chunk{background:%1;}").arg(color));
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