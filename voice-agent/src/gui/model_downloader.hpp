// src/gui/model_downloader.hpp
#pragma once
#include <QObject>
#include <QPair>
#include <QString>
#include <QUrl>
#include <QVector>
#include "gui/model_catalog.hpp"

class QFile;
class QElapsedTimer;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace voice_agent {
namespace gui {

// ========== 模型下载器 ==========
// 基于 Qt6::Network，支持 HTTPS、跳转、流式写盘与进度/错误上报。
// 断点续传：中断/失败/取消均保留 .part 临时文件，下次下载用 HTTP Range 续传；
//           服务器不支持 Range（返回 200）时自动回退全量重下。
// 完整性校验：按 Content-Length 核对落地文件大小，连接提前关闭（Qt 未报错）
//             不会误存半成品；已存在的正式文件用 Range 探测校验，完整则跳过，
//             不完整则自动转为 .part 续传（修复历史半成品文件）。
// 自动重试：网络错误按 1/2/4/8/15s 指数退避重试，最多 5 次；单次请求每有
//           10MB 实际进度就重置重试预算（持续有进度就不封顶）。
// 停滞看门狗：连续 30 秒速度 <1KB/s 视为连接停滞，主动断开并重连续传。
// 镜像：huggingface.co 可映射到 hf-mirror.com（QSettings download/hf_mirror，默认开）；
//       注意 hf-mirror 对部分 LFS 文件会 308 跳回官方源。
// 速度：每秒发一次 speedUpdated（bps<0 表示清除速度显示）。
// 每次只下载一个模型条目：主文件 + 附加文件顺序下载；
// 需要时（extractTarBz2）用系统 tar 解压到目标目录。全部完成后才发 downloaded。
class ModelDownloader final : public QObject {
    Q_OBJECT

public:
    explicit ModelDownloader(QObject* parent = nullptr);
    ~ModelDownloader() override;

    // 开始下载；若正在下载则忽略。destRelPath 相对 models/ 目录。
    // 若存在同名 .part 临时文件则自动从断点续传。
    void start(const ModelEntry& entry);

    // 取消当前下载（保留 .part 临时文件，下次可续传）
    void cancel();

    bool isDownloading() const { return downloading_; }

signals:
    void progressChanged(int percent, qint64 bytesReceived, qint64 bytesTotal);
    void speedUpdated(qint64 bytesPerSecond);          // 每秒一次；<0 清除速度显示
    void statusChanged(const QString& text);           // 阶段文本（开始/重试/解压/完成）
    void downloaded(const QString& destRelPath);       // 整个条目完成（含附加文件/解压）
    void downloadFailed(const ModelEntry& entry, const QString& error);

private:
    void startNext_();
    void downloadCurrentFile_();   // 下载 queueIndex_ 指向的文件（重试也走这里）
    void startProbe_(const QUrl& url);   // Range 探测已存在文件是否完整
    void handleProbeFinished_();
    void onTick_();                // 每秒统计速度 + 停滞看门狗
    void scheduleRetry_(const QString& lastError);
    void handleFinished_();
    void writeChunk_();
    void extractAndFinish_();
    void finishOk_();
    void fail_(const QString& error);
    static QUrl applyMirror_(const QUrl& url);

    QNetworkAccessManager* mgr_ = nullptr;
    QNetworkReply* reply_ = nullptr;
    QFile* file_ = nullptr;
    ModelEntry active_;
    QVector<QPair<QUrl, QString>> queue_;  // (url, 相对 models/ 路径)
    int queueIndex_ = -1;
    QString tempPath_;
    QString destAbsPath_;
    qint64 totalBytes_ = 0;   // 已完成文件的累计字节（跨文件，用于整体进度）
    qint64 resumePos_ = 0;    // 当前文件 .part 已有字节（Range 起点）
    qint64 curReceived_ = 0;  // 当前请求已接收字节（不含断点部分）
    qint64 expectedFileBytes_ = -1; // 本次请求期望的总大小（断点 + Content-Length）
    qint64 resetMark_ = 0;    // 重试预算重置标记（当前请求内已接收字节）
    int zeroStreak_ = 0;      // 连续停滞秒数（看门狗）
    bool rangeChecked_ = false;  // 已核对服务器是否接受 Range
    bool probing_ = false;       // 正在 Range 探测已存在文件
    bool downloading_ = false;
    bool canceled_ = false;

    // 自动重试与速度统计
    int retryCount_ = 0;
    static constexpr int kMaxRetries_ = 5;
    QTimer* retryTimer_ = nullptr;
    QTimer* tickTimer_ = nullptr;
    QElapsedTimer* tickClock_ = nullptr;
    qint64 lastTickBytes_ = 0;
};

}  // namespace voice_agent::gui
}  // namespace voice_agent
