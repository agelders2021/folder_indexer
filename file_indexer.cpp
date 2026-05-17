// File Indexer — Win32/SQLite port of file_indexer.py
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include "sqlite3.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <atomic>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <thread>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;

// ── Control IDs ──────────────────────────────────────────────────────────────
enum {
    IDC_TAB = 100,
    IDC_DIR_EDIT, IDC_BROWSE_BTN, IDC_START_BTN, IDC_CANCEL_BTN, IDC_CLEAR_BTN,
    IDC_PROGRESS, IDC_LOG_LIST,
    IDC_SEARCH_EDIT, IDC_RB_FILENAME, IDC_RB_PATH, IDC_RB_EXT,
    IDC_EXT_EDIT, IDC_RESULTS_LIST, IDC_RESULT_COUNT,
    IDC_STATS_EDIT, IDC_REFRESH_BTN,
    IDM_COPY_DIR = 200, IDM_COPY_FILENAME
};

// Custom window messages
#define WM_IDX_PROGRESS (WM_USER + 1)   // wParam = file count
#define WM_IDX_DONE     (WM_USER + 2)   // wParam = total count
#define WM_IDX_LOG      (WM_USER + 3)   // lParam = new wstring*

// ── Globals ───────────────────────────────────────────────────────────────────
static HWND g_hwnd       = nullptr;
static HWND g_tabCtrl    = nullptr;
static int  g_currentTab = 0;

// Index tab controls
static HWND g_dirEdit    = nullptr;
static HWND g_browseBtn  = nullptr;
static HWND g_startBtn   = nullptr;
static HWND g_cancelBtn  = nullptr;
static HWND g_clearBtn   = nullptr;
static HWND g_progress   = nullptr;
static HWND g_logList    = nullptr;
static HWND g_logLabel   = nullptr;

// Search tab controls
static HWND g_searchEdit  = nullptr;
static HWND g_rbFilename  = nullptr;
static HWND g_rbPath      = nullptr;
static HWND g_rbExt       = nullptr;
static HWND g_extEdit     = nullptr;
static HWND g_resultsList = nullptr;
static HWND g_resultCount = nullptr;
static HWND g_searchHint  = nullptr;
static HWND g_searchLabel = nullptr;
static HWND g_extLabel    = nullptr;

// Stats tab controls
static HWND g_statsEdit   = nullptr;
static HWND g_refreshBtn  = nullptr;

// Tab control groups for show/hide
static std::vector<HWND> g_tabCtrls[3];

static std::wstring       g_dbPath;
static std::atomic<bool>  g_cancelFlag{false};
static std::atomic<bool>  g_indexing{false};

// ── String utilities ──────────────────────────────────────────────────────────
static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring Utf8ToWide(const char* u) {
    if (!u || !*u) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u, -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u, -1, w.data(), n);
    return w;
}

static std::wstring FormatSize(long long bytes) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double v = (double)bytes;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f %s", v, units[i]);
    return buf;
}

// ── Database layer ────────────────────────────────────────────────────────────
static sqlite3* OpenDb() {
    sqlite3* db = nullptr;
    std::string path = WideToUtf8(g_dbPath);
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) return nullptr;
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    return db;
}

static void InitDb() {
    sqlite3* db = OpenDb();
    if (!db) return;
    sqlite3_exec(db, R"(
        CREATE TABLE IF NOT EXISTS files (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            filename    TEXT NOT NULL,
            extension   TEXT,
            full_path   TEXT NOT NULL UNIQUE,
            directory   TEXT NOT NULL,
            size_bytes  INTEGER,
            indexed_at  TEXT NOT NULL,
            human_dir   TEXT
        );
        CREATE INDEX IF NOT EXISTS idx_filename  ON files(filename COLLATE NOCASE);
        CREATE INDEX IF NOT EXISTS idx_extension ON files(extension COLLATE NOCASE);
        CREATE INDEX IF NOT EXISTS idx_directory ON files(directory);
    )", nullptr, nullptr, nullptr);
    // Migrate: add human_dir if missing
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "PRAGMA table_info(files)", -1, &st, nullptr);
    bool hasHumanDir = false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char* col = (const char*)sqlite3_column_text(st, 1);
        if (col && std::string(col) == "human_dir") { hasHumanDir = true; break; }
    }
    sqlite3_finalize(st);
    if (!hasHumanDir)
        sqlite3_exec(db, "ALTER TABLE files ADD COLUMN human_dir TEXT", nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

static void ClearIndex() {
    sqlite3* db = OpenDb();
    if (!db) return;
    sqlite3_exec(db, "DELETE FROM files", nullptr, nullptr, nullptr);
    sqlite3_close(db);
}

struct FileRow {
    std::wstring filename, extension, fullPath, directory, humanDir;
    long long    sizeBytes = 0;
    bool         hasSizeBytes = false;
};

static std::vector<FileRow> SearchFiles(const std::wstring& query,
                                         const std::wstring& searchBy,
                                         const std::wstring& extFilter,
                                         int limit = 2000)
{
    sqlite3* db = OpenDb();
    std::vector<FileRow> results;
    if (!db) return results;

    std::string q8   = WideToUtf8(L"%" + query + L"%");
    std::string ext8 = WideToUtf8(extFilter);
    // strip leading dot from filter
    if (!ext8.empty() && ext8[0] == '.') ext8 = ext8.substr(1);

    std::string sql;
    std::vector<std::string> params;

    if (searchBy == L"filename") {
        sql = "SELECT filename,extension,full_path,directory,human_dir,size_bytes "
              "FROM files WHERE filename LIKE ? COLLATE NOCASE";
        params.push_back(q8);
    } else if (searchBy == L"path") {
        sql = "SELECT filename,extension,full_path,directory,human_dir,size_bytes "
              "FROM files WHERE full_path LIKE ? COLLATE NOCASE";
        params.push_back(q8);
    } else { // extension
        std::string eq = query.empty() ? "%" : ("%" + WideToUtf8(query) + "%");
        sql = "SELECT filename,extension,full_path,directory,human_dir,size_bytes "
              "FROM files WHERE extension LIKE ? COLLATE NOCASE";
        params.push_back(eq);
    }
    if (!ext8.empty()) {
        sql += " AND extension = ? COLLATE NOCASE";
        params.push_back("." + ext8);
    }
    sql += " ORDER BY filename COLLATE NOCASE LIMIT " + std::to_string(limit);

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        sqlite3_close(db); return results;
    }
    for (int i = 0; i < (int)params.size(); ++i)
        sqlite3_bind_text(st, i + 1, params[i].c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(st) == SQLITE_ROW) {
        FileRow r;
        auto col = [&](int i) -> std::wstring {
            const char* t = (const char*)sqlite3_column_text(st, i);
            return t ? Utf8ToWide(t) : std::wstring{};
        };
        r.filename  = col(0);
        r.extension = col(1);
        r.fullPath  = col(2);
        r.directory = col(3);
        r.humanDir  = col(4);
        if (sqlite3_column_type(st, 5) != SQLITE_NULL) {
            r.sizeBytes    = sqlite3_column_int64(st, 5);
            r.hasSizeBytes = true;
        }
        // Filter out .lnk files
        if (r.extension == L".lnk") continue;
        results.push_back(std::move(r));
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return results;
}

struct StatsResult {
    long long total = 0;
    std::wstring lastIndexed;
    std::vector<std::pair<std::wstring, long long>> topExts;
};

static StatsResult GetStats() {
    StatsResult s;
    sqlite3* db = OpenDb();
    if (!db) return s;

    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM files", -1, &st, nullptr);
    if (sqlite3_step(st) == SQLITE_ROW) s.total = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db, "SELECT MAX(indexed_at) FROM files", -1, &st, nullptr);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char* t = (const char*)sqlite3_column_text(st, 0);
        s.lastIndexed = t ? Utf8ToWide(t) : L"never";
    }
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db,
        "SELECT extension, COUNT(*) AS cnt FROM files "
        "GROUP BY extension ORDER BY cnt DESC LIMIT 10",
        -1, &st, nullptr);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char* e = (const char*)sqlite3_column_text(st, 0);
        long long   c = sqlite3_column_int64(st, 1);
        s.topExts.push_back({e ? Utf8ToWide(e) : L"(none)", c});
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return s;
}

// ── Google Drive helpers ──────────────────────────────────────────────────────
static std::wstring FindGDriveRoot(const std::wstring& rootDir) {
    fs::path p(rootDir);
    while (true) {
        if (fs::exists(p / L".shortcut-targets-by-id")) return p.wstring();
        fs::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return rootDir;
}

static std::wstring StripGDriveRoot(const std::wstring& path) {
    if (path.empty()) return path;
    std::wstring lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    for (auto& marker : {std::wstring(L"my drive\\"), std::wstring(L"shared drives\\")}) {
        auto pos = lower.find(marker);
        if (pos != std::wstring::npos)
            return path.substr(pos + marker.size());
    }
    return path;
}

// Runs a PowerShell command, forces UTF-8 output, returns decoded wide string.
static std::wstring RunPowerShell(const std::wstring& command) {
    // Prepend encoding setup so non-ASCII paths survive the pipe
    std::wstring fullCmd =
        L"powershell.exe -NoProfile -NonInteractive -Command "
        L"\"[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; " + command + L"\"";

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE hRead, hWrite;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return {};
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    // Open NUL for stderr — GetStdHandle returns NULL in a GUI-subsystem process
    HANDLE hNul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE,
                              &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.hStdOutput = hWrite;
    si.hStdError  = (hNul != INVALID_HANDLE_VALUE) ? hNul : hWrite;
    si.hStdInput  = INVALID_HANDLE_VALUE;
    si.dwFlags    = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};
    bool ok = !!CreateProcessW(nullptr, fullCmd.data(), nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
    if (!ok) { CloseHandle(hRead); return {}; }

    // Read UTF-8 bytes from the pipe
    std::string rawUtf8;
    char buf[4096];
    DWORD nRead;
    while (ReadFile(hRead, buf, sizeof(buf), &nRead, nullptr) && nRead)
        rawUtf8.append(buf, nRead);

    WaitForSingleObject(pi.hProcess, 300000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);

    return Utf8ToWide(rawUtf8.c_str());
}

// Returns {normcase(target) -> humanPrefix}, sorted longest-key-first
static std::vector<std::pair<std::wstring,std::wstring>>
ResolveShortcuts(const std::wstring& searchRoot) {
    std::wstring escaped = searchRoot;
    size_t pos = 0;
    while ((pos = escaped.find(L'\'', pos)) != std::wstring::npos) {
        escaped.replace(pos, 1, L"''");
        pos += 2;
    }
    std::wstring ps =
        L"$shell = New-Object -ComObject WScript.Shell; "
        L"Get-ChildItem -LiteralPath '" + escaped + L"' "
        L"-Recurse -Filter '*.lnk' -ErrorAction SilentlyContinue | "
        L"ForEach-Object { try { "
        L"$t = $shell.CreateShortcut($_.FullName).TargetPath; "
        L"if ($t) { $_.FullName + [char]9 + $t } } catch {} }";

    std::wstring wide = RunPowerShell(ps);

    std::unordered_map<std::wstring,std::wstring> map;
    std::wistringstream ss(wide);
    std::wstring line;
    while (std::getline(ss, line)) {
        // strip \r
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        auto tab = line.find(L'\t');
        if (tab == std::wstring::npos) continue;
        std::wstring lnk    = line.substr(0, tab);
        std::wstring target = line.substr(tab + 1);
        if (target.empty()) continue;
        fs::path lnkP(lnk);
        std::wstring humanPrefix = (lnkP.parent_path() / lnkP.stem()).wstring();
        // normcase = lowercase on Windows
        std::wstring ncTarget = target;
        std::transform(ncTarget.begin(), ncTarget.end(), ncTarget.begin(), ::towlower);
        map[ncTarget] = humanPrefix;
    }

    // Sort longest key first
    std::vector<std::pair<std::wstring,std::wstring>> sorted(map.begin(), map.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b){ return a.first.size() > b.first.size(); });
    return sorted;
}

static std::wstring LookupShortcut(
    const std::wstring& fullPath,
    const std::vector<std::pair<std::wstring,std::wstring>>& sortedMap)
{
    std::wstring ncFull = fullPath;
    std::transform(ncFull.begin(), ncFull.end(), ncFull.begin(), ::towlower);

    for (const auto& [ncTarget, humanPfx] : sortedMap) {
        if (ncFull.size() > ncTarget.size() &&
            ncFull.substr(0, ncTarget.size()) == ncTarget &&
            ncFull[ncTarget.size()] == L'\\')
        {
            std::wstring suffix = fullPath.substr(ncTarget.size()); // includes leading backslash
            fs::path humanPath = fs::path(humanPfx + suffix);
            return humanPath.parent_path().wstring();
        }
        if (ncFull == ncTarget) {
            return fs::path(humanPfx).parent_path().wstring();
        }
    }
    return {};
}

// ── Indexing thread ───────────────────────────────────────────────────────────
struct IndexParams { std::wstring rootDir; HWND hwnd; };

static void PostLog(HWND hwnd, const std::wstring& msg) {
    PostMessage(hwnd, WM_IDX_LOG, 0, (LPARAM)new std::wstring(msg));
}

static DWORD WINAPI IndexThread(LPVOID pv) {
    auto* p = static_cast<IndexParams*>(pv);
    HWND hwnd = p->hwnd;
    std::wstring rootDir = p->rootDir;
    delete p;

    PostLog(hwnd, L"Resolving Google Drive shortcuts...");
    std::wstring gdRoot = FindGDriveRoot(rootDir);
    auto shortcutMap    = ResolveShortcuts(gdRoot);
    {
        wchar_t buf[128];
        swprintf_s(buf, L"Resolved %d shortcut target(s).", (int)shortcutMap.size());
        PostLog(hwnd, buf);
    }

    sqlite3* db = OpenDb();
    if (!db) {
        PostLog(hwnd, L"ERROR: Could not open database.");
        PostMessage(hwnd, WM_IDX_DONE, 0, 0);
        g_indexing = false;
        return 0;
    }

    const char* upsertSql = R"(
        INSERT INTO files (filename,extension,full_path,directory,size_bytes,indexed_at,human_dir)
        VALUES (?,?,?,?,?,?,?)
        ON CONFLICT(full_path) DO UPDATE SET
            filename=excluded.filename, extension=excluded.extension,
            directory=excluded.directory, size_bytes=excluded.size_bytes,
            indexed_at=excluded.indexed_at, human_dir=excluded.human_dir
    )";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, upsertSql, -1, &stmt, nullptr);

    // Get current timestamp
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char timeBuf[32];
    snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02dT%02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    // .lnk files are Windows shortcuts — the iterator sees them as regular files and
    // never enters the folders they point to. So we also walk .shortcut-targets-by-id
    // directly when it exists outside the user's chosen root.
    std::vector<std::wstring> walkRoots = {rootDir};
    {
        std::wstring stDir = gdRoot + L"\\.shortcut-targets-by-id";
        std::error_code ec2;
        if (fs::is_directory(stDir, ec2) && !ec2) {
            std::wstring ncRoot = rootDir, ncST = stDir;
            std::transform(ncRoot.begin(), ncRoot.end(), ncRoot.begin(), ::towlower);
            std::transform(ncST.begin(),   ncST.end(),   ncST.begin(),   ::towlower);
            // Only add if it isn't already inside the chosen root
            if (ncST.rfind(ncRoot, 0) != 0) {
                walkRoots.push_back(stDir);
                PostLog(hwnd, L"Also walking .shortcut-targets-by-id for shortcut contents...");
            }
        }
    }

    long long total = 0, batchCount = 0;
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);

    auto bind = [&](int i, const std::wstring& w) {
        if (w.empty()) sqlite3_bind_null(stmt, i);
        else sqlite3_bind_text(stmt, i, WideToUtf8(w).c_str(), -1, SQLITE_TRANSIENT);
    };

    for (const auto& walkRoot : walkRoots) {
        if (g_cancelFlag) break;
        std::error_code ec;
        fs::recursive_directory_iterator it(walkRoot,
            fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;

        for (; it != end && !g_cancelFlag; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const auto& entry = *it;
            if (!entry.is_regular_file(ec)) { ec.clear(); continue; }

            fs::path path = entry.path();
            std::wstring fullPath  = path.wstring();
            std::wstring filename  = path.filename().wstring();
            std::wstring ext       = path.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            std::wstring directory = path.parent_path().wstring();

            long long size = -1;
            std::error_code sec;
            auto sz = fs::file_size(path, sec);
            if (!sec) size = (long long)sz;

            std::wstring humanDir = LookupShortcut(fullPath, shortcutMap);

            sqlite3_reset(stmt);
            bind(1, filename);
            if (ext.empty()) sqlite3_bind_null(stmt, 2);
            else sqlite3_bind_text(stmt, 2, WideToUtf8(ext).c_str(), -1, SQLITE_TRANSIENT);
            bind(3, fullPath);
            bind(4, directory);
            if (size < 0) sqlite3_bind_null(stmt, 5);
            else          sqlite3_bind_int64(stmt, 5, size);
            sqlite3_bind_text(stmt, 6, timeBuf, -1, SQLITE_STATIC);
            if (humanDir.empty()) sqlite3_bind_null(stmt, 7);
            else sqlite3_bind_text(stmt, 7, WideToUtf8(humanDir).c_str(), -1, SQLITE_TRANSIENT);

            sqlite3_step(stmt);
            ++total; ++batchCount;

            if (batchCount >= 500) {
                sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
                sqlite3_exec(db, "BEGIN",  nullptr, nullptr, nullptr);
                batchCount = 0;
            }
            if (total % 100 == 0)
                PostMessage(hwnd, WM_IDX_PROGRESS, (WPARAM)total, 0);
        }
    }

    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    PostMessage(hwnd, WM_IDX_DONE, (WPARAM)total, 0);
    g_indexing = false;
    return 0;
}

// ── UI helpers ────────────────────────────────────────────────────────────────
static void ShowTab(int idx) {
    for (int t = 0; t < 3; ++t)
        for (HWND h : g_tabCtrls[t])
            ShowWindow(h, t == idx ? SW_SHOW : SW_HIDE);
    g_currentTab = idx;
}

static void CopyToClipboard(const std::wstring& text) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        memcpy(GlobalLock(hMem), text.c_str(), bytes);
        GlobalUnlock(hMem);
        SetClipboardData(CF_UNICODETEXT, hMem);
    }
    CloseClipboard();
}

static void RefreshStats() {
    StatsResult s = GetStats();
    std::wostringstream oss;
    oss << L"  Database location  : " << g_dbPath << L"\r\n"
        << L"  Total files indexed: " << s.total << L"\r\n"
        << L"  Last indexed at    : " << (s.lastIndexed.empty() ? L"never" : s.lastIndexed) << L"\r\n"
        << L"\r\n"
        << L"  Top 10 extensions by file count:\r\n"
        << L"  " << std::wstring(38, L'-') << L"\r\n";
    for (auto& [ext, cnt] : s.topExts) {
        int barLen = s.total > 0 ? (int)std::min((long long)30, cnt * 200 / s.total) : 0;
        std::wstring bar(barLen, L'|');
        wchar_t line[128];
        swprintf_s(line, L"  %-12s  %8lld   %s\r\n", ext.c_str(), cnt, bar.c_str());
        oss << line;
    }
    SetWindowTextW(g_statsEdit, oss.str().c_str());
}

static void RunSearch() {
    wchar_t qbuf[512] = {}, ebuf[64] = {};
    GetWindowTextW(g_searchEdit, qbuf, 512);
    GetWindowTextW(g_extEdit,    ebuf, 64);

    std::wstring query(qbuf), extFilter(ebuf);

    std::wstring searchBy = L"filename";
    if (SendMessage(g_rbPath, BM_GETCHECK, 0, 0) == BST_CHECKED)      searchBy = L"path";
    else if (SendMessage(g_rbExt, BM_GETCHECK, 0, 0) == BST_CHECKED)  searchBy = L"extension";

    // Clear list
    ListView_DeleteAllItems(g_resultsList);

    if (query.empty() && extFilter.empty()) {
        SetWindowTextW(g_resultCount, L"");
        return;
    }

    auto rows = SearchFiles(query, searchBy, extFilter);

    int idx = 0;
    for (const auto& r : rows) {
        bool isShortcut = r.fullPath.find(L".shortcut-targets-by-id") != std::wstring::npos;
        std::wstring displayDir;
        if (!r.humanDir.empty())
            displayDir = StripGDriveRoot(r.humanDir);
        else if (!isShortcut)
            displayDir = StripGDriveRoot(r.directory);

        LVITEMW lvi{};
        lvi.mask    = LVIF_TEXT;
        lvi.iItem   = idx;
        lvi.pszText = const_cast<wchar_t*>(r.filename.c_str());
        ListView_InsertItem(g_resultsList, &lvi);
        ListView_SetItemText(g_resultsList, idx, 1, const_cast<wchar_t*>(displayDir.c_str()));

        std::wstring sizeStr = r.hasSizeBytes ? FormatSize(r.sizeBytes) : L"";
        ListView_SetItemText(g_resultsList, idx, 2, const_cast<wchar_t*>(sizeStr.c_str()));
        ++idx;
    }

    wchar_t cbuf[64];
    swprintf_s(cbuf, L"%d result(s)", (int)rows.size());
    SetWindowTextW(g_resultCount, cbuf);
}

// ── WndProc ───────────────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE: {
        RECT rc; GetClientRect(hwnd, &rc);
        int W = rc.right, H = rc.bottom;

        // Tab control
        g_tabCtrl = CreateWindowExW(0, WC_TABCONTROLW, nullptr,
            WS_CHILD|WS_VISIBLE|TCS_HOTTRACK,
            0, 0, W, H, hwnd, (HMENU)IDC_TAB, GetModuleHandleW(nullptr), nullptr);
        SendMessage(g_tabCtrl, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

        TCITEMW ti{TCIF_TEXT};
        ti.pszText = const_cast<wchar_t*>(L"  Index  ");  TabCtrl_InsertItem(g_tabCtrl, 0, &ti);
        ti.pszText = const_cast<wchar_t*>(L"  Search  "); TabCtrl_InsertItem(g_tabCtrl, 1, &ti);
        ti.pszText = const_cast<wchar_t*>(L"  Stats  ");  TabCtrl_InsertItem(g_tabCtrl, 2, &ti);

        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        int y0 = 34; // below tab headers

        auto Make = [&](const wchar_t* cls, const wchar_t* txt, DWORD style,
                        int x, int y, int w, int h, int id) -> HWND {
            HWND hw = CreateWindowExW(0, cls, txt,
                WS_CHILD | style, x, y, w, h,
                hwnd, (HMENU)(INT_PTR)id,
                GetModuleHandleW(nullptr), nullptr);
            SendMessage(hw, WM_SETFONT, (WPARAM)hFont, TRUE);
            return hw;
        };

        // ── Index tab controls ─────────────────────────────────────────────
        HWND dirLbl  = Make(L"STATIC",  L"Directory:", WS_GROUP, 10, y0+4, 80, 20, 0);
        g_dirEdit    = Make(L"EDIT", L"", WS_BORDER|ES_AUTOHSCROLL, 94, y0, W-224, 24, IDC_DIR_EDIT);
        g_browseBtn  = Make(L"BUTTON", L"Browse...", BS_PUSHBUTTON, W-124, y0, 76, 24, IDC_BROWSE_BTN);
        g_startBtn   = Make(L"BUTTON", L"Start",  BS_PUSHBUTTON, 10, y0+32, 70, 26, IDC_START_BTN);
        g_cancelBtn  = Make(L"BUTTON", L"Cancel", BS_PUSHBUTTON, 86, y0+32, 70, 26, IDC_CANCEL_BTN);
        g_clearBtn   = Make(L"BUTTON", L"Clear DB",BS_PUSHBUTTON, 162, y0+32, 70, 26, IDC_CLEAR_BTN);
        g_progress   = Make(PROGRESS_CLASSW, nullptr,
                            PBS_MARQUEE|PBS_SMOOTH, 10, y0+67, W-20, 18, IDC_PROGRESS);
        g_logLabel   = Make(L"STATIC", L"Index log:", 0, 10, y0+93, 80, 18, 0);
        g_logList    = Make(L"LISTBOX", nullptr,
                            LBS_NOSEL|LBS_NOINTEGRALHEIGHT|WS_VSCROLL|WS_HSCROLL|WS_BORDER,
                            10, y0+113, W-20, H-y0-123, IDC_LOG_LIST);

        g_tabCtrls[0] = {dirLbl, g_dirEdit, g_browseBtn, g_startBtn, g_cancelBtn,
                         g_clearBtn, g_progress, g_logLabel, g_logList};

        // ── Search tab controls ────────────────────────────────────────────
        g_searchLabel = Make(L"STATIC", L"Search:", 0, 10, y0+4, 52, 20, 0);
        g_searchEdit  = Make(L"EDIT", L"", WS_BORDER|ES_AUTOHSCROLL, 66, y0, 260, 24, IDC_SEARCH_EDIT);
        g_rbFilename  = Make(L"BUTTON", L"Filename",  BS_AUTORADIOBUTTON|WS_GROUP, 10,  y0+34, 80, 20, IDC_RB_FILENAME);
        g_rbPath      = Make(L"BUTTON", L"Full Path", BS_AUTORADIOBUTTON, 96,  y0+34, 80, 20, IDC_RB_PATH);
        g_rbExt       = Make(L"BUTTON", L"Extension", BS_AUTORADIOBUTTON, 182, y0+34, 80, 20, IDC_RB_EXT);
        g_extLabel    = Make(L"STATIC", L"Ext filter:", 0, 270, y0+36, 66, 18, 0);
        g_extEdit     = Make(L"EDIT", L"", WS_BORDER|ES_AUTOHSCROLL, 340, y0+33, 80, 22, IDC_EXT_EDIT);
        SendMessage(g_rbFilename, BM_SETCHECK, BST_CHECKED, 0);

        g_resultsList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
            WS_CHILD|LVS_REPORT|LVS_SHOWSELALWAYS|LVS_SINGLESEL,
            10, y0+62, W-20, H-y0-100, hwnd, (HMENU)IDC_RESULTS_LIST,
            GetModuleHandleW(nullptr), nullptr);
        SendMessage(g_resultsList, WM_SETFONT, (WPARAM)hFont, TRUE);
        ListView_SetExtendedListViewStyle(g_resultsList,
            LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES);

        LVCOLUMNW lvc{LVCF_TEXT|LVCF_WIDTH};
        lvc.cx = 260; lvc.pszText = const_cast<wchar_t*>(L"Filename");  ListView_InsertColumn(g_resultsList, 0, &lvc);
        lvc.cx = 700; lvc.pszText = const_cast<wchar_t*>(L"Directory"); ListView_InsertColumn(g_resultsList, 1, &lvc);
        lvc.cx = 80;  lvc.pszText = const_cast<wchar_t*>(L"Size");      ListView_InsertColumn(g_resultsList, 2, &lvc);

        g_resultCount = Make(L"STATIC", L"", 0, 10, H-34, 200, 18, IDC_RESULT_COUNT);
        g_searchHint  = Make(L"STATIC", L"Right-click a result to copy directory or filename",
                             0, 220, H-34, 400, 18, 0);

        g_tabCtrls[1] = {g_searchLabel, g_searchEdit, g_rbFilename, g_rbPath, g_rbExt,
                         g_extLabel, g_extEdit, g_resultsList, g_resultCount, g_searchHint};

        // ── Stats tab controls ─────────────────────────────────────────────
        g_refreshBtn = Make(L"BUTTON", L"Refresh Stats", BS_PUSHBUTTON, 10, y0+4, 110, 26, IDC_REFRESH_BTN);
        g_statsEdit  = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD|ES_MULTILINE|ES_READONLY|WS_VSCROLL|ES_AUTOVSCROLL,
            10, y0+38, W-20, H-y0-50, hwnd, (HMENU)IDC_STATS_EDIT,
            GetModuleHandleW(nullptr), nullptr);
        // Consolas font for stats
        HFONT hMono = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  DEFAULT_QUALITY, FIXED_PITCH|FF_MODERN, L"Consolas");
        SendMessage(g_statsEdit, WM_SETFONT, (WPARAM)hMono, TRUE);

        g_tabCtrls[2] = {g_refreshBtn, g_statsEdit};

        ShowTab(0);
        return 0;
    }

    case WM_SIZE: {
        int W = LOWORD(lParam), H = HIWORD(lParam);
        if (!g_tabCtrl) break;
        SetWindowPos(g_tabCtrl, nullptr, 0, 0, W, H, SWP_NOZORDER|SWP_NOMOVE);
        int y0 = 34;
        // Index tab
        SetWindowPos(g_dirEdit,    nullptr, 94, y0,     W-224, 24, SWP_NOZORDER);
        SetWindowPos(g_browseBtn,  nullptr, W-124, y0,  76,    24, SWP_NOZORDER);
        SetWindowPos(g_progress,   nullptr, 10, y0+67,  W-20,  18, SWP_NOZORDER);
        SetWindowPos(g_logList,    nullptr, 10, y0+113, W-20, H-y0-123, SWP_NOZORDER);
        // Search tab
        SetWindowPos(g_resultsList, nullptr, 10, y0+62, W-20, H-y0-100, SWP_NOZORDER);
        SetWindowPos(g_resultCount, nullptr, 10, H-34, 200, 18, SWP_NOZORDER);
        SetWindowPos(g_searchHint,  nullptr, 220, H-34, W-230, 18, SWP_NOZORDER);
        // Stats tab
        SetWindowPos(g_statsEdit,  nullptr, 10, y0+38, W-20, H-y0-50, SWP_NOZORDER);
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam), note = HIWORD(wParam);

        if (id == IDC_BROWSE_BTN) {
            IFileOpenDialog* pDlg = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                           CLSCTX_ALL, IID_PPV_ARGS(&pDlg)))) {
                DWORD opts = 0;
                pDlg->GetOptions(&opts);
                pDlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                pDlg->SetTitle(L"Select directory to index");
                // Pre-select current dir edit contents if valid
                wchar_t cur[MAX_PATH] = {};
                GetWindowTextW(g_dirEdit, cur, MAX_PATH);
                if (fs::is_directory(cur)) {
                    IShellItem* pCur = nullptr;
                    if (SUCCEEDED(SHCreateItemFromParsingName(cur, nullptr,
                                                              IID_PPV_ARGS(&pCur)))) {
                        pDlg->SetFolder(pCur);
                        pCur->Release();
                    }
                }
                if (SUCCEEDED(pDlg->Show(hwnd))) {
                    IShellItem* pItem = nullptr;
                    if (SUCCEEDED(pDlg->GetResult(&pItem))) {
                        PWSTR pszPath = nullptr;
                        if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                            SetWindowTextW(g_dirEdit, pszPath);
                            CoTaskMemFree(pszPath);
                        }
                        pItem->Release();
                    }
                }
                pDlg->Release();
            }
        }
        else if (id == IDC_START_BTN) {
            if (g_indexing) break;
            wchar_t dir[MAX_PATH] = {};
            GetWindowTextW(g_dirEdit, dir, MAX_PATH);
            if (!fs::is_directory(dir)) {
                MessageBoxW(hwnd, L"Please select a valid directory first.",
                            L"Error", MB_ICONERROR);
                break;
            }
            g_cancelFlag = false;
            g_indexing   = true;
            ListBox_ResetContent(g_logList);
            SendMessage(g_progress, PBM_SETMARQUEE, TRUE, 50);
            EnableWindow(g_startBtn, FALSE);

            auto* params     = new IndexParams{dir, hwnd};
            HANDLE hThread   = CreateThread(nullptr, 0, IndexThread, params, 0, nullptr);
            if (hThread) CloseHandle(hThread);
        }
        else if (id == IDC_CANCEL_BTN) {
            g_cancelFlag = true;
        }
        else if (id == IDC_CLEAR_BTN) {
            if (MessageBoxW(hwnd, L"Clear the entire file index?", L"Confirm",
                            MB_YESNO|MB_ICONQUESTION) == IDYES) {
                ClearIndex();
                ListBox_AddString(g_logList, L"Database cleared.");
            }
        }
        else if (id == IDC_REFRESH_BTN) {
            RefreshStats();
        }
        else if ((id == IDC_SEARCH_EDIT || id == IDC_EXT_EDIT) && note == EN_CHANGE) {
            RunSearch();
        }
        else if (id == IDC_RB_FILENAME || id == IDC_RB_PATH || id == IDC_RB_EXT) {
            RunSearch();
        }
        else if (id == IDM_COPY_DIR || id == IDM_COPY_FILENAME) {
            int sel = ListView_GetNextItem(g_resultsList, -1, LVNI_SELECTED);
            if (sel >= 0) {
                wchar_t buf[1024] = {};
                int col = (id == IDM_COPY_DIR) ? 1 : 0;
                ListView_GetItemText(g_resultsList, sel, col, buf, 1024);
                CopyToClipboard(buf);
            }
        }
        break;
    }

    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lParam);
        if (hdr->idFrom == IDC_TAB && hdr->code == TCN_SELCHANGE) {
            int tab = TabCtrl_GetCurSel(g_tabCtrl);
            ShowTab(tab);
            if (tab == 2) RefreshStats();
        }
        else if (hdr->idFrom == IDC_RESULTS_LIST && hdr->code == NM_RCLICK) {
            auto* nml = reinterpret_cast<NMITEMACTIVATE*>(lParam);
            if (nml->iItem >= 0) {
                ListView_SetItemState(g_resultsList, nml->iItem,
                                      LVIS_SELECTED|LVIS_FOCUSED,
                                      LVIS_SELECTED|LVIS_FOCUSED);
                HMENU hMenu = CreatePopupMenu();
                AppendMenuW(hMenu, MF_STRING, IDM_COPY_DIR,      L"Copy directory");
                AppendMenuW(hMenu, MF_STRING, IDM_COPY_FILENAME, L"Copy filename");
                POINT pt; GetCursorPos(&pt);
                TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(hMenu);
            }
        }
        break;
    }

    case WM_IDX_PROGRESS: {
        wchar_t title[64];
        swprintf_s(title, L"File Indexer — %llu files...", (unsigned long long)wParam);
        SetWindowTextW(hwnd, title);
        break;
    }

    case WM_IDX_DONE: {
        SendMessage(g_progress, PBM_SETMARQUEE, FALSE, 0);
        EnableWindow(g_startBtn, TRUE);
        wchar_t buf[64];
        swprintf_s(buf, L"%llu files indexed.", (unsigned long long)wParam);
        ListBox_AddString(g_logList, buf);
        SendMessage(g_logList, LB_SETTOPINDEX,
                    SendMessage(g_logList, LB_GETCOUNT, 0, 0) - 1, 0);
        SetWindowTextW(hwnd, L"File Indexer");
        break;
    }

    case WM_IDX_LOG: {
        auto* msg2 = reinterpret_cast<std::wstring*>(lParam);
        ListBox_AddString(g_logList, msg2->c_str());
        SendMessage(g_logList, LB_SETTOPINDEX,
                    SendMessage(g_logList, LB_GETCOUNT, 0, 0) - 1, 0);
        delete msg2;
        break;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ── WinMain ───────────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    // DB path: %USERPROFILE%\file_index.db
    wchar_t profile[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, profile);
    g_dbPath = std::wstring(profile) + L"\\file_index.db";

    // Init common controls (ListView, Tab, Progress)
    INITCOMMONCONTROLSEX icc{sizeof(icc),
        ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);

    CoInitialize(nullptr);
    InitDb();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wc.lpszClassName = L"FileIndexerWnd";
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, L"FileIndexerWnd", L"File Indexer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 680,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(g_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    CoUninitialize();
    return (int)msg.wParam;
}
