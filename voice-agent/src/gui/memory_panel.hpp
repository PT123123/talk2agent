// src/gui/memory_panel.hpp
#pragma once
#include <QHash>
#include <QString>
#include <QWidget>

class QLabel;

namespace voice_agent {
namespace gui {

// 内存占用面板（右栏"内存"页）。
//
// 展示两层口径：
//   1) 进程 / 系统：私有内存占物理内存的比例条 —— 回答"这台机器现在还剩多少"
//   2) 各模型：LLM / ASR / TTS / VAD 各自在进程私有内存里占多少（横向堆叠条）
//
// 模型占用来自 controller 在每个模型加载前后采样的私有内存差值，
// 不是模型文件大小 —— 磁盘大小和运行时占用不是一个量级。
class MemoryPanel final : public QWidget {
    Q_OBJECT

public:
    explicit MemoryPanel(QWidget* parent = nullptr);

    // 由 MainWindow 的定时器周期调用（GUI 线程，直接采样，快）
    void refresh();

    // 消费 controller 的 modelStatus：缓存各模型占用供 refresh 画条
    void onModelStatus(const QVariantMap& models);

    QLabel* procLine_ = nullptr;      // 进程私有内存 / 工作集
    QLabel* procBar_ = nullptr;       // 进程占系统比例
    QLabel* modelHead_ = nullptr;     // "各模型占用"小标题
    QWidget* modelHost_ = nullptr;    // 各模型行容器
    QLabel* hint_ = nullptr;

    // key ∈ {LLM,ASR,TTS,VAD} → 字节
    QHash<QString, qint64> modelRam_;
    // key → 展示名（保持固定顺序用）
    QHash<QString, QString> modelName_;
    qint64 procPrivate_ = 0;
    qint64 sysTotal_ = 0;
    qint64 sysAvail_ = 0;
};

}  // namespace voice_agent::gui
}  // namespace voice_agent