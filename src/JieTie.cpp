// ============================================================
// 截贴 (JieTie) — 截图自动变文件 / 自动保存 / 直接粘贴到文件夹
// ------------------------------------------------------------
// 挂在系统托盘后台运行。监听剪贴板里的图片（系统截图
// Win+Shift+PrtSc / Win+Shift+S、QQ/微信截图、任意复制的图片），
// 按模式处理：
//
//   Mode=clip  （默认）截图后剪贴板里就"是一个文件"，
//              在任意文件夹 / 桌面按 Ctrl+V 直接粘贴出 PNG 文件；
//              图片数据仍保留在剪贴板，粘贴到聊天框还是图片。
//   Mode=save  自动保存 PNG 到指定文件夹（连续截图连续保存）。
//   Mode=both  两个都做：既存到文件夹，也能 Ctrl+V 粘贴。
//
// 托盘图标：双击打开保存文件夹，右键菜单切换模式/设置/暂停/自启。
//
// 配置文件：exe 同目录 JieTie.ini（首次运行自动生成）
//   [Settings]
//   Mode=clip            clip / save / both
//   Folder=              保存文件夹（留空 = 桌面\Screenshots）
//   Prefix=screenshot    文件名前缀
//   MinWidth=0           小于该宽度不处理（0 = 不限制）
//   MinHeight=0          小于该高度不处理
//   Notify=0             处理完弹通知（1 = 开）
//   Paused=0             启动时是否暂停
//
// 编译（MinGW-w64 g++，见 build.bat）：
//   g++ -municode -mwindows -std=c++17 -O2 -static -static-libgcc
//       -static-libstdc++ -finput-charset=UTF-8 JieTie.cpp -o JieTie.exe
//       -lgdiplus -lshlwapi -lshell32 -lole32 -luuid -ladvapi32 -lgdi32
// ============================================================

#define _WIN32_WINNT 0x0601
#define WINVER       0x0601

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cwchar>

// ---------------- 常量 ----------------
static const wchar_t kMutexName[]  = L"Local\\JieTie_SingleInstance_v1";
static const wchar_t kWndClass[]   = L"JieTie_MsgWnd_v1";
static const wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kRunValue[]   = L"JieTie";
static const UINT    WM_TRAYICON   = WM_APP + 1;
static const UINT    TRAY_ID       = 1;
static const UINT    TIMER_CLIP    = 2;   // 剪贴板更新去抖定时器

enum {
  IDM_OPEN      = 1001,
  IDM_PAUSE     = 1002,
  IDM_SETFOLDER = 1003,
  IDM_AUTOSTART = 1004,
  IDM_EXIT      = 1005,
  IDM_MODE_CLIP = 1011,
  IDM_MODE_SAVE = 1012,
  IDM_MODE_BOTH = 1013,
};

enum { MODE_CLIP = 0, MODE_SAVE = 1, MODE_BOTH = 2 };

// ---------------- 全局状态 ----------------
static HINSTANCE g_hInst     = NULL;
static HWND      g_hwnd      = NULL;
static HICON     g_hIcon     = NULL;
static ULONG_PTR g_gdipToken = 0;

static std::wstring g_iniPath;
static std::wstring g_folder;                 // 自动保存文件夹
static std::wstring g_stageDir;               // 暂存文件夹（clip 模式）
static std::wstring g_prefix = L"screenshot";
static int          g_mode    = MODE_CLIP;
static bool         g_paused  = false;
static bool         g_notify  = false;
static int          g_minW    = 0;
static int          g_minH    = 0;

static uint64_t  g_lastHash     = 0;   // 1.5 秒内相同内容去重
static ULONGLONG g_lastHashTick = 0;
static bool      g_hasLast      = false;

static volatile LONG g_busy   = 0;              // 工作线程互斥
static volatile LONG g_ownSeq = 0x7FFFFFFF;     // 自己改写剪贴板后的序号，用于跳过自己的更新
static std::wstring g_wkPath;
static bool         g_wkSaved   = false;
static bool         g_wkAttempted = false;

// ---------------- 小工具 ----------------
static std::wstring ExePath() {
  wchar_t buf[MAX_PATH] = L"";
  GetModuleFileNameW(NULL, buf, MAX_PATH);
  return buf;
}

static std::wstring ExeDir() {
  std::wstring s = ExePath();
  size_t p = s.find_last_of(L"\\/");
  return (p == std::wstring::npos) ? L"." : s.substr(0, p);
}

static std::wstring Trim(const std::wstring& s) {
  size_t a = s.find_first_not_of(L" \t\r\n\"'");
  if (a == std::wstring::npos) return L"";
  size_t b = s.find_last_not_of(L" \t\r\n\"'");
  return s.substr(a, b - a + 1);
}

static std::wstring SanitizeName(const std::wstring& s) {
  static const wchar_t* bad = L"\\/:*?\"<>|";
  std::wstring r;
  for (size_t i = 0; i < s.size(); i++) {
    wchar_t c = s[i];
    if (c && !wcschr(bad, c)) r += c;
  }
  if (r.size() > 48) r.resize(48);
  return r;
}

static std::wstring IniGet(const wchar_t* key, const wchar_t* def) {
  wchar_t buf[2048] = L"";
  GetPrivateProfileStringW(L"Settings", key, def, buf, 2048, g_iniPath.c_str());
  return buf;
}

static void IniSet(const wchar_t* key, const std::wstring& val) {
  WritePrivateProfileStringW(L"Settings", key, val.c_str(), g_iniPath.c_str());
}

static std::wstring DefaultFolder() {
  wchar_t desk[MAX_PATH] = L"";
  if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, desk)))
    return std::wstring(desk) + L"\\Screenshots";
  return ExeDir() + L"\\Screenshots";
}

static bool DirExists(const std::wstring& d) {
  DWORD a = GetFileAttributesW(d.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void InitStageDir() {
  wchar_t tmp[MAX_PATH] = L"";
  DWORD n = GetTempPathW(MAX_PATH, tmp);
  if (n == 0 || n >= MAX_PATH) tmp[0] = 0, lstrcpyW(tmp, L".\\");
  g_stageDir = std::wstring(tmp);
  if (!g_stageDir.empty() && g_stageDir[g_stageDir.size() - 1] == L'\\')
    g_stageDir.erase(g_stageDir.size() - 1);
  g_stageDir += L"\\JieTie";
  SHCreateDirectoryExW(NULL, g_stageDir.c_str(), NULL);
}

// 暂存文件 7 天后清理（粘贴时 Explorer 是复制，暂存原件可随时删）
static void CleanOldStageFiles() {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((g_stageDir + L"\\*.png").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  FILETIME nowFt;
  GetSystemTimeAsFileTime(&nowFt);
  ULARGE_INTEGER now;
  now.LowPart = nowFt.dwLowDateTime;
  now.HighPart = nowFt.dwHighDateTime;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    ULARGE_INTEGER ft;
    ft.LowPart = fd.ftLastWriteTime.dwLowDateTime;
    ft.HighPart = fd.ftLastWriteTime.dwHighDateTime;
    if (now.QuadPart > ft.QuadPart &&
        (now.QuadPart - ft.QuadPart) / 10000000ULL > 7ULL * 24 * 3600)
      DeleteFileW((g_stageDir + L"\\" + fd.cFileName).c_str());
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

static void LoadConfig() {
  g_iniPath = ExeDir() + L"\\JieTie.ini";
  g_folder  = Trim(IniGet(L"Folder", L""));
  if (g_folder.empty()) {
    g_folder = DefaultFolder();
    IniSet(L"Folder", g_folder);
  }
  std::wstring p = SanitizeName(Trim(IniGet(L"Prefix", L"screenshot")));
  if (!p.empty()) g_prefix = p;

  std::wstring m = Trim(IniGet(L"Mode", L""));
  if (m.empty()) {
    m = L"clip";
    IniSet(L"Mode", m);   // 首次运行写进配置，方便用户自己改
  }
  if (m == L"save")      g_mode = MODE_SAVE;
  else if (m == L"both") g_mode = MODE_BOTH;
  else                   g_mode = MODE_CLIP;

  g_minW   = _wtoi(IniGet(L"MinWidth",  L"0").c_str());
  g_minH   = _wtoi(IniGet(L"MinHeight", L"0").c_str());
  g_notify = IniGet(L"Notify", L"0") == L"1";
  g_paused = IniGet(L"Paused", L"0") == L"1";
}

static void SaveModeToIni() {
  const wchar_t* m = (g_mode == MODE_SAVE) ? L"save" : (g_mode == MODE_BOTH ? L"both" : L"clip");
  IniSet(L"Mode", m);
}

// ---------------- 开机自启（HKCU Run） ----------------
static bool AutostartEnabled() {
  HKEY k = NULL;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
  LONG r = RegQueryValueExW(k, kRunValue, NULL, NULL, NULL, NULL);
  RegCloseKey(k);
  return r == ERROR_SUCCESS;
}

static void SetAutostart(bool on) {
  HKEY k = NULL;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
  if (on) {
    std::wstring cmd = L"\"" + ExePath() + L"\"";
    RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                   (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(k, kRunValue);
  }
  RegCloseKey(k);
}

// ---------------- 哈希去重 ----------------
static uint64_t Fnv1a64(const void* data, size_t len) {
  const BYTE* p = (const BYTE*)data;
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ULL; }
  return h;
}

static bool IsDupHash(uint64_t h) {
  ULONGLONG now = GetTickCount64();
  if (g_hasLast && h == g_lastHash && (now - g_lastHashTick) < 1500) return true;
  g_lastHash = h;
  g_lastHashTick = now;
  g_hasLast = true;
  return false;
}

// ---------------- PNG / 文件辅助 ----------------
static bool PngGetSize(const BYTE* p, DWORD size, UINT* w, UINT* h) {
  if (size < 24) return false;
  if (memcmp(p, "\x89PNG\r\n\x1a\n", 8) != 0) return false;
  if (memcmp(p + 12, "IHDR", 4) != 0) return false;
  *w = ((UINT)p[16] << 24) | ((UINT)p[17] << 16) | ((UINT)p[18] << 8) | (UINT)p[19];
  *h = ((UINT)p[20] << 24) | ((UINT)p[21] << 16) | ((UINT)p[22] << 8) | (UINT)p[23];
  return true;
}

static DWORD PngRealSize(const BYTE* p, DWORD size) {
  if (size < 8 || memcmp(p, "\x89PNG\r\n\x1a\n", 8) != 0) return size;
  DWORD off = 8;
  while (off + 8 <= size) {
    DWORD len = ((DWORD)p[off] << 24) | ((DWORD)p[off+1] << 16) | ((DWORD)p[off+2] << 8) | (DWORD)p[off+3];
    const BYTE* type = p + off + 4;
    DWORD next = off + 12 + len;
    if (next > size) return size;
    if (memcmp(type, "IEND", 4) == 0) return next;
    off = next;
  }
  return size;
}

static bool GetPngClsid(CLSID* clsid) {
  UINT num = 0, size = 0;
  if (Gdiplus::GetImageEncodersSize(&num, &size) != Gdiplus::Ok || size == 0) return false;
  std::vector<BYTE> buf(size);
  Gdiplus::ImageCodecInfo* info = (Gdiplus::ImageCodecInfo*)buf.data();
  if (Gdiplus::GetImageEncoders(num, size, info) != Gdiplus::Ok) return false;
  for (UINT i = 0; i < num; i++) {
    if (wcscmp(info[i].MimeType, L"image/png") == 0) { *clsid = info[i].Clsid; return true; }
  }
  return false;
}

static std::vector<BYTE> ReadAllBytes(const std::wstring& path) {
  std::vector<BYTE> v;
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) return v;
  LARGE_INTEGER sz = {};
  if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < (512LL << 20)) {
    v.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    if (ReadFile(f, v.data(), (DWORD)v.size(), &rd, NULL)) v.resize(rd);
    else v.clear();
  }
  CloseHandle(f);
  return v;
}

static std::wstring MakeUniquePathIn(const std::wstring& dir) {
  SHCreateDirectoryExW(NULL, dir.c_str(), NULL);
  for (int i = 0; i < 200; i++) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t core[160];
    _snwprintf(core, 159, L"%s_%04d%02d%02d_%02d%02d%02d_%03d", g_prefix.c_str(),
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    core[159] = 0;
    std::wstring name(core);
    if (i) name += L"_" + std::to_wstring(i + 1);
    name += L".png";
    std::wstring path = dir + L"\\" + name;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return path;
    Sleep(2);
  }
  return dir + L"\\" + g_prefix + L"_" + std::to_wstring(GetTickCount64()) + L".png";
}

// ---------------- GDI+ 位图辅助 ----------------
static Gdiplus::Bitmap* CloneBitmap(Gdiplus::Bitmap* src) {
  if (!src) return NULL;
  Gdiplus::Status s = src->GetLastStatus();
  if (s != Gdiplus::Ok) return NULL;
  UINT w = src->GetWidth(), h = src->GetHeight();
  if (w == 0 || h == 0) return NULL;
  Gdiplus::Bitmap* c = src->Clone(0, 0, w, h, PixelFormat32bppARGB);
  if (!c || c->GetLastStatus() != Gdiplus::Ok) { delete c; return NULL; }
  return c;
}

static Gdiplus::Bitmap* DecodePngBytes(const std::vector<BYTE>& bytes) {
  if (bytes.empty()) return NULL;
  IStream* st = SHCreateMemStream(bytes.data(), (UINT)bytes.size());
  if (!st) return NULL;
  Gdiplus::Bitmap* raw = Gdiplus::Bitmap::FromStream(st, FALSE);
  Gdiplus::Bitmap* bmp = CloneBitmap(raw);
  if (raw) delete raw;
  st->Release();
  return bmp;
}

// 把位图渲染成 32bpp 的 DIB 字节（40 字节 BITMAPINFOHEADER + 像素），用于放进剪贴板
static bool RenderDib32(Gdiplus::Bitmap* bmp, std::vector<BYTE>& out) {
  out.clear();
  UINT w = bmp->GetWidth(), h = bmp->GetHeight();
  if (!w || !h || w > 20000 || h > 20000) return false;
  HBITMAP hbm = NULL;
  if (bmp->GetHBITMAP(Gdiplus::Color(255, 255, 255, 255), &hbm) != Gdiplus::Ok || !hbm)
    return false;

  BITMAPINFO bi = {};
  bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth       = (LONG)w;
  bi.bmiHeader.biHeight      = (LONG)h;    // 正值 = 自下而上（兼容性最好）
  bi.bmiHeader.biPlanes      = 1;
  bi.bmiHeader.biBitCount    = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  out.resize(sizeof(BITMAPINFOHEADER) + (size_t)w * 4 * h);
  HDC hdc = GetDC(NULL);
  int lines = GetDIBits(hdc, hbm, 0, h, out.data() + sizeof(BITMAPINFOHEADER), &bi, DIB_RGB_COLORS);
  ReleaseDC(NULL, hdc);
  DeleteObject(hbm);
  if (lines != (int)h) { out.clear(); return false; }
  memcpy(out.data(), &bi.bmiHeader, sizeof(BITMAPINFOHEADER));
  return true;
}

// ---------------- 剪贴板写入（关键：让图片变成"可粘贴的文件"） ----------------
static bool SetClipDataBlock(UINT fmt, const void* data, size_t size) {
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1);
  if (!h) return false;
  void* p = GlobalLock(h);
  if (!p) { GlobalFree(h); return false; }
  if (data && size) memcpy(p, data, size);
  GlobalUnlock(h);
  if (!SetClipboardData(fmt, h)) { GlobalFree(h); return false; }
  return true;   // 成功后内存归系统所有，不能自己 Free
}

static HGLOBAL MakeHDrop(const std::wstring& path) {
  const size_t bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
  if (!h) return NULL;
  DROPFILES* df = (DROPFILES*)GlobalLock(h);
  if (!df) { GlobalFree(h); return NULL; }
  df->pFiles = sizeof(DROPFILES);
  df->fWide  = TRUE;
  wchar_t* dst = (wchar_t*)((BYTE*)df + sizeof(DROPFILES));
  memcpy(dst, path.c_str(), path.size() * sizeof(wchar_t));
  dst[path.size()]     = 0;
  dst[path.size() + 1] = 0;   // 双 null 结尾
  GlobalUnlock(h);
  return h;
}

// 重建剪贴板：CF_HDROP（文件）+ CF_DIB（图片）+ "PNG"（无损）
// 这样在文件夹里 Ctrl+V 会生成 PNG 文件，在聊天框里 Ctrl+V 仍是图片
static bool RebuildClipboard(Gdiplus::Bitmap* bmp, const std::wstring& filePath,
                             const std::vector<BYTE>& png) {
  std::vector<BYTE> dib;
  if (!RenderDib32(bmp, dib)) return false;

  bool opened = false;
  for (int i = 0; i < 25 && !opened; i++) {
    if (OpenClipboard(g_hwnd)) opened = true;
    else Sleep(20);
  }
  if (!opened) return false;

  EmptyClipboard();

  HGLOBAL hDrop = MakeHDrop(filePath);
  if (hDrop && !SetClipboardData(CF_HDROP, hDrop)) GlobalFree(hDrop);

  if (!dib.empty()) {
    if (SetClipDataBlock(CF_DIB, dib.data(), dib.size())) {
      // 同时放一份 CF_BITMAP（有些老程序只认这个格式）：用 DDB 位图，跨进程最稳
      BITMAPINFO* bi = (BITMAPINFO*)dib.data();
      LONG bw = bi->bmiHeader.biWidth;
      LONG bh = bi->bmiHeader.biHeight < 0 ? -bi->bmiHeader.biHeight : bi->bmiHeader.biHeight;
      HDC hdc = GetDC(NULL);
      HBITMAP hbm = CreateCompatibleBitmap(hdc, bw, bh);
      if (hbm && SetDIBits(hdc, hbm, 0, bh, dib.data() + sizeof(BITMAPINFOHEADER), bi, DIB_RGB_COLORS) == (int)bh) {
        if (!SetClipboardData(CF_BITMAP, hbm)) DeleteObject(hbm);
      } else if (hbm) {
        DeleteObject(hbm);
      }
      ReleaseDC(NULL, hdc);
    }
  }

  UINT pngFmt = RegisterClipboardFormatW(L"PNG");
  if (pngFmt && !png.empty()) SetClipDataBlock(pngFmt, png.data(), png.size());

  CloseClipboard();
  g_ownSeq = (LONG)GetClipboardSequenceNumber();   // 记下这次是自己改的
  return true;
}

// ---------------- 剪贴板读取 ----------------
// 返回：0=没有图片, 1=处理完毕（含被过滤/重复）, 2=需要重试
static int AnalyzeClipboardOnce(Gdiplus::Bitmap** outBmp) {
  *outBmp = NULL;
  const DWORD seq1 = GetClipboardSequenceNumber();

  // ---- 1) 原生 PNG 字节 ----
  UINT pngFmt = RegisterClipboardFormatW(L"PNG");
  if (pngFmt && IsClipboardFormatAvailable(pngFmt)) {
    HANDLE h = GetClipboardData(pngFmt);
    if (!h) return 2;
    SIZE_T sz = GlobalSize(h);
    BYTE* p = (BYTE*)GlobalLock(h);
    if (!p) return 2;
    std::vector<BYTE> bytes;
    UINT w = 0, hh = 0;
    bool okSize = (sz >= 24) && PngGetSize(p, (DWORD)sz, &w, &hh);
    if (okSize) bytes.assign(p, p + sz);
    GlobalUnlock(h);
    if (!okSize) return 2;
    if (w < (UINT)g_minW || hh < (UINT)g_minH) return 1;
    if (IsDupHash(Fnv1a64(bytes.data(), bytes.size()))) return 1;
    if (seq1 != GetClipboardSequenceNumber()) return 2;
    *outBmp = DecodePngBytes(bytes);
    return *outBmp ? 1 : 2;
  }

  // ---- 2) DIBV5 / DIB ----
  const UINT dibFmts[2] = { CF_DIBV5, CF_DIB };
  for (int f = 0; f < 2; f++) {
    if (!IsClipboardFormatAvailable(dibFmts[f])) continue;
    HANDLE h = GetClipboardData(dibFmts[f]);
    if (!h) return 2;
    void* p = GlobalLock(h);
    if (!p) return 2;
    SIZE_T sz = GlobalSize(h);
    BITMAPINFO* bmi = (BITMAPINFO*)p;
    if (sz < bmi->bmiHeader.biSize + 4) { GlobalUnlock(h); return 2; }
    BYTE* bits = (BYTE*)p + bmi->bmiHeader.biSize;
    if (bmi->bmiHeader.biCompression == BI_BITFIELDS &&
        bmi->bmiHeader.biSize == sizeof(BITMAPINFOHEADER))
      bits += 3 * sizeof(DWORD);
    if (bmi->bmiHeader.biBitCount <= 8) {
      DWORD colors = bmi->bmiHeader.biClrUsed
        ? bmi->bmiHeader.biClrUsed : (1u << bmi->bmiHeader.biBitCount);
      bits += colors * sizeof(RGBQUAD);
    }
    LONG bw = bmi->bmiHeader.biWidth;
    LONG bh = bmi->bmiHeader.biHeight < 0 ? -bmi->bmiHeader.biHeight : bmi->bmiHeader.biHeight;
    uint64_t hash = Fnv1a64(p, (size_t)sz);
    bool skip = IsDupHash(hash) || bw <= 0 || bh <= 0 ||
                bw < g_minW || bh < g_minH;
    Gdiplus::Bitmap* bmp = NULL;
    if (!skip) {
      Gdiplus::Bitmap* raw = Gdiplus::Bitmap::FromBITMAPINFO(bmi, bits);
      if (raw) { bmp = CloneBitmap(raw); delete raw; }
    }
    GlobalUnlock(h);
    if (seq1 != GetClipboardSequenceNumber()) { delete bmp; return 2; }
    if (bmp) { *outBmp = bmp; return 1; }
    return skip ? 1 : 2;
  }

  // ---- 3) CF_BITMAP 兜底 ----
  if (IsClipboardFormatAvailable(CF_BITMAP)) {
    HBITMAP hbm = (HBITMAP)GetClipboardData(CF_BITMAP);
    if (!hbm) return 2;
    Gdiplus::Bitmap* raw = Gdiplus::Bitmap::FromHBITMAP(hbm, NULL);
    Gdiplus::Bitmap* bmp = CloneBitmap(raw);
    if (raw) delete raw;
    if (seq1 != GetClipboardSequenceNumber()) { delete bmp; return 2; }
    *outBmp = bmp;
    return bmp ? 1 : 2;
  }

  return 0;   // 剪贴板里没有图片
}

// ---------------- 处理一张截到的图 ----------------
static void ProcessCapturedBitmap(Gdiplus::Bitmap* bmp) {
  const bool doSave = (g_mode == MODE_SAVE || g_mode == MODE_BOTH);
  const bool doClip = (g_mode == MODE_CLIP || g_mode == MODE_BOTH);

  CLSID clsid;
  if (!GetPngClsid(&clsid)) return;

  // 文件落点：自动保存 -> 用户文件夹；只做粘贴 -> 暂存目录
  std::wstring filePath = MakeUniquePathIn(doSave ? g_folder : g_stageDir);
  g_wkPath = filePath;
  g_wkAttempted = true;
  if (bmp->Save(filePath.c_str(), &clsid, NULL) != Gdiplus::Ok) {
    g_wkSaved = false;
    return;
  }
  g_wkSaved = true;

  if (doClip) {
    std::vector<BYTE> png = ReadAllBytes(filePath);
    if (!RebuildClipboard(bmp, filePath, png)) {
      // 剪贴板被别的程序占着，尽力而为即可
    }
  }
}

// 工作线程：等源程序把剪贴板写完/渲染好，再带重试地读取处理
static void ShowBalloon(const wchar_t* title, const wchar_t* msg);   // 前置声明

static DWORD WINAPI SaveThreadProc(LPVOID) {
  Sleep(150);   // OLE 延迟渲染的源程序需要时间响应
  if ((DWORD)g_ownSeq == GetClipboardSequenceNumber()) {
    InterlockedExchange(&g_busy, 0);
    return 0;   // 这次更新是我们自己造成的
  }

  Gdiplus::Bitmap* bmp = NULL;
  int result = 0;
  for (int attempt = 0; attempt < 6; attempt++) {
    bool opened = false;
    for (int i = 0; i < 25 && !opened; i++) {
      if (OpenClipboard(NULL)) opened = true;
      else Sleep(20);
    }
    if (!opened) break;
    result = AnalyzeClipboardOnce(&bmp);
    CloseClipboard();
    if (result != 2) break;
    Sleep(150);
  }

  if (bmp) {
    ProcessCapturedBitmap(bmp);
    delete bmp;
  }

  if (result == 1 && g_wkAttempted) {
    if (g_wkSaved) {
      if (g_notify) {
        if (g_mode == MODE_CLIP) {
          std::wstring name = g_wkPath.substr(g_wkPath.find_last_of(L"\\/") + 1);
          ShowBalloon(L"截贴 已就绪", (name + L" 可 Ctrl+V 粘贴到文件夹").c_str());
        } else {
          std::wstring name = g_wkPath.substr(g_wkPath.find_last_of(L"\\/") + 1);
          ShowBalloon(L"截贴 已保存", name.c_str());
        }
      }
    } else {
      ShowBalloon(L"截贴 保存失败", g_wkPath.c_str());
    }
  }

  g_wkAttempted = false;
  g_wkSaved = false;
  g_wkPath.clear();
  InterlockedExchange(&g_busy, 0);
  return 0;
}

// ---------------- 托盘 ----------------
static HICON MakeTrayIcon() {
  Gdiplus::Bitmap bmp(32, 32, PixelFormat32bppARGB);
  Gdiplus::Graphics g(&bmp);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  Gdiplus::SolidBrush body(Gdiplus::Color(255, 0, 122, 204));
  g.FillRectangle(&body, 2, 10, 28, 18);
  g.FillRectangle(&body, 8, 6, 10, 6);
  Gdiplus::SolidBrush inner(Gdiplus::Color(255, 255, 255, 255));
  g.FillEllipse(&inner, 11, 13, 10, 10);
  g.FillEllipse(&body, 13, 15, 6, 6);
  HICON icon = NULL;
  bmp.GetHICON(&icon);
  return icon ? icon : LoadIconW(NULL, IDI_APPLICATION);
}

static void TrayModify(DWORD flags, const wchar_t* tip, const wchar_t* infoTitle, const wchar_t* info) {
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g_hwnd;
  nid.uID = TRAY_ID;
  nid.uFlags = flags;
  if (tip) lstrcpynW(nid.szTip, tip, 128);
  if (info) lstrcpynW(nid.szInfo, info, 256);
  if (infoTitle) lstrcpynW(nid.szInfoTitle, infoTitle, 64);
  nid.dwInfoFlags = NIIF_INFO;
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static std::wstring TrayTipText() {
  std::wstring t;
  if (g_mode == MODE_CLIP)      t = L"截贴 截图转文件（Ctrl+V 粘贴）";
  else if (g_mode == MODE_SAVE) t = L"截贴 截图自动保存";
  else                          t = L"截贴 自动保存 + 可粘贴";
  if (g_paused) t += L"（已暂停）";
  return t;
}

static void UpdateTrayTip() {
  std::wstring t = TrayTipText();
  TrayModify(NIF_TIP, t.c_str(), NULL, NULL);
}

static void ShowBalloon(const wchar_t* title, const wchar_t* msg) {
  TrayModify(NIF_INFO, NULL, title, msg);
}

static void OpenFolder() {
  SHCreateDirectoryExW(NULL, g_folder.c_str(), NULL);
  ShellExecuteW(g_hwnd, L"open", g_folder.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

// ---------------- 托盘菜单 ----------------
static void ShowTrayMenu() {
  POINT pt;
  GetCursorPos(&pt);
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, IDM_OPEN, L"打开保存文件夹(&O)");
  AppendMenuW(m, MF_STRING | (g_paused ? MF_CHECKED : 0), IDM_PAUSE,
              g_paused ? L"继续工作(&P)" : L"暂停(&P)");
  AppendMenuW(m, MF_SEPARATOR, 0, NULL);
  AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"截图后：");
  AppendMenuW(m, MF_STRING | (g_mode == MODE_CLIP ? MF_CHECKED : 0), IDM_MODE_CLIP,
              L"  变成文件，Ctrl+V 粘贴到文件夹(&1)");
  AppendMenuW(m, MF_STRING | (g_mode == MODE_SAVE ? MF_CHECKED : 0), IDM_MODE_SAVE,
              L"  自动保存到文件夹(&2)");
  AppendMenuW(m, MF_STRING | (g_mode == MODE_BOTH ? MF_CHECKED : 0), IDM_MODE_BOTH,
              L"  两种都做(&3)");
  AppendMenuW(m, MF_SEPARATOR, 0, NULL);
  AppendMenuW(m, MF_STRING, IDM_SETFOLDER, L"设置保存文件夹(&F)...");
  AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), IDM_AUTOSTART, L"开机自启动(&A)");
  AppendMenuW(m, MF_SEPARATOR, 0, NULL);
  AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出(&X)");
  SetForegroundWindow(g_hwnd);
  TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
  PostMessageW(g_hwnd, WM_NULL, 0, 0);
  DestroyMenu(m);
}

// ---------------- 选择文件夹对话框 ----------------
static void PickFolder() {
  IFileDialog* pfd = NULL;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                              IID_IFileDialog, (void**)&pfd)))
    return;
  DWORD opts = 0;
  pfd->GetOptions(&opts);
  pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  pfd->SetTitle(L"选择截图保存文件夹");
  IShellItem* cur = NULL;
  if (SUCCEEDED(SHCreateItemFromParsingName(g_folder.c_str(), NULL, IID_IShellItem, (void**)&cur)) && cur) {
    pfd->SetFolder(cur);
    cur->Release();
  }
  if (SUCCEEDED(pfd->Show(g_hwnd))) {
    IShellItem* res = NULL;
    if (SUCCEEDED(pfd->GetResult(&res)) && res) {
      PWSTR p = NULL;
      if (SUCCEEDED(res->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        g_folder = p;
        SHCreateDirectoryExW(NULL, g_folder.c_str(), NULL);
        IniSet(L"Folder", g_folder);
        CoTaskMemFree(p);
      }
      res->Release();
    }
  }
  pfd->Release();
}

// ---------------- 窗口过程 ----------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
  case WM_CLIPBOARDUPDATE:
    // 不立即抢剪贴板：去抖 120ms，等源程序的 OLE 写入/延迟渲染告一段落
    if (g_paused) return 0;
    if ((DWORD)g_ownSeq == GetClipboardSequenceNumber()) return 0;   // 自己改的，跳过
    SetTimer(hwnd, TIMER_CLIP, 120, NULL);
    return 0;
  case WM_TIMER:
    if (wParam == TIMER_CLIP) {
      KillTimer(hwnd, TIMER_CLIP);
      if (InterlockedCompareExchange(&g_busy, 1, 0) == 0)
        CloseHandle(CreateThread(NULL, 0, SaveThreadProc, NULL, 0, NULL));
    }
    return 0;
  case WM_TRAYICON:
    if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) ShowTrayMenu();
    else if (lParam == WM_LBUTTONDBLCLK) OpenFolder();
    return 0;
  case WM_COMMAND:
    switch (LOWORD(wParam)) {
    case IDM_OPEN:  OpenFolder(); break;
    case IDM_PAUSE:
      g_paused = !g_paused;
      IniSet(L"Paused", g_paused ? L"1" : L"0");
      UpdateTrayTip();
      break;
    case IDM_MODE_CLIP:
    case IDM_MODE_SAVE:
    case IDM_MODE_BOTH: {
      int old = g_mode;
      g_mode = (LOWORD(wParam) == IDM_MODE_CLIP) ? MODE_CLIP
             : (LOWORD(wParam) == IDM_MODE_SAVE) ? MODE_SAVE : MODE_BOTH;
      if (g_mode != old) {
        SaveModeToIni();
        UpdateTrayTip();
        if (g_mode == MODE_CLIP)
          ShowBalloon(L"截贴 模式已切换", L"截图后到任意文件夹按 Ctrl+V 即可粘贴出 PNG 文件");
        else if (g_mode == MODE_SAVE)
          ShowBalloon(L"截贴 模式已切换", (L"截图自动保存到：" + g_folder).c_str());
        else
          ShowBalloon(L"截贴 模式已切换", L"自动保存到文件夹，同时也可 Ctrl+V 粘贴");
      }
      break;
    }
    case IDM_SETFOLDER: PickFolder(); break;
    case IDM_AUTOSTART: SetAutostart(!AutostartEnabled()); break;
    case IDM_EXIT:      DestroyWindow(hwnd); break;
    }
    return 0;
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------- 入口 ----------------
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
  g_hInst = hInst;
  SetProcessDPIAware();
  if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 1;

  HANDLE mutex = CreateMutexW(NULL, TRUE, kMutexName);
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    MessageBoxW(NULL,
      L"截贴 已在后台运行（任务栏右下角托盘图标）。",
      L"截贴", MB_OK | MB_ICONINFORMATION);
    CloseHandle(mutex);
    CoUninitialize();
    return 0;
  }

  LoadConfig();
  InitStageDir();
  CleanOldStageFiles();

  Gdiplus::GdiplusStartupInput gsi;
  if (Gdiplus::GdiplusStartup(&g_gdipToken, &gsi, NULL) != Gdiplus::Ok) {
    MessageBoxW(NULL, L"GDI+ 初始化失败", L"截贴", MB_ICONERROR);
    return 1;
  }

  WNDCLASSEXW wc = {};
  wc.cbSize        = sizeof(wc);
  wc.lpfnWndProc   = WndProc;
  wc.hInstance     = hInst;
  wc.lpszClassName = kWndClass;
  if (!RegisterClassExW(&wc)) return 1;

  g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWndClass, L"截贴", WS_POPUP,
                           0, 0, 0, 0, NULL, NULL, hInst, NULL);
  if (!g_hwnd) return 1;

  if (!AddClipboardFormatListener(g_hwnd)) {
    MessageBoxW(NULL, L"注册剪贴板监听失败（需要 Windows Vista 及以上）", L"截贴", MB_ICONERROR);
    return 1;
  }

  g_hIcon = MakeTrayIcon();
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g_hwnd;
  nid.uID = TRAY_ID;
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  nid.uCallbackMessage = WM_TRAYICON;
  nid.hIcon = g_hIcon;
  {
    std::wstring t = TrayTipText();
    lstrcpynW(nid.szTip, t.c_str(), 128);
  }
  Shell_NotifyIconW(NIM_ADD, &nid);

  // 启动提示：说明当前模式怎么用
  if (g_mode == MODE_CLIP) {
    ShowBalloon(L"截贴 已启动", L"截图后，在桌面或任意文件夹按 Ctrl+V 就能直接粘贴出 PNG 文件");
  } else if (g_mode == MODE_SAVE) {
    ShowBalloon(L"截贴 已启动", (L"截图将自动保存到：" + g_folder).c_str());
  } else {
    ShowBalloon(L"截贴 已启动", (L"截图自动保存到 " + g_folder + L"，也可 Ctrl+V 粘贴到任意文件夹").c_str());
  }

  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  nid.uFlags = 0;
  Shell_NotifyIconW(NIM_DELETE, &nid);
  RemoveClipboardFormatListener(g_hwnd);
  if (g_hIcon) DestroyIcon(g_hIcon);
  Gdiplus::GdiplusShutdown(g_gdipToken);
  CoUninitialize();
  if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
  return 0;
}
