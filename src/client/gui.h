#pragma once
#include <string>

#include "client/core.h"

namespace gui {

// Runs the client window until it is closed. Returns the process exit code.
// With shot_path set, the window stays hidden, renders for shot_delay_ms,
// saves itself as PNG and returns (layout checks).
int run(ClientCore& core, const std::wstring& shot_path = L"", unsigned shot_delay_ms = 1500);

}  // namespace gui
