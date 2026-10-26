#pragma once

#include "modes/ExploreMode.h"
#include "modes/KeyframeMode.h"

using namespace vke;

// The application: the explorer, with keyframe generation, and video assembly
// from the keyframes, as a second mode. File > Generate Keyframes... opens the
// New dialog filled in with the current view. Once a sequence is started or
// opened, keyframe mode has the window until Back to explorer.
//
// Only the mode on screen renders and gets input. The other keeps its state,
// so going back returns to the same view.
class App : public Renderer
{
public:
    // `keyframeDefaults` fill in the New dialog. With `generate` set they are
    // rendered straight away and the application starts in keyframe mode; the
    // constructor then throws if they cannot be.
    App(std::shared_ptr<VulkanContext> ctx,
        std::shared_ptr<SwapChain> swapChain,
        const std::string& shaderDir,
        const KeyframeOptions& keyframeDefaults,
        bool generate);

    // Opens the sequence in `folder` in keyframe mode, as File > Open
    // Keyframes... does.
    void requestOpen(const std::string& folder) { _keyframes->requestOpen(folder); }

    void update() override;
    void dispatchCompute(VkCommandBuffer commandBuffer) override;
    void recordToCommandBuffer(VkCommandBuffer commandBuffer, uint32_t swapChainImageIndex) override;
    void onSwapChainRecreated() override;
    void buildUI() override;

    void handleMouseClick(float mouseX, float mouseY) override;
    void handleMouseDrag(float dx, float dy) override;
    void handleMouseWheel(float dy) override;
    void handleKeyDown(int key, int scancode, int modifiers) override;

private:
    enum class Mode
    {
        Explore,
        Keyframes,
    };
    Mode _mode = Mode::Explore;

    std::unique_ptr<ExploreMode> _explorer;
    std::unique_ptr<KeyframeMode> _keyframes;

    // The mode on screen.
    Renderer& active();

    // The explorer's menu bar; keyframe mode draws its own.
    void buildMenuBar();
};
