#include "launcher/net.h"

#include <windows.h>

#include <wininet.h>

#include <vector>

#include "common/version.h"

namespace net {
namespace {

constexpr DWORD kConnectMs = 15000, kReceiveMs = 25000;

struct Handle {
  HINTERNET h = nullptr;
  Handle() = default;
  explicit Handle(HINTERNET x) : h(x) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() {
    if (h) InternetCloseHandle(h);
  }
  explicit operator bool() const { return h != nullptr; }
};

HINTERNET open_session(const Proxy& proxy) {
  const wchar_t* agent = L"tmntnet-launcher/" TEXT(TMNTNET_VERSION);
  HINTERNET h = nullptr;
  if (proxy.mode == Proxy::Own && !proxy.address.empty())
    h = InternetOpenW(agent, INTERNET_OPEN_TYPE_PROXY, proxy.address.c_str(), L"<local>", 0);
  else
    h = InternetOpenW(agent, proxy.mode == Proxy::None ? INTERNET_OPEN_TYPE_DIRECT : INTERNET_OPEN_TYPE_PRECONFIG, nullptr,
                      nullptr, 0);
  if (h) {
    DWORD ms = kConnectMs;
    InternetSetOptionW(h, INTERNET_OPTION_CONNECT_TIMEOUT, &ms, sizeof(ms));
    ms = kReceiveMs;
    InternetSetOptionW(h, INTERNET_OPTION_RECEIVE_TIMEOUT, &ms, sizeof(ms));
    InternetSetOptionW(h, INTERNET_OPTION_SEND_TIMEOUT, &ms, sizeof(ms));
  }
  return h;
}

std::wstring host_of(const std::wstring& url) {
  wchar_t host[256] = L"";
  URL_COMPONENTSW c{sizeof(c)};
  c.lpszHostName = host;
  c.dwHostNameLength = 256;
  return InternetCrackUrlW(url.c_str(), 0, 0, &c) ? std::wstring(host) : std::wstring();
}

constexpr DWORD kFlags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_PRAGMA_NOCACHE |
                         INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_UI | INTERNET_FLAG_KEEP_CONNECTION;

// One request; the answer's status in `status`. False: no answer.
HINTERNET request(HINTERNET session, const std::wstring& url, bool follow, DWORD& status, Failure& why) {
  why.host = host_of(url);
  HINTERNET h = InternetOpenUrlW(session, url.c_str(), nullptr, 0, kFlags | (follow ? 0 : INTERNET_FLAG_NO_AUTO_REDIRECT), 0);
  if (!h) {
    why.error = GetLastError();
    return nullptr;
  }
  DWORD len = sizeof(status);
  status = 0;
  if (!HttpQueryInfoW(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, nullptr)) status = 200;  // (not HTTP)
  return h;
}

bool location_of(HINTERNET h, const std::wstring& url, std::wstring& location) {
  wchar_t buf[4096];
  DWORD len = sizeof(buf);
  if (!HttpQueryInfoW(h, HTTP_QUERY_LOCATION, buf, &len, nullptr)) return false;
  // (a path alone is taken from the URL asked)
  wchar_t whole[4096];
  DWORD n = 4096;
  if (InternetCombineUrlW(url.c_str(), buf, whole, &n, 0)) location.assign(whole, n);
  else location = buf;
  return !location.empty();
}

bool is_redirect(DWORD status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

}  // namespace

std::wstring Failure::text() const {
  if (cancelled) return L"cancelled";
  if (status) return L"HTTP " + std::to_wstring(status) + L" from " + host;
  return L"error " + std::to_wstring(error) + L" at " + host;
}

bool redirect_of(const std::wstring& url, const Proxy& proxy, std::wstring& location, Failure& why) {
  why = Failure();
  Handle session(open_session(proxy));
  if (!session) {
    why.error = GetLastError();
    return false;
  }
  DWORD status = 0;
  Handle h(request(session.h, url, false, status, why));
  if (!h) return false;
  if (!is_redirect(status) || !location_of(h.h, url, location)) {
    why.status = status;
    return false;
  }
  return true;
}

bool get(const std::wstring& url, const Proxy& proxy, std::string& body, Failure& why, size_t limit) {
  why = Failure();
  body.clear();
  Handle session(open_session(proxy));
  if (!session) {
    why.error = GetLastError();
    return false;
  }
  DWORD status = 0;
  Handle h(request(session.h, url, true, status, why));
  if (!h) return false;
  if (status != 200) {
    why.status = status;
    return false;
  }
  char buf[16384];
  for (;;) {
    DWORD got = 0;
    if (!InternetReadFile(h.h, buf, sizeof(buf), &got)) {
      why.error = GetLastError();
      return false;
    }
    if (!got) break;
    body.append(buf, got);
    if (body.size() > limit) break;
  }
  return true;
}

bool download(const std::wstring& url, const std::wstring& file, const Proxy& proxy,
              const std::function<void(uint64_t got, uint64_t total)>& progress, const std::atomic<bool>& cancel,
              Failure& why) {
  why = Failure();
  Handle session(open_session(proxy));
  if (!session) {
    why.error = GetLastError();
    return false;
  }
  std::wstring at = url;
  for (int hop = 0; hop < 8; ++hop) {
    if (cancel) {
      why.cancelled = true;
      return false;
    }
    DWORD status = 0;
    Handle h(request(session.h, at, false, status, why));
    if (!h) return false;
    if (is_redirect(status)) {
      std::wstring next;
      if (!location_of(h.h, at, next)) {
        why.status = status;
        return false;
      }
      at = next;
      continue;
    }
    if (status != 200) {
      why.status = status;
      return false;
    }
    uint64_t total = 0;
    {
      wchar_t text[32] = L"";
      DWORD len = sizeof(text);
      if (HttpQueryInfoW(h.h, HTTP_QUERY_CONTENT_LENGTH, text, &len, nullptr)) total = _wcstoui64(text, nullptr, 10);
    }
    const std::wstring part = file + L".part";
    HANDLE out = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
      why.error = GetLastError();
      why.host = L"(disk)";
      return false;
    }
    std::vector<char> buf(65536);
    uint64_t got_all = 0;
    bool ok = true;
    if (progress) progress(0, total);
    for (;;) {
      if (cancel) {
        why.cancelled = true;
        ok = false;
        break;
      }
      DWORD got = 0;
      if (!InternetReadFile(h.h, buf.data(), (DWORD)buf.size(), &got)) {
        why.error = GetLastError();
        ok = false;
        break;
      }
      if (!got) break;
      DWORD written = 0;
      if (!WriteFile(out, buf.data(), got, &written, nullptr) || written != got) {
        why.error = GetLastError();
        why.host = L"(disk)";
        ok = false;
        break;
      }
      got_all += got;
      if (progress) progress(got_all, total);
    }
    CloseHandle(out);
    if (ok && total && got_all != total) {  // (the connection ended early)
      why.error = ERROR_INTERNET_CONNECTION_ABORTED;
      ok = false;
    }
    if (ok && !MoveFileExW(part.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      why.error = GetLastError();
      why.host = L"(disk)";
      ok = false;
    }
    if (!ok) DeleteFileW(part.c_str());
    return ok;
  }
  why.status = 310;  // (redirects without end)
  return false;
}

}  // namespace net
