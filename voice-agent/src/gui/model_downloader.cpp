// src/gui/model_downloader.cpp
#include "gui/model_downloader.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
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
    queue_.clear();
    queue_.append(qMakePair(entry.url, entry.destRelPath));
    for (const auto& f : entry.extraFiles)
        queue_.append(qMakePair(f.url, f.destRelPath));
    queueIndex_ = -1;
    totalBytes_ = 0;
    downloading_ = true;

    emit statusChanged(QStringLiteral("开始下载：%1").arg(entry.name));
    startNext_();
}

void ModelDownloader::startNext_() {
    ++queueIndex_;
    if (queueIndex_ >= queue_.size()) {
        // 全部文件下载完：需要解压则解压，否则直接收尾
        if (active_.extractTarBz2)
            extractAndFinish_();
        else
            finishOk_();
        return;
    }

    const auto& item = queue_.at(queueIndex_);

    // 目标：models/<destRelPath>，先确保父目录存在，写到 .part 临时文件
    const QString destAbs = QDir::current().filePath(
        QStringLiteral("models/") + item.second);
    destAbsPath_ = destAbs;
    QFileInfo fi(destAbs);
    QDir().mkpath(fi.absolutePath());
    tempPath_ = destAbs + QStringLiteral(".part");

    if (!file_) file_ = new QFile(this);
    file_->setFileName(tempPath_);
    if (!file_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail_(QStringLiteral("无法创建临时文件：%1").arg(file_->errorString()));
        return;
    }

    if (queue_.size() > 1)
        emit statusChanged(QStringLiteral("下载 %1（%2/%3）：%4")
            .arg(active_.name)
            .arg(queueIndex_ + 1)
            .arg(queue_.size())
            .arg(fi.fileName()));

    QNetworkRequest req(item.first);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(60000);  // 60s 无数据则超时
    reply_ = mgr_->get(req);

    connect(reply_, &QNetworkReply::downloadProgress, this,
            [this](qint64 bytesReceived, qint64 bytesTotal) {
                // 整体进度 = 累计字节 / 条目预估总大小
                const qint64 got = totalBytes_ + bytesReceived;
                qint64 total = active_.sizeBytes > 0 ? active_.sizeBytes : bytesTotal;
                if (total <= 0) total = got;
                int percent = static_cast<int>(got * 100 / total);
                if (percent > 100) percent = 100;
                emit progressChanged(percent, got, total);
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
        QFile::remove(tempPath_);
        fail_(errText);
        return;
    }

    // 成功：临时文件重命名为正式文件
    const qint64 fileSize = QFileInfo(tempPath_).size();
    if (!QFile::rename(tempPath_, destAbsPath_)) {
        // 目标已存在则覆盖
        QFile::remove(destAbsPath_);
        if (!QFile::rename(tempPath_, destAbsPath_)) {
            QFile::remove(tempPath_);
            fail_(QStringLiteral("保存文件失败：%1").arg(destAbsPath_));
            return;
        }
    }
    totalBytes_ += fileSize;

    startNext_();
}

void ModelDownloader::extractAndFinish_() {
    // 用系统 tar（Windows 11 自带 bsdtar）解压 .tar.bz2 到压缩包所在目录
    const QString archive = destAbsPath_;
    const QFileInfo fi(archive);
    if (!QFile::exists(archive)) {
        fail_(QStringLiteral("解压失败：压缩包不存在"));
        return;
    }

    emit statusChanged(QStringLiteral("解压中：%1").arg(fi.fileName()));

    auto* proc = new QProcess(this);
    proc->setProgram(QStringLiteral("tar"));
    proc->setArguments({QStringLiteral("-xjf"), archive,
                        QStringLiteral("-C"), fi.absolutePath()});
    connect(proc, &QProcess::finished, this,
            [this, proc, archive](int code, QProcess::ExitStatus) {
                proc->deleteLater();
                if (code != 0) {
                    fail_(QStringLiteral("解压失败（tar 退出码 %1）").arg(code));
                    return;
                }
                QFile::remove(archive);  // 解压成功后删除压缩包，释放空间
                finishOk_();
            });
    proc->start();
}

void ModelDownloader::finishOk_() {
    downloading_ = false;
    emit statusChanged(QStringLiteral("下载完成：%1").arg(active_.name));
    emit downloaded(active_.destRelPath);
}

void ModelDownloader::fail_(const QString& error) {
    downloading_ = false;
    emit downloadFailed(active_, error);
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
