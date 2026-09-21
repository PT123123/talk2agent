// src/gui/latency_panel.cpp
#include "gui/latency_panel.hpp"

#include <QHeaderView>
#include <QLabel>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

namespace voice_agent {
namespace gui {

LatencyPanel::LatencyPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);

    note_ = new QLabel(QStringLiteral(
        "各模型“最近一次推理”耗时（毫秒，每 0.5s 刷新）。“—”表示该阶段在本会话中尚未被调用："
        "VAD/ASR 仅在语音链路触发，TTS 在合成/播报时触发。"
        "对话页底部的“轮次耗时”会按每一轮对话展示更完整的阶段分布。"),
        this);
    note_->setWordWrap(true);
    note_->setStyleSheet(QStringLiteral("color:#666;"));
    root->addWidget(note_);

    table_ = new QTableWidget(this);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setColumnCount(2);
    table_->setHorizontalHeaderLabels(
        {QStringLiteral("阶段"), QStringLiteral("最近耗时 (ms)")});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    const QStringList stages = {
        QStringLiteral("VAD 单帧推理"),
        QStringLiteral("ASR 整段转写"),
        QStringLiteral("LLM 首token"),
        QStringLiteral("LLM 总生成"),
        QStringLiteral("TTS 合成"),
    };
    table_->setRowCount(stages.size());
    for (int i = 0; i < stages.size(); ++i) {
        rowIndex_[stages[i]] = i;
        table_->setItem(i, 0, new QTableWidgetItem(stages[i]));
        table_->setItem(i, 1, new QTableWidgetItem(QStringLiteral("—")));
    }
    root->addWidget(table_, 1);

    // 定时采集
    auto* timer = new QTimer(this);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this, &LatencyPanel::refreshRequested);
    timer->start();
}

void LatencyPanel::onLatencySnapshot(const QVariantMap& stages) {
    for (auto it = stages.constBegin(); it != stages.constEnd(); ++it) {
        const auto row = rowIndex_.find(it.key());
        if (row == rowIndex_.end()) continue;
        QTableWidgetItem* cell = table_->item(*row, 1);
        if (cell) cell->setText(QStringLiteral("%1 ms").arg(it.value().toDouble(), 0, 'f', 1));
    }
}

}  // namespace voice_agent::gui
}  // namespace voice_agent