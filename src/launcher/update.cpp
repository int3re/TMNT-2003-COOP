#include "launcher/update.h"

#include <windows.h>

#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "common/game_dir.h"

namespace update {
namespace {

// --- text ---------------------------------------------------------------------

std::string between(const std::string& s, const char* open, const char* close, size_t from = 0, size_t* end = nullptr) {
  size_t a = s.find(open, from);
  if (a == std::string::npos) return "";
  a += strlen(open);
  size_t b = s.find(close, a);
  if (b == std::string::npos) return "";
  if (end) *end = b + strlen(close);
  return s.substr(a, b - a);
}

void append_utf8(std::string& out, unsigned code) {
  if (code < 0x80) out += (char)code;
  else if (code < 0x800) {
    out += (char)(0xC0 | (code >> 6));
    out += (char)(0x80 | (code & 0x3F));
  } else if (code < 0x10000) {
    out += (char)(0xE0 | (code >> 12));
    out += (char)(0x80 | ((code >> 6) & 0x3F));
    out += (char)(0x80 | (code & 0x3F));
  } else {
    out += (char)(0xF0 | (code >> 18));
    out += (char)(0x80 | ((code >> 12) & 0x3F));
    out += (char)(0x80 | ((code >> 6) & 0x3F));
    out += (char)(0x80 | (code & 0x3F));
  }
}

// &amp; and the like, once.
std::string unescape(const std::string& s) {
  static const struct {
    const char* name;
    unsigned code;
  } kNamed[] = {{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0}, {"laquo", 0xAB},
                {"raquo", 0xBB}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"middot", 0xB7}};
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') {
      out += s[i];
      continue;
    }
    size_t semi = s.find(';', i);
    if (semi == std::string::npos || semi - i > 10) {
      out += s[i];
      continue;
    }
    std::string name = s.substr(i + 1, semi - i - 1);
    unsigned code = 0;
    if (!name.empty() && name[0] == '#')
      code = (unsigned)strtoul(name.c_str() + ((name.size() > 1 && (name[1] == 'x' || name[1] == 'X')) ? 2 : 1), nullptr,
                               (name.size() > 1 && (name[1] == 'x' || name[1] == 'X')) ? 16 : 10);
    else
      for (auto& n : kNamed)
        if (name == n.name) code = n.code;
    if (!code) {
      out += s[i];
      continue;
    }
    append_utf8(out, code);
    i = semi;
  }
  return out;
}

// A page's piece as plain text: tags out (a picture's insides with them),
// spaces folded.
std::string plain(const std::string& html) {
  std::string out;
  for (size_t i = 0; i < html.size(); ++i) {
    if (html[i] == '<') {
      if (html.compare(i, 4, "<svg") == 0) {
        size_t e = html.find("</svg>", i);
        if (e == std::string::npos) break;
        i = e + 5;
        continue;
      }
      size_t e = html.find('>', i);
      if (e == std::string::npos) break;
      i = e;
      out += ' ';
      continue;
    }
    out += html[i];
  }
  out = unescape(out);
  std::string folded;
  bool space = true;
  for (char c : out) {
    const bool sp = c == ' ' || c == '\n' || c == '\r' || c == '\t';
    if (sp && space) continue;
    folded += sp ? ' ' : c;
    space = sp;
  }
  while (!folded.empty() && folded.back() == ' ') folded.pop_back();
  for (size_t i = 0; i + 1 < folded.size(); ++i)  // (a tag's end before a comma leaves no space there)
    if (folded[i] == ' ' && strchr(",.:;!?)", folded[i + 1])) folded.erase(i, 1);
  return folded;
}

std::string trimmed(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
  size_t a = 0;
  while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
  return s.substr(a);
}

// "2026-11-20T15:04:33Z" -> "20 ноября 2026".
std::wstring date_of(const std::string& iso) {
  static const wchar_t* kMonths[] = {L"января", L"февраля", L"марта", L"апреля", L"мая", L"июня",
                                     L"июля", L"августа", L"сентября", L"октября", L"ноября", L"декабря"};
  if (iso.size() < 10 || iso[4] != '-' || iso[7] != '-') return L"";
  int y = atoi(iso.substr(0, 4).c_str()), m = atoi(iso.substr(5, 2).c_str()), d = atoi(iso.substr(8, 2).c_str());
  if (m < 1 || m > 12 || d < 1 || d > 31) return L"";
  return std::to_wstring(d) + L" " + kMonths[m - 1] + L" " + std::to_wstring(y);
}

bool is_hex64(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return s;
}

// The launcher's block in a release's page: key=value lines after the marker.
// GitHub writes a code block's text twice: into an attribute of the box
// around it (for its copy button), then as the code itself. The code is the
// last of the two, and the page's last mention of the marker.
bool from_block(const std::string& html, Release& out) {
  size_t at = html.rfind("tmntnet-release ");
  if (at == std::string::npos) return false;
  size_t end = html.find("</code>", at);
  if (end == std::string::npos) end = html.find("</pre>", at);
  if (end == std::string::npos) end = html.size();
  const std::string block = unescape(html.substr(at, end - at));
  if (atoi(block.c_str() + strlen("tmntnet-release ")) != 1) return false;  // (the block's own version)
  const size_t first = block.find('\n');
  from_lines(first == std::string::npos ? "" : block.substr(first + 1), out);
  return true;
}

}  // namespace

void from_lines(const std::string& text, Release& out) {
  std::istringstream lines(text);
  std::string line;
  out.notes.clear();
  while (std::getline(lines, line)) {
    line = trimmed(line);
    size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq), value = trimmed(line.substr(eq + 1));
    if (key == "protocol") out.protocol = atoi(value.c_str());
    else if (key == "protocol_since") out.protocol_since = widen(value);
    else if (key == "date") out.date = date_of(value);
    else if (key == "setup") out.setup = widen(value);
    else if (key == "setup_size") out.setup_size = _strtoui64(value.c_str(), nullptr, 10);
    else if (key == "setup_sha256") out.setup_sha256 = is_hex64(lower(value)) ? widen(lower(value)) : L"";
    else if (key == "launcher") out.launcher = widen(value);
    else if (key == "launcher_size") out.launcher_size = _strtoui64(value.c_str(), nullptr, 10);
    else if (key == "launcher_sha256") out.launcher_sha256 = is_hex64(lower(value)) ? widen(lower(value)) : L"";
    else if (key == "title") out.title = widen(value);
    else if (key == "note" && out.notes.size() < 4) out.notes.push_back(widen(value));
  }
}

namespace {

// A release's page written by hand (before the launcher): its first
// paragraph as the title, the bold starts of its first list as the notes, the
// installer's SHA-256 from the table of files.
void from_prose(const std::string& html, Release& out) {
  std::string first = plain(between(html, "<p>", "</p>"));
  while (!first.empty() && first.back() == '.') first.pop_back();
  if (first.size() > 160) {  // (bytes: about 85 Russian letters)
    size_t cut = first.rfind(' ', 160);
    first = first.substr(0, cut == std::string::npos ? 160 : cut) + "…";
  }
  out.title = widen(first);
  const std::string list = between(html, "<ul>", "</ul>");
  size_t at = 0;
  while (out.notes.size() < 4) {
    size_t end = 0;
    std::string item = between(list, "<li>", "</li>", at, &end);
    if (item.empty()) break;
    at = end;
    std::string lead = plain(between(item, "<strong>", "</strong>"));
    if (lead.empty()) {
      lead = plain(item);
      size_t stop = lead.find(". ");
      if (stop != std::string::npos) lead = lead.substr(0, stop);
      if (lead.size() > 150) lead = lead.substr(0, lead.rfind(' ', 150)) + "…";
    }
    while (!lead.empty() && lead.back() == '.') lead.pop_back();
    if (!lead.empty()) out.notes.push_back(widen(lead));
  }
  size_t file = html.find(narrow(out.setup));
  while (file != std::string::npos) {
    for (size_t i = file; i + 64 <= html.size() && i < file + 400; ++i)
      if (is_hex64(html.substr(i, 64)) && (i + 64 == html.size() || !isxdigit((unsigned char)html[i + 64]))) {
        out.setup_sha256 = widen(html.substr(i, 64));
        return;
      }
    file = html.find(narrow(out.setup), file + 1);
  }
}

}  // namespace

std::wstring widen(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}

std::string narrow(const std::wstring& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

int compare(const std::wstring& a, const std::wstring& b) {
  const wchar_t *p = a.c_str(), *q = b.c_str();
  for (;;) {
    wchar_t *pe = nullptr, *qe = nullptr;
    const unsigned long x = wcstoul(p, &pe, 10), y = wcstoul(q, &qe, 10);
    if (x != y) return x < y ? -1 : 1;
    const bool more_p = pe != p && *pe == L'.', more_q = qe != q && *qe == L'.';
    if (!more_p && !more_q) return 0;
    p = more_p ? pe + 1 : L"0";
    q = more_q ? qe + 1 : L"0";
  }
}

bool from_feed(const std::string& atom, const std::wstring& tag, Release& out) {
  const std::string want = "/releases/tag/" + narrow(tag) + "\"";
  size_t at = 0;
  for (;;) {
    size_t end = 0;
    const std::string entry = between(atom, "<entry>", "</entry>", at, &end);
    if (entry.empty()) return false;
    at = end;
    if (entry.find(want) == std::string::npos) continue;
    out.date = date_of(between(entry, "<updated>", "</updated>"));
    size_t open = entry.find("<content");
    if (open == std::string::npos) return true;
    open = entry.find('>', open);
    size_t close = entry.find("</content>", open);
    if (open == std::string::npos || close == std::string::npos) return true;
    const std::string html = unescape(entry.substr(open + 1, close - open - 1));
    if (!from_block(html, out)) from_prose(html, out);
    return true;
  }
}

bool latest(const std::wstring& base, const net::Proxy& proxy, Release& out, net::Failure& why) {
  out = Release();
  std::wstring where;
  if (!net::redirect_of(base + L"/releases/latest", proxy, where, why)) return false;
  const wchar_t* mark = L"/releases/tag/";
  size_t at = where.find(mark);
  if (at == std::wstring::npos) {  // (a repository without releases)
    why.status = 404;
    return false;
  }
  out.tag = where.substr(at + wcslen(mark));
  size_t cut = out.tag.find_first_of(L"?#/");
  if (cut != std::wstring::npos) out.tag.resize(cut);
  out.version = (!out.tag.empty() && (out.tag[0] == L'v' || out.tag[0] == L'V')) ? out.tag.substr(1) : out.tag;
  // The notes are a nicety: the version alone is enough to update by.
  std::string feed;
  net::Failure ignored;
  if (net::get(base + L"/releases.atom", proxy, feed, ignored)) from_feed(feed, out.tag, out);
  return true;
}

std::wstring installed_version(const std::wstring& game_dir) {
  if (game_dir.empty()) return L"";
  std::ifstream in(gamedir::join(game_dir, L"tmntnet-install.txt"));
  std::string line;
  if (!in || !std::getline(in, line)) return L"";
  // "# tmntnet 0.13.9 host install. ..."
  const char* mark = "tmntnet ";
  size_t at = line.find(mark);
  if (at == std::string::npos) return L"";
  at += strlen(mark);
  size_t end = line.find(' ', at);
  const std::string v = line.substr(at, end == std::string::npos ? std::string::npos : end - at);
  if (v.empty() || !(v[0] >= '0' && v[0] <= '9')) return L"";
  return widen(v);
}

bool game_running(const std::wstring& game_dir) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  const std::wstring want = gamedir::join(game_dir, L"TMNT.EXE");
  bool running = false;
  PROCESSENTRY32W e{sizeof(e)};
  for (BOOL ok = Process32FirstW(snap, &e); ok && !running; ok = Process32NextW(snap, &e)) {
    if (_wcsicmp(e.szExeFile, L"TMNT.EXE") != 0) continue;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
    if (!p) {  // (another user's or an elevated one: its folder is not told)
      running = true;
      break;
    }
    wchar_t path[MAX_PATH * 2];
    DWORD len = MAX_PATH * 2;
    if (!QueryFullProcessImageNameW(p, 0, path, &len) || _wcsicmp(path, want.c_str()) == 0) running = true;
    CloseHandle(p);
  }
  CloseHandle(snap);
  return running;
}

bool can_write(const std::wstring& dir) {
  const std::wstring probe = gamedir::join(dir, L"tmntnet-write-test.tmp");
  HANDLE f = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  CloseHandle(f);
  return true;
}

std::wstring sha256_of(const std::wstring& file) {
  HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (f == INVALID_HANDLE_VALUE) return L"";
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::wstring out;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
      BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
    std::vector<unsigned char> buf(1 << 16);
    bool ok = true;
    for (;;) {
      DWORD got = 0;
      if (!ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr)) {
        ok = false;
        break;
      }
      if (!got) break;
      if (BCryptHashData(hash, buf.data(), got, 0) != 0) {
        ok = false;
        break;
      }
    }
    unsigned char digest[32];
    if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
      static const wchar_t* kHex = L"0123456789abcdef";
      for (unsigned char b : digest) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
      }
    }
  }
  if (hash) BCryptDestroyHash(hash);
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  CloseHandle(f);
  return out;
}

std::wstring cache_dir() {
  wchar_t lab[MAX_PATH * 2];  // (lab runs keep their files apart)
  const DWORD n = GetEnvironmentVariableW(L"TMNTNET_LAUNCHER_CACHE", lab, MAX_PATH * 2);
  if (n && n < MAX_PATH * 2) {
    CreateDirectoryW(lab, nullptr);
    return lab;
  }
  PWSTR local = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) dir = local;
  CoTaskMemFree(local);
  if (dir.empty()) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    dir = tmp;
  }
  dir = gamedir::join(dir, L"tmntnet");
  CreateDirectoryW(dir.c_str(), nullptr);
  dir = gamedir::join(dir, L"cache");
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

bool run_setup(const std::wstring& setup, const wchar_t* action, const std::wstring& dir_in, std::wstring& what) {
  std::wstring dir = dir_in;
  while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();  // (\" would swallow the quote)
  const std::wstring log = gamedir::join(cache_dir(), L"setup-log.txt");
  DeleteFileW(log.c_str());
  SetEnvironmentVariableW(L"TMNTNET_SETUP_LOG", log.c_str());
  const std::wstring args = std::wstring(L"/quiet ") + action + L" \"" + dir + L"\"";
  HANDLE process = nullptr;
  if (can_write(dir)) {
    std::wstring cmd = L"\"" + setup + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(setup.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      what = L"установщик не запустился (ошибка " + std::to_wstring(GetLastError()) + L")";
      return false;
    }
    CloseHandle(pi.hThread);
    process = pi.hProcess;
  } else {  // (a folder under Program Files: Windows asks for the administrator's yes)
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = setup.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
      what = GetLastError() == ERROR_CANCELLED ? L"папка игры защищена от записи: нужно разрешение администратора"
                                               : L"установщик не запустился (ошибка " + std::to_wstring(GetLastError()) + L")";
      return false;
    }
    process = sei.hProcess;
  }
  WaitForSingleObject(process, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(process, &code);
  CloseHandle(process);
  if (code == 0) return true;
  // Its own words, when it left them (TMNTNET_SETUP_LOG, 0.14.0 and newer).
  std::ifstream in(log, std::ios::binary);
  std::string line, last;
  while (std::getline(in, line)) {
    line = trimmed(line);
    if (!line.empty()) last = line;
  }
  what = last.empty() ? L"установщик сообщил об ошибке (код " + std::to_wstring(code) + L")" : widen(last);
  return false;
}

}  // namespace update
