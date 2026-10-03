#pragma once

#include "third_party/imgui/imgui.h"

#include <cstddef>
#include <string>
#include <string_view>

void InitializePlowBot();
void RegisterPlowBotLua(void* lua);
void DrawPlowBot(ImVec2 position, ImVec2 size, float scale);
void PlowQueueAdminCaption(std::string caption);

enum class PlowLoad
{
    Waiting,
    Loaded,
    Pending,
    Error,
    Unloaded,
};
void PlowSetScript(std::string version, std::size_t bytes);
void PlowSetLoad(PlowLoad kind, std::string text);
bool PlowTakeReload();
bool PlowAutoReload();
bool PlowBotBusy();
std::string PlowScriptVersion(std::string_view code);
