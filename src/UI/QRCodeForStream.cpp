#include "QRCodeForStream.h"

#include <string>
#include <string_view>

#include "QRScanner.h"
#include "MhyApi.hpp"

namespace
{
// 直播流延迟的上限，仅用于兜底，避免误填超大值把内存吃光。
// 队列占用约为「码率 × 延迟」，例如 8Mbps × 60s ≈ 60MB。
constexpr int64_t MAX_STREAM_DELAY_MS{ 60000 };
}

QRCodeForStream::QRCodeForStream(QObject* parent) :
    QThread(parent),
    pAvdictionary(nullptr),
    pAVFormatContext(nullptr),
    pSwsContext(nullptr),
    pAVFrame(nullptr),
    pAVPacket(nullptr),
    pAVCodecContext(nullptr),
    m_stop(false),
    servertype(ServerType::Official)

{
    av_log_set_level(AV_LOG_FATAL);
    m_config = &(ConfigDate::getInstance());
}

QRCodeForStream::~QRCodeForStream()
{
    if (!this->isInterruptionRequested())
    {
        m_stop.store(false);
    }
    this->requestInterruption();
    this->wait();
}

void QRCodeForStream::setLoginInfo(const std::string_view uid, const std::string_view gameToken)
{
    this->uid = uid;
    this->gameToken = gameToken;
}

void QRCodeForStream::setLoginInfo(const std::string_view uid, const std::string_view gameToken, const std::string& name)
{
    this->uid = uid;
    this->gameToken = gameToken;
    this->m_name = name;
}

void QRCodeForStream::setLoginInfo1(const std::string_view uid, const std::string_view stoken, const std::string_view mid)
{
    this->uid = uid;
    this->gameToken = stoken;
    this->mid = mid;
}

void QRCodeForStream::setServerType(const ServerType servertype)
{
    this->servertype = servertype;
}

void QRCodeForStream::LoginOfficial()
{
    // 把「取帧 -> 转 BGR -> 交给线程池识别」原样装进 lambda，
    // 让延迟路径与直通路径共用同一份逻辑，识别部分一个字未改。
    const auto processDecoded = [&]() -> bool
    {
        if (pAVFrame == nullptr)
        {
            std::cerr << "Error allocating frame" << std::endl;
            ret = ScanRet::LIVESTOP;
            return false;
        }
        while (avcodec_receive_frame(pAVCodecContext, pAVFrame) == 0)
        {
            cv::Mat img(videoStreamHeight, videoStreamWidth, CV_8UC3);
            uint8_t* dstData[1] = { img.data };
            const int dstLinesize[1] = { static_cast<int>(img.step) };
            sws_scale(pSwsContext, pAVFrame->data, pAVFrame->linesize, 0, pAVFrame->height,
                      dstData, dstLinesize);
#ifndef SHOW
            cv::imshow("Video_Stream", img);
            cv::waitKey(1);
#endif
            threadPool.tryStart([&, img = std::move(img)]() {
                thread_local QRScanner qrScanners;
                std::string str;
                qrScanners.decodeSingle(img, str);
                std::string ticket;
                if (!parseOfficialQRCode(str, ticket))
                {
                    return;
                }
                if (lastTicket == ticket)
                {
                    return;
                }
                if (mtx.try_lock())
                {
                    if (!m_stop.load())
                    {
                        mtx.unlock();
                        return;
                    }
                    const std::string passportQrUrl = PandaScanQRCode(scanUrl, ticket, gameType);
                    if (!passportQrUrl.empty())
                    {
                        lastTicket = ticket;
                        lastQrCode = passportQrUrl;
                        nlohmann::json config = nlohmann::json::parse(m_config->getConfig());
                        if (config["auto_login"])
                        {
                            continueLastLogin();
                        }
                        else
                        {
                            Q_EMIT loginConfirm(gameType, false);
                        }
                    }
                    else
                    {
                        Q_EMIT loginResults(ScanRet::FAILURE_1);
                    }
                    stop();
                    mtx.unlock();
                }
            });
        }
        av_frame_unref(pAVFrame);
        return true;
    };

    while (m_stop.load())
    {
        if (av_read_frame(pAVFormatContext, pAVPacket) < 0)
        {
            ret = ScanRet::LIVESTOP;
            break;
        }
        if (pAVPacket->stream_index != videoStreamIndex)
        {
            continue;
        }
        if (m_delayMs > 0)
        {
            // 延迟路径：先入队，再只放行「时间戳已落后 delayMs」的包。
            // 读循环保持全速，TCP 不回压，因此延迟精确且有界。
            enqueueDelayed(pAVPacket);
            bool ok = true;
            while (!m_packetQueue.empty() && headPacketDue())
            {
                AVPacket* pkt = m_packetQueue.front().pkt;
                m_packetQueue.pop_front();
                avcodec_send_packet(pAVCodecContext, pkt);
                av_packet_free(&pkt);
                if (!processDecoded())
                {
                    ok = false;
                    break;
                }
            }
            if (!ok)
            {
                break;
            }
        }
        else
        {
            // 未设置延迟：与改动前完全一致的直通路径。
            avcodec_send_packet(pAVCodecContext, pAVPacket);
            if (!processDecoded())
            {
                break;
            }
        }
        av_packet_unref(pAVPacket);
    }
}

void QRCodeForStream::LoginBH3BiliBili()
{
    // 同 LoginOfficial：识别部分原样装进 lambda，两条路径共用。
    const auto processDecoded = [&]() -> bool
    {
        if (pAVFrame == nullptr)
        {
            std::cerr << "Error allocating frame" << std::endl;
            ret = ScanRet::LIVESTOP;
            return false;
        }

        while (avcodec_receive_frame(pAVCodecContext, pAVFrame) == 0)
        {
            cv::Mat img(videoStreamHeight, videoStreamWidth, CV_8UC3);
            uint8_t* dstData[1] = { img.data };
            const int dstLinesize[1] = { static_cast<int>(img.step) };
            sws_scale(pSwsContext, pAVFrame->data, pAVFrame->linesize, 0, pAVFrame->height,
                      dstData, dstLinesize);
#ifndef SHOW
            cv::imshow("Video_Stream", img);
            cv::waitKey(1);
#endif
            threadPool.tryStart([&, img = std::move(img)]() {
                thread_local QRScanner qrScanners;
                std::string str;
                qrScanners.decodeSingle(img, str);
                std::string ticket;
                if (!parseOfficialQRCode(str, ticket) || gameType != GameType::Honkai3)
                {
                    return;
                }
                if (lastTicket == ticket)
                {
                    return;
                }
                if (mtx.try_lock())
                {
                    if (!m_stop.load())
                    {
                        mtx.unlock();
                        return;
                    }
                    if (ret = scanCheck(ticket); ret == ScanRet::SUCCESS)
                    {
                        lastTicket = ticket;
                        nlohmann::json config = nlohmann::json::parse(m_config->getConfig());
                        if (config["auto_login"])
                        {
                            continueLastLogin();
                        }
                        else
                        {
                            Q_EMIT loginConfirm(GameType::Honkai3_BiliBili, false);
                        }
                    }
                    else
                    {
                        Q_EMIT loginResults(ret);
                    }
                    stop();
                    mtx.unlock();
                }
            });
        }
        av_frame_unref(pAVFrame);
        return true;
    };

    while (m_stop.load())
    {
        if (av_read_frame(pAVFormatContext, pAVPacket) < 0)
        {
            ret = ScanRet::LIVESTOP;
            break;
        }
        if (pAVPacket->stream_index != videoStreamIndex)
        {
            continue;
        }
        if (m_delayMs > 0)
        {
            enqueueDelayed(pAVPacket);
            bool ok = true;
            while (!m_packetQueue.empty() && headPacketDue())
            {
                AVPacket* pkt = m_packetQueue.front().pkt;
                m_packetQueue.pop_front();
                avcodec_send_packet(pAVCodecContext, pkt);
                av_packet_free(&pkt);
                if (!processDecoded())
                {
                    ok = false;
                    break;
                }
            }
            if (!ok)
            {
                break;
            }
        }
        else
        {
            avcodec_send_packet(pAVCodecContext, pAVPacket);
            if (!processDecoded())
            {
                break;
            }
        }
        av_packet_unref(pAVPacket);
    }
}

void QRCodeForStream::setStreamHW()
{
    if (pAVCodecContext->width < pAVCodecContext->height ||
        pAVCodecContext->height == 480 ||
        pAVCodecContext->height == 720)
    {
        videoStreamWidth = pAVCodecContext->width;
        videoStreamHeight = pAVCodecContext->height;
    }
    else
    {
        videoStreamWidth = pAVCodecContext->width / 1.5;
        videoStreamHeight = pAVCodecContext->height / 1.5;
    }
}

void QRCodeForStream::stop()
{
    m_stop.store(false);
}

void QRCodeForStream::loadStreamDelay()
{
    m_delayMs = 0;
    m_latestPts = AV_NOPTS_VALUE;
    try
    {
        const nlohmann::json config = nlohmann::json::parse(m_config->getConfig());
        // 用 value() 而不是 []：旧配置文件里没有这个字段时取默认值 0，
        // 此时走直通路径，行为与未加延迟时完全一致。
        int64_t delay = config.value("stream_delay_ms", 0);
        if (delay < 0)
        {
            delay = 0;
        }
        else if (delay > MAX_STREAM_DELAY_MS)
        {
            delay = MAX_STREAM_DELAY_MS;
        }
        m_delayMs = delay;
    }
    catch (const std::exception& e)
    {
        std::cerr << "stream_delay_ms 解析失败，按 0 处理: " << e.what() << std::endl;
        m_delayMs = 0;
    }
    if (m_delayMs > 0)
    {
        std::cout << "直播流延迟已启用: " << m_delayMs << " ms" << std::endl;
    }
}

void QRCodeForStream::enqueueDelayed(const AVPacket* pkt)
{
    AVPacket* copy = av_packet_clone(pkt);
    if (copy == nullptr)
    {
        return;
    }
    // 归一化成毫秒。FLV 的 time_base 本就是 1/1000，这里写成通用换算以兼容其他封装。
    const AVStream* stream = pAVFormatContext->streams[videoStreamIndex];
    const int64_t raw = (copy->pts != AV_NOPTS_VALUE) ? copy->pts : copy->dts;
    int64_t ptsMs = -1;
    if (raw != AV_NOPTS_VALUE)
    {
        ptsMs = av_rescale_q(raw, stream->time_base, AVRational{ 1, 1000 });
        if (m_latestPts == AV_NOPTS_VALUE || ptsMs > m_latestPts)
        {
            m_latestPts = ptsMs;
        }
    }
    m_packetQueue.push_back(DelayedPacket{ copy, ptsMs });
}

bool QRCodeForStream::headPacketDue() const
{
    // m_latestPts 是已入队的最新时间戳，压住它前面 delayMs 这段不放行。
    // 时间戳缺失（极少见）时不拦截，否则闸门永远打不开。
    const int64_t ptsMs = m_packetQueue.front().ptsMs;
    return ptsMs < 0 || m_latestPts == AV_NOPTS_VALUE || ptsMs <= m_latestPts - m_delayMs;
}

void QRCodeForStream::clearDelayedPackets()
{
    for (auto& item : m_packetQueue)
    {
        av_packet_free(&item.pkt);
    }
    m_packetQueue.clear();
    m_latestPts = AV_NOPTS_VALUE;
}

void QRCodeForStream::setUrl(const std::string& url, const std::map<std::string, std::string> heard)
{
    streamUrl = url;
    for (const auto& it : heard)
    {
        av_dict_set(&pAvdictionary, it.first.c_str(), it.second.c_str(), 0);
    }
    av_dict_set(&pAvdictionary, "max_delay", "0", 0);
    av_dict_set(&pAvdictionary, "probesize", "1024", 0);
    av_dict_set(&pAvdictionary, "packetsize", "128", 0);
    av_dict_set(&pAvdictionary, "rtbufsize", "0", 0);
    av_dict_set(&pAvdictionary, "delay", "0", 0);
    av_dict_set(&pAvdictionary, "buffer_size", "1000", 0);
    av_dict_set(&pAvdictionary, "rw_timeout", "5000000", 0);
}

auto QRCodeForStream::init() -> bool
{
    pAVFormatContext = avformat_alloc_context();
    if (avformat_open_input(&pAVFormatContext, streamUrl.c_str(), NULL, &pAvdictionary) != 0)
    {
        std::cerr << "Error opening input file" << std::endl;
        return false;
    }
    if (avformat_find_stream_info(pAVFormatContext, NULL) < 0)
    {
        std::cerr << "Error finding stream information" << std::endl;
        return false;
    }
    AVStream* videoStream = nullptr;
    for (int i = 0; i < pAVFormatContext->nb_streams; i++)
    {
        if (pAVFormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoStream = pAVFormatContext->streams[i];
            break;
        }
    }
    if (videoStream == nullptr)
    {
        std::cerr << "No video stream found" << std::endl;
        return false;
    }
    videoStreamIndex = videoStream->index;
    const AVCodec* decoder{ avcodec_find_decoder(videoStream->codecpar->codec_id) };
    if (decoder == nullptr)
    {
        std::cerr << "Codec not found" << std::endl;
        return false;
    }
    pAVCodecContext = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(pAVCodecContext, videoStream->codecpar);
    if (avcodec_open2(pAVCodecContext, decoder, NULL) < 0)
    {
        std::cerr << "Error opening codec" << std::endl;
        return false;
    }
    setStreamHW();
    pSwsContext = sws_getContext(
        pAVCodecContext->width, pAVCodecContext->height, pAVCodecContext->pix_fmt,
        videoStreamWidth, videoStreamHeight, AV_PIX_FMT_BGR24, SWS_BILINEAR, NULL, NULL, NULL);
    pAVPacket = av_packet_alloc();
    pAVFrame = av_frame_alloc();
    return true;
}

void QRCodeForStream::continueLastLogin()
{
    switch (servertype)
    {
        using enum ServerType;
    case Official:
    {
        bool b = ScanPassportQRLogin(lastQrCode, gameToken, mid) &&
                 ConfirmPassportQRLogin(lastQrCode, gameToken, mid);
        if (b)
        {
            Q_EMIT loginResults(ScanRet::SUCCESS);
        }
        else
        {
            Q_EMIT loginResults(ScanRet::FAILURE_2);
        }
    }
    break;
    case BH3_BiliBili:
    {
        ret = scanConfirm(lastTicket, uid, gameToken, m_name);
        Q_EMIT loginResults(ret);
    }
    break;
    default:
        break;
    }
}

void QRCodeForStream::run()
{
    threadPool.setMaxThreadCount(threadNumber);
    m_stop.store(true);
    ret = ScanRet::UNKNOW;
    clearDelayedPackets();
    loadStreamDelay();
    //TODO 获取直播流地址放在这里
    if (init())
    {
#ifndef SHOW
        cv::namedWindow("Video_Stream", cv::WINDOW_AUTOSIZE);
        cv::resizeWindow("Video_Stream", videoStreamWidth / 2, videoStreamHeight / 2);
#endif
        switch (servertype)
        {
            using enum ServerType;
        case Official:
            LoginOfficial();
            break;
        case BH3_BiliBili:
            LoginBH3BiliBili();
            break;
        default:
            break;
        }
    }
    else
    {
        ret = ScanRet::STREAMERROR;
    }
    if (ret == ScanRet::LIVESTOP || ret == ScanRet::STREAMERROR)
    {
        emit loginResults(ret);
    }
#ifndef SHOW
    cv::destroyWindow("Video_Stream");
#endif
    clearDelayedPackets();
    avformat_close_input(&pAVFormatContext);
    avcodec_free_context(&pAVCodecContext);
    sws_freeContext(pSwsContext);
    av_dict_free(&pAvdictionary);
    av_frame_free(&pAVFrame);
    av_packet_free(&pAVPacket);
    pAVFormatContext = nullptr;
    pAVCodecContext = nullptr;
    pSwsContext = nullptr;
    pAvdictionary = nullptr;
    pAVFrame = nullptr;
    pAVPacket = nullptr;
}
