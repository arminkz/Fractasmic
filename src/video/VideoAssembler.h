#pragma once

#include "fractal/Palette.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

// What a video is assembled with.
struct VideoOptions
{
    std::string inputDir;           // keyframe folder with keyframes.json
    std::string outputPath;         // its extension picks the container, e.g. .mp4

    int framesPerSecond = 60;
    // How long the zoom takes to get from one keyframe to the next, that is,
    // to double the magnification.
    double secondsPerKeyframe = 1.0;

    // The video's width; its height follows from the keyframes' aspect ratio.
    // 0 means the keyframes' own width.
    uint32_t width = 0;

    int palette = kDefaultPalette;  // index into palettes()
    // Radians the palette shifts by per doubling of the zoom. 0 fixes the
    // colours to the picture.
    double paletteZoomSpeed = kPalettePhasePerDoubling;

    std::string codec = "h264";     // "h264" or "h265"
    int quality = 18;               // CRF: lower is better and larger
    bool useGpu = true;             // NVIDIA NVENC instead of libx264/libx265
};

// The video `options` make from `keyframeCount` keyframes of
// keyframeWidth x keyframeHeight, or why they cannot make one.
struct VideoPlan
{
    uint32_t width = 0;
    uint32_t height = 0;
    size_t frameCount = 0;
    std::string problem;            // empty if the options are usable
};
VideoPlan planVideo(const VideoOptions& options, uint32_t keyframeWidth, uint32_t keyframeHeight, size_t keyframeCount);

// Turns a keyframe sequence into a video file on a thread of its own. Each
// frame is blended from the two keyframes around it, coloured, and piped to
// ffmpeg; see FrameRenderer and VideoEncoder.
class VideoAssembler
{
public:
    // Reads the sequence and starts. Throws std::runtime_error, before
    // anything is written, if the options do not fit it.
    explicit VideoAssembler(VideoOptions options);
    // Cancels a run still going and waits for it to stop.
    ~VideoAssembler();

    VideoAssembler(const VideoAssembler&) = delete;
    VideoAssembler& operator=(const VideoAssembler&) = delete;

    const VideoOptions& options() const { return _options; }
    const VideoPlan& plan() const { return _plan; }

    // Asks the run to stop after the frame in progress. The unfinished video
    // is deleted.
    void cancel() { _cancelRequested = true; }

    size_t framesDone() const { return _framesDone; }
    double elapsedSeconds() const;

    // Done, failed or cancelled. The rest is only meaningful after this.
    bool finished() const { return _finished; }
    bool cancelled() const { return _cancelRequested; }
    const std::string& error() const { return _error; }    // empty unless it failed

private:
    VideoOptions _options;
    VideoPlan _plan;

    std::atomic<bool> _cancelRequested{ false };
    std::atomic<bool> _finished{ false };
    std::atomic<size_t> _framesDone{ 0 };
    std::chrono::steady_clock::time_point _startTime;
    double _totalSeconds = 0.0;     // set before _finished
    std::string _error;             // set before _finished

    std::thread _thread;

    void run();
    void assemble();
};
