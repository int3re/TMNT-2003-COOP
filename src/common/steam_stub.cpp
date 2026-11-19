// Built instead of steam_real.cpp / lobby_steam.cpp when CMake has no
// Steamworks SDK (TMNTNET_WITH_STEAM=OFF or STEAMWORKS_SDK_DIR unset): real
// Steam is unavailable, ENet (LAN / IP) and the lab mock still work.
#include "common/log.h"
#include "common/steam_backend.h"

namespace steam {
namespace real {

bool boot() {
  LOGW("Steam недоступен (Steam unavailable): this tmntnet was built without the Steamworks SDK; "
       "using ENet (LAN / IP) only");
  return false;
}
void pump() {}
void shutdown() {}
uint64_t my_id() { return 0; }
std::string my_name() { return ""; }
std::string unavailable_reason() { return "эта сборка tmntnet без поддержки Steam"; }
std::unique_ptr<steamnet::IMessages> messages() { return nullptr; }
std::unique_ptr<lobby::IHost> host(const Config&, const std::string&) { return nullptr; }
std::unique_ptr<lobby::IBrowser> browser(const std::string&) { return nullptr; }

}  // namespace real
}  // namespace steam
