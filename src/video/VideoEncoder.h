#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct VideoSettings
{
    std::string outputPath;
    uint32_t width = 0;
    uint32_t height = 0;
    int framesPerSecond = 60;
    std::string codec = "h264";     // "h264" or "h265"
    int quality = 18;               // CRF: lower is better and larger
    bool useGpu = true;             // NVIDIA NVENC instead of libx264/libx265
};

// Encodes frames by piping them to an `ffmpeg` process. Using the command-line
// tool keeps the encoder out of the build: ffmpeg only has to be on the PATH
// when a video is assembled. GPU encoding needs an NVIDIA card and an ffmpeg
// built with NVENC.
class VideoEncoder
{
public:
    // Starts ffmpeg; throws std::runtime_error if it cannot be started.
    explicit VideoEncoder(const VideoSettings& settings);
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    // Appends one frame of tightly packed 8-bit RGB, top row first. Throws,
    // with ffmpeg's own message, if it has stopped accepting data.
    void writeFrame(const std::vector<uint8_t>& rgb);

    // Ends the stream and waits for ffmpeg to write the file. Throws, with
    // ffmpeg's own message, if it reported failure.
    void finish();

private:
    FILE* _pipe = nullptr;
    size_t _frameBytes = 0;    // number of bytes in a single frame.

    // ffmpeg's error output goes to this temporary file, so a failure can be
    // reported with ffmpeg's reason even without a terminal to show it.
    std::string _logPath;
    // The end of that output, for an error message.
    std::string ffmpegMessage() const;
};
