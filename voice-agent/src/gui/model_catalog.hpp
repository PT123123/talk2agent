// src/gui/model_catalog.hpp
#pragma once
#include <QString>
#include <QUrl>
#include <vector>

namespace voice_agent {
namespace gui {

// 模型类别（与 configs/agent.yaml 的模型字段一一对应）
enum class ModelCategory : int { Vad = 0, Asr = 1, Tts = 2, Llm = 3 };

// 类别显示名 / 配置字段名
QString modelCategoryName(ModelCategory c);
QString modelCategoryConfigKey(ModelCategory c);

// 一个可供下载的开源模型条目（URL 均经过人工核验）
struct ModelFile {
    QUrl url;            // 附加文件下载地址
    QString destRelPath; // 相对 models/ 目录的保存路径
};

struct ModelEntry {
    ModelCategory category{ModelCategory::Llm};
    QString id;            // 唯一标识（用于排重 / 本地文件名）
    QString name;          // 展示名
    QString description;   // 一句话说明
    QUrl url;              // 主文件直接下载地址
    QString destRelPath;   // 主文件相对 models/ 目录的保存路径
    std::vector<ModelFile> extraFiles; // 附加文件（多文件模型，顺序下载）
    bool extractTarBz2{false}; // 主文件为 .tar.bz2，下载后解压到目标目录并删除压缩包
    qint64 sizeBytes{0};   // 大约字节数（主文件 + 附加文件合计）
    QString sizeDisplay;   // 人类可读大小
};

// 内置可下载模型清单（用户 + 开源社区的免费小模型）
const std::vector<ModelEntry>& downloadableModels();

// 本地 models/ 目录内、已知类别的模型文件（按类别分组，仅列存在的）
// 返回 map: category -> 相对 models 名称列表
struct LocalModel {
    ModelCategory category;
    QString relPath;
    QString name;      // 文件名
    qint64 sizeBytes;
};
std::vector<LocalModel> scanLocalModels();

// 返回某类别当前所有条目（下载清单中该类别 + 本地扫描到）的展示名列表
QStringList categoryEntries(ModelCategory c);

// 一次“模型切换”→ 各类别应使用的模型相对路径（相对 models/）
struct ModelPaths {
    QString vad;   // 空 = 使用默认
    QString asr;
    QString tts;
    QString llm;
};

}  // namespace voice_agent::gui
}  // namespace voice_agent