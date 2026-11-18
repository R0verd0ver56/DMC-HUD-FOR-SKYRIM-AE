// Precompiled header force-included in every source of the plugins (including the
// CMake-generated __<Plugin>Plugin.cpp, which would otherwise miss the std includes
// and the "sv" string-view literal).
#pragma once

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

using namespace std::literals;
