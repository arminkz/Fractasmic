#include "App.h"

#include "ui/FontAwesome.h"

#include "imgui.h"

namespace {

void requestQuit()
{
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
}

} // namespace


App::App(std::shared_ptr<VulkanContext> ctx,
                         std::shared_ptr<SwapChain> swapChain,
                         const std::string& shaderDir,
                         const KeyframeOptions& keyframeDefaults,
                         bool generate)
    : Renderer(std::move(ctx), std::move(swapChain))
{
    _explorer = std::make_unique<ExploreMode>(_ctx, _swapChain, shaderDir);
    _keyframes = std::make_unique<KeyframeMode>(_ctx, _swapChain, shaderDir, keyframeDefaults, generate);

    // A generation asked for on the command line is already running.
    if (_keyframes->hasSequence()) {
        _mode = Mode::Keyframes;
    }
}


Renderer& App::active()
{
    if (_mode == Mode::Explore) return *_explorer;
    return *_keyframes;
}


void App::buildUI()
{
    // Before anything is drawn: a request carried out here can replace what the
    // keyframe mode draws with, and decides which mode this frame shows.
    switch (_keyframes->handleRequests()) {
        case KeyframeMode::Transition::Enter:
            _mode = Mode::Keyframes;
            break;
        case KeyframeMode::Transition::Leave:
            _mode = Mode::Explore;
            _explorer->resume();
            break;
        case KeyframeMode::Transition::None:
            break;
    }

    if (_mode == Mode::Explore) {
        buildMenuBar();
        _explorer->buildUI();
    } else {
        _keyframes->buildUI();
    }

    // The dialogs open from either mode's menu.
    _keyframes->buildDialogs();
}


void App::buildMenuBar()
{
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem(ICON_FA_FILM " Generate Keyframes...")) {
            const ExploreMode::Location here = _explorer->location();
            _keyframes->showNewDialog(here.re, here.im, here.zoom, here.iterations);
        }
        ImGui::SetItemTooltip("Renders a zoom sequence towards the current view");
        if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN " Open Keyframes...")) {
            _keyframes->showOpenDialog();
        }
        ImGui::SetItemTooltip("Browses a sequence generated earlier");
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_RIGHT_FROM_BRACKET " Quit")) {
            requestQuit();
        }
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}


void App::update()
{
    // The presenter tells this renderer which frame in flight it is recording;
    // the mode doing the work needs to know.
    Renderer& renderer = active();
    renderer.setCurrentFrame(getCurrentFrame());
    renderer.update();
}


void App::dispatchCompute(VkCommandBuffer commandBuffer)
{
    active().dispatchCompute(commandBuffer);
}


void App::recordToCommandBuffer(VkCommandBuffer commandBuffer, uint32_t swapChainImageIndex)
{
    active().recordToCommandBuffer(commandBuffer, swapChainImageIndex);
}


void App::onSwapChainRecreated()
{
    // Both modes hold framebuffers for the swapchain images, shown or not.
    _explorer->onSwapChainRecreated();
    _keyframes->onSwapChainRecreated();
}


void App::handleMouseClick(float mouseX, float mouseY)
{
    active().handleMouseClick(mouseX, mouseY);
}


void App::handleMouseDrag(float dx, float dy)
{
    active().handleMouseDrag(dx, dy);
}


void App::handleMouseWheel(float dy)
{
    active().handleMouseWheel(dy);
}


void App::handleKeyDown(int key, int scancode, int modifiers)
{
    active().handleKeyDown(key, scancode, modifiers);
}
