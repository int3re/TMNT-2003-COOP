// Which DirectInput keyboard codes the game currently treats as P2's buttons.
#pragma once
#include <cstdint>

#include "common/config.h"

namespace p2binds {

// Resolves the game's live binding table (signature first, RVA fallback only
// for a known exe hash). Safe to call from the init thread.
void init(const Config& cfg, const uint8_t exe_sha256[32]);

// Fills codes[kNumSlots] with a DIK code per slot (-1 = none). Prefers the
// game's live table (follows in-game rebinding); falls back to [mod] p2_keys.
// Called from the game thread only.
void current(int codes[tmnp::kNumSlots]);

}  // namespace p2binds
