// src/gui/latency_panel.hpp
#pragma once
#include <QWidget>
#include <QHash>
#include <QVariantMap>

class QTableWidget;
class QLabel;

namespace voice_agent {
namespace gui {

// ========== 延时调试面板 ==========
// 每隔固定间隔向控制器请求一次各模型"最近一次推理耗时"快照，实时刷新表格。
// 阶段：VAD 单帧推理 / ASR 整段转写 / LLM 首token / LLM 总生成 / TTS 合成。
class LatencyPanel final : public QWidget {
    Q_OBJECT

public:
    explicit LatencyPanel(QWidget* parent = nullptr);

signals:
    void refreshRequested();   // 定时触发的采集请求

public slots:
    void onLatencySnapshot(const QVariantMap& stages);

private:
    QTableWidget* table_ = nullptr;
    QLabel* note_ = nullptr;
    // 阶段名 → 行索引
    QHash<QString, int> rowIndex_;
};

}  // namespace voice_agent::gui
}  // namespace voice_agent