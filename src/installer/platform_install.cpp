#include "installer/platform_install.h"

#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <iterator>

#include "common/utf8_path.h"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace strojanka::installer::os {

#if defined(_WIN32)

namespace {

const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Strojanka";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"Strojanka";
const wchar_t* kServerTitle = L"Strojanka Server";  // set by src/server/main.cpp

std::wstring wide(const std::string& s) { return utf8_path(s).wstring(); }

struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

std::wstring known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) out = raw;
    CoTaskMemFree(raw);
    return out;
}

void make_link(const std::wstring& lnk, const std::wstring& target, const std::wstring& workdir, const std::wstring& icon,
               const std::wstring& description) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&link)))) return;
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(workdir.c_str());
    link->SetIconLocation(icon.c_str(), 0);
    link->SetDescription(description.c_str());
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
        file->Save(lnk.c_str(), TRUE);
        file->Release();
    }
    link->Release();
}

std::string pick(void* owner, const std::string& initial, bool folder, bool backup = false) {
    ComScope com;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (!folder) {
        const COMDLG_FILTERSPEC db_filters[] = {{L"Strojanka database (*.db)", L"*.db"}, {L"All files", L"*.*"}};
        const COMDLG_FILTERSPEC backup_filters[] = {{L"Strojanka backup (*.strojanka_backup)", L"*.strojanka_backup"}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, backup ? backup_filters : db_filters);
    }
    fs::path start = utf8_path(initial);
    std::error_code ec;
    while (!start.empty() && !fs::is_directory(start, ec)) {
        if (start == start.parent_path()) break;
        start = start.parent_path();
    }
    IShellItem* item = nullptr;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.wstring().c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        dlg->SetFolder(item);
        item->Release();
    }
    std::string result;
    if (SUCCEEDED(dlg->Show(static_cast<HWND>(owner)))) {
        IShellItem* chosen = nullptr;
        if (SUCCEEDED(dlg->GetResult(&chosen))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(chosen->GetDisplayName(SIGDN_FILESYSPATH, &path))) result = path_utf8(fs::path(path));
            CoTaskMemFree(path);
            chosen->Release();
        }
    }
    dlg->Release();
    return result;
}

void set_value(HKEY key, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void set_dword(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof value);
}

}  // namespace

std::string env(const char* name) {
    wchar_t buf[32768];
    const std::wstring wname(name, name + std::char_traits<char>::length(name));
    const DWORD len = GetEnvironmentVariableW(wname.c_str(), buf, 32768);
    return len ? path_utf8(fs::path(std::wstring(buf, len))) : std::string();
}

bool file_in_use(const std::string& path) {
    const std::wstring p = wide(path);
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(h);
    return false;
}

int run_capture(const std::string& exe, const std::vector<std::string>& args, std::string& output) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return -1;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = L"\"" + wide(exe) + L"\"";
    for (const auto& a : args) cmd += L" \"" + wide(a) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(write_end);
    if (!started) {
        CloseHandle(read_end);
        output = "could not start " + exe;
        return -1;
    }
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(read_end, buf, sizeof buf, &got, nullptr) && got > 0) output.append(buf, got);
    CloseHandle(read_end);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

bool open_path(const std::string& path) {
    const fs::path p = utf8_path(path);
    const std::wstring dir = p.parent_path().wstring();
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL)) > 32;
}

std::string pick_folder(void* owner, const std::string& initial) { return pick(owner, initial, true); }
std::string pick_file(void* owner, const std::string& initial, bool backup) { return pick(owner, initial, false, backup); }

void create_shortcuts(const std::string& install_dir, bool start_menu, bool desktop) {
    ComScope com;
    const fs::path dir = utf8_path(install_dir);
    const std::wstring target = (dir / "StrojankaServer.exe").wstring();
    const std::wstring icon = target;
    const std::wstring description = L"Strojanka Server: the stock alert bot with its dashboard and console";
    if (start_menu) make_link(known_folder(FOLDERID_Programs) + L"\\Strojanka.lnk", target, dir.wstring(), icon, description);
    if (desktop) make_link(known_folder(FOLDERID_Desktop) + L"\\Strojanka.lnk", target, dir.wstring(), icon, description);
}

void remove_shortcuts() {
    ComScope com;
    DeleteFileW((known_folder(FOLDERID_Programs) + L"\\Strojanka.lnk").c_str());
    DeleteFileW((known_folder(FOLDERID_Desktop) + L"\\Strojanka.lnk").c_str());
}

void register_app(const std::string& install_dir, const std::string& version, unsigned long size_kb) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    const fs::path dir = utf8_path(install_dir);
    const std::wstring setup = L"\"" + (dir / "StrojankaSetup.exe").wstring() + L"\"";
    set_value(key, L"DisplayName", L"Strojanka");
    set_value(key, L"DisplayVersion", wide(version));
    set_value(key, L"Publisher", L"Strojanka");
    set_value(key, L"DisplayIcon", (dir / "strojanka.exe").wstring());
    set_value(key, L"InstallLocation", dir.wstring());
    set_value(key, L"UninstallString", setup + L" --uninstall");
    set_value(key, L"ModifyPath", setup);
    set_dword(key, L"EstimatedSize", size_kb);
    set_dword(key, L"NoRepair", 1);
    RegCloseKey(key);
}

void unregister_app() { RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey); }

Integration current_integration() {
    ComScope com;
    Integration i;
    std::error_code ec;
    i.start_menu = fs::exists(fs::path(known_folder(FOLDERID_Programs) + L"\\Strojanka.lnk"), ec);
    i.desktop = fs::exists(fs::path(known_folder(FOLDERID_Desktop) + L"\\Strojanka.lnk"), ec);
    i.autostart = RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    return i;
}

void set_autostart(const std::string& install_dir, bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    if (enabled) {
        const fs::path exe = utf8_path(install_dir) / "StrojankaServer.exe";
        set_value(key, kRunValue, L"\"" + exe.wstring() + L"\" --autostart");
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

namespace {

struct ServerWindows {
    std::wstring exe;  // lower-case full path of the StrojankaServer.exe to match
    std::vector<HWND> found;
};

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

BOOL CALLBACK collect_server_window(HWND hwnd, LPARAM param) {
    auto* sw = reinterpret_cast<ServerWindows*>(param);
    wchar_t title[64] = {};
    GetWindowTextW(hwnd, title, 64);
    if (std::wstring(title) != kServerTitle) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return TRUE;
    wchar_t path[MAX_PATH * 4] = {};
    DWORD size = static_cast<DWORD>(std::size(path));
    if (QueryFullProcessImageNameW(process, 0, path, &size) && lower(path) == sw->exe) sw->found.push_back(hwnd);
    CloseHandle(process);
    return TRUE;
}

}  // namespace

bool close_running_server(const std::string& install_dir, int timeout_seconds) {
    const fs::path exe = utf8_path(install_dir) / "StrojankaServer.exe";
    if (!file_in_use(path_utf8(exe))) return true;
    ServerWindows sw;
    sw.exe = lower(fs::absolute(exe).wstring());
    EnumWindows(collect_server_window, reinterpret_cast<LPARAM>(&sw));
    if (sw.found.empty()) return false;  // running, but not as a window we can ask
    for (HWND h : sw.found) PostMessageW(h, WM_CLOSE, 0, 0);
    for (int waited = 0; waited < timeout_seconds * 4; ++waited) {
        if (!file_in_use(path_utf8(exe))) return true;
        Sleep(250);
    }
    return !file_in_use(path_utf8(exe));
}

void delete_after_exit(const std::string& path) {
    const fs::path p = utf8_path(path);
    std::wstring cmd = L"cmd.exe /c ping 127.0.0.1 -n 4 >nul & del /f /q \"" + p.wstring() + L"\" & rmdir \"" +
                       p.parent_path().wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

#else  // macOS and other POSIX systems

namespace {

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

std::string read_command(const std::string& cmd) {
    std::string out;
    if (FILE* f = popen(cmd.c_str(), "r")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        pclose(f);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

std::string apple_pick(const char* what, const std::string& initial) {
    std::string script = std::string("POSIX path of (choose ") + what;
    std::error_code ec;
    if (!initial.empty() && fs::is_directory(utf8_path(initial), ec)) {
        script += " default location (POSIX file \"" + initial + "\")";
    }
    script += ")";
    return read_command("osascript -e " + shell_quote(script) + " 2>/dev/null");
}

}  // namespace

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

bool file_in_use(const std::string&) { return false; }

int run_capture(const std::string& exe, const std::vector<std::string>& args, std::string& output) {
    std::string cmd = shell_quote(exe);
    for (const auto& a : args) cmd += " " + shell_quote(a);
    cmd += " 2>&1";
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return -1;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) output.append(buf, n);
    const int status = pclose(f);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

bool open_path(const std::string& path) { return std::system(("open " + shell_quote(path)).c_str()) == 0; }

std::string pick_folder(void*, const std::string& initial) { return apple_pick("folder", initial); }
std::string pick_file(void*, const std::string& initial, bool) {
    return apple_pick("file", fs::path(initial).parent_path().string());
}

void create_shortcuts(const std::string&, bool, bool) {}
void remove_shortcuts() {}
void register_app(const std::string&, const std::string&, unsigned long) {}
void unregister_app() {}
Integration current_integration() { return {}; }
void set_autostart(const std::string&, bool) {}
bool close_running_server(const std::string&, int) { return false; }
void delete_after_exit(const std::string&) {}

#endif

}  // namespace strojanka::installer::os
