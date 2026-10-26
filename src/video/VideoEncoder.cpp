#include "VideoEncoder.h"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {

#ifdef _WIN32

// Wraps a string in double quotes for cmd.exe, which _popen runs the command
// through, so a path with spaces or other special characters reaches ffmpeg
// intact. Windows file names cannot contain double quotes, so nothing inside
// needs escaping. cmd.exe still expands %NAME% inside quotes, so a path that
// contains an environment variable's name between percent signs gets changed.
std::string shellQuote(const std::string& text)
{
    return "\"" + text + "\"";
}

// Starts `command` with its stdin connected to the returned stream.
FILE* openPipe(const std::string& command)
{
    // Binary mode: in text mode every 0x0A byte of a frame would be written
    // as 0x0D 0x0A. Windows has no SIGPIPE; a write after ffmpeg has exited
    // simply fails.
    return _popen(command.c_str(), "wb");
}

int closePipe(FILE* pipe)
{
    return _pclose(pipe);
}

#else

// Wraps a string in single quotes for the shell, so a path with spaces or
// other special characters reaches ffmpeg intact.
std::string shellQuote(const std::string& text)
{
    std::string quoted = "'";
    for (char c : text) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

// Starts `command` with its stdin connected to the returned stream.
FILE* openPipe(const std::string& command)
{
    // If ffmpeg exits early, writing to the pipe should fail with an error
    // ignore the SIGPIPE signal coming from the kernel. (otherwise our process is also killed)
    std::signal(SIGPIPE, SIG_IGN);
    // POSIX popen takes no "b": its streams are always binary.
    return popen(command.c_str(), "w");
}

int closePipe(FILE* pipe)
{
    return pclose(pipe);
}

#endif

} // namespace


VideoEncoder::VideoEncoder(const VideoSettings& settings)
    : _frameBytes(static_cast<size_t>(settings.width) * settings.height * 3)   // 3 bytes for each pixel (RGB)
{
    const bool h265 = settings.codec == "h265";
    const std::string quality = std::to_string(settings.quality);

    std::string command = "ffmpeg -hide_banner -loglevel error -y";
    // Input: raw RGB frames on stdin.
    command += " -f rawvideo -pix_fmt rgb24";
    command += " -s " + std::to_string(settings.width) + "x" + std::to_string(settings.height);
    command += " -r " + std::to_string(settings.framesPerSecond);
    command += " -i -";
    // Output: 4:2:0, which every player handles, converted and tagged as
    // BT.709 so the colours are not shifted on playback.
    command += " -vf scale=out_color_matrix=bt709";
    if (settings.useGpu) {
        // Constant quality: -cq plays the part of -crf. NVENC still caps the
        // bitrate unless told otherwise, and fractal detail needs far more
        // than its default, so the cap is set too high to be reached.
        command += std::string(" -c:v ") + (h265 ? "hevc_nvenc" : "h264_nvenc");
        command += " -rc vbr -cq " + quality + " -b:v 0 -maxrate 500M -bufsize 1000M";
        command += " -preset p5 -pix_fmt yuv420p";
    } else {
        command += std::string(" -c:v ") + (h265 ? "libx265" : "libx264");
        command += " -crf " + quality;
        command += " -preset medium -pix_fmt yuv420p";
    }
    command += " -colorspace bt709 -color_primaries bt709 -color_trc bt709";
    if (h265) {
        // Lets Apple players open H.265 in an MP4.
        command += " -tag:v hvc1";
    }
    // Puts the index at the front so the file starts playing before it has fully loaded.
    command += " -movflags +faststart";
    command += " " + shellQuote(settings.outputPath);

    // The shell's own complaint, such as ffmpeg not being found, lands there too.
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    _logPath = (std::filesystem::temp_directory_path() / ("fractasmic-ffmpeg-" + std::to_string(stamp) + ".log")).string();
    command += " 2> " + shellQuote(_logPath);

    _pipe = openPipe(command);
    if (!_pipe) {
        throw std::runtime_error("Could not start ffmpeg. Is it installed and on the PATH?");
    }
}


VideoEncoder::~VideoEncoder()
{
    if (_pipe) {
        closePipe(_pipe);
    }
    std::error_code ignored;
    std::filesystem::remove(_logPath, ignored);
}


std::string VideoEncoder::ffmpegMessage() const
{
    // The last few lines are where the reason is.
    std::ifstream log(_logPath);
    std::vector<std::string> lines;
    for (std::string line; std::getline(log, line);) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    if (lines.empty()) return "ffmpeg gave no reason.";

    std::string message;
    for (size_t i = lines.size() > 3 ? lines.size() - 3 : 0; i < lines.size(); ++i) {
        message += (message.empty() ? "" : "\n") + lines[i];
    }
    return message;
}


void VideoEncoder::writeFrame(const std::vector<uint8_t>& rgb)
{
    if (rgb.size() != _frameBytes || std::fwrite(rgb.data(), 1, _frameBytes, _pipe) != _frameBytes) {
        // Let ffmpeg finish writing its reason before reading it.
        closePipe(_pipe);
        _pipe = nullptr;
        throw std::runtime_error("ffmpeg stopped accepting frames:\n" + ffmpegMessage());
    }
}


void VideoEncoder::finish()
{
    const int status = closePipe(_pipe);
    _pipe = nullptr;
    if (status != 0) {
        throw std::runtime_error("ffmpeg reported an error, so the video may be incomplete:\n" + ffmpegMessage());
    }
}
