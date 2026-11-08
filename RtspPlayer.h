#pragma once

#include "BlockingQueue.h"

#include <SDL3/SDL.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class RtspPlayer
{
public:
    RtspPlayer() = default;
    ~RtspPlayer();

    RtspPlayer(const RtspPlayer&) = delete;
    RtspPlayer(RtspPlayer&&) = delete;
    RtspPlayer& operator=(const RtspPlayer&) = delete;
    RtspPlayer& operator=(RtspPlayer&&) = delete;

public:
    bool Open(const std::string& url);
    void Play();
    void Stop();

private:
    struct FormatDeleter
    {
        void operator()(AVFormatContext* p) const
        {
            if (p) avformat_close_input(&p);
        }
    };
    struct CodecDeleter
    {
        void operator()(AVCodecContext* p) const
        {
            if (p) avcodec_free_context(&p);
        }
    };
    struct PacketDeleter
    {
        void operator()(AVPacket* p) const
        {
            if (p) av_packet_free(&p);
        }
    };
    struct FrameDeleter
    {
        void operator()(AVFrame* p) const
        {
            if (p) av_frame_free(&p);
        }
    };
    struct SwrDeleter
    {
        void operator()(SwrContext* p) const
        {
            if (p) swr_free(&p);
        }
    };
    struct SwsDeleter
    {
        void operator()(SwsContext* p) const
        {
            if (p) sws_freeContext(p);
        }
    };
    struct WindowDeleter
    {
        void operator()(SDL_Window* p) const
        {
            if (p) SDL_DestroyWindow(p);
        }
    };
    struct RendererDeleter
    {
        void operator()(SDL_Renderer* p) const
        {
            if (p) SDL_DestroyRenderer(p);
        }
    };
    struct TextureDeleter
    {
        void operator()(SDL_Texture* p) const
        {
            if (p) SDL_DestroyTexture(p);
        }
    };
    struct AudioStreamDeleter
    {
        void operator()(SDL_AudioStream* p) const
        {
            if (p) SDL_DestroyAudioStream(p);
        }
    };

    using FormatPtr = std::unique_ptr<AVFormatContext, FormatDeleter>;
    using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;
    using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
    using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
    using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;
    using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;
    using WindowPtr = std::unique_ptr<SDL_Window, WindowDeleter>;
    using RendererPtr = std::unique_ptr<SDL_Renderer, RendererDeleter>;
    using TexturePtr = std::unique_ptr<SDL_Texture, TextureDeleter>;
    using AudioStreamPtr = std::unique_ptr<SDL_AudioStream, AudioStreamDeleter>;

    struct VideoFrame
    {
        FramePtr frame;
        double pts = 0.0; // normalized seconds
    };

    struct AudioBuffer
    {
        std::vector<std::uint8_t> data;
        double pts = 0.0; // normalized seconds
    };

    bool InitializeFFmpeg(const std::string& url);
    bool InitializeVideoDecoder();
    bool InitializeAudioDecoder();
    bool InitializeSDL();
    bool InitializeAudioOutput();

    void StartThreads();
    void StopThreads();

    void DemuxThread(std::stop_token stopToken);
    void VideoDecodeThread(std::stop_token stopToken);
    void AudioDecodeThread(std::stop_token stopToken);

    bool DecodeVideoPacket(AVPacket* packet);
    bool DecodeAudioPacket(AVPacket* packet);
    bool ReceiveVideoFrames();
    bool ReceiveAudioFrames();
    bool FlushVideoDecoder();
    bool FlushAudioDecoder();

    bool ConvertAudioFrame(AVFrame* frame);
    bool FlushResampler();

    void NotifyDataReady();
    void HandleDataReady();
    void SubmitAudio();
    void ScheduleVideoFrame();
    void RenderVideoFrame(AVFrame* frame);

    double NormalizeTimestamp(std::int64_t timestamp, AVRational timeBase) const;
    double GetAudioClock() const;
    double GetVideoClock() const;
    double GetMasterClock() const;
    int GetNextVideoWaitTimeout() const;

    static int InterruptCallback(void* opaque);
    static void LogFFmpegError(const char* operation, int error);
    void PrintStatistics();

private:
    // FFmpeg objects
    FormatPtr m_formatContext;
    CodecPtr m_videoCodecContext;
    CodecPtr m_audioCodecContext;
    SwsPtr m_swsContext;
    SwrPtr m_swrContext;

    AVStream* m_videoAVStream{ nullptr };
    AVStream* m_audioAVStream{ nullptr };

    int m_videoStreamIndex{ -1 };
    int m_audioStreamIndex{ -1 };
    double m_timestampOrigin{ 0.0 };

    // SDL objects (created, used and destroyed on the main thread)
    WindowPtr m_window;
    RendererPtr m_renderer;
    TexturePtr m_texture;
    AudioStreamPtr m_sdlAudioStream;
    AVFrame* m_yuvFrame{ nullptr };
    bool m_sdlInitialized{ false };

    Uint32 m_dataReadyEvent{ 0 };
    std::atomic<bool> m_dataReadyEventPending{ false };

    // Queues
    BlockingQueue<PacketPtr> m_videoPacketQueue{ 128 };
    BlockingQueue<PacketPtr> m_audioPacketQueue{ 128 };
    BlockingQueue<VideoFrame> m_videoFrameQueue{ 60 };
    BlockingQueue<AudioBuffer> m_audioBufferQueue{ 32 };

    // Worker threads
    std::jthread m_demuxThread;
    std::jthread m_videoDecodeThread;
    std::jthread m_audioDecodeThread;

    std::atomic<bool> m_stopping{ false };
    std::atomic<bool> m_videoDone{ false };
    std::atomic<bool> m_audioDone{ false };

    // Video decoder thread only
    double m_videoNextPts{ 0.0 };
    bool m_videoNextPtsValid{ false };

    // Audio decoder thread only
    double m_audioNextPts{ 0.0 };
    bool m_audioNextPtsValid{ false };

    // Main thread only: pending video frame and video clock
    std::optional<VideoFrame> m_pendingVideoFrame;
    bool m_videoClockStarted{ false };
    double m_videoClockBasePts{ 0.0 };
    std::chrono::steady_clock::time_point m_videoClockBaseTime{};

    // Main thread only: audio clock
    bool m_audioClockStarted{ false };
    double m_audioClockBasePts{ 0.0 };
    std::int64_t m_audioSamplesSubmitted{ 0 };

    // FPS stats
    std::chrono::steady_clock::time_point m_lastStatsTime{};
    std::atomic<uint64_t> m_decodedFrameCount{ 0 };
    std::atomic<uint64_t> m_renderedFrameCount{ 0 };
    std::uint64_t m_lastDecodedFrameCount{ 0 };
    std::uint64_t m_lastRenderedFrameCount{ 0 };
};
