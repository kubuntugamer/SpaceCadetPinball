#pragma once

// Shared miniaudio configuration. Both the implementation translation unit
// (miniaudio_impl.cpp) and any user (Sound.cpp) must agree on these defines
// because they change the layout of miniaudio's public structs.

// We only use the low-level device + decoder APIs, so trim everything else to
// keep compile times and the binary smaller.
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH

#include "miniaudio.h"
