#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <string_view>

struct GuiLuaThread
{
    std::uintptr_t id;
    std::string timestamp;
    std::string resource;
};

bool StartGui(HMODULE module);
bool GuiVisible();
bool GuiTakeLuaCode(std::string& code, std::string& resource);
bool GuiTakeUnloadThread(std::uintptr_t& id);
bool GuiTakeTramCommand(std::string& command);
bool GuiTramBotEnabled();
bool GuiTramSirenEnabled();
void GuiQueueTramAdminCaption(std::string caption);
void GuiAddLuaThread(const GuiLuaThread& thread);
void GuiRemoveLuaThread(std::uintptr_t id);
void GuiClearLuaThreads();
void GuiAppendEvent(std::string_view event);
void GuiUpdateTramState(std::string_view key, std::string_view value);
void GuiResetTramState();
