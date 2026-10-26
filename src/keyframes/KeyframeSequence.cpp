#include "KeyframeSequence.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace {

std::string readTextFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Cannot read " + path);
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// finds a number value of a key in a json object
uint32_t findNumber(const std::string& json, const std::string& key)
{
    const std::regex pattern("\"" + key + "\"\\s*:\\s*(\\d+)");
    std::smatch match;
    if (!std::regex_search(json, match, pattern)) {
        throw std::runtime_error("keyframes.json has no \"" + key + "\".");
    }
    return static_cast<uint32_t>(std::stoul(match[1].str()));
}

// finds a string value of a key in a json object
std::string findString(const std::string& json, const std::string& key)
{
    const std::regex pattern("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch match;
    if (!std::regex_search(json, match, pattern)) {
        throw std::runtime_error("keyframes.json has no \"" + key + "\".");
    }
    return match[1].str();
}

// finds a true/false value of a key in a json object, or `fallback` if the key is missing
bool findFlag(const std::string& json, const std::string& key, bool fallback)
{
    const std::regex pattern("\"" + key + "\"\\s*:\\s*(true|false)");
    std::smatch match;
    if (!std::regex_search(json, match, pattern)) {
        return fallback;
    }
    return match[1].str() == "true";
}

} // namespace


Keyframe readKeyframe(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot read " + path);
    }

    // PFM header: "Pf", then "<width> <height>", then a scale whose sign gives
    // the byte order (negative = little-endian), each followed by whitespace.
    std::string magic;
    uint32_t width = 0;
    uint32_t height = 0;
    float scale = 0.0f;
    file >> magic >> width >> height >> scale;
    file.get();     // the single whitespace byte before the pixel data

    if (!file || magic != "Pf" || width == 0 || height == 0) {
        throw std::runtime_error(path + " is not a greyscale PFM file.");
    }
    if (scale >= 0.0f) {
        throw std::runtime_error(path + " is big-endian, which is not supported.");
    }

    Keyframe keyframe;
    keyframe.width = width;
    keyframe.height = height;
    keyframe.pixels.resize(static_cast<size_t>(width) * height);

    // PFM stores the bottom row first; keep the top row first in memory.
    for (uint32_t row = height; row-- > 0;) {
        file.read(reinterpret_cast<char*>(keyframe.pixels.data() + static_cast<size_t>(row) * width),
                  static_cast<std::streamsize>(width * sizeof(float)));
    }
    if (!file) {
        throw std::runtime_error(path + " is truncated.");
    }

    return keyframe;
}


KeyframeSequence::KeyframeSequence(const std::string& folder)
    : _folder(folder)
{
    const std::string json = readTextFile((std::filesystem::path(folder) / "keyframes.json").string());

    _re = findString(json, "re");
    _im = findString(json, "im");
    _iterations = static_cast<int>(findNumber(json, "iterations"));
    // Sequences from before automatic iterations have neither this nor the
    // per-keyframe limits: every keyframe used `iterations`.
    _autoIterations = findFlag(json, "autoIterations", false);
    _width = findNumber(json, "width");
    _height = findNumber(json, "height");
    if (_width == 0 || _height == 0) {
        throw std::runtime_error("keyframes.json gives an empty keyframe size.");
    }

    // Each entry is { "file": "...", "zoom": "...", "iterations": n }.
    const std::regex entryPattern("\"file\"\\s*:\\s*\"([^\"]+)\"\\s*,\\s*\"zoom\"\\s*:\\s*\"([^\"]*)\""
                                  "(?:\\s*,\\s*\"iterations\"\\s*:\\s*(\\d+))?");
    for (std::sregex_iterator it(json.begin(), json.end(), entryPattern), end; it != end; ++it) {
        _files.push_back((*it)[1].str());
        _zooms.push_back((*it)[2].str());
        _keyframeIterations.push_back((*it)[3].matched ? std::stoi((*it)[3].str()) : _iterations);
    }
    if (_files.empty()) {
        throw std::runtime_error("keyframes.json lists no keyframes.");
    }
}


Keyframe KeyframeSequence::load(size_t index) const
{
    const std::string path = (std::filesystem::path(_folder) / _files.at(index)).string();

    Keyframe keyframe = readKeyframe(path);
    if (keyframe.width != _width || keyframe.height != _height) {
        throw std::runtime_error(path + " does not have the size keyframes.json gives.");
    }
    return keyframe;
}
