// src/gui/settings_panel.hpp
#pragma once
#include <QWidget>
#include <array>
#include "gui/model_catalog.hpp"

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QGridLayout;
class QTabWidget;
class QSlider;
class QSpinBox;
class QCheckBox;
class QRadioButton;

namespace voice_agent {
namespace gui {

class ModelDownloader;
class LatencyPanel;

// ========== 设置面板（模型）==========
// 每个类别（VAD/ASR/TTS/LLM）提供：
//   - “来源清单”下拉：本类别的可下载免费小模型 + 本地已存在模型
//   - 下载按钮 + 进度条 + 状态/错误文本（下载信息展示）
//   - “当前使用”下拉 + 底部“应用切换”：把选中的模型应用到助手
class SettingsPanel final : public QWidget {
    Q_OBJECT

public:
    explicit SettingsPanel(QWidget* parent = nullptr);
    ~SettingsPanel() override;

    // 应用当前选择 → 发出 modelPaths / modelsApplied
    void applySelection();

    // 由外部（MainWindow）注入当前实际启用的模型路径，用于高亮“当前使用”
    void setActiveModels(const ModelPaths& paths);

    // 延时调试子面板（供 MainWindow 连接控制器）
    LatencyPanel* latencyPanel() const { return latency_; }

signals:
    void modelsApplied(const ModelPaths& paths);   // 用户点击“应用切换”
    void audioDeviceApplied(const QString& deviceName); // 应用麦克风设备（空 = 默认）
    void ttsParamsApplied(double speed, double pitch, int speakerId); // 应用 TTS 语速/发音人

    // 语音输入方式：true = VAD 自动切分；false = 手动按键（PTT）
    void inputModeApplied(bool vadEnabled);

    // 打断门槛：enabled=false 时退回“开口即打断”
    void bargeInSettingsApplied(bool enabled, int minSpeechMs, int duckPercent);

public slots:
    // 由外部定时刷新输入电平（dBFS）
    void setInputLevel(float db);

    // 外部状态回填（避免设置面板与输入卡上的 VAD 开关各说各话）
    void setInputMode(bool vadEnabled);

private:
    void buildModelTab_();
    void buildLatencyTab_();
    void buildBehaviorTab_();
    void buildAudioTab_();
    void refreshAudioDevices_();
    // 输入方式单选切换（带QSignalBlocker 之外的显式同步，避免递归）
    void onInputModeSelected_();
    struct Row {
        ModelCategory category;
        QComboBox* installed = nullptr;   // “当前使用”下拉（本地已存在）
        QComboBox* downloadSrc = nullptr; // “来源清单”下拉（可下载条目）
        QPushButton* downloadBtn = nullptr;
        QProgressBar* progress = nullptr;
        QLabel* status = nullptr;
    };

    void refreshInstalledCombos_();
    void fillDownloadSource_(const Row& row);
    void startDownload_(int rowIndex);
    void downloadFailed_(ModelCategory cat, const QString& error);
    int activeRowForCategory_(ModelCategory cat) const;

    // 用缓存的进度+速度渲染当前下载行状态文本
    void renderDownloadStatus_();

    std::array<Row, 4> rows_;
    ModelDownloader* downloader_ = nullptr;
    LatencyPanel* latency_ = nullptr;
    QTabWidget* innerTabs_ = nullptr;
    int currentRow_ = -1;   // 正在下载的行索引
    QLabel* activeSummary_ = nullptr;

    // 下载进度/速度缓存（progressChanged 与 speedUpdated 交错到达时统一渲染）
    qint64 dlSpeed_ = -1;   // 字节/秒；<0 = 不显示
    int dlPct_ = 0;
    qint64 dlGot_ = 0;
    qint64 dlTotal_ = 0;

    // 麦克风调试子页
    QComboBox* micCombo_ = nullptr;
    QLabel* micStatus_ = nullptr;
    QProgressBar* levelBar_ = nullptr;
    QLabel* levelLabel_ = nullptr;

    // TTS 语音参数子页（语速滑块 / 发音人数值）
    QSlider* ttsSpeedSlider_ = nullptr;
    QLabel* ttsSpeedValue_ = nullptr;
    QSpinBox* ttsSpeakerSpin_ = nullptr;

    // 语音输入方式（VAD 自动 / 手动按键）
    QRadioButton* inputVad_ = nullptr;
    QRadioButton* inputPtt_ = nullptr;

    // 打断门槛
    QCheckBox* bargeThresholdOn_ = nullptr;
    QSpinBox* bargeMinSpeechSpin_ = nullptr;
    QSlider* bargeDuckSlider_ = nullptr;
    QLabel* bargeDuckValue_ = nullptr;
};

}  // namespace voice_agent::gui
}  // namespace voice_agent