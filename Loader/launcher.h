#pragma once

#include <functional>
#include <stop_token>
#include <string_view>

namespace Launcher
{
using LogSink = std::function<void(std::wstring_view)>;

int Run(std::stop_token stop, const LogSink& log);
}
