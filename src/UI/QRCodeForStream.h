#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <string_view>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
};

#include <QThread>
#include <QMutex>
#include <QtConcurrent/QtConcurrent>
#include <QFuture>
#include <QThreadPool>

#include "ApiDefs.hpp"
#include "ConfigDate.h"
#include "ScannerBase.hpp"

class QRCodeForStream final :
    public QThread,
    public ScannerBase
{
    Q_OBJECT
public:
    QRCodeForStream(QObject* parent = nullptr);
    ~QRCodeForStream();
    Q_DISABLE_COPY_MOVE(QRCodeForStream)

    void setLoginInfo(const std::string_view uid, const std::string_view gameToken);
    void setLoginInfo(const std::string_view uid, const std::string_view gameToken, const std::string& name);
    void setLoginInfo1(const std::string_view uid, const std::string_view stoken, const std::string_view mid);
    void setServerType(const ServerType servertype);
    void setUrl(const std::string& url, const std::map<std::string, std::string> heard = {});
    auto init() -> bool;
    void run();
    void stop();
    void continueLastLogin();

Q_SIGNALS:
    void loginResults(const ScanRet ret);
    void loginConfirm(const GameType gameType, bool b);

private:
    std::mutex mtx;
    void LoginOfficial();
    void LoginBH3BiliBili();
    void setStreamHW();

    // ---- 直播流延迟（新增，见 README「直播流延迟」）----
    // 已编码包 + 其归一到毫秒的时间戳。缓存编码包而不是解码后的帧：
    // 1080p 一帧 BGR 约 6MB，缓冲 10 秒就要 1.8GB，而编码包只要几 MB。
    struct DelayedPacket
    {
        AVPacket* pkt;
        int64_t ptsMs;
    };
    void loadStreamDelay();
    void enqueueDelayed(const AVPacket* pkt);
    bool headPacketDue() const;
    void clearDelayedPackets();
    std::deque<DelayedPacket> m_packetQueue;
    int64_t m_delayMs{ 0 };
    int64_t m_latestPts{ AV_NOPTS_VALUE };
    // ---- 直播流延迟 结束 ----

    std::string streamUrl{};
    std::string m_name;
    ConfigDate* m_config;
    ServerType servertype;
    ScanRet ret = ScanRet::UNKNOW;
    AVDictionary* pAvdictionary;
    AVFormatContext* pAVFormatContext;
    AVCodecContext* pAVCodecContext;
    SwsContext* pSwsContext;
    AVFrame* pAVFrame;
    AVPacket* pAVPacket;
    int videoStreamIndex{ 0 };
    int videoStreamWidth{};
    int videoStreamHeight{};
    const int threadNumber{ 2 };
    QThreadPool threadPool;
    std::atomic<bool> m_stop;
};
