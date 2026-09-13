// Original minimal dependency substitute: real Windows COM interfaces, no GPU
// creation, shader tools, DXC headers, or SDK runtime linkage.
#pragma once
#ifndef NB_NATIVE_DEFERRED_REPLAY_TEST
#error This header is only for the isolated deferred replay CPU control.
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
