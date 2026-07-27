#include "ui/FileDialog.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

#include "sage/core/Log.h"
#include "ui/Localization.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace d3d::filedialog {

// Экранирование нужно ОБЕИМ веткам: на Windows им пользуется RevealInFileManager,
// на остальных системах — все диалоги. Поэтому оно вне #ifdef.
std::string QuoteForShell(const std::string& value) {
#ifdef _WIN32
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
#else
    // Одинарные кавычки отключают ВСЮ подстановку; единственный спецслучай —
    // сама одинарная кавычка.
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
#endif
}

namespace {
// Короткое имя внутри файла — экранирование зовётся здесь на каждой строке.
inline std::string Quote(const std::string& value) { return QuoteForShell(value); }
} // namespace

// ============================================================================
//  Windows: родной диалог, без отдельного процесса
// ============================================================================
#ifdef _WIN32

namespace {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

std::string Narrow(const wchar_t* s) {
    if (!s || !*s) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out((size_t)(n - 1), '\0'); // n включает завершающий ноль
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// Фильтр для comdlg32 — список пар, разделённых нулями и закрытый двойным
// нулём: "Имя\0*.a;*.b\0Имя2\0*.c\0\0". Шаблоны у нас разделены пробелами,
// а windows ждёт точку с запятой.
std::wstring BuildFilter(const std::vector<Filter>& filters) {
    std::wstring out;
    for (const Filter& f : filters) {
        std::string patterns = f.Patterns;
        for (char& c : patterns) {
            if (c == ' ') c = ';';
        }
        out += Widen(f.Name.empty() ? patterns : f.Name + " (" + patterns + ")");
        out.push_back(L'\0');
        out += Widen(patterns);
        out.push_back(L'\0');
    }
    out += Widen(T("Все файлы"));
    out.push_back(L'\0');
    out += L"*.*";
    out.push_back(L'\0');
    out.push_back(L'\0'); // закрывающий двойной ноль
    return out;
}

// Расширение по умолчанию из первого шаблона первого фильтра: "*.d3dproj" даёт
// "d3dproj". Без него сохранение без набранного расширения даёт файл без него.
std::wstring DefaultExtension(const std::vector<Filter>& filters) {
    for (const Filter& f : filters) {
        const size_t dot = f.Patterns.find('.');
        if (dot == std::string::npos) continue;
        std::string ext;
        for (size_t i = dot + 1; i < f.Patterns.size(); ++i) {
            const char c = f.Patterns[i];
            if (c == ' ' || c == ';' || c == '*') break;
            ext.push_back(c);
        }
        if (!ext.empty() && ext != "*") return Widen(ext);
    }
    return {};
}

// COM нужен диалогу выбора каталога. GLFW инициализирует COM на своём потоке,
// поэтому «уже инициализировано» — нормальный ответ, а не ошибка; и в этом
// случае освобождать его нельзя, он не наш.
class ComScope {
public:
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        m_owned = SUCCEEDED(hr);
    }
    ~ComScope() {
        if (m_owned) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool m_owned = false;
};

// GUID'ы объявлены здесь, а не взяты из libuuid: так набор библиотек для сборки
// не зависит от версии mingw, в которой эти символы то есть, то лежат в другом
// архиве.
const CLSID kClsidFileOpenDialog = {
    0xDC1C5A9C, 0xE88A, 0x4DDE, {0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7}};
const IID kIidIFileOpenDialog = {
    0xD57C7288, 0xD4AD, 0x4768, {0xBE, 0x02, 0x9D, 0x96, 0x95, 0x32, 0xD9, 0x60}};
const IID kIidIShellItem = {
    0x43826D1E, 0xE718, 0x42EE, {0xBC, 0x55, 0xA1, 0xE2, 0x61, 0xC3, 0x7B, 0xFE}};

// Окно-владелец: без него модальный диалог не блокирует главное окно, и по нему
// можно кликать, пока диалог открыт.
HWND OwnerWindow() { return GetActiveWindow(); }

// Старый диалог каталога — запасной путь, если IFileDialog не создался
// (Windows до Vista или урезанная сборка системы).
bool PickFolderLegacy(const std::string& title, const std::string& startDir, std::string& out) {
    const std::wstring wtitle = Widen(title);
    const std::wstring wstart = Widen(startDir);

    BROWSEINFOW bi{};
    bi.hwndOwner = OwnerWindow();
    bi.lpszTitle = wtitle.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    // Стартовый каталог задаётся колбэком: у BROWSEINFO нет поля под него.
    bi.lParam = (LPARAM)(wstart.empty() ? nullptr : wstart.c_str());
    bi.lpfn = [](HWND hwnd, UINT msg, LPARAM, LPARAM data) -> int {
        if (msg == BFFM_INITIALIZED && data) {
            SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
        }
        return 0;
    };

    LPITEMIDLIST idl = SHBrowseForFolderW(&bi);
    if (!idl) return false;

    wchar_t path[MAX_PATH] = {0};
    const bool ok = SHGetPathFromIDListW(idl, path) != FALSE;
    CoTaskMemFree(idl);
    if (!ok) return false;
    out = Narrow(path);
    return !out.empty();
}

} // namespace

bool Available() { return true; } // родной диалог есть в любой Windows

const std::string& Backend() {
    static const std::string name = "windows";
    return name;
}

bool OpenFile(const std::string& title, const std::vector<Filter>& filters,
              const std::string& startDir, std::string& out) {
    std::wstring filter = BuildFilter(filters);
    const std::wstring wtitle = Widen(title);
    const std::wstring wstart = Widen(startDir);
    std::vector<wchar_t> buffer(4096, L'\0');

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = OwnerWindow();
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = (DWORD)buffer.size();
    ofn.lpstrTitle = wtitle.empty() ? nullptr : wtitle.c_str();
    ofn.lpstrInitialDir = wstart.empty() ? nullptr : wstart.c_str();
    // OFN_NOCHANGEDIR обязателен: без него диалог меняет ТЕКУЩИЙ КАТАЛОГ
    // процесса, а по нему инструмент ищет ассеты и шейдеры. Один поход в
    // «Открыть» — и относительные пути проекта начинают вести не туда.
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;

    if (!GetOpenFileNameW(&ofn)) return false; // отмена или ошибка
    out = Narrow(buffer.data());
    return !out.empty();
}

bool SaveFile(const std::string& title, const std::vector<Filter>& filters,
              const std::string& startDir, const std::string& suggestedName, std::string& out) {
    std::wstring filter = BuildFilter(filters);
    const std::wstring wtitle = Widen(title);
    const std::wstring wstart = Widen(startDir);
    const std::wstring wext = DefaultExtension(filters);

    std::vector<wchar_t> buffer(4096, L'\0');
    const std::wstring wname = Widen(suggestedName);
    if (!wname.empty() && wname.size() + 1 < buffer.size()) {
        std::copy(wname.begin(), wname.end(), buffer.begin());
    }

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = OwnerWindow();
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = (DWORD)buffer.size();
    ofn.lpstrTitle = wtitle.empty() ? nullptr : wtitle.c_str();
    ofn.lpstrInitialDir = wstart.empty() ? nullptr : wstart.c_str();
    ofn.lpstrDefExt = wext.empty() ? nullptr : wext.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;

    if (!GetSaveFileNameW(&ofn)) return false;
    out = Narrow(buffer.data());
    return !out.empty();
}

bool PickFolder(const std::string& title, const std::string& startDir, std::string& out) {
    ComScope com;

    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(kClsidFileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  kIidIFileOpenDialog, (void**)&dialog);
    if (FAILED(hr) || !dialog) return PickFolderLegacy(title, startDir, out);

    DWORD options = 0;
    dialog->GetOptions(&options);
    // FOS_PICKFOLDERS превращает обычный диалог открытия в выбор каталога —
    // это и есть современный «выберите папку» из проводника.
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);

    const std::wstring wtitle = Widen(title);
    if (!wtitle.empty()) dialog->SetTitle(wtitle.c_str());

    if (!startDir.empty()) {
        const std::wstring wstart = Widen(startDir);
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(wstart.c_str(), nullptr, kIidIShellItem,
                                                  (void**)&item)) && item) {
            dialog->SetFolder(item);
            item->Release();
        }
    }

    bool picked = false;
    if (SUCCEEDED(dialog->Show(OwnerWindow()))) {
        IShellItem* result = nullptr;
        if (SUCCEEDED(dialog->GetResult(&result)) && result) {
            PWSTR path = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                out = Narrow(path);
                picked = !out.empty();
                CoTaskMemFree(path);
            }
            result->Release();
        }
    }
    dialog->Release();
    return picked;
}

bool RevealInFileManager(const std::string& path) {
    if (path.empty()) return false;
    const std::wstring wpath = Widen(path);
    const DWORD attrs = GetFileAttributesW(wpath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;

    HINSTANCE rc;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        rc = ShellExecuteW(nullptr, L"open", wpath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        // Для файла открываем каталог и ВЫДЕЛЯЕМ его — так видно, что именно
        // получилось, а не просто «где-то здесь».
        const std::wstring args = L"/select,\"" + wpath + L"\"";
        rc = ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
    // ShellExecute отдаёт код ошибки как «псевдо-HINSTANCE»; больше 32 — успех.
    return (INT_PTR)rc > 32;
}

// ============================================================================
//  Linux и macOS: штатная утилита окружения
// ============================================================================
#else

namespace {

// Запускает команду и возвращает весь её вывод без завершающего перевода
// строки. Пусто — команда не запустилась или ничего не напечатала.
std::string RunAndRead(const std::string& command) {
    std::FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return {};
    std::string out;
    char buffer[1024];
    while (std::fgets(buffer, sizeof(buffer), pipe)) out += buffer;
    pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

// Есть ли исполняемый файл в PATH.
bool HasCommand(const char* name) {
    const std::string probe = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(probe.c_str()) == 0;
}

// Какой утилитой открывать диалоги. Ищем один раз за запуск: system() на
// каждый вопрос — это порождение процесса, а спрашивают об этом при отрисовке
// каждого диалога.
enum class Backend { None, Zenity, KDialog, OsaScript };

Backend DetectBackend() {
#ifdef __APPLE__
    if (HasCommand("osascript")) return Backend::OsaScript;
#else
    // zenity первым: он есть в GNOME и во многих минимальных окружениях,
    // kdialog — в KDE.
    if (HasCommand("zenity")) return Backend::Zenity;
    if (HasCommand("kdialog")) return Backend::KDialog;
#endif
    return Backend::None;
}

Backend CurrentBackend() {
    static const Backend backend = [] {
        const Backend b = DetectBackend();
        if (b == Backend::None) {
            LOG_INFO("FileDialog") << "Системного диалога выбора файлов не нашлось "
                                      "(zenity/kdialog/osascript) — путь вводится строкой";
        }
        return b;
    }();
    return backend;
}

// Шаблоны всех фильтров одной строкой: "*.glb *.gltf *.obj". Нужно тем
// утилитам, которые принимают один общий список, а не набор именованных.
std::string AllPatterns(const std::vector<Filter>& filters) {
    std::string out;
    for (const Filter& f : filters) {
        if (!out.empty()) out += ' ';
        out += f.Patterns;
    }
    return out;
}

// Разбор ответа. Пустая строка означает отмену: утилиты в этом случае и
// печатают пустоту, и возвращают ненулевой код.
bool Accept(const std::string& answer, std::string& out) {
    if (answer.empty()) return false;
    out = answer;
    return true;
}

} // namespace

bool Available() { return CurrentBackend() != Backend::None; }

const std::string& Backend() {
    static const std::string name = [] {
        switch (CurrentBackend()) {
            case Backend::Zenity:     return std::string("zenity");
            case Backend::KDialog:    return std::string("kdialog");
            case Backend::OsaScript:  return std::string("osascript");
            default:                  return std::string();
        }
    }();
    return name;
}

bool OpenFile(const std::string& title, const std::vector<Filter>& filters,
              const std::string& startDir, std::string& out) {
    std::ostringstream cmd;
    switch (CurrentBackend()) {
        case Backend::Zenity: {
            cmd << "zenity --file-selection --title=" << Quote(title);
            if (!startDir.empty()) cmd << " --filename=" << Quote(startDir + "/");
            for (const Filter& f : filters) {
                cmd << " --file-filter=" << Quote(f.Name + " | " + f.Patterns);
            }
            cmd << " 2>/dev/null";
            break;
        }
        case Backend::KDialog: {
            // kdialog берёт фильтры одной строкой вида "*.a *.b|Имя".
            const std::string patterns = AllPatterns(filters);
            cmd << "kdialog --title " << Quote(title) << " --getopenfilename "
                << Quote(startDir.empty() ? "." : startDir);
            if (!patterns.empty()) cmd << ' ' << Quote(patterns + "|" + T("Поддерживаемые файлы"));
            cmd << " 2>/dev/null";
            break;
        }
        case Backend::OsaScript: {
            cmd << "osascript -e " << Quote("POSIX path of (choose file with prompt \"" + title + "\")")
                << " 2>/dev/null";
            break;
        }
        default:
            return false;
    }
    return Accept(RunAndRead(cmd.str()), out);
}

bool SaveFile(const std::string& title, const std::vector<Filter>& filters,
              const std::string& startDir, const std::string& suggestedName, std::string& out) {
    std::ostringstream cmd;
    switch (CurrentBackend()) {
        case Backend::Zenity: {
            cmd << "zenity --file-selection --save --confirm-overwrite --title=" << Quote(title);
            const std::string start = startDir.empty() ? suggestedName : startDir + "/" + suggestedName;
            if (!start.empty()) cmd << " --filename=" << Quote(start);
            for (const Filter& f : filters) {
                cmd << " --file-filter=" << Quote(f.Name + " | " + f.Patterns);
            }
            cmd << " 2>/dev/null";
            break;
        }
        case Backend::KDialog: {
            const std::string patterns = AllPatterns(filters);
            const std::string start = startDir.empty() ? suggestedName : startDir + "/" + suggestedName;
            cmd << "kdialog --title " << Quote(title) << " --getsavefilename "
                << Quote(start.empty() ? "." : start);
            if (!patterns.empty()) cmd << ' ' << Quote(patterns + "|" + T("Поддерживаемые файлы"));
            cmd << " 2>/dev/null";
            break;
        }
        case Backend::OsaScript: {
            std::string script = "POSIX path of (choose file name with prompt \"" + title + "\"";
            if (!suggestedName.empty()) script += " default name \"" + suggestedName + "\"";
            script += ")";
            cmd << "osascript -e " << Quote(script) << " 2>/dev/null";
            break;
        }
        default:
            return false;
    }
    return Accept(RunAndRead(cmd.str()), out);
}

bool PickFolder(const std::string& title, const std::string& startDir, std::string& out) {
    std::ostringstream cmd;
    switch (CurrentBackend()) {
        case Backend::Zenity:
            cmd << "zenity --file-selection --directory --title=" << Quote(title);
            if (!startDir.empty()) cmd << " --filename=" << Quote(startDir + "/");
            cmd << " 2>/dev/null";
            break;
        case Backend::KDialog:
            cmd << "kdialog --title " << Quote(title) << " --getexistingdirectory "
                << Quote(startDir.empty() ? "." : startDir) << " 2>/dev/null";
            break;
        case Backend::OsaScript:
            cmd << "osascript -e "
                << Quote("POSIX path of (choose folder with prompt \"" + title + "\")")
                << " 2>/dev/null";
            break;
        default:
            return false;
    }
    return Accept(RunAndRead(cmd.str()), out);
}

bool RevealInFileManager(const std::string& path) {
    if (path.empty()) return false;
#ifdef __APPLE__
    const char* opener = "open";
#else
    const char* opener = "xdg-open";
#endif
    if (!HasCommand(opener)) return false;
    // В фоне и с заглушенным выводом: файловый менеджер живёт своей жизнью, и
    // ждать его завершения означало бы подвесить инструмент до тех пор, пока
    // человек не закроет окно проводника.
    const std::string cmd = std::string(opener) + " " + Quote(path) + " >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0;
}

#endif // _WIN32

} // namespace d3d::filedialog
