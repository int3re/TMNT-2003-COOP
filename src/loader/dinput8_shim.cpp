// Minimal dinput8.dll shim. tmnt.exe does LoadLibraryA("dinput8.dll") +
// GetProcAddress("DirectInput8Create"), so a DLL with this name next to the
// exe is loaded first. It forwards to the system dinput8.dll and, on the first
// DirectInput8Create call (outside the loader lock), loads tmntnet.asi.
// Not needed if Ultimate ASI Loader already loads tmntnet.asi.
#include <windows.h>
#include <unknwn.h>

#include <string>

namespace {

HMODULE g_self = nullptr;
HMODULE g_real = nullptr;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK load_once(PINIT_ONCE, PVOID, PVOID*) {
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  g_real = LoadLibraryW((std::wstring(sys) + L"\\dinput8.dll").c_str());

  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
  std::wstring dir(path, n);
  dir.resize(dir.find_last_of(L"\\/") + 1);
  // Like Ultimate ASI Loader: LoadLibrary, then InitializeASI outside DllMain.
  if (HMODULE asi = LoadLibraryW((dir + L"tmntnet.asi").c_str())) {
    if (auto init = reinterpret_cast<void (*)()>(GetProcAddress(asi, "InitializeASI"))) init();
  } else {
    OutputDebugStringA("[dinput8 shim] tmntnet.asi not loaded\n");
  }
  return TRUE;
}

FARPROC real_proc(const char* name) {
  InitOnceExecuteOnce(&g_once, load_once, nullptr, nullptr);
  return g_real ? GetProcAddress(g_real, name) : nullptr;
}

}  // namespace

extern "C" {

HRESULT WINAPI Shim_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID iid, LPVOID* out,
                                       LPUNKNOWN outer) {
  using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
  auto fn = reinterpret_cast<Fn>(real_proc("DirectInput8Create"));
  if (!fn) return E_FAIL;
  return fn(inst, ver, iid, out, outer);
}

HRESULT WINAPI Shim_DllCanUnloadNow() {
  using Fn = HRESULT(WINAPI*)();
  auto fn = reinterpret_cast<Fn>(real_proc("DllCanUnloadNow"));
  return fn ? fn() : S_FALSE;
}

HRESULT WINAPI Shim_DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID* out) {
  using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
  auto fn = reinterpret_cast<Fn>(real_proc("DllGetClassObject"));
  return fn ? fn(clsid, iid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI Shim_DllRegisterServer() { return E_NOTIMPL; }
HRESULT WINAPI Shim_DllUnregisterServer() { return E_NOTIMPL; }

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_self = module;
    DisableThreadLibraryCalls(module);
  }
  return TRUE;
}
