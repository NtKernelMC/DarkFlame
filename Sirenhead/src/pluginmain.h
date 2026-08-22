#pragma once

#include <Windows.h>

#include "pluginsdk/bridgemain.h"
#include "pluginsdk/_plugins.h"

#define PLUGIN_NAME "Sirenhead"
#define PLUGIN_VERSION 1

#define PLUG_EXPORT extern "C" __declspec(dllexport)

extern int pluginHandle;
extern HWND hwndDlg;
extern int hMenu;

