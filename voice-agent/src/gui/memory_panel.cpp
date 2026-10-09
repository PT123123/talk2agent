// src/gui/memory_panel.cpp
#include "gui/memory_panel.hpp"

#include "util/mem_probe.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace voice_agent {
namespace gui {

namespace {

// 固定顺序：大的排前面，一眼看出谁在吃内存。
struct RowSpec {
    const char* key;
    const char* label;
    const char* color;   // 条形填充色
};
const RowSpec kRows[] = {
    {"LLM", "LLM 语言模型", "#4a9eff"},
    {"ASR", "ASR 语音转写",  "#34c77b"},
    {"TTS", "TTS 语音合成",  "#f0a742"},
    {"VAD", "VAD 端点检测",  "#b57edc"},
};

// 字节 → 人类可读（与 util/mem_probe 的 format_bytes 同口径）
QString fmtBytes(qint64 b) {
    return QString::fromStdString(format_bytes(static_cast<std::uint64_t>(b < 0 ? 0 : b)));
}

// 画一条水平比例条：已用段用 color 填充，剩余段是深灰轨道。
// QLabel 的富文本支持 div 的 width + background-color，直接当条用。
QString barHtml(double ratio, const char* color, int widthPx = 250) {
    const double r = ratio < 0.0 ? 0.0 : (ratio > 1.0 ? 1.0 : ratio);
    const int filled = static_cast<int>(r * widthPx);
    return QStringLiteral(
               "<div style='background:#2c2c2c;border-radius:4px;height:10px;width:%1px;'>"
               "<div style='background:%2;border-radius:4px;height:10px;width:%3px;'></div>"
               "</div>")
        .arg(widthPx)
        .arg(QLatin1String(color))
        .arg(filled);
}

}  // namespace

MemoryPanel::MemoryPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("内存占用"), this);
    title->setObjectName("sectionLabel");
    root->addWidget(title);

    procLine_ = new QLabel(QStringLiteral("正在采样…"), this);
    procLine_->setWordWrap(true);
    procLine_->setTextFormat(Qt::RichText);
    procLine_->setStyleSheet(QStringLiteral("color:#d4d4d4; font-size:12px;"));
    root->addWidget(procLine_);

    procBar_ = new QLabel(this);
    procBar_->setTextFormat(Qt::RichText);
    procBar_->setStyleSheet(QStringLiteral("font-size:12px;"));
    procBar_->setWordWrap(true);
    root->addWidget(procBar_);

    modelHead_ = new QLabel(QStringLiteral("各模型占用"), this);
    modelHead_->setStyleSheet(
        QStringLiteral("color:#9b9b9b; font-size:11px; font-weight:bold; margin-top:6px;"));
    root->addWidget(modelHead_);

    modelHost_ = new QWidget(this);
    auto* ml = new QVBoxLayout(modelHost_);
    ml->setContentsMargins(0, 0, 0, 0);
    ml->setSpacing(6);
    for (const auto& r : kRows) {
        const QString key = QString::fromLatin1(r.key);
        modelName_.insert(key, QString::fromLatin1(r.label));
        auto* row = new QLabel(QStringLiteral("%1：—").arg(QLatin1String(r.label)), modelHost_);
        row->setObjectName(QStringLiteral("memRow_%1").arg(QLatin1String(r.key)));
        row->setWordWrap(true);
        row->setTextFormat(Qt::RichText);
        row->setStyleSheet(QStringLiteral("color:#b4b4b4; font-size:12px;"));
        ml->addWidget(row);
    }
    root->addWidget(modelHost_);

    hint_ = new QLabel(QStringLiteral(
        "口径：进程私有内存（不含其他进程共享页）。模型占用 = 该模型加载前后私有内存增量，"
        "比模型文件大小更接近真实占用；VAD 默认不加载，占用为 0。"),
        this);
    hint_->setWordWrap(true);
    hint_->setStyleSheet(QStringLiteral("color:#7a7a7a; font-size:11px;"));
    root->addWidget(hint_);

    root->addStretch(1);
}

void MemoryPanel::onModelStatus(const QVariantMap& models) {
    // "_proc" 是 controller 顺带捎上的进程/系统总量
    const QVariantMap proc = models.value(QStringLiteral("_proc")).toMap();
    if (!proc.isEmpty()) {
        procPrivate_ = proc.value(QStringLiteral("private_bytes")).toLongLong();
        sysTotal_ = proc.value(QStringLiteral("sys_total")).toLongLong();
        sysAvail_ = proc.value(QStringLiteral("sys_available")).toLongLong();
    }
    for (const auto& r : kRows) {
        const QString key = QString::fromLatin1(r.key);
        const QVariantMap m = models.value(key).toMap();
        if (m.isEmpty()) continue;
        modelRam_.insert(key, m.value(QStringLiteral("ram_bytes")).toLongLong());
    }
}

void MemoryPanel::refresh() {
    const MemSample s = mem_probe();
    if (s.valid) {
        procPrivate_ = static_cast<qint64>(s.private_bytes);
        sysTotal_ = static_cast<qint64>(s.sys_total);
        sysAvail_ = static_cast<qint64>(s.sys_available);
    }
    if (procPrivate_ <= 0) return;

    // ===== 进程 / 系统 =====
    const double sysRatio =
        sysTotal_ > 0 ? static_cast<double>(procPrivate_) / sysTotal_ : 0.0;
    const qint64 usedSys = sysTotal_ > sysAvail_ ? sysTotal_ - sysAvail_ : 0;

    procLine_->setText(
        QStringLiteral("<b>本进程私有内存</b> %1　"
                       "<span style='color:#9b9b9b;'>工作集 %2</span>")
            .arg(fmtBytes(procPrivate_),
                 fmtBytes(static_cast<qint64>(s.working_set))));

    procBar_->setText(
        QStringLiteral("%1 占物理内存 %2　"
                       "<span style='color:#9b9b9b;'>系统已用 %3 / %4</span>")
            .arg(barHtml(sysRatio, "#ff9a52"),
                 QStringLiteral("<b>%1%</b>").arg(sysRatio * 100.0, 0, 'f', 1),
                 fmtBytes(usedSys), fmtBytes(sysTotal_)));

    // ===== 各模型 =====
    // 比例分母取"进程私有内存"和"模型总和"里更大的那个：
    // 用进程做分母时小模型会挤成一条线（LLM 4G 旁边ASR 100MB 看不出来），
    // 用总和做分母则条形直观表达"这几个模型内部谁最大"。
    qint64 sumRam = 0;
    for (const auto& r : kRows) sumRam += modelRam_.value(QString::fromLatin1(r.key), 0);
    const qint64 denom = qMax<qint64>(sumRam, 1);

    for (const auto& r : kRows) {
        const QString key = QString::fromLatin1(r.key);
        auto* row = modelHost_->findChild<QLabel*>(
            QStringLiteral("memRow_%1").arg(QLatin1String(r.key)));
        if (!row) continue;

        const qint64 ram = modelRam_.value(key, 0);
        const QString name = modelName_.value(key);
        if (ram <= 0) {
            row->setText(QStringLiteral("<span style='color:#7a7a7a;'>%1：未占用</span>")
                             .arg(name.toHtmlEscaped()));
            continue;
        }
        const double withinModels = static_cast<double>(ram) / denom;
        const double shareOfProc = static_cast<double>(ram) / procPrivate_;
        row->setText(
            QStringLiteral("%1　<b>%2</b>　<span style='color:#9b9b9b;'>占模型合计 %3% · 占进程 %4%</span><br>%5")
                .arg(name.toHtmlEscaped(),
                     fmtBytes(ram),
                     QStringLiteral("%1").arg(withinModels * 100.0, 0, 'f', 0),
                     QStringLiteral("%1").arg(shareOfProc * 100.0, 0, 'f', 1),
                     barHtml(withinModels, r.color)));
    }
}

}  // namespace voice_agent::gui
}  // namespace voice_agent