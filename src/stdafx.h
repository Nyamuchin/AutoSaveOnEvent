// ============================================================================
//  Spore AutoSave On Event
//
//  stdafx.h - precompiled header. Pulls in the Windows headers and, through
//  BasicIncludes.h, the whole Spore ModAPI SDK surface we need.
// ============================================================================
#pragma once

#include "targetver.h"

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used Windows headers
#include <windows.h>
#include <cstdarg>
#include <cstdint>

// Pulls in the Spore ModAPI: game modes, messaging, Simulator, editors, ...
#include <Spore\BasicIncludes.h>
