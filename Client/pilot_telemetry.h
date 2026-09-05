#pragma once

#include "third_party/imgui/imgui.h"

void InitializePilotTelemetry();
void RegisterPilotTelemetryLua(void* lua);
void DrawPilotTelemetry(ImVec2 position, ImVec2 size, float scale);
