// src/gui/model_downloader.hpp
#pragma once
#include <QObject>
#include <QString>
#include "gui/model_catalog.hpp"

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

namespace voice_agent {
namespace gui {

// ========== 模型下载器 ==========
// 基于 Qt6::Network，支持 HTTPS、跳转、流式写盘与进度/错误上报。
// 每次只下载一个模型：调用 start() 后，成功/失败均通过信号结束。
class ModelDownloader final : public QObject {
    Q_OBJECT

public:
    explicit ModelDownloader(QObject* parent = nullptr);
    ~ModelDownloader() override;

    // 开始下载；若正在下载则忽略。destRelPath 相对 models/ 目录。
    void start(const ModelEntry& entry);

    // 取消当前下载（保留已写部分作为部分文件）
    void cancel();

    bool isDownloading() const { return downloading_; }

signals:
    void progressChanged(int percent, qint64 bytesReceived, qint64 bytesTotal);
    void statusChanged(const QString& text);           // 阶段文本（开始/校验/完成）
    void downloaded(const QString& destRelPath);       // 下载完成（已落到 models/）
    void downloadFailed(const ModelEntry& entry, const QString& error);

private:
    void handleFinished_();
    void writeChunk_();

    QNetworkAccessManager* mgr_ = nullptr;
    QNetworkReply* reply_ = nullptr;
    QFile* file_ = nullptr;
    ModelEntry active_;
    QString tempPath_;
    QString destAbsPath_;
    bool downloading_ = false;
};

}  // namespace voice_agent::gui
}  // namespace voice_agent