#include "VideoAssembler.h"

#include "FrameRenderer.h"
#include "VideoEncoder.h"
#include "keyframes/KeyframeSequence.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>

VideoPlan planVideo(const VideoOptions& options, uint32_t keyframeWidth, uint32_t keyframeHeight, size_t keyframeCount)
{
    VideoPlan plan;

    if (options.outputPath.empty()) {
        plan.problem = "Choose a file to write the video to.";
    } else if (std::filesystem::path(options.outputPath).extension().empty()) {
        plan.problem = "The video file needs an extension, such as .mp4; it picks the container.";
    } else if (options.framesPerSecond <= 0 || !(options.secondsPerKeyframe > 0.0)) {
        plan.problem = "The frame rate and the seconds per keyframe must be positive.";
    } else if (options.quality < 0 || options.quality > 51) {
        plan.problem = "The quality must be from 0 to 51.";
    } else if (keyframeWidth == 0 || keyframeHeight == 0 || keyframeCount == 0) {
        plan.problem = "The sequence has no keyframes.";
    }
    if (!plan.problem.empty()) return plan;

    // The height follows the keyframes' aspect ratio, rounded to an even
    // number: 4:2:0 video needs both dimensions even.
    const uint32_t width = options.width == 0 ? keyframeWidth : options.width;
    const uint32_t height = 2 * static_cast<uint32_t>(std::lround(width * static_cast<double>(keyframeHeight)
                                                                  / (2.0 * keyframeWidth)));
    if (width % 2 != 0 || width < 2 || height < 2) {
        plan.problem = "The video's width must be even, and at least 2.";
        return plan;
    }

    // One keyframe step per `secondsPerKeyframe`; the last frame is the last
    // keyframe itself.
    const double framesPerKeyframe = options.framesPerSecond * options.secondsPerKeyframe;
    plan.width = width;
    plan.height = height;
    plan.frameCount = static_cast<size_t>(std::floor(static_cast<double>(keyframeCount - 1) * framesPerKeyframe)) + 1;
    return plan;
}


VideoAssembler::VideoAssembler(VideoOptions options)
    : _options(std::move(options))
{
    const KeyframeSequence sequence(_options.inputDir);
    _plan = planVideo(_options, sequence.width(), sequence.height(), sequence.count());
    if (!_plan.problem.empty()) {
        throw std::runtime_error(_plan.problem);
    }

    _startTime = std::chrono::steady_clock::now();
    _thread = std::thread([this] { run(); });
}


VideoAssembler::~VideoAssembler()
{
    _cancelRequested = true;
    if (_thread.joinable()) {
        _thread.join();
    }
}


double VideoAssembler::elapsedSeconds() const
{
    if (_finished) return _totalSeconds;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - _startTime).count();
}


void VideoAssembler::run()
{
    try {
        assemble();
    } catch (const std::exception& e) {
        _error = e.what();
    }

    // A cancelled video stops part way; ffmpeg closes it as a shorter file
    // that would only be mistaken for a finished one.
    if (_cancelRequested) {
        std::error_code ignored;
        std::filesystem::remove(_options.outputPath, ignored);
    }

    _totalSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - _startTime).count();
    _finished = true;
}


void VideoAssembler::assemble()
{
    const KeyframeSequence sequence(_options.inputDir);

    VideoSettings settings;
    settings.outputPath = _options.outputPath;
    settings.width = _plan.width;
    settings.height = _plan.height;
    settings.framesPerSecond = _options.framesPerSecond;
    settings.codec = _options.codec;
    settings.quality = _options.quality;
    settings.useGpu = _options.useGpu;
    VideoEncoder encoder(settings);

    // One core is left for the window, which stays responsive meanwhile. The
    // count is 0 where the system does not tell.
    const unsigned threadCount = std::max(2u, std::thread::hardware_concurrency()) - 1;
    FrameRenderer renderer(_plan.width, _plan.height, palettes()[static_cast<size_t>(_options.palette)], threadCount);

    const double framesPerKeyframe = _options.framesPerSecond * _options.secondsPerKeyframe;
    const size_t lastKeyframe = sequence.count() - 1;

    // Only the two keyframes around the current frame are kept in memory.
    size_t outerIndex = 0;
    Keyframe outer = sequence.load(0);
    Keyframe inner;
    bool hasInner = false;
    if (lastKeyframe > 0) {
        inner = sequence.load(1);
        hasInner = true;
    }

    const double firstLog2Zoom = sequence.log2Zoom(0);

    for (size_t frame = 0; frame < _plan.frameCount; ++frame) {
        if (_cancelRequested) return;

        // Position in the sequence, in keyframes: the whole part picks the
        // outer keyframe, the fraction is how far the zoom is towards the next.
        const double position = std::min(static_cast<double>(frame) / framesPerKeyframe, static_cast<double>(lastKeyframe));
        const size_t index = std::min(static_cast<size_t>(std::floor(position)), lastKeyframe);
        const double fraction = position - static_cast<double>(index);

        // Step forward: the inner keyframe becomes the outer one.
        while (outerIndex < index) {
            outerIndex++;
            outer = std::move(inner);
            hasInner = outerIndex < lastKeyframe;
            if (hasInner) {
                inner = sequence.load(outerIndex + 1);
            }
        }

        // Each keyframe doubles the zoom, so the position counts doublings.
        const float phase = palettePhase(firstLog2Zoom + position, _options.paletteZoomSpeed);

        encoder.writeFrame(renderer.render(outer, hasInner ? &inner : nullptr, fraction, phase));
        _framesDone = frame + 1;
    }

    encoder.finish();
}
