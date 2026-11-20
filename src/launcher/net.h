#pragma once
// HTTP for the launcher (WinINet): a page's text, where a URL redirects to, a
// file to disk. Directly, through Windows' proxy settings or through a given
// proxy.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace net {

struct Proxy {
  enum Mode { None, System, Own } mode = System;
  std::wstring address;  // host:port, for Own
};

struct Failure {
  unsigned status = 0;   // the server's answer, when there was one
  unsigned error = 0;    // else Windows' error (12029: no connection, 12031: reset, 12002: timeout)
  std::wstring host;     // the server the request went to
  bool cancelled = false;
  std::wstring text() const;
};

// Where a URL redirects to; the redirect is not followed.
bool redirect_of(const std::wstring& url, const Proxy& proxy, std::wstring& location, Failure& why);

// A page's bytes.
bool get(const std::wstring& url, const Proxy& proxy, std::string& body, Failure& why, size_t limit = 4u << 20);

// A file to disk (written as <file>.part, renamed when whole). Redirects are
// followed one by one, so that `why` names the server that did not answer.
bool download(const std::wstring& url, const std::wstring& file, const Proxy& proxy,
              const std::function<void(uint64_t got, uint64_t total)>& progress, const std::atomic<bool>& cancel,
              Failure& why);

}  // namespace net
