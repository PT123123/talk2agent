// src/gui/model_downloader.cpp
#include "gui/model_downloader.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>

namespace voice_agent {
namespace gui {

ModelDownloader::ModelDownloader(QObject* parent)
    : QObject(parent), mgr_(new QNetworkAccessManager(this)) {}

ModelDownloader::~ModelDownloader() {
    if (file_) { delete file_; file_ = nullptr; }
    if (reply_) { reply_->abort(); reply_ = nullptr; }
}

void ModelDownloader::start(const ModelEntry& entry) {
    if (downloading_) return;
    active_ = entry;
    downloading_ = true;

    // 目标：models/<destRelPath>，先确保父目录存在，写到 .part 临时文件
    const QString destAbs = QDir::current().filePath(
        QStringLiteral("models/") + entry.destRelPath);
    destAbsPath_ = destAbs;
    QFileInfo fi(destAbs);
    QDir().mkpath(fi.absolutePath());
    tempPath_ = destAbs + QStringLiteral(".part");

    if (!file_) file_ = new QFile(this);
    file_->setFileName(tempPath_);
    if (!file_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        downloading_ = false;
        emit downloadFailed(entry, QStringLiteral("无法创建临时文件：%1")
                                         .arg(file_->errorString()));
        return;
    }

    emit statusChanged(QStringLiteral("开始下载：%1").arg(entry.name));

    QNetworkRequest req(entry.url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(60000);  // 60s 无数据则超时
    reply_ = mgr_->get(req);

    connect(reply_, &QNetworkReply::downloadProgress, this,
            [this](qint64 bytesReceived, qint64 bytesTotal) {
                int percent = 0;
                if (bytesTotal > 0)
                    percent = static_cast<int>(bytesReceived * 100 / bytesTotal);
                emit progressChanged(percent, bytesReceived, bytesTotal);
            });
    connect(reply_, &QNetworkReply::readyRead, this,
            &ModelDownloader::writeChunk_);
    connect(reply_, &QNetworkReply::errorOccurred, this,
            [this](QNetworkReply::NetworkError) {
                // 在 finished_ 中统一处理错误，这里仅保留供展示
                emit statusChanged(QStringLiteral("下载出错：%1")
                                   .arg(reply_ ? reply_->errorString()
                                               : QStringLiteral("未知错误")));
            });
    connect(reply_, &QNetworkReply::finished, this,
            &ModelDownloader::handleFinished_);
}

void ModelDownloader::writeChunk_() {
    if (!reply_ || !file_ || !reply_->isOpen()) return;
    file_->write(reply_->readAll());
}

void ModelDownloader::handleFinished_() {
    QNetworkReply* r = reply_;
    reply_ = nullptr;
    if (!r) return;

    const bool ok = (r->error() == QNetworkReply::NoError);
    const QString errText = ok ? QString() : r->errorString();
    r->deleteLater();
    file_->close();

    if (!ok) {
        downloading_ = false;
        QFile::remove(tempPath_);
        emit downloadFailed(active_, errText);
        return;
    }

    // 成功：临时文件重命名为正式文件
    if (!QFile::rename(tempPath_, destAbsPath_)) {
        // 目标已存在则覆盖
        QFile::remove(destAbsPath_);
        if (!QFile::rename(tempPath_, destAbsPath_)) {
            QFile::remove(tempPath_);
            downloading_ = false;
            emit downloadFailed(active_, QStringLiteral("保存文件失败"));
            return;
        }
    }

    downloading_ = false;
    emit statusChanged(QStringLiteral("下载完成：%1").arg(active_.name));
    emit downloaded(active_.destRelPath);
}

void ModelDownloader::cancel() {
    if (!downloading_) return;
    if (reply_) { reply_->abort(); reply_ = nullptr; }
    if (file_) { file_->close(); }
    QFile::remove(tempPath_);
    downloading_ = false;
    emit statusChanged(QStringLiteral("下载已取消"));
}

}  // namespace voice_agent::gui
}  // namespace voice_agent