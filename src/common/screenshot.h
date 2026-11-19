// Back buffer -> PNG for Direct3D 9 (in-game F12, client/UI previews).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct IDirect3DDevice9;

namespace shot {

// Copies the current back buffer (32-bit formats only) as BGRA rows, alpha 255.
bool capture(IDirect3DDevice9* dev, std::vector<uint8_t>& bgra, unsigned& w, unsigned& h);

// Encodes BGRA pixels with WIC. Initializes COM for the calling thread.
bool save_png(const std::wstring& path, const std::vector<uint8_t>& bgra, unsigned w, unsigned h);

}  // namespace shot
