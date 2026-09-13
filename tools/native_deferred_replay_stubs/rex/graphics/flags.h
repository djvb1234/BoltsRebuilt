// Original isolated CPU-test flag substitute. No SDK flag registry or game.
#pragma once
#ifndef NB_NATIVE_DEFERRED_REPLAY_TEST
#error This header is only for the isolated deferred replay CPU control.
#endif
#define REXCVAR_DEFINE_BOOL(name, initial, category, description) bool name = initial
#define REXCVAR_GET(name) (name)
#define XE_GPU_FINE_GRAINED_DRAW_SCOPES 0
extern bool nb_native_deferred_storage;
