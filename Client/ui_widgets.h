#pragma once

#include "third_party/imgui/imgui.h"
#include "third_party/imgui/imgui_internal.h"
#include <algorithm>
#include <cstdio>

// Shared native slider behaviour with the Dark Flame track and circular handle.
inline bool DarkFlameSliderInt(const char* id, int* value, int minimum, int maximum,
    const char* format, float scale)
{
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float valueWidth = 78 * scale;
    const float height = ImGui::GetFrameHeight();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(start, ImVec2(start.x + width, start.y + height),
        ImGui::GetColorU32(ImGuiCol_FrameBg), 6 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 18 * scale);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    ImGui::SetNextItemWidth(std::max(36 * scale, width - valueWidth));
    // Ctrl-click text editing keeps its native rendering and validation.
    const bool editing = ImGui::TempInputIsActive(ImGui::GetID(id));
    if(editing) ImGui::PopStyleColor();
    const bool changed = ImGui::SliderInt(id, value, minimum, maximum, format,
        ImGuiSliderFlags_AlwaysClamp);
    const ImVec2 end = ImGui::GetItemRectMax();
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    const bool textInput = ImGui::TempInputIsActive(ImGui::GetItemID());
    ImGui::PopStyleColor(editing ? 5 : 6);
    ImGui::PopStyleVar();
    if(!textInput)
    {
        const float x0 = start.x + 2 + 9 * scale;
        const float x1 = end.x - 2 - 9 * scale;
        const float y = start.y + height / 2;
        const float fraction = static_cast<float>(std::clamp(*value, minimum, maximum) - minimum)
            / static_cast<float>(maximum - minimum);
        const float x = x0 + (x1 - x0) * fraction;
        draw->AddLine(ImVec2(x0, y), ImVec2(x1, y),
            ImGui::GetColorU32(ImVec4(0.18f, 0.12f, 0.28f, 1)), 6 * scale);
        draw->AddLine(ImVec2(x0, y), ImVec2(x, y),
            ImGui::GetColorU32(ImVec4(0.65f, 0.22f, 0.87f, 1)), 6 * scale);
        draw->AddCircleFilled(ImVec2(x, y), 9 * scale,
            ImGui::GetColorU32(ImVec4(active || hovered ? 0.83f : 0.72f, 0.29f, 1, 1)));
        draw->AddCircle(ImVec2(x, y), 9 * scale,
            ImGui::GetColorU32(ImVec4(0.08f, 0.04f, 0.14f, 1)), 0, scale);
        if(focused) draw->AddCircle(ImVec2(x, y), 12 * scale,
            ImGui::GetColorU32(ImGuiCol_CheckMark), 0, scale);
    }
    char label[64]{};
    std::snprintf(label, sizeof(label), format, *value);
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(start.x + width - textSize.x - 10 * scale,
        start.y + (height - textSize.y) / 2), ImGui::GetColorU32(ImGuiCol_Text), label);
    return changed;
}
