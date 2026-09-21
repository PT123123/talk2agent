// src/gui/model_downloader.hpp
#pragma once
#include <QObject>
#include <QPair>
#include <QString>
#include <QUrl>
#include <QVector>
#include "gui/model_catalog.hpp"

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

namespace voice_agent {
namespace gui {

// ========== 模型下载器 ==========
// 基于 Qt6::Network，支持 HTTPS、跳转、流式写盘与进度/错误上报。
// 每次只下载一个模型条目：主文件 + 附加文件顺序下载；
// 需要时（extractTarBz2）用系统 tar 解压到目标目录。全部完成后才发 downloaded。
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
    void statusChanged(const QString& text);           // 阶段文本（开始/校验/解压/完成）
    void downloaded(const QString& destRelPath);       // 整个条目完成（含附加文件/解压）
    void downloadFailed(const ModelEntry& entry, const QString& error);

private:
    void startNext_();
    void handleFinished_();
    void writeChunk_();
    void extractAndFinish_();
    void finishOk_();
    void fail_(const QString& error);

    QNetworkAccessManager* mgr_ = nullptr;
    QNetworkReply* reply_ = nullptr;
    QFile* file_ = nullptr;
    ModelEntry active_;
    QVector<QPair<QUrl, QString>> queue_;  // (url, 相对 models/ 路径)
    int queueIndex_ = -1;
    QString tempPath_;
    QString destAbsPath_;
    qint64 totalBytes_ = 0;  // 已成功下载的累计字节（跨文件，用于整体进度）
    bool downloading_ = false;
};

}  // namespace voice_agent::gui
}  // namespace voice_agent
