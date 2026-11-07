// Wardrobe - a native window serving the app from a local folder (app\ beside
// the exe) over a private virtual host, so it runs with no network, no login
// and its own crisp taskbar icon. It also bridges the page to Claude Code on
// this computer: the page posts a request, we hand it to `claude -p` with the
// photos saved as temp files, and post the answer back.
#include <windows.h>
#include <wincrypt.h>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <wrl.h>
#include "WebView2.h"

using namespace Microsoft::WRL;

static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;
static const UINT WM_BRIDGE_REPLY = WM_APP + 1;

static std::string Narrow(const std::wstring& w) {
  if (w.empty()) return std::string();
  int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string out(len, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], len, nullptr, nullptr);
  return out;
}
static std::wstring Widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring out(len, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
  return out;
}

static bool WriteFileBytes(const std::wstring& path, const void* data, DWORD size) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  BOOL ok = WriteFile(h, data, size, &written, nullptr);
  CloseHandle(h);
  return ok && written == size;
}
static std::string ReadFileBytes(const std::wstring& path) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::string();
  DWORD size = GetFileSize(h, nullptr);
  std::string out(size, 0);
  DWORD rd = 0;
  if (size) ReadFile(h, &out[0], size, &rd, nullptr);
  CloseHandle(h);
  out.resize(rd);
  return out;
}

// Runs one page request against Claude Code and posts the reply to the UI thread.
static void RunBridgeRequest(HWND hwnd, std::string payload) {
  // payload: REQ\n<id>\n<nImages>\n<img b64>...\n<prompt...>
  auto reply = [hwnd](const std::string& id, bool ok, const std::string& text) {
    std::string msg = "RES\n" + id + "\n" + (ok ? "OK" : "ERR") + "\n" + text;
    auto* heap = new std::wstring(Widen(msg));
    if (!PostMessageW(hwnd, WM_BRIDGE_REPLY, 0, (LPARAM)heap)) delete heap;
  };
  size_t p1 = payload.find('\n', 4);
  size_t p2 = payload.find('\n', p1 + 1);
  if (p1 == std::string::npos || p2 == std::string::npos) return;
  std::string id = payload.substr(4, p1 - 4);
  int nImages = atoi(payload.c_str() + p1 + 1);
  size_t pos = p2 + 1;

  wchar_t tempRoot[MAX_PATH];
  GetTempPathW(MAX_PATH, tempRoot);
  std::wstring dir = std::wstring(tempRoot) + L"wardrobe-ai\\" + Widen(id);
  std::wstring parent = std::wstring(tempRoot) + L"wardrobe-ai";
  CreateDirectoryW(parent.c_str(), nullptr);
  CreateDirectoryW(dir.c_str(), nullptr);

  std::string pathNote;
  for (int i = 0; i < nImages; i++) {
    size_t end = payload.find('\n', pos);
    if (end == std::string::npos) { reply(id, false, "The request arrived malformed."); return; }
    std::string b64 = payload.substr(pos, end - pos);
    pos = end + 1;
    DWORD binLen = 0;
    if (!CryptStringToBinaryA(b64.c_str(), (DWORD)b64.size(), CRYPT_STRING_BASE64, nullptr, &binLen, nullptr, nullptr)) continue;
    std::vector<BYTE> bin(binLen);
    if (!CryptStringToBinaryA(b64.c_str(), (DWORD)b64.size(), CRYPT_STRING_BASE64, bin.data(), &binLen, nullptr, nullptr)) continue;
    bool isPng = binLen > 3 && bin[0] == 0x89 && bin[1] == 'P';
    wchar_t name[32];
    swprintf_s(name, 32, L"photo%d.%s", i + 1, isPng ? L"png" : L"jpg");
    std::wstring fp = dir + L"\\" + name;
    WriteFileBytes(fp, bin.data(), binLen);
    char line[640];
    sprintf_s(line, 640, "Photo %d is the image file at: %s\n", i + 1, Narrow(fp).c_str());
    pathNote += line;
  }
  std::string prompt = payload.substr(pos);
  if (!pathNote.empty())
    prompt = "Image files for this request (read each one):\n" + pathNote + "\n" + prompt;

  std::wstring promptFile = dir + L"\\prompt.txt";
  std::wstring outFile = dir + L"\\out.txt";
  std::wstring errFile = dir + L"\\err.txt";
  WriteFileBytes(promptFile, prompt.data(), (DWORD)prompt.size());

  std::wstring cmd = L"cmd.exe /c claude -p --output-format text --allowedTools Read,WebSearch < \"" +
                     promptFile + L"\" > \"" + outFile + L"\" 2> \"" + errFile + L"\"";
  std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
  cmdBuf.push_back(0);

  STARTUPINFOW si = {}; si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
    reply(id, false, "Couldn't start a command prompt on this computer.");
    return;
  }
  DWORD wait = WaitForSingleObject(pi.hProcess, 170000);
  if (wait == WAIT_TIMEOUT) {
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    reply(id, false, "Claude Code took longer than 3 minutes and was stopped. Try again, or switch to Gemini in Settings.");
    return;
  }
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess);

  std::string out = ReadFileBytes(outFile);
  std::string err = ReadFileBytes(errFile);
  // trim
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
  if (code == 0 && !out.empty()) { reply(id, true, out); return; }
  if (code == 9009 || err.find("not recognized") != std::string::npos) {
    reply(id, false, "Claude Code isn't installed on this computer (the 'claude' command wasn't found). Install it, sign in once, and try again - or switch to Gemini in Settings.");
    return;
  }
  std::string detail = !err.empty() ? err : out;
  if (detail.size() > 600) detail = detail.substr(0, 600);
  reply(id, false, detail.empty() ? "Claude Code returned nothing. Is it signed in? Run 'claude' once in a terminal to check." : detail);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SIZE:
      if (g_controller) { RECT r; GetClientRect(hwnd, &r); g_controller->put_Bounds(r); }
      return 0;
    case WM_DPICHANGED: {
      RECT* r = (RECT*)lp;
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_BRIDGE_REPLY: {
      std::wstring* text = (std::wstring*)lp;
      if (text) {
        if (g_webview) g_webview->PostWebMessageAsString(text->c_str());
        delete text;
      }
      return 0;
    }
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
  // Without this, Windows bitmap-stretches the window on scaled displays
  // and everything renders blurry.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  WNDCLASSW wc = {};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.lpszClassName = L"WardrobeApp";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(RGB(253, 250, 251));
  wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
  RegisterClassW(&wc);

  HWND hwnd = CreateWindowExW(0, L"WardrobeApp", L"Wardrobe", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1280, 860,
                              nullptr, nullptr, hInst, nullptr);
  HICON big = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 256, 256, 0);
  if (big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
  ShowWindow(hwnd, nCmdShow);

  // the app folder lives beside the exe
  wchar_t exePath[MAX_PATH];
  GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  std::wstring appDir(exePath);
  appDir = appDir.substr(0, appDir.find_last_of(L'\\')) + L"\\app";

  wchar_t udf[MAX_PATH];
  ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\WardrobeApp", udf, MAX_PATH);

  CreateCoreWebView2EnvironmentWithOptions(nullptr, udf, nullptr,
    Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
      [hwnd, appDir](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
        if (FAILED(hr) || !env) {
          MessageBoxW(hwnd, L"WebView2 runtime is missing. Install 'WebView2 Runtime' from Microsoft and try again.", L"Wardrobe", MB_ICONERROR);
          return hr;
        }
        env->CreateCoreWebView2Controller(hwnd,
          Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [hwnd, appDir](HRESULT hr2, ICoreWebView2Controller* ctrl) -> HRESULT {
              if (FAILED(hr2) || !ctrl) return hr2;
              g_controller = ctrl;
              ctrl->get_CoreWebView2(&g_webview);
              RECT r; GetClientRect(hwnd, &r);
              ctrl->put_Bounds(r);
              if (g_webview) {
                // serve the local app folder on a private host
                ComPtr<ICoreWebView2_3> wv3;
                if (SUCCEEDED(g_webview.As(&wv3)) && wv3) {
                  wv3->SetVirtualHostNameToFolderMapping(
                    L"wardrobe.local", appDir.c_str(),
                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                } else {
                  MessageBoxW(hwnd, L"This WebView2 runtime is too old for local mode. Update 'WebView2 Runtime' from Microsoft.", L"Wardrobe", MB_ICONERROR);
                }
                // page -> Claude Code bridge
                EventRegistrationToken tok;
                g_webview->add_WebMessageReceived(
                  Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                    [hwnd](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                      LPWSTR raw = nullptr;
                      if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                        std::wstring wmsg(raw);
                        CoTaskMemFree(raw);
                        if (wmsg.rfind(L"REQ\n", 0) == 0) {
                          std::string payload = Narrow(wmsg);
                          std::thread(RunBridgeRequest, hwnd, std::move(payload)).detach();
                        }
                      }
                      return S_OK;
                    }).Get(), &tok);
                // a fresh query string every launch defeats stale HTTP caches
                wchar_t url[256];
                swprintf_s(url, 256, L"https://wardrobe.local/index.html?r=%llu",
                         (unsigned long long)GetTickCount64());
                g_webview->Navigate(url);
              }
              return S_OK;
            }).Get());
        return S_OK;
      }).Get());

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}
