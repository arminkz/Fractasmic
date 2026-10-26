#pragma once

#include "fractal/BigMath.h"

#include <cstdint>
#include <string>
#include <vector>


struct Keyframe
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> pixels;  // top row first

    float at(uint32_t x, uint32_t y) const { return pixels[static_cast<size_t>(y) * width + x]; }
};

// Reads a keyframe's PFM file
Keyframe readKeyframe(const std::string& path);


class KeyframeSequence
{
public:

    explicit KeyframeSequence(const std::string& folder);

    // The centre, as the decimal strings the sequence was generated from.
    const std::string& re() const { return _re; }
    const std::string& im() const { return _im; }
    // The iteration limit every keyframe uses, or with autoIterations() the
    // most any may use.
    int iterations() const { return _iterations; }
    bool autoIterations() const { return _autoIterations; }

    uint32_t width() const { return _width; }
    uint32_t height() const { return _height; }
    size_t count() const { return _files.size(); }

    const std::string& fileName(size_t index) const { return _files.at(index); }
    const std::string& zoom(size_t index) const { return _zooms.at(index); }
    // The iteration limit keyframe `index` was rendered with; 0 if it has not been.
    int iterations(size_t index) const { return _keyframeIterations.at(index); }

    // The binary logarithm of that magnification.
    double log2Zoom(size_t index) const { return BigMath::log2OfDecimal(zoom(index)); }

    // Reads one keyframe's PFM file
    Keyframe load(size_t index) const;

private:
    std::string _folder;
    std::string _re;
    std::string _im;
    int _iterations = 0;
    bool _autoIterations = false;
    uint32_t _width = 0;
    uint32_t _height = 0;
    std::vector<std::string> _files;
    std::vector<std::string> _zooms;
    std::vector<int> _keyframeIterations;
};
