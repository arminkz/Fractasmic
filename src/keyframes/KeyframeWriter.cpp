#include "KeyframeWriter.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

std::string keyframeFileName(size_t index)
{
    char name[32];
    std::snprintf(name, sizeof(name), "keyframe_%05zu.pfm", index);
    return name;
}


void writePfm(const std::string& path, const std::vector<float>& pixels, uint32_t width, uint32_t height)
{
    // Written under a temporary name and renamed, so an interrupted run never
    // leaves a truncated keyframe that a later run would take for a finished one.
    const std::string temporaryPath = path + ".part";

    std::ofstream file(temporaryPath, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot write " + temporaryPath);
    }

    // "Pf" is single-channel float; a negative scale means little-endian.
    file << "Pf\n" << width << " " << height << "\n-1.0\n";

    // PFM stores the bottom row first.
    for (uint32_t row = height; row-- > 0;) {
        file.write(reinterpret_cast<const char*>(pixels.data() + static_cast<size_t>(row) * width),
                   static_cast<std::streamsize>(width * sizeof(float)));
    }

    file.close();
    if (!file) {
        throw std::runtime_error("Failed while writing " + temporaryPath);
    }

    std::filesystem::rename(temporaryPath, path);
}


void writeManifest(const std::string& path, const KeyframeOptions& options, const std::vector<KeyframeInfo>& keyframes)
{
    std::ofstream file(path);
    if (!file) {
        throw std::runtime_error("Cannot write " + path);
    }

    // Every string here is a number or a generated file name, so nothing needs escaping.
    file << "{\n";
    file << "  \"re\": \"" << options.re << "\",\n";
    file << "  \"im\": \"" << options.im << "\",\n";
    file << "  \"width\": " << options.width << ",\n";
    file << "  \"height\": " << options.height << ",\n";
    file << "  \"autoIterations\": " << (options.autoIterations ? "true" : "false") << ",\n";
    file << "  \"iterations\": " << iterationBound(options) << ",\n";
    file << "  \"zoomFactor\": 2,\n";
    file << "  \"zoom\": \"magnification relative to a view whose half-height is 1\",\n";
    file << "  \"value\": \"smooth iteration count; 0 means inside the set\",\n";
    // Each keyframe's own limit; 0 for one not rendered yet.
    file << "  \"keyframes\": [\n";
    for (size_t i = 0; i < keyframes.size(); ++i) {
        file << "    { \"file\": \"" << keyframes[i].fileName << "\", \"zoom\": \"" << keyframes[i].zoom
             << "\", \"iterations\": " << keyframes[i].iterations << " }"
             << (i + 1 < keyframes.size() ? "," : "") << "\n";
    }
    file << "  ]\n";
    file << "}\n";
}
