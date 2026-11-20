// tmntnet.asi entry points.
// - InitializeASI(): called by Ultimate ASI Loader (and our dinput8 shim)
//   right after LoadLibrary, outside the loader lock and before the game's
//   own code runs. Patches that must precede the game (video mode list) go here.
// - DllMain only starts a fallback thread for loaders that never call
//   InitializeASI: no LoadLibrary, sockets or hooks under the loader lock.
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/log.h"
#include "common/sha256.h"
#include "common/version.h"
#include "mod/ambient.h"
#include "mod/audio.h"
#include "mod/binds.h"
#include "mod/autosave.h"
#include "mod/native_menu.h"
#include "mod/net_pages.h"
#include "mod/mod_pages.h"
#include "mod/option_pages.h"
#include "mod/keyconf.h"
#include "mod/costume.h"
#include "mod/own_camera.h"
#include "mod/host_menus.h"
#include "mod/movie_sync.h"
#include "mod/net_pause.h"
#include "mod/level_marks.h"
#include "mod/load_guard.h"
#include "mod/lockstep.h"
#include "mod/voice_sync.h"
#include "mod/game_options.h"
#include "mod/csel.h"
#include "mod/display.h"
#include "mod/dinput_hook.h"
#include "mod/known_exe.h"
#include "mod/four_hud.h"
#include "mod/four_players.h"
#include "mod/main_menu.h"
#include "mod/map_turtles.h"
#include "mod/revive.h"
#include "mod/story_menu.h"
#include "mod/nametags.h"
#include "mod/net_host.h"
#include "mod/net_peer.h"
#include "mod/overlay.h"
#include "mod/p2_binds.h"
#include "mod/quality.h"
#include "mod/settings_ui.h"
#include "mod/shared_state.h"
#include "mod/ui.h"
#include "mod/widescreen.h"

RemotePlayer g_p2;
std::atomic<uint32_t> g_frame{0};

namespace {

volatile LONG g_started = 0;
HANDLE g_init_called = nullptr;  // signalled by InitializeASI

// A crash (an access violation and the like, in the game or in tmntnet) goes
// to tmntnet.log: where, the registers and the return addresses on the stack
// that point into tmnt.exe or tmntnet.asi. First chance, at most three a run.
uintptr_t g_mod_lo = 0, g_mod_hi = 0;

bool code_address(uintptr_t v) {
  return (v >= 0x401000 && v < 0x5A0000) || (v >= g_mod_lo && v < g_mod_hi);
}

// Code of another module (Steam, Direct3D, Windows): its name from its own
// export directory (read from memory: no loader calls inside an exception).
bool other_code(uintptr_t v, char* name, size_t n, uintptr_t& base) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (v < 0x10000 || !VirtualQuery(reinterpret_cast<const void*>(v), &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE ||
      !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
    return false;
  base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
  __try {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const char* s = "?";
    if (dos->e_magic == IMAGE_DOS_SIGNATURE && nt->Signature == IMAGE_NT_SIGNATURE && d.VirtualAddress)
      s = reinterpret_cast<const char*>(base + reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + d.VirtualAddress)->Name);
    snprintf(name, n, "%s", s);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    snprintf(name, n, "?");
  }
  return true;
}

LONG CALLBACK crash_log(EXCEPTION_POINTERS* e) {
  DWORD code = e->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_INT_OVERFLOW &&
      code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION)
    return EXCEPTION_CONTINUE_SEARCH;
  static volatile LONG n = 0;
  if (InterlockedIncrement(&n) > 3) return EXCEPTION_CONTINUE_SEARCH;
  const CONTEXT* c = e->ContextRecord;
  char where[64] = "", mod[48];
  uintptr_t base = 0, at = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
  if (!code_address(at) && other_code(at, mod, sizeof(mod), base))
    snprintf(where, sizeof(where), " = %s+%X", mod, (unsigned)(at - base));
  LOGE("crash: exception %08lX at %p%s, thread %lu (eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX ebp %08lX "
       "esp %08lX)",
       code, e->ExceptionRecord->ExceptionAddress, where, GetCurrentThreadId(), c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi,
       c->Edi, c->Ebp, c->Esp);
  // Return addresses: tmnt.exe's and ours as they are, other modules' by name.
  char line[1200];
  int len = 0;
  const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(c->Esp);
  for (int i = 0; i < 256 && len < (int)sizeof(line) - 64; ++i) {
    uintptr_t v = 0;
    __try {
      v = sp[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      break;
    }
    if (code_address(v))
      len += snprintf(line + len, sizeof(line) - len, " %08X", (unsigned)v);
    else if (other_code(v, mod, sizeof(mod), base))
      len += snprintf(line + len, sizeof(line) - len, " %s+%X", mod, (unsigned)(v - base));
  }
  line[len] = 0;
  LOGE("crash: on the stack:%s (tmntnet.asi at %08X)", line, (unsigned)g_mod_lo);
  return EXCEPTION_CONTINUE_SEARCH;
}

// The log of the run before is kept: tmntnet.log is written anew at every
// start, and a game that hung or fell is started again before anybody looks
// (a real game, 0.13.4: a friend's game hung in a load, both logs were gone).
// It goes to tmntnet-logs\tmntnet-<its last line's time>.log; the last ten stay.
void keep_last_log(const std::wstring& dir) {
  const std::wstring log = dir + L"tmntnet.log", folder = dir + L"tmntnet-logs";
  WIN32_FILE_ATTRIBUTE_DATA a{};
  if (!GetFileAttributesExW(log.c_str(), GetFileExInfoStandard, &a) || (!a.nFileSizeLow && !a.nFileSizeHigh)) return;
  CreateDirectoryW(folder.c_str(), nullptr);
  FILETIME local{};
  SYSTEMTIME t{};
  FileTimeToLocalFileTime(&a.ftLastWriteTime, &local);
  FileTimeToSystemTime(&local, &t);
  wchar_t name[64];
  swprintf(name, 64, L"\\tmntnet-%04u%02u%02u-%02u%02u%02u.log", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  if (!MoveFileExW(log.c_str(), (folder + name).c_str(), MOVEFILE_REPLACE_EXISTING)) return;
  // (their names sort by time)
  std::vector<std::wstring> kept;
  WIN32_FIND_DATAW f{};
  HANDLE h = FindFirstFileW((folder + L"\\tmntnet-*.log").c_str(), &f);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    kept.push_back(f.cFileName);
  } while (FindNextFileW(h, &f));
  FindClose(h);
  std::sort(kept.begin(), kept.end());
  for (size_t i = 0; i + 10 < kept.size(); ++i) DeleteFileW((folder + L"\\" + kept[i]).c_str());
}

// A game that stands still says where. No keyboard poll (the game asks once a
// frame) for four seconds: the game thread is stopped for a moment, its place
// and the return addresses on its stack are taken, and it goes on; then they
// are written like a crash's. Again every few seconds while it lasts. A load
// that takes long looks the same and is told the same: a few lines.
void say_where(DWORD thread_id, unsigned seconds) {
  HANDLE th = thread_id ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, thread_id)
                        : nullptr;
  if (!th) {
    LOGW("watchdog: no frame for %u s (the game's thread is not known)", seconds);
    return;
  }
  // (Nothing that takes a lock while it is stopped: it may hold that lock.)
  constexpr int kWords = 1024;
  static uintptr_t words[kWords];
  int n = 0;
  CONTEXT c{};
  c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  bool ok = false;
  if (SuspendThread(th) != (DWORD)-1) {
    ok = GetThreadContext(th, &c) != 0;
    if (ok) {
      const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(c.Esp);
      __try {
        for (; n < kWords; ++n) words[n] = sp[n];
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
    }
    ResumeThread(th);
  }
  CloseHandle(th);
  if (!ok) {
    LOGW("watchdog: no frame for %u s (the game's thread could not be asked)", seconds);
    return;
  }
  char where[64] = "", mod[48];
  uintptr_t base = 0;
  if (!code_address(c.Eip) && other_code(c.Eip, mod, sizeof(mod), base))
    snprintf(where, sizeof(where), " = %s+%X", mod, (unsigned)(c.Eip - base));
  LOGW("watchdog: no frame for %u s: the game's thread is at %08lX%s (eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX "
       "edi %08lX ebp %08lX esp %08lX)",
       seconds, c.Eip, where, c.Eax, c.Ebx, c.Ecx, c.Edx, c.Esi, c.Edi, c.Ebp, c.Esp);
  // The game's own places and tmntnet's say what the game was doing: all of
  // them. Of the other modules only the nearest few (what it waits in): the
  // stack is full of their old leftovers (lab: a line of nothing else).
  char ours[1000], near_by[560];
  int a = 0, b = 0, others = 0;
  for (int i = 0; i < n; ++i) {
    if (code_address(words[i])) {
      if (a < (int)sizeof(ours) - 12) a += snprintf(ours + a, sizeof(ours) - a, " %08X", (unsigned)words[i]);
    } else if (others < 8 && other_code(words[i], mod, sizeof(mod), base)) {
      ++others;
      b += snprintf(near_by + b, sizeof(near_by) - b, " %s+%X", mod, (unsigned)(words[i] - base));
    }
  }
  ours[a] = 0, near_by[b] = 0;
  LOGW("watchdog: on its stack: nearest%s; the game's and tmntnet's:%s (tmntnet.asi at %08X)", near_by, ours,
       (unsigned)g_mod_lo);
}

// Lab ([debug] profile=1): while this PC catches up with a running game, the
// game's thread is asked where it is every 2 ms; at the end the places that
// came up most are logged: the exe's by 256 bytes, the other modules by name,
// and, for places outside the exe, the first return address into the exe on
// the stack (who in the game called there).
// The exported name at or before an address of a module (its nearest symbol).
void export_before(uintptr_t base, uintptr_t addr, char* out, size_t n) {
  out[0] = 0;
  __try {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!d.VirtualAddress) return;
    auto* e = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + d.VirtualAddress);
    auto* names = reinterpret_cast<const DWORD*>(base + e->AddressOfNames);
    auto* ords = reinterpret_cast<const WORD*>(base + e->AddressOfNameOrdinals);
    auto* funcs = reinterpret_cast<const DWORD*>(base + e->AddressOfFunctions);
    uintptr_t best = 0;
    const char* name = nullptr;
    for (DWORD i = 0; i < e->NumberOfNames; ++i) {
      const uintptr_t f = base + funcs[ords[i]];
      if (f <= addr && f > best) best = f, name = reinterpret_cast<const char*>(base + names[i]);
    }
    if (name && addr - best < 0x400) snprintf(out, n, "%s", name);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// A stack word that is a return address: code right after a call.
bool after_call(uintptr_t a) {
  if (!code_address(a)) return false;
  __try {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(a);
    if (p[-5] == 0xE8) return true;                           // call rel32
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;  // call reg / [reg]
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;  // call [reg + d8]
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;  // call [d32] / [reg + d32]
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return false;
}

DWORD WINAPI profiler(void*) {
  struct Place {
    uintptr_t at;
    char name[40];
    unsigned n;
  };
  static Place places[512];
  int count = 0;
  unsigned samples = 0;
  bool was = false;
  auto add = [&](uintptr_t at, const char* name) {
    for (int i = 0; i < count; ++i)
      if (places[i].at == at && !strcmp(places[i].name, name)) {
        ++places[i].n;
        return;
      }
    if (count < 512) {
      places[count].at = at, places[count].n = 1;
      snprintf(places[count].name, sizeof(places[count].name), "%s", name);
      ++count;
    }
  };
  for (;;) {
    Sleep(2);
    const bool now = lockstep::catching_up();
    if (now) {
      const DWORD id = lockstep::game_thread();
      HANDLE th = id ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id) : nullptr;
      if (th) {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        static uintptr_t words[256];
        int n = 0;
        if (SuspendThread(th) != (DWORD)-1) {
          if (GetThreadContext(th, &c)) {
            const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(c.Esp);
            __try {
              for (; n < 256; ++n) words[n] = sp[n];
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
          }
          ResumeThread(th);
        }
        CloseHandle(th);
        if (c.Eip) {
          ++samples;
          char mod[48];
          uintptr_t base = 0;
          if (c.Eip >= 0x401000 && c.Eip < 0x5A0000) {
            add(c.Eip & ~0xFFu, "exe");
          } else if (c.Eip >= g_mod_lo && c.Eip < g_mod_hi) {
            add((c.Eip - g_mod_lo) & ~0xFFu, "tmntnet");
          } else {
            // In another module: its nearest export, and who of the game or tmntnet called down there.
            const bool known = other_code(c.Eip, mod, sizeof(mod), base);
            char sym[40] = "";
            if (known) export_before(base, c.Eip, sym, sizeof(sym));
            uintptr_t from = 0;
            for (int i = 0; i < n && !from; ++i)
              if (after_call(words[i])) from = words[i];
            char name[40];
            snprintf(name, sizeof(name), "%.14s!%.22s", known ? mod : "?", sym);
            add(from >= g_mod_lo && from < g_mod_hi ? 0xA0000000u | (uint32_t)(from - g_mod_lo) : (uint32_t)from, name);
          }
        }
      }
    } else if (was && samples) {
      std::sort(places, places + count, [](const Place& a, const Place& b) { return a.n > b.n; });
      LOGI("profile: %u samples while catching up; the places that came up most:", samples);
      char line[1400];
      int len = 0;
      for (int i = 0; i < count && i < 40 && len < (int)sizeof(line) - 64; ++i)
        len += snprintf(line + len, sizeof(line) - len, " %s<%08X:%.1f%%", places[i].name, (unsigned)places[i].at,
                        places[i].n * 100.0 / samples);
      LOGI("profile:%s", line);
      count = 0, samples = 0;
    }
    was = now;
  }
  return 0;
}

DWORD WINAPI watchdog(void*) {
  uint32_t last = 0;
  DWORD since = GetTickCount(), next = 4000;
  int said = 0;
  bool stuck = false;
  for (;;) {
    Sleep(500);
    const uint32_t polls = g_frame.load(std::memory_order_relaxed);
    const DWORD now = GetTickCount();
    if (polls != last) {
      if (stuck) LOGW("watchdog: the game goes on after %lu s without a frame", (now - since) / 1000);
      last = polls, since = now, next = 4000, stuck = false;
      continue;
    }
    if (!last || now - since < next || said >= 16) continue;  // (not begun yet; not long enough; said enough)
    next += next < 20000 ? 5000 : 30000;
    stuck = true;
    ++said;
    if (lockstep::waiting_for_buttons())
      LOGW("watchdog: no frame for %lu s: the game waits for another PC's buttons", (now - since) / 1000);
    else
      say_where(lockstep::game_thread(), (unsigned)((now - since) / 1000));
  }
}

void install_crash_log() {
  HMODULE self = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&crash_log), &self) && self) {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(self) + dos->e_lfanew);
    g_mod_lo = reinterpret_cast<uintptr_t>(self);
    g_mod_hi = g_mod_lo + nt->OptionalHeader.SizeOfImage;
  }
  AddVectoredExceptionHandler(0, crash_log);
}

bool running_in_tmnt() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  const wchar_t* name = wcsrchr(path, L'\\');
  return _wcsicmp(name ? name + 1 : path, L"tmnt.exe") == 0;
}

void init(const char* how) {
  if (InterlockedExchange(&g_started, 1) != 0) return;  // ASI loader + shim both load us

  std::wstring dir = module_dir(nullptr);
  Config cfg;
  bool have_ini = load_config(dir + L"tmntnet.ini", cfg);
  keep_last_log(dir);
  tlog::open(dir + L"tmntnet.log", cfg.log_level, false);
  tlog::set_frame_counter(&g_frame);
  LOGI("tmntnet " TMNTNET_VERSION " (protocol v%u) in %ls, started by %s", tmnp::kVersion,
       dir.c_str(), how);
  if (!have_ini) LOGW("tmntnet.ini not found next to tmnt.exe, using defaults (role=host)");
  LOGI("logs of the runs before: %lstmntnet-logs\\ (the last ten)", dir.c_str());
  install_crash_log();
  if (HANDLE w = CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr)) CloseHandle(w);
  if (cfg.debug_profile)
    if (HANDLE w = CreateThread(nullptr, 0, profiler, nullptr, 0, nullptr)) CloseHandle(w);

  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  uint8_t hash[32] = {};
  if (sha256_file(exe, hash)) {
    const KnownExe* known = find_known_exe(hash);
    LOGI("tmnt.exe sha256 %s (%s)", to_hex(hash, 32).c_str(),
         known ? known->label : "unknown build: only signature-based lookups");
  } else {
    LOGW("cannot hash %ls", exe);
  }

  // Game-side features work on any PC that runs the game.
  lockstep::install(cfg);  // (first: WinMain's single-instance check)
  voice_sync::install(cfg);
  load_guard::install(cfg);
  widescreen::install(cfg);
  csel::install(cfg);
  map_turtles::install(cfg);
  nametags::install(cfg);
  four::install(cfg);  // (after the two above: it hooks the accessor they find)
  four_hud::install(cfg);
  revive::install(cfg);
  story_menu::install(cfg);
  host_menus::install(cfg);
  net_pause::install(cfg);
  movie_sync::install(cfg);
  level_marks::install(cfg);
  costume::install(cfg);
  own_camera::install(cfg);
  main_menu::install(cfg);
  option_pages::install(cfg);
  keyconf::install(cfg);
  ui::install(cfg);
  settings_ui::install(cfg);
  binds::init(cfg);
  game_options::init();
  autosave::install(cfg);
  native_menu::install(cfg);
  net_pages::install(cfg);
  mod_pages::install(cfg);
  display::install(cfg);
  quality::install(cfg);
  ambient::install(cfg);
  audio::install(cfg);
  overlay::install(cfg);

  if (!cfg.is_host()) {
    // role=client inside the game: this PC is the other one of a lockstep
    // session (the game on every PC), joining the host itself.
    LOGI("role=%s: this game joins the host's (the game on every PC)", cfg.role.c_str());
    p2binds::init(cfg, hash);
    if (!install_dinput_hooks(cfg)) LOGE("DirectInput hook failed");
    start_net_peer(cfg, hash);
    return;
  }
  p2binds::init(cfg, hash);
  if (!install_dinput_hooks(cfg)) {
    LOGE("DirectInput hook failed; netplay disabled");
    return;
  }
  start_net_host(cfg, hash);
}

DWORD WINAPI fallback_thread(void*) {
  // Give an ASI loader the chance to call InitializeASI first.
  if (WaitForSingleObject(g_init_called, 3000) == WAIT_TIMEOUT)
    init("DllMain fallback (loader did not call InitializeASI; widescreen mode list may be missed)");
  return 0;
}

}  // namespace

extern "C" __declspec(dllexport) void InitializeASI() {
  if (!running_in_tmnt()) return;
  if (g_init_called) SetEvent(g_init_called);
  init("InitializeASI");
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    // ASI loaders inject into every game they find: only act inside tmnt.exe.
    if (running_in_tmnt()) {
      g_init_called = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      HANDLE t = CreateThread(nullptr, 0, fallback_thread, nullptr, 0, nullptr);
      if (t) CloseHandle(t);
    }
  }
  return TRUE;
}
