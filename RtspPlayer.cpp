#include "RtspPlayer.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace
{
    constexpr int AudioSampleRate = 48000;
    constexpr int AudioChannels = 2;
    constexpr int BytesPerSample = 2;
    constexpr int BytesPerAudioFrame = AudioChannels * BytesPerSample;
    constexpr double kLateFrameThresholdSeconds = 0.100;
    constexpr double kEarlyFrameThresholdSeconds = 0.003;

    bool IsValidTimestamp(double value)
    {
        return std::isfinite(value);
    }
}

RtspPlayer::~RtspPlayer()
{
    Stop();
}

void RtspPlayer::LogFFmpegError(const char* operation, int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, buffer, sizeof(buffer));
    std::cerr << operation << " failed: " << buffer << '\n';
}

bool RtspPlayer::Open(const std::string& url)
{
    Stop();

    m_videoPacketQueue.Reset();
    m_audioPacketQueue.Reset();
    m_videoFrameQueue.Reset();
    m_audioBufferQueue.Reset();

    m_stopping.store(false);
    m_videoDone.store(false);
    m_audioDone.store(false);
    m_dataReadyEventPending.store(false);

    m_videoNextPts = 0.0;
    m_videoNextPtsValid = false;
    m_audioNextPts = 0.0;
    m_audioNextPtsValid = false;

    m_videoClockStarted = false;
    m_audioClockStarted = false;
    m_audioSamplesSubmitted = 0;
    m_pendingVideoFrame.reset();

    if (avformat_network_init() < 0)
    {
        std::cerr << "avformat_network_init failed\n";
        Stop();
        return false;
    }

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::cerr << "SDL_Init(video) failed: " << SDL_GetError() << '\n';
        Stop();
        return false;
    }

    m_sdlInitialized = true;

    // Audio is optional. Failure to initialize audio must not prevent
    // video-only playback.
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        std::cerr << "SDL audio initialization unavailable: " << SDL_GetError() << '\n';
    }

    m_dataReadyEvent = SDL_RegisterEvents(1);
    if (m_dataReadyEvent == static_cast<Uint32>(-1))
    {
        std::cerr << "SDL_RegisterEvents failed: " << SDL_GetError() << '\n';
        Stop();
        return false;
    }

    if (!InitializeFFmpeg(url) || !InitializeSDL())
    {
        Stop();
        return false;
    }

    m_audioDone.store(m_audioCodecContext == nullptr);
    return true;
}

bool RtspPlayer::InitializeFFmpeg(const std::string& url)
{
    AVFormatContext* rawFormat = avformat_alloc_context();
    if (!rawFormat)
    {
        std::cerr << "avformat_alloc_context failed\n";
        return false;
    }

    rawFormat->interrupt_callback.callback = &RtspPlayer::InterruptCallback;
    rawFormat->interrupt_callback.opaque = this;

    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    // rw_timeout is in microseconds for FFmpeg protocols that support it.
    av_dict_set(&options, "rw_timeout", "5000000", 0);

    const int openResult = avformat_open_input(&rawFormat, url.c_str(), nullptr, &options);
    av_dict_free(&options);

    if (openResult < 0)
    {
        LogFFmpegError("avformat_open_input", openResult);
        if (rawFormat)
            avformat_free_context(rawFormat);
        return false;
    }

    m_formatContext.reset(rawFormat);

    int result = avformat_find_stream_info(m_formatContext.get(), nullptr);
    if (result < 0)
    {
        LogFFmpegError("avformat_find_stream_info", result);
        return false;
    }

    m_videoStreamIndex = av_find_best_stream(m_formatContext.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    if (m_videoStreamIndex < 0)
    {
        std::cerr << "The input has no decodable video stream\n";
        return false;
    }

    m_videoAVStream = m_formatContext->streams[m_videoStreamIndex];

    m_audioStreamIndex = av_find_best_stream(m_formatContext.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (m_audioStreamIndex >= 0)
        m_audioAVStream = m_formatContext->streams[m_audioStreamIndex];

    // Use one common timestamp origin for both streams
    if (m_formatContext->start_time != AV_NOPTS_VALUE)
    {
        m_timestampOrigin = static_cast<double>(m_formatContext->start_time) / AV_TIME_BASE;
    }
    else
    {
        double earliest = std::numeric_limits<double>::max();

        for (unsigned int i = 0; i < m_formatContext->nb_streams; ++i)
        {
            const AVStream* stream = m_formatContext->streams[i];
            if (stream->start_time == AV_NOPTS_VALUE)
                continue;

            const double start = static_cast<double>(stream->start_time) * av_q2d(stream->time_base);
            earliest = std::min(earliest, start);
        }

        m_timestampOrigin =
            earliest == std::numeric_limits<double>::max() ? 0.0 : earliest;
    }

    if (!InitializeVideoDecoder())
        return false;

    // Audio is optional. If decoder or output setup fails, continue video-only.
    if (m_audioAVStream)
    {
        if (!InitializeAudioDecoder())
        {
            m_audioCodecContext.reset();
            m_audioAVStream = nullptr;
            m_audioStreamIndex = -1;
        }
    }

    return true;
}

bool RtspPlayer::InitializeVideoDecoder()
{
    const AVCodec* codec = avcodec_find_decoder(m_videoAVStream->codecpar->codec_id);
    if (!codec)
    {
        std::cerr << "Video decoder not found\n";
        return false;
    }

    AVCodecContext* raw = avcodec_alloc_context3(codec);
    if (!raw)
        return false;

    m_videoCodecContext.reset(raw);

    int result = avcodec_parameters_to_context(m_videoCodecContext.get(), m_videoAVStream->codecpar);
    if (result < 0)
    {
        LogFFmpegError("avcodec_parameters_to_context(video)", result);
        return false;
    }

    m_videoCodecContext->thread_count = 0;
    m_videoCodecContext->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    result = avcodec_open2(m_videoCodecContext.get(), codec, nullptr);
    if (result < 0)
    {
        LogFFmpegError("avcodec_open2(video)", result);
        return false;
    }

    return true;
}

bool RtspPlayer::InitializeAudioDecoder()
{
    const AVCodec* codec = avcodec_find_decoder(m_audioAVStream->codecpar->codec_id);
    if (!codec)
    {
        std::cerr << "Audio decoder not found; using video-only playback\n";
        return false;
    }

    AVCodecContext* raw = avcodec_alloc_context3(codec);
    if (!raw)
        return false;

    m_audioCodecContext.reset(raw);

    int result = avcodec_parameters_to_context(m_audioCodecContext.get(), m_audioAVStream->codecpar);
    if (result < 0)
    {
        LogFFmpegError("avcodec_parameters_to_context(audio)", result);
        return false;
    }

    m_audioCodecContext->thread_count = 0;

    result = avcodec_open2(m_audioCodecContext.get(), codec, nullptr);
    if (result < 0)
    {
        LogFFmpegError("avcodec_open2(audio)", result);
        return false;
    }

    return true;
}

bool RtspPlayer::InitializeSDL()
{
    const int width = std::max(1, m_videoCodecContext->width);
    const int height = std::max(1, m_videoCodecContext->height);

    m_window.reset(SDL_CreateWindow("FFmpeg RTSP Player", width, height, SDL_WINDOW_RESIZABLE));
    if (!m_window)
    {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << '\n';
        return false;
    }

    m_renderer.reset(SDL_CreateRenderer(m_window.get(), nullptr));
    if (!m_renderer)
    {
        std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << '\n';
        return false;
    }

    m_texture.reset(SDL_CreateTexture(m_renderer.get(), SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, width, height));
    if (!m_texture)
    {
        std::cerr << "SDL_CreateTexture failed: " << SDL_GetError() << '\n';
        return false;
    }

    m_yuvFrame = av_frame_alloc();
    if (!m_yuvFrame)
        return false;

    m_yuvFrame->format = AV_PIX_FMT_YUV420P;
    m_yuvFrame->width = width;
    m_yuvFrame->height = height;

    const int result = av_frame_get_buffer(m_yuvFrame, 32);
    if (result < 0)
    {
        LogFFmpegError("av_frame_get_buffer", result);
        return false;
    }

    if (m_audioCodecContext && !InitializeAudioOutput())
    {
        std::cerr << "Audio output unavailable; continuing video-only\n";
        m_swrContext.reset();
        m_audioCodecContext.reset();
        m_audioAVStream = nullptr;
        m_audioStreamIndex = -1;
    }

    return true;
}

bool RtspPlayer::InitializeAudioOutput()
{
    if (!SDL_WasInit(SDL_INIT_AUDIO))
    {
        std::cerr << "SDL audio subsystem is not initialized\n";
        return false;
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = AudioChannels;
    spec.freq = AudioSampleRate;

    m_sdlAudioStream.reset(SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr));

    if (!m_sdlAudioStream)
    {
        std::cerr << "SDL_OpenAudioDeviceStream failed: " << SDL_GetError() << '\n';
        return false;
    }

    AVChannelLayout outputLayout = AV_CHANNEL_LAYOUT_STEREO;
    SwrContext* rawSwr = nullptr;

    const int result = swr_alloc_set_opts2(&rawSwr, &outputLayout, AV_SAMPLE_FMT_S16, AudioSampleRate, &m_audioCodecContext->ch_layout,
        m_audioCodecContext->sample_fmt, m_audioCodecContext->sample_rate, 0, nullptr);

    av_channel_layout_uninit(&outputLayout);

    if (result < 0)
    {
        swr_free(&rawSwr);
        LogFFmpegError("swr_alloc_set_opts2", result);
        return false;
    }

    m_swrContext.reset(rawSwr);

    const int initResult = swr_init(m_swrContext.get());
    if (initResult < 0)
    {
        LogFFmpegError("swr_init", initResult);
        return false;
    }

    if (!SDL_ResumeAudioStreamDevice(m_sdlAudioStream.get()))
    {
        std::cerr << "SDL_ResumeAudioStreamDevice failed: " << SDL_GetError() << '\n';
        return false;
    }

    return true;
}

double RtspPlayer::NormalizeTimestamp(std::int64_t timestamp, AVRational timeBase) const
{
    if (timestamp == AV_NOPTS_VALUE)
        return std::numeric_limits<double>::quiet_NaN();
    else
        return static_cast<double>(timestamp) * av_q2d(timeBase) - m_timestampOrigin;
}

int RtspPlayer::InterruptCallback(void* opaque)
{
    const auto* player = static_cast<RtspPlayer*>(opaque);
    return player->m_stopping.load() ? 1 : 0;
}

void RtspPlayer::StartThreads()
{
    m_demuxThread = std::jthread([this](std::stop_token token) { DemuxThread(token); });

    m_videoDecodeThread = std::jthread([this](std::stop_token token) { VideoDecodeThread(token); });

    if (m_audioCodecContext)
    {
        m_audioDecodeThread = std::jthread([this](std::stop_token token) { AudioDecodeThread(token); });
    }
    else
    {
        m_audioDone.store(true);
    }
}

void RtspPlayer::StopThreads()
{
    m_stopping.store(true);

    m_demuxThread.request_stop();
    m_videoDecodeThread.request_stop();
    m_audioDecodeThread.request_stop();

    m_videoPacketQueue.Close();
    m_audioPacketQueue.Close();
    m_videoFrameQueue.Close();
    m_audioBufferQueue.Close();

    if (m_demuxThread.joinable())
        m_demuxThread.join();
    if (m_videoDecodeThread.joinable())
        m_videoDecodeThread.join();
    if (m_audioDecodeThread.joinable())
        m_audioDecodeThread.join();
}

void RtspPlayer::Stop()
{
    m_stopping.store(true);
    StopThreads();

    m_pendingVideoFrame.reset();

    if (m_yuvFrame)
        av_frame_free(&m_yuvFrame);

    m_sdlAudioStream.reset();
    m_texture.reset();
    m_renderer.reset();
    m_window.reset();

    m_swrContext.reset();
    m_swsContext.reset();

    m_audioCodecContext.reset();
    m_videoCodecContext.reset();
    m_formatContext.reset();

    m_videoAVStream = nullptr;
    m_audioAVStream = nullptr;
    m_videoStreamIndex = -1;
    m_audioStreamIndex = -1;

    m_videoClockStarted = false;
    m_audioClockStarted = false;
    m_audioSamplesSubmitted = 0;

    m_videoDone.store(false);
    m_audioDone.store(false);

    if (m_sdlInitialized)
    {
        SDL_Quit();
        m_sdlInitialized = false;
    }
}

void RtspPlayer::DemuxThread(std::stop_token stopToken)
{
    while (!stopToken.stop_requested() && !m_stopping.load())
    {
        PacketPtr packet(av_packet_alloc());
        if (!packet)
            break;

        const int result = av_read_frame(m_formatContext.get(), packet.get());
        if (result < 0)
        {
            if (result != AVERROR_EOF && !m_stopping.load())
                LogFFmpegError("av_read_frame", result);
            break;
        }

        if (packet->stream_index == m_videoStreamIndex)
        {
            if (!m_videoPacketQueue.Push(std::move(packet)))
                break;
        }
        else if (packet->stream_index == m_audioStreamIndex)
        {
            if (!m_audioPacketQueue.Push(std::move(packet)))
                break;
        }
    }

    m_videoPacketQueue.Close();
    m_audioPacketQueue.Close();
}

void RtspPlayer::VideoDecodeThread(std::stop_token stopToken)
{
    PacketPtr packet;

    while (!stopToken.stop_requested() && m_videoPacketQueue.Pop(packet, stopToken))
    {
        if (!DecodeVideoPacket(packet.get()))
        {
            std::cerr << "Video decoding failed\n";
            break;
        }
        packet.reset();
    }

    if (!stopToken.stop_requested() && !m_stopping.load())
        FlushVideoDecoder();

    m_videoDone.store(true);
    NotifyDataReady();
}

void RtspPlayer::AudioDecodeThread(std::stop_token stopToken)
{
    PacketPtr packet;

    while (!stopToken.stop_requested() && m_audioPacketQueue.Pop(packet, stopToken))
    {
        if (!DecodeAudioPacket(packet.get()))
        {
            std::cerr << "Audio decoding failed\n";
            break;
        }
        packet.reset();
    }

    if (!stopToken.stop_requested() && !m_stopping.load())
    {
        FlushAudioDecoder();
        FlushResampler();
    }

    m_audioDone.store(true);
    NotifyDataReady();
}

bool RtspPlayer::DecodeVideoPacket(AVPacket* packet)
{
    int result = avcodec_send_packet(m_videoCodecContext.get(), packet);

    if (result == AVERROR(EAGAIN))
    {
        if (!ReceiveVideoFrames())
            return false;

        result = avcodec_send_packet(m_videoCodecContext.get(), packet);
    }

    if (result < 0)
    {
        LogFFmpegError("avcodec_send_packet(video)", result);
        return false;
    }

    return ReceiveVideoFrames();
}

bool RtspPlayer::ReceiveVideoFrames()
{
    while (true)
    {
        FramePtr frame(av_frame_alloc());
        if (!frame)
            return false;

        const int result = avcodec_receive_frame(m_videoCodecContext.get(), frame.get());

        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            return true;

        if (result < 0)
        {
            LogFFmpegError("avcodec_receive_frame(video)", result);
            return false;
        }

        double pts = NormalizeTimestamp(frame->best_effort_timestamp, m_videoAVStream->time_base);

        AVRational frameRate = av_guess_frame_rate(m_formatContext.get(), m_videoAVStream, frame.get());

        double frameDuration = 1.0 / 30.0;
        if (frameRate.num > 0 && frameRate.den > 0)
            frameDuration = av_q2d(av_inv_q(frameRate));

        if (!IsValidTimestamp(pts))
            pts = m_videoNextPtsValid ? m_videoNextPts : 0.0;

        m_videoNextPts = pts + frameDuration;
        m_videoNextPtsValid = true;

        VideoFrame output;
        output.frame = std::move(frame);
        output.pts = pts;

        if (!m_videoFrameQueue.PushDropOldest(std::move(output)))
            return false;

        NotifyDataReady();
    }
}

bool RtspPlayer::FlushVideoDecoder()
{
    int result = avcodec_send_packet(m_videoCodecContext.get(), nullptr);

    if (result == AVERROR(EAGAIN))
    {
        if (!ReceiveVideoFrames())
            return false;
        result = avcodec_send_packet(m_videoCodecContext.get(), nullptr);
    }

    if (result < 0 && result != AVERROR_EOF)
    {
        LogFFmpegError("video decoder flush", result);
        return false;
    }

    return ReceiveVideoFrames();
}

bool RtspPlayer::DecodeAudioPacket(AVPacket* packet)
{
    int result = avcodec_send_packet(m_audioCodecContext.get(), packet);

    if (result == AVERROR(EAGAIN))
    {
        if (!ReceiveAudioFrames())
            return false;

        result = avcodec_send_packet(m_audioCodecContext.get(), packet);
    }

    if (result < 0)
    {
        LogFFmpegError("avcodec_send_packet(audio)", result);
        return false;
    }

    return ReceiveAudioFrames();
}

bool RtspPlayer::ReceiveAudioFrames()
{
    while (true)
    {
        FramePtr frame(av_frame_alloc());
        if (!frame)
            return false;

        const int result = avcodec_receive_frame(m_audioCodecContext.get(), frame.get());

        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            return true;

        if (result < 0)
        {
            LogFFmpegError("avcodec_receive_frame(audio)", result);
            return false;
        }

        if (!ConvertAudioFrame(frame.get()))
            return false;
    }
}

bool RtspPlayer::FlushAudioDecoder()
{
    int result = avcodec_send_packet(m_audioCodecContext.get(), nullptr);

    if (result == AVERROR(EAGAIN))
    {
        if (!ReceiveAudioFrames())
            return false;

        result = avcodec_send_packet(m_audioCodecContext.get(), nullptr);
    }

    if (result < 0 && result != AVERROR_EOF)
    {
        LogFFmpegError("audio decoder flush", result);
        return false;
    }

    return ReceiveAudioFrames();
}

bool RtspPlayer::ConvertAudioFrame(AVFrame* frame)
{
    if (!m_swrContext || frame->sample_rate <= 0)
        return false;

    const int outputCapacity = swr_get_out_samples(m_swrContext.get(), frame->nb_samples);

    if (outputCapacity <= 0)
        return true;

    // Capture the resampler delay before consuming this input frame.
    const double resamplerDelay = static_cast<double>(swr_get_delay(m_swrContext.get(), frame->sample_rate)) / frame->sample_rate;

    AudioBuffer output;
    output.data.resize(static_cast<std::size_t>(outputCapacity) * BytesPerAudioFrame);

    std::uint8_t* outputPlanes[] = { output.data.data() };

    const AVSampleFormat inputFormat = static_cast<AVSampleFormat>(frame->format);

    const int inputPlaneCount = av_sample_fmt_is_planar(inputFormat) ? std::max(1, frame->ch_layout.nb_channels) : 1;

    std::vector<const std::uint8_t*> inputPlanes(static_cast<std::size_t>(inputPlaneCount));

    for (int i = 0; i < inputPlaneCount; ++i)
        inputPlanes[static_cast<std::size_t>(i)] = frame->extended_data[i];

    const int converted = swr_convert(m_swrContext.get(), outputPlanes, outputCapacity, inputPlanes.data(), frame->nb_samples);

    if (converted < 0)
    {
        LogFFmpegError("swr_convert", converted);
        return false;
    }

    if (converted == 0)
        return true;

    output.data.resize(static_cast<std::size_t>(converted) * BytesPerAudioFrame);

    const double inputPts = NormalizeTimestamp(frame->best_effort_timestamp, m_audioAVStream->time_base);

    double pts = IsValidTimestamp(inputPts) ? inputPts - resamplerDelay : (m_audioNextPtsValid ? m_audioNextPts : 0.0);

    // Preserve continuity for small timestamp rounding differences, but accept a real timestamp discontinuity
    if (m_audioNextPtsValid && IsValidTimestamp(inputPts) && std::abs(pts - m_audioNextPts) < 0.050)
    {
        pts = m_audioNextPts;
    }

    output.pts = pts;
    m_audioNextPts = pts + static_cast<double>(converted) / AudioSampleRate;
    m_audioNextPtsValid = true;

    if (!m_audioBufferQueue.Push(std::move(output)))
        return false;

    NotifyDataReady();
    return true;
}

bool RtspPlayer::FlushResampler()
{
    if (!m_swrContext)
        return true;

    while (true)
    {
        const int capacity = swr_get_out_samples(m_swrContext.get(), 0);
        if (capacity <= 0)
            break;

        AudioBuffer output;
        output.data.resize(static_cast<std::size_t>(capacity) * BytesPerAudioFrame);

        std::uint8_t* outputPlanes[] = { output.data.data() };

        const int converted = swr_convert(m_swrContext.get(), outputPlanes, capacity, nullptr, 0);

        if (converted < 0)
        {
            LogFFmpegError("swr_convert(flush)", converted);
            return false;
        }

        if (converted == 0)
            break;

        output.data.resize(static_cast<std::size_t>(converted) * BytesPerAudioFrame);

        output.pts = m_audioNextPtsValid ? m_audioNextPts : 0.0;
        m_audioNextPts = output.pts + static_cast<double>(converted) / AudioSampleRate;
        m_audioNextPtsValid = true;

        if (!m_audioBufferQueue.Push(std::move(output)))
            return false;

        NotifyDataReady();
    }

    return true;
}

void RtspPlayer::NotifyDataReady()
{
    if (m_stopping.load())
        return;

    if (m_dataReadyEventPending.exchange(true))
        return;

    SDL_Event event{};
    event.type = m_dataReadyEvent;

    if (!SDL_PushEvent(&event))
    {
        std::cerr << "SDL_PushEvent failed: " << SDL_GetError() << '\n';
        m_dataReadyEventPending.store(false);
    }
}

void RtspPlayer::HandleDataReady()
{
    SubmitAudio();

    // Clear after consuming available audio. If audio arrived in the
    // meantime, post another wake-up. Video queue is inspected by scheduler.
    m_dataReadyEventPending.store(false);

    if (!m_audioBufferQueue.Empty())
        NotifyDataReady();
}

void RtspPlayer::SubmitAudio()
{
    if (!m_sdlAudioStream)
        return;

    AudioBuffer buffer;

    while (m_audioBufferQueue.TryPop(buffer))
    {
        const int sampleFrames = static_cast<int>(buffer.data.size() / BytesPerAudioFrame);

        if (sampleFrames <= 0)
            continue;

        if (!SDL_PutAudioStreamData(m_sdlAudioStream.get(), buffer.data.data(), static_cast<int>(buffer.data.size())))
        {
            std::cerr << "SDL_PutAudioStreamData failed: " << SDL_GetError() << '\n';
            continue;
        }

        if (!m_audioClockStarted)
        {
            m_audioClockBasePts = buffer.pts;
            m_audioSamplesSubmitted = 0;
            m_audioClockStarted = true;
        }

        m_audioSamplesSubmitted += sampleFrames;
    }
}

double RtspPlayer::GetAudioClock() const
{
    if (!m_sdlAudioStream || !m_audioClockStarted)
        return 0.0;

    const int queuedBytes = SDL_GetAudioStreamQueued(m_sdlAudioStream.get());

    if (queuedBytes < 0)
        return m_audioClockBasePts;

    const std::int64_t queuedSamples = queuedBytes / BytesPerAudioFrame;

    const std::int64_t playedSamples = std::clamp<std::int64_t>(m_audioSamplesSubmitted - queuedSamples, 0, m_audioSamplesSubmitted);

    return m_audioClockBasePts + static_cast<double>(playedSamples) / AudioSampleRate;
}

double RtspPlayer::GetVideoClock() const
{
    if (!m_videoClockStarted)
        return 0.0;

    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_videoClockBaseTime).count();

    return m_videoClockBasePts + elapsed;
}

double RtspPlayer::GetMasterClock() const
{
    if (m_sdlAudioStream && m_audioClockStarted)
        return GetAudioClock();

    return GetVideoClock();
}

void RtspPlayer::ScheduleVideoFrame()
{
    while (true)
    {
        if (!m_pendingVideoFrame)
        {
            VideoFrame next;
            if (!m_videoFrameQueue.TryPop(next))
                return;
            m_pendingVideoFrame = std::move(next);
        }

        // If an audio stream is expected, wait for its clock to start.
        // If audio decoding ended without producing audio, fall back to video.
        if (m_sdlAudioStream && !m_audioClockStarted && !m_audioDone.load())
            return;

        if (!m_videoClockStarted)
        {
            m_videoClockBasePts = m_pendingVideoFrame->pts;
            m_videoClockBaseTime = std::chrono::steady_clock::now();
            m_videoClockStarted = true;
        }

        const double difference = m_pendingVideoFrame->pts - GetMasterClock();

        if (difference > kEarlyFrameThresholdSeconds)
        {
            return;
        }
        else if (difference < -kLateFrameThresholdSeconds)
        {
            m_pendingVideoFrame.reset();
            continue;
        }

        RenderVideoFrame(m_pendingVideoFrame->frame.get());

        m_pendingVideoFrame.reset();
    }
}

int RtspPlayer::GetNextVideoWaitTimeout() const
{
    if (!m_pendingVideoFrame)
        return -1;

    if (m_sdlAudioStream && !m_audioClockStarted && !m_audioDone.load())
        return -1;

    const double difference = m_pendingVideoFrame->pts - GetMasterClock();

    if (difference <= 0.0)
        return 0;

    constexpr double kMaximumWaitSeconds = 1.0;
    const double waitSeconds = std::min(difference, kMaximumWaitSeconds);

    return static_cast<int>(std::ceil(waitSeconds * 1000.0));
}

void RtspPlayer::RenderVideoFrame(AVFrame* frame)
{
    if (!frame || !m_renderer || !m_yuvFrame)
        return;

    const int width = frame->width;
    const int height = frame->height;

    SwsContext* cached = sws_getCachedContext(m_swsContext.release(), width, height, static_cast<AVPixelFormat>(frame->format), width, height,
        AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    m_swsContext.reset(cached);

    if (!m_swsContext)
    {
        std::cerr << "sws_getCachedContext failed\n";
        return;
    }

    if (m_yuvFrame->width != width || m_yuvFrame->height != height)
    {
        av_frame_unref(m_yuvFrame);
        m_yuvFrame->format = AV_PIX_FMT_YUV420P;
        m_yuvFrame->width = width;
        m_yuvFrame->height = height;

        const int allocResult = av_frame_get_buffer(m_yuvFrame, 32);
        if (allocResult < 0)
        {
            LogFFmpegError("av_frame_get_buffer(resize)", allocResult);
            return;
        }

        m_texture.reset(SDL_CreateTexture(m_renderer.get(), SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, width, height));

        if (!m_texture)
        {
            std::cerr << "SDL_CreateTexture(resize) failed: " << SDL_GetError() << '\n';
            return;
        }
    }

    if (av_frame_make_writable(m_yuvFrame) < 0)
        return;

    std::uint8_t* destinationData[4] =
    {
        m_yuvFrame->data[0],
        m_yuvFrame->data[1],
        m_yuvFrame->data[2],
        nullptr
    };

    int destinationLinesize[4] =
    {
        m_yuvFrame->linesize[0],
        m_yuvFrame->linesize[1],
        m_yuvFrame->linesize[2],
        0
    };

    sws_scale(m_swsContext.get(), frame->data, frame->linesize, 0, height, destinationData, destinationLinesize);

    if (!SDL_UpdateYUVTexture(m_texture.get(), nullptr, m_yuvFrame->data[0], m_yuvFrame->linesize[0], m_yuvFrame->data[1], m_yuvFrame->linesize[1], m_yuvFrame->data[2], m_yuvFrame->linesize[2]))
    {
        std::cerr << "SDL_UpdateYUVTexture failed: " << SDL_GetError() << '\n';
        return;
    }

    int outputWidth = 0;
    int outputHeight = 0;
    if (!SDL_GetRenderOutputSize(m_renderer.get(), &outputWidth, &outputHeight))
    {
        outputWidth = width;
        outputHeight = height;
    }

    SDL_FRect destination{ 0.0f, 0.0f, static_cast<float>(outputWidth), static_cast<float>(outputHeight) };

    SDL_RenderClear(m_renderer.get());
    SDL_RenderTexture(m_renderer.get(), m_texture.get(), nullptr, &destination);
    SDL_RenderPresent(m_renderer.get());
}

void RtspPlayer::Play()
{
    if (!m_formatContext || !m_videoCodecContext || !m_renderer)
    {
        std::cerr << "Call Open() successfully before Play()\n";
        return;
    }

    StartThreads();

    bool running = true;
    SDL_Event event{};

    while (running && !m_stopping.load())
    {
        ScheduleVideoFrame();

        const bool videoFinished = m_videoDone.load() && m_videoFrameQueue.Empty() && !m_pendingVideoFrame;

        const bool audioDecodeFinished = m_audioDone.load() && m_audioBufferQueue.Empty();

        int queuedAudioBytes = 0;
        if (m_sdlAudioStream)
            queuedAudioBytes = SDL_GetAudioStreamQueued(m_sdlAudioStream.get());

        if (videoFinished && audioDecodeFinished && queuedAudioBytes <= 0)
            break;

        int timeout = GetNextVideoWaitTimeout();

        // Once decoding is finished, let SDL consume its remaining audio.
        // Polling here is only for the final device drain, not normal playback.
        if (videoFinished && audioDecodeFinished && queuedAudioBytes > 0 && (timeout < 0 || timeout > 20))
        {
            timeout = 20;
        }

        const bool gotEvent = (timeout < 0) ? SDL_WaitEvent(&event) : SDL_WaitEventTimeout(&event, timeout);

        if (!gotEvent)
            continue;

        if (event.type == SDL_EVENT_QUIT)
        {
            running = false;
        }
        else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)
        {
            running = false;
        }
        else if (event.type == m_dataReadyEvent)
        {
            HandleDataReady();
        }
    }

    StopThreads();
}
