// Integration test without the game: a stand-in "tmnt.exe" that loads
// tmntnet.asi and polls a DirectInput keyboard exactly like TMNT does
// (GetDeviceState(256) once per 60 Hz frame). Run tmntnet-client.exe --test
// against it; this program reports which P2 keys the mod injected.
//
//   tmnt.exe [seconds]   -> exit 0 if right, left and jump were all injected
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <cstdio>
#include <cstdlib>

namespace {

struct Watch {
  const char* name;
  BYTE dik;
  int frames;
};

}  // namespace

int main(int argc, char** argv) {
  int seconds = argc > 1 ? atoi(argv[1]) : 12;

  HMODULE asi = LoadLibraryW(L"tmntnet.asi");
  if (!asi) {
    printf("FAIL: cannot load tmntnet.asi (error %lu)\n", GetLastError());
    return 2;
  }
  // Same as Ultimate ASI Loader: InitializeASI right after LoadLibrary.
  auto init = reinterpret_cast<void (*)()>(GetProcAddress(asi, "InitializeASI"));
  if (!init) {
    printf("FAIL: tmntnet.asi does not export InitializeASI\n");
    return 2;
  }
  init();

  HWND wnd = CreateWindowExW(0, L"STATIC", L"fake tmnt", WS_OVERLAPPED, 0, 0, 64, 64, nullptr,
                             nullptr, GetModuleHandleW(nullptr), nullptr);
  IDirectInput8A* di = nullptr;
  IDirectInputDevice8A* kb = nullptr;
  if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                IID_IDirectInput8A, (void**)&di, nullptr)) ||
      FAILED(di->CreateDevice(GUID_SysKeyboard, &kb, nullptr)) ||
      FAILED(kb->SetDataFormat(&c_dfDIKeyboard)) ||
      FAILED(kb->SetCooperativeLevel(wnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE)) ||
      FAILED(kb->Acquire())) {
    printf("FAIL: DirectInput keyboard setup\n");
    return 2;
  }

  // The game's default P2 keyboard binds for the slots the --test pattern uses.
  Watch watch[] = {{"right (PgDn)", DIK_NEXT, 0},
                   {"left (Delete)", DIK_DELETE, 0},
                   {"jump (NumPad0)", DIK_NUMPAD0, 0}};
  BYTE prev[256] = {};
  int frames = 0;
  DWORD start = GetTickCount();
  while (GetTickCount() - start < (DWORD)seconds * 1000) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    BYTE keys[256] = {};
    if (SUCCEEDED(kb->GetDeviceState(sizeof(keys), keys))) {
      ++frames;
      for (auto& w : watch) {
        bool down = keys[w.dik] & 0x80;
        if (down) ++w.frames;
        if (down != bool(prev[w.dik] & 0x80))
          printf("[%6lu ms] %-15s %s\n", GetTickCount() - start, w.name, down ? "DOWN" : "up");
      }
      memcpy(prev, keys, sizeof(prev));
    }
    Sleep(16);
  }

  bool ok = true;
  printf("%d frames polled\n", frames);
  for (auto& w : watch) {
    printf("  %-15s held for %d frames\n", w.name, w.frames);
    ok = ok && w.frames > 0;
  }
  printf(ok ? "PASS\n" : "FAIL: some P2 keys never reached the game's keyboard state\n");
  kb->Unacquire();
  kb->Release();
  di->Release();
  return ok ? 0 : 1;
}
