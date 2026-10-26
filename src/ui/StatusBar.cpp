#include "StatusBar.h"

#include "FontAwesome.h"

#include "imgui.h"
// For BeginViewportSideBar() and RenderTextEllipsis().
#include "imgui_internal.h"

#include <algorithm>

namespace {

// Shows `text` in a box `width` wide on the current line, cut short with an
// ellipsis if it does not fit. Hovering a cut-short text shows it whole.
void clippedText(const std::string& text, float width)
{
    // Lowered to the line's text baseline, as ImGui::Text() would be; a menu
    // bar puts it at the frame padding.
    const ImVec2 lineMin = ImGui::GetCursorScreenPos();
    const ImVec2 min(lineMin.x, lineMin.y + ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset);
    const ImVec2 max(min.x + width, min.y + ImGui::GetTextLineHeight());

    // The dummy reserves the box and makes it hoverable.
    ImGui::Dummy(ImVec2(width, max.y - lineMin.y));
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), min, max, max.x,
                              text.c_str(), text.c_str() + text.size(), nullptr);

    if (ImGui::IsItemHovered() && ImGui::CalcTextSize(text.c_str()).x > width) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

} // namespace


namespace ui {

void buildStatusBar(const std::string& zoom, const std::string& re, const std::string& im)
{
    // Built the way BeginMainMenuBar() builds the bar along the top.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetFrameHeight(), flags)) {
        if (ImGui::BeginMenuBar()) {
            ImGui::Text(ICON_FA_MAGNIFYING_GLASS " %sx", zoom.c_str());

            // Re and Im side by side. At depth they run to hundreds of digits,
            // more than fits; then each is cut to half the space left.
            const std::string reText = "Re " + re;
            const std::string imText = "Im " + im;
            const float available = ImGui::GetContentRegionAvail().x - 2.0f * ImGui::GetStyle().ItemSpacing.x;
            float reWidth = ImGui::CalcTextSize(reText.c_str()).x;
            float imWidth = ImGui::CalcTextSize(imText.c_str()).x;
            if (reWidth + imWidth > available) {
                reWidth = imWidth = std::max(0.0f, available * 0.5f);
            }
            clippedText(reText, reWidth);
            clippedText(imText, imWidth);

            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

} // namespace ui
