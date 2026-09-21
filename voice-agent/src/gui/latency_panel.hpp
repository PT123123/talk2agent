// src/gui/latency_panel.hpp
#pragma once
#include <QPair>
#include <QString>
#include <QVector>
#include <QWidget>
#include <QHash>
#include <QVariantMap>

class QTableWidget;
class QLabel;
class QTextBrowser;

namespace voice_agent {
namespace gui {

// ========== 延时调试面板 ==========
// 每隔固定间隔向控制器请求一次各模型"最近一次推理耗时"快照，实时刷新表格。
// 阶段：VAD 单帧推理 / ASR 整段转写 / LLM 首token / LLM 总生成 / TTS 合成。
//
// 下方"反馈建议"区：消费每轮 turnTimeline（阶段耗时+合计），
// 判定哪些阶段是瓶颈 / 哪些非瓶颈可并行，并输出对应模型的调整建议。
// 只做记录+建议，**不**自动改动任何模型参数（需人工确认后手动应用）。
class LatencyPanel final : public QWidget {
    Q_OBJECT

public:
    explicit LatencyPanel(QWidget* parent = nullptr);

signals:
    void refreshRequested();   // 定时触发的采集请求

public slots:
    void onLatencySnapshot(const QVariantMap& stages);
    void onTurnFeedback(const QVariantMap& timeline);

private:
    // 解析一轮阶段耗时 → 返回"阶段名→耗时"的有序列表（仅已执行阶段）
    QVector<QPair<QString, double>> collectStages_(const QVariantMap& timeline) const;
    // 生成"阶段 → 瓶颈判定/调整建议"的文本，追加到反馈历史区
    void appendFeedback_(const QVariantMap& timeline);

    QTableWidget* table_ = nullptr;
    QLabel* note_ = nullptr;
    QTextBrowser* feedback_ = nullptr;
    // 阶段名 → 行索引
    QHash<QString, int> rowIndex_;
    int feedbackIndex_ = 0;   // 反馈记录序号
};

}  // namespace voice_agent::gui
}  // namespace voice_agent