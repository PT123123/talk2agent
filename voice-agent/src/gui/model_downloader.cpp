// src/gui/model_downloader.cpp
#include "gui/model_downloader.hpp"
#include "util/paths.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

namespace voice_agent {
namespace gui {

namespace {
// 网络错误重试退避序列（毫秒），最多 kMaxRetries_ 次
constexpr int kRetryBackoffMs[] = {1000, 2000, 4000, 8000, 15000};
// 看门狗：连续 N 秒速度低于阈值视为连接停滞，主动断开重连续传
constexpr int kStallSeconds = 30;
constexpr qint64 kStallBps = 1024;          // 1KB/s
constexpr qint64 kRetryBudgetResetBytes = 10 * 1024 * 1024;  // 单请求 10MB 进度重置重试预算
}  // namespace

ModelDownloader::ModelDownloader(QObject* parent)
    : QObject(parent), mgr_(new QNetworkAccessManager(this)) {
    retryTimer_ = new QTimer(this);
    retryTimer_->setSingleShot(true);
    connect(retryTimer_, &QTimer::timeout, this, &ModelDownloader::downloadCurrentFile_);

    tickTimer_ = new QTimer(this);
    connect(tickTimer_, &QTimer::timeout, this, &ModelDownloader::onTick_);

    tickClock_ = new QElapsedTimer();
}

ModelDownloader::~ModelDownloader() {
    canceled_ = true;  // 析构中的 abort 不触发重试
    if (reply_) { reply_->abort(); reply_ = nullptr; }
    delete tickClock_;
    if (file_) { delete file_; file_ = nullptr; }
}

// HF 镜像：download/hf_mirror=true（默认）时把 huggingface.co 映射到 hf-mirror.com，
// 仅改主机名、路径不变；GitHub 等其他源不受影响。
// 注意：hf-mirror 对部分 LFS 文件会 308 跳回 huggingface.co（相当于无加速）。
QUrl ModelDownloader::applyMirror_(const QUrl& url) {
    if (url.host().compare(QStringLiteral("huggingface.co"), Qt::CaseInsensitive) != 0)
        return url;
    const QSettings s(QStringLiteral("VoiceAgent"), QStringLiteral("gui"));
    if (!s.value(QStringLiteral("download/hf_mirror"), true).toBool())
        return url;
    QUrl mirrored = url;
    mirrored.setHost(QStringLiteral("hf-mirror.com"));
    return mirrored;
}

void ModelDownloader::start(const ModelEntry& entry) {
    if (downloading_) return;
    active_ = entry;
    canceled_ = false;
    retryCount_ = 0;
    queue_.clear();
    queue_.append(qMakePair(applyMirror_(entry.url), entry.destRelPath));
    for (const auto& f : entry.extraFiles)
        queue_.append(qMakePair(applyMirror_(f.url), f.destRelPath));
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
    downloadCurrentFile_();
}

void ModelDownloader::downloadCurrentFile_() {
    if (queueIndex_ < 0 || queueIndex_ >= queue_.size()) return;
    const auto& item = queue_.at(queueIndex_);

    // 目标：<用户模型目录>/<destRelPath>，先确保父目录存在，写到 .part 临时文件
    const QString destAbs = QDir::cleanPath(
        QString::fromStdString(models_root()) + QLatin1Char('/') + item.second);
    destAbsPath_ = destAbs;
    tempPath_ = destAbs + QStringLiteral(".part");
    QDir().mkpath(QFileInfo(destAbs).absolutePath());

    // 已有正式文件 → Range 探测校验：完整则跳过，不完整转为 .part 续传
    if (QFileInfo::exists(destAbs)) {
        startProbe_(item.first);
        return;
    }

    // 断点续传：.part 已有字节则从该偏移继续（服务器不支持 Range 会回退全量）
    const QFileInfo partInfo(tempPath_);
    resumePos_ = partInfo.exists() ? partInfo.size() : 0;

    if (!file_) file_ = new QFile(this);
    file_->setFileName(tempPath_);
    const QIODevice::OpenMode mode = resumePos_ > 0
        ? (QIODevice::WriteOnly | QIODevice::Append)
        : (QIODevice::WriteOnly | QIODevice::Truncate);
    if (!file_->open(mode)) {
        fail_(QStringLiteral("无法创建临时文件：%1").arg(file_->errorString()));
        return;
    }

    const QFileInfo fi(destAbs);
    if (queue_.size() > 1)
        emit statusChanged(QStringLiteral("下载 %1（%2/%3）：%4")
            .arg(active_.name)
            .arg(queueIndex_ + 1)
            .arg(queue_.size())
            .arg(fi.fileName()));
    else if (resumePos_ > 0)
        emit statusChanged(QStringLiteral("从断点继续：%1（已有 %2 MB）")
            .arg(fi.fileName())
            .arg(resumePos_ / 1024 / 1024));

    QNetworkRequest req(item.first);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(60000);  // 60s 无数据则超时（超时走自动重试 + 续传）
    if (resumePos_ > 0)
        req.setRawHeader(QByteArrayLiteral("Range"),
                         QByteArrayLiteral("bytes=") +
                             QByteArray::number(resumePos_) + QByteArrayLiteral("-"));
    rangeChecked_ = false;
    curReceived_ = 0;
    resetMark_ = 0;
    expectedFileBytes_ = -1;
    zeroStreak_ = 0;
    lastTickBytes_ = resumePos_;
    tickClock_->start();
    canceled_ = false;
    reply_ = mgr_->get(req);

    connect(reply_, &QNetworkReply::downloadProgress, this,
            [this](qint64 bytesReceived, qint64 bytesTotal) {
                curReceived_ = bytesReceived;
                // 期望总大小 = 断点 + 本次响应的 Content-Length（200 回退后 resumePos_ 归零，自动纠正）
                if (bytesTotal > 0) expectedFileBytes_ = resumePos_ + bytesTotal;
                // 重试预算随实际进度恢复：持续有进度就不封顶
                if (curReceived_ - resetMark_ >= kRetryBudgetResetBytes) {
                    resetMark_ = curReceived_;
                    retryCount_ = 0;
                }
                // 整体进度 = 已完成文件累计 + 当前文件断点 + 本次接收
                const qint64 got = totalBytes_ + resumePos_ + bytesReceived;
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
                // 在 finished_ 中统一处理错误（重试/失败），这里仅即时提示
                emit statusChanged(QStringLiteral("下载出错：%1")
                                   .arg(reply_ ? reply_->errorString()
                                               : QStringLiteral("未知错误")));
            });
    connect(reply_, &QNetworkReply::finished, this,
            &ModelDownloader::handleFinished_);

    tickTimer_->start(1000);
}

// Range 探测已存在的正式文件：416=已是完整大小（跳过）；206=不完整（转 .part 续传）；
// 200=服务器不支持 Range（假定完整）。
void ModelDownloader::startProbe_(const QUrl& url) {
    probing_ = true;
    expectedFileBytes_ = -1;
    zeroStreak_ = 0;
    emit speedUpdated(-1);
    emit statusChanged(QStringLiteral("校验已存在文件：%1")
                           .arg(QFileInfo(destAbsPath_).fileName()));

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(30000);
    req.setRawHeader(QByteArrayLiteral("Range"),
                     QByteArrayLiteral("bytes=") +
                         QByteArray::number(QFileInfo(destAbsPath_).size()) +
                         QByteArrayLiteral("-"));
    canceled_ = false;
    reply_ = mgr_->get(req);
    connect(reply_, &QNetworkReply::errorOccurred, this, [this](QNetworkReply::NetworkError) {
        emit statusChanged(QStringLiteral("校验出错：%1")
                           .arg(reply_ ? reply_->errorString() : QStringLiteral("未知错误")));
    });
    connect(reply_, &QNetworkReply::finished, this, &ModelDownloader::handleProbeFinished_);
}

void ModelDownloader::handleProbeFinished_() {
    QNetworkReply* r = reply_;
    reply_ = nullptr;
    probing_ = false;
    if (!r) return;

    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool canceled = canceled_;
    const bool ok = (r->error() == QNetworkReply::NoError);
    const QString errText = ok ? QString() : r->errorString();
    r->deleteLater();
    if (canceled) return;

    const qint64 destSize = QFileInfo(destAbsPath_).size();

    if (!ok && status != 416) {
        if (retryCount_ < kMaxRetries_) {
            scheduleRetry_(errText);
            return;
        }
        fail_(QStringLiteral("校验失败：%1").arg(errText));
        return;
    }

    if (status == 206) {
        // 服务器认为该偏移之后还有数据 → 已存在文件不完整，转为 .part 续传
        // （同时修复历史半成品文件）；.part 与正式文件取较大者
        const qint64 partSize = QFileInfo(tempPath_).exists()
                                    ? QFileInfo(tempPath_).size() : -1;
        if (partSize > destSize) {
            QFile::remove(destAbsPath_);
        } else {
            QFile::remove(tempPath_);
            if (!QFile::rename(destAbsPath_, tempPath_)) {
                fail_(QStringLiteral("无法转为续传文件：%1").arg(destAbsPath_));
                return;
            }
        }
        downloadCurrentFile_();  // 此时正式文件不存在，走 .part 断点续传
        return;
    }

    // 416（超出文件末尾）或 200（服务器不支持 Range）→ 视为完整，跳过
    totalBytes_ += destSize;
    QFile::remove(tempPath_);  // 清掉可能残留的 .part
    emit statusChanged(QStringLiteral("已存在（校验通过）：%1")
                           .arg(QFileInfo(destAbsPath_).fileName()));
    startNext_();
}

void ModelDownloader::onTick_() {
    if (probing_) return;
    const qint64 nowBytes = resumePos_ + curReceived_;
    const qint64 dt = tickClock_->elapsed();
    if (dt <= 0) return;
    const qint64 bps = (nowBytes - lastTickBytes_) * 1000 / dt;
    lastTickBytes_ = nowBytes;
    tickClock_->restart();
    emit speedUpdated(bps);

    // 停滞看门狗：长时间几乎无数据也主动断开，走自动重试 + 续传，
    // 避免“看起来卡死”干等 Qt 的 60s 无数据超时
    if (bps < kStallBps) {
        if (++zeroStreak_ >= kStallSeconds) {
            zeroStreak_ = 0;
            emit statusChanged(QStringLiteral("连接停滞超过 %1 秒，自动重连…")
                                   .arg(kStallSeconds));
            if (reply_) reply_->abort();  // → 错误路径 → 退避重试 + 续传
        }
    } else {
        zeroStreak_ = 0;
    }
}

void ModelDownloader::scheduleRetry_(const QString& lastError) {
    const int delay = kRetryBackoffMs[qMin(retryCount_, 4)];
    ++retryCount_;
    tickTimer_->stop();
    zeroStreak_ = 0;
    emit speedUpdated(-1);
    emit statusChanged(QStringLiteral("网络中断（%1），%2 秒后自动重试（第 %3/%4 次）")
                           .arg(lastError)
                           .arg(delay / 1000)
                           .arg(retryCount_)
                           .arg(kMaxRetries_));
    retryTimer_->start(delay);
}

void ModelDownloader::writeChunk_() {
    if (!reply_ || !file_ || !reply_->isOpen()) return;

    // 首包前核对状态码：服务器忽略 Range 仍返回 200 时，回退全量重写
    if (!rangeChecked_) {
        rangeChecked_ = true;
        const QVariant code =
            reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (code.isValid() && code.toInt() == 200 && resumePos_ > 0) {
            file_->close();
            if (!file_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                fail_(QStringLiteral("无法重建临时文件：%1").arg(file_->errorString()));
                return;
            }
            resumePos_ = 0;
            lastTickBytes_ = 0;
        }
    }
    file_->write(reply_->readAll());
}

void ModelDownloader::handleFinished_() {
    QNetworkReply* r = reply_;
    reply_ = nullptr;
    if (!r) return;

    const int httpStatus =
        r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool canceled = canceled_;
    const bool ok = (r->error() == QNetworkReply::NoError);
    const QString errText = ok ? QString() : r->errorString();
    r->deleteLater();
    if (file_) file_->close();
    tickTimer_->stop();

    // 用户取消/析构：保留 .part，下次下载可从断点继续
    if (canceled) return;

    // 服务器告知的大小与落地文件不符 → 连接提前关闭但 Qt 未报错，
    // 绝不能把半成品改名（会覆盖完整文件）；按可续传错误处理
    const qint64 fileSize = QFileInfo(tempPath_).size();
    const bool incomplete = ok && expectedFileBytes_ > 0 && fileSize < expectedFileBytes_;

    if (!ok || incomplete) {
        // 416 Range Not Satisfiable：.part 已是完整大小，视为完成
        if (!ok && httpStatus == 416) {
            totalBytes_ += fileSize;
            startNext_();
            return;
        }
        if (retryCount_ < kMaxRetries_) {
            scheduleRetry_(incomplete
                ? QStringLiteral("连接提前关闭（%1/%2 MB）")
                      .arg(fileSize / 1024 / 1024)
                      .arg(expectedFileBytes_ / 1024 / 1024)
                : errText);
            return;
        }
        fail_(QStringLiteral("%1（已重试 %2 次；重新点击“下载”可从断点继续）")
                  .arg(incomplete ? QStringLiteral("传输不完整") : errText)
                  .arg(kMaxRetries_));
        return;
    }
    retryCount_ = 0;

    // 校验通过：临时文件重命名为正式文件
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
    tickTimer_->stop();
    emit speedUpdated(-1);
    emit statusChanged(QStringLiteral("下载完成：%1").arg(active_.name));
    emit downloaded(active_.destRelPath);
}

void ModelDownloader::fail_(const QString& error) {
    downloading_ = false;
    tickTimer_->stop();
    retryTimer_->stop();
    emit speedUpdated(-1);
    // 注意：.part 保留，重新点击“下载”可从断点继续
    emit downloadFailed(active_, error);
}

void ModelDownloader::cancel() {
    if (!downloading_) return;
    canceled_ = true;
    retryTimer_->stop();
    tickTimer_->stop();
    if (reply_) { reply_->abort(); reply_ = nullptr; }
    if (file_) { file_->close(); }
    // 保留 .part 临时文件，下次下载从断点继续
    downloading_ = false;
    emit speedUpdated(-1);
    emit statusChanged(QStringLiteral("下载已取消（已下载部分保留，可续传）"));
}

}  // namespace voice_agent::gui
}  // namespace voice_agent
