// src/gui/latency_panel.cpp
#include "gui/latency_panel.hpp"

#include <QHeaderView>
#include <QLabel>
#include <QStringList>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

namespace voice_agent {
namespace gui {

namespace {

// 阶段展示名（与主窗口/时间轴保持一致的命名）
QString stageDisplayName(const QString& key) {
    if (key == QLatin1String("speech"))        return QStringLiteral("采集/VAD");
    if (key == QLatin1String("asr"))           return QStringLiteral("ASR 转写");
    if (key == QLatin1String("llm_first"))     return QStringLiteral("LLM 首token");
    if (key == QLatin1String("llm_generate"))  return QStringLiteral("LLM 生成");
    if (key == QLatin1String("tools"))         return QStringLiteral("工具执行");
    if (key == QLatin1String("tts"))           return QStringLiteral("TTS 合成");
    return key;
}

// 判定"瓶颈"：耗时占比 ≥ 该比例 且 ≥ 绝对下限(ms)
constexpr double kBottleneckRatio = 0.30;
constexpr double kBottleneckMinMs = 1500.0;

}  // namespace

LatencyPanel::LatencyPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);

    note_ = new QLabel(QStringLiteral(
        "上半：各模型“最近一次推理”耗时（毫秒，每 0.5s 刷新）。“—”表示该阶段在本会话中尚未被调用："
        "VAD/ASR 仅在语音链路触发，TTS 在合成/播报时触发。"
        "底部“反馈建议”：按每轮完整阶段耗时判定瓶颈，并给出对应模型的调整建议（仅记录+建议，不自动改动参数）。"),
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

    // ===== 反馈建议历史区 =====
    auto* fbHead = new QLabel(
        QStringLiteral("反馈建议（每轮一次 · 瓶颈=占比≥30%且≥1500ms · 仅建议不自动应用）"), this);
    fbHead->setStyleSheet(QStringLiteral("font-weight:600; color:#333; margin-top:8px;"));
    root->addWidget(fbHead);

    feedback_ = new QTextBrowser(this);
    feedback_->setReadOnly(true);
    feedback_->setOpenExternalLinks(false);
    feedback_->setStyleSheet(
        QStringLiteral("QTextBrowser{font-family:'Consolas','Cascadia Mono',monospace;"
                       "font-size:12px; background:#fafafa; border:1px solid #d0d0d0;}"));
    root->addWidget(feedback_, 2);

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

QVector<QPair<QString, double>> LatencyPanel::collectStages_(
    const QVariantMap& timeline) const {
    QVector<QPair<QString, double>> out;
    const QVariantList list = timeline.value(QStringLiteral("stages")).toList();
    for (const auto& s : list) {
        const QVariantMap m = s.toMap();
        out.append(qMakePair(m.value(QStringLiteral("name")).toString(),
                             m.value(QStringLiteral("ms")).toDouble()));
    }
    return out;
}

void LatencyPanel::onTurnFeedback(const QVariantMap& timeline) {
    if (!feedback_ || timeline.isEmpty()) return;
    appendFeedback_(timeline);
}

void LatencyPanel::appendFeedback_(const QVariantMap& timeline) {
    const auto stages = collectStages_(timeline);
    if (stages.isEmpty()) return;

    const QString mode = timeline.value(QStringLiteral("mode")).toString() == QLatin1String("voice")
                             ? QStringLiteral("语音") : QStringLiteral("文本");
    double total = 0.0;
    for (const auto& p : stages) total += p.second;

    auto fmt = [](double ms) -> QString {
        return QStringLiteral("%1 ms").arg(ms, 0, 'f', 0);
    };

    // ===== 瓶颈判定 =====
    QString bottleneck;
    QStringList nonBottleneck;
    QStringList bottlenecks;
    for (const auto& p : stages) {
        if (total > 0.0 &&
            p.second >= kBottleneckMinMs &&
            (p.second / total) >= kBottleneckRatio) {
            bottlenecks.append(stageDisplayName(p.first));
        } else if (p.second > 0.0) {
            nonBottleneck.append(stageDisplayName(p.first));
        }
    }
    if (!bottlenecks.isEmpty()) {
        bottleneck = bottlenecks.join(QLatin1String("、"));
    }

    // ===== 每条已执行阶段一行 + 调整建议 =====
    QStringList suggestLines;
    for (const auto& p : stages) {
        const QString key = p.first;
        const bool isBottleneck = bottlenecks.contains(stageDisplayName(key));
        QString line = QStringLiteral("  · ") + stageDisplayName(key) +
                       QStringLiteral(" (%1, %2%)")
                           .arg(fmt(p.second))
                           .arg(total > 0.0 ? p.second / total * 100.0 : 0.0, 0, 'f', 0);

        // 针对各模型生成"只建议、不自动应用"的调整方向
        QString advice;
        if (key == QLatin1String("llm_first") || key == QLatin1String("llm_generate")) {
            advice = isBottleneck
                ? QStringLiteral("建议手动调小 LLM max_tokens（当前 512→示例 256）并减少召回记忆条数，压缩生成耗时")
                : QStringLiteral("LLM 非瓶颈，可略调大 max_tokens 留足生成余地（GPU 下纯并行收益有限）");
        } else if (key == QLatin1String("tts")) {
            advice = isBottleneck
                ? QStringLiteral("建议手动调小 TTS sample_rate(24000→16000) 或调大 speed，压缩 CPU 合成耗时")
                : QStringLiteral("TTS 非瓶颈且可并行：建议手动调大 num_threads（当前源码写死=2，可试 4~6）增强并行");
        } else if (key == QLatin1String("asr")) {
            advice = isBottleneck
                ? QStringLiteral("ASR 瓶颈（罕见）：建议调小转写 chunk / 波束")
                : QStringLiteral("ASR 非瓶颈可并行：可调大 num_threads（当前 4）");
        } else if (key == QLatin1String("speech")) {
            advice = isBottleneck
                ? QStringLiteral("采集/VAD 瓶颈：检查放音回声/AEC 或设备回调延迟")
                : QStringLiteral("采集/VAD 非瓶颈：可略调低 VAD threshold 提升灵敏度");
        } else if (key == QLatin1String("tools")) {
            advice = isBottleneck
                ? QStringLiteral("工具执行慢：外部搜索/API 网络延迟为主")
                : QStringLiteral("工具调用正常");
        }
        if (!advice.isEmpty()) line += QStringLiteral("— ") + advice;
        suggestLines.append(line);
    }

    // ===== 生成 HTML 块并追加 =====
    ++feedbackIndex_;
    QString block;
    block += QStringLiteral("<div style='background:#fff;border:1px solid #e0e0e0;"
                            "border-radius:6px;padding:6px 8px;margin-bottom:6px;'>");
    block += QStringLiteral("<b>#%1 [%2]</b> 合计 %3 · 瓶颈判定: <b>%4</b>"
                            "<br/><span style='color:#666;'>")
                 .arg(feedbackIndex_)
                 .arg(mode)
                 .arg(fmt(total))
                 .arg(!bottleneck.isEmpty()
                          ? QStringLiteral("<font color='#c0392b'>存在瓶颈：%1</font>").arg(bottleneck)
                          : QStringLiteral("<font color='#27ae60'>无瓶颈</font>"));
    if (!nonBottleneck.isEmpty()) {
        block += QStringLiteral("　可并行/非瓶颈：%1").arg(nonBottleneck.join(QLatin1String("、")));
    }
    block += QStringLiteral("</span><br/>") + suggestLines.join(QLatin1Char('\n')) +
             QStringLiteral("<br/><span style='color:#a0a0a0;'>仅记录+建议，未自动改动参数。</span>");
    block += QStringLiteral("</div>");

    feedback_->append(block);
    feedback_->moveCursor(QTextCursor::End);
}

}  // namespace voice_agent::gui
}  // namespace voice_agent