#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Writing a keyframe sequence: what KeyframeMode is asked to render, and the
// files it renders it to. KeyframeSequence reads them back.

// What the sequence is to show; filled in from the command line or the New
// dialog.
struct KeyframeOptions
{
    // Centre and final zoom as decimal strings, so no digits are lost on the
    // way to MPFR.
    std::string re;
    std::string im;
    std::string zoom;

    // With autoIterations, each keyframe gets the iteration limit it needs, at
    // most maxIterations. Otherwise every keyframe uses `iterations`.
    bool autoIterations = true;
    int iterations = 4096;
    int maxIterations = 1000000;

    uint32_t width = 1920;
    uint32_t height = 1080;

    std::string outputDir = "keyframes";

    // Close the window after the last keyframe. Command line only.
    bool exitWhenDone = false;
};

// One entry of the sequence, as recorded in keyframes.json.
struct KeyframeInfo
{
    std::string fileName;   // relative to the output folder
    std::string zoom;       // decimal string
    int iterations = 0;     // the limit it was rendered with; 0 until it is
};

// The iteration limit a sequence with these options records: the one every
// keyframe uses, or with autoIterations the most any may use.
inline int iterationBound(const KeyframeOptions& options)
{
    return options.autoIterations ? options.maxIterations : options.iterations;
}

// File name of keyframe `index`, e.g. keyframe_00042.pfm.
std::string keyframeFileName(size_t index);

// Writes a greyscale PFM: a three-line text header, then little-endian float32
// rows, bottom row first. `pixels` holds width * height values with the top row
// first. The file appears under its final name only once it is complete.
void writePfm(const std::string& path, const std::vector<float>& pixels, uint32_t width, uint32_t height);

// Writes keyframes.json: everything a video assembler needs to interpret the
// PFM files.
void writeManifest(const std::string& path, const KeyframeOptions& options, const std::vector<KeyframeInfo>& keyframes);
