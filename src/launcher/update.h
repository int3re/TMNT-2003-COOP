#pragma once
// What the launcher knows and does about versions: the latest release on
// GitHub, the mod in the game folder, the installer's run.
//
// GitHub is asked through github.com alone (its API host and its download
// host are closed off on some networks while the site itself opens):
//   <repo>/releases/latest   redirects to .../releases/tag/v0.13.9: the version
//   <repo>/releases.atom     the releases' pages; the notes of a release made
//                            with tools/release_notes.py carry a block
//                            "tmntnet-release 1" of key=value lines for the
//                            launcher (sizes, SHA-256, protocol, short notes).
// Only the files themselves come from the download host.
#include <cstdint>
#include <string>
#include <vector>

#include "launcher/net.h"

namespace update {

struct Release {
  std::wstring tag;       // v0.13.9
  std::wstring version;   // 0.13.9
  std::wstring date;      // 20 ноября 2026, or empty
  int protocol = 0;       // 0: the release does not say
  std::wstring protocol_since;  // the first version with this protocol, or empty
  std::wstring setup = L"tmntnet-setup.exe";
  uint64_t setup_size = 0;
  std::wstring setup_sha256;    // lower hex, or empty
  std::wstring launcher;        // the launcher's file in the release, or empty
  uint64_t launcher_size = 0;
  std::wstring launcher_sha256;
  std::wstring title;
  std::vector<std::wstring> notes;
};

// Versions as numbers with dots: <0, 0, >0.
int compare(const std::wstring& a, const std::wstring& b);

// The latest release of the repository at `base` (https://github.com/owner/name).
bool latest(const std::wstring& base, const net::Proxy& proxy, Release& out, net::Failure& why);
// The release `tag` out of the feed's text; false: the feed has no such release.
bool from_feed(const std::string& atom, const std::wstring& tag, Release& out);
// key=value lines (the block of a release's page, dist/release.txt) into a release.
void from_lines(const std::string& text, Release& out);

// The mod's version in the game folder (tmntnet-install.txt), or empty.
std::wstring installed_version(const std::wstring& game_dir);
// A TMNT.EXE of that folder is running.
bool game_running(const std::wstring& game_dir);
bool can_write(const std::wstring& dir);
std::wstring sha256_of(const std::wstring& file);  // lower hex, empty on failure
std::wstring cache_dir();                          // %LOCALAPPDATA%\tmntnet\cache (made)

// tmntnet-setup.exe /quiet <action> "<dir>"; action: L"/host" or L"/uninstall".
// False: it failed, `what` says how (its own last line when it left one).
bool run_setup(const std::wstring& setup, const wchar_t* action, const std::wstring& dir, std::wstring& what);

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& s);

}  // namespace update
