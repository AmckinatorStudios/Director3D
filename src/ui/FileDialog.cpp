#include "ui/FileDialog.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "sage/core/Log.h"
#include "ui/Localization.h"

#ifdef _WIN32
#define D3D_POPEN _popen
#define D3D_PCLOSE _pclose
#else
#define D3D_POPEN popen
#define D3D_PCLOSE pclose
#endif

namespace d3d::filedialog {

namespace {

// Запускает команду и возвращает весь её вывод без завершающего перевода
// строки. Пусто — команда не запустилась или ничего не напечатала.
std::string RunAndRead(const std::string& command) {
    std::FILE* pipe = D3D_POPEN(command.c_str(), "r");
    if (!pipe) return {};
    std::string out;
    char buffer[1024];
    while (std::fgets(buffer, sizeof(buffer), pipe)) out += buffer;
    D3D_PCLOSE(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

// Есть ли исполняемый файл в PATH.
bool HasCommand(const char* name) {
#ifdef _WIN32
    const std::string probe = std::string("where ") + name + " >NUL 2>NUL";
#else
    const std::string probe = std::string("command -v ") + name + " >/dev/null 2>&1";
#endif
    return std::system(probe.c_str()) == 0;
}

} // namespace

// Пути и заголовки приходят от пользователя и из проекта; без экранирования
// пробел ломал бы команду, а кавычка позволяла бы дописать к ней произвольный
// вызов оболочки.
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

// Какой утилитой открывать диалоги. Ищем один раз за запуск: system() на
// каждый вопрос — это порождение процесса, а спрашивают об этом при отрисовке
// каждого диалога.
enum class Backend { None, Zenity, KDialog, OsaScript, PowerShell };

Backend DetectBackend() {
#ifdef _WIN32
    if (HasCommand("powershell")) return Backend::PowerShell;
#elif defined(__APPLE__)
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
                                      "(zenity/kdialog/osascript/powershell) — путь вводится строкой";
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
            case Backend::PowerShell: return std::string("powershell");
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
        case Backend::PowerShell: {
            std::string filter;
            for (const Filter& f : filters) {
                if (!filter.empty()) filter += "|";
                std::string patterns = f.Patterns;
                for (char& c : patterns) if (c == ' ') c = ';';
                filter += f.Name + "|" + patterns;
            }
            if (filter.empty()) filter = std::string(T("Все файлы")) + "|*.*";
            std::string script = "Add-Type -AssemblyName System.Windows.Forms;";
            script += "$d=New-Object System.Windows.Forms.OpenFileDialog;";
            script += "$d.Title='" + title + "';";
            script += "$d.Filter='" + filter + "';";
            if (!startDir.empty()) script += "$d.InitialDirectory='" + startDir + "';";
            script += "if($d.ShowDialog() -eq 'OK'){$d.FileName}";
            cmd << "powershell -NoProfile -Command " << Quote(script);
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
        case Backend::PowerShell: {
            std::string filter;
            for (const Filter& f : filters) {
                if (!filter.empty()) filter += "|";
                std::string patterns = f.Patterns;
                for (char& c : patterns) if (c == ' ') c = ';';
                filter += f.Name + "|" + patterns;
            }
            if (filter.empty()) filter = std::string(T("Все файлы")) + "|*.*";
            std::string script = "Add-Type -AssemblyName System.Windows.Forms;";
            script += "$d=New-Object System.Windows.Forms.SaveFileDialog;";
            script += "$d.Title='" + title + "';";
            script += "$d.Filter='" + filter + "';";
            script += "$d.FileName='" + suggestedName + "';";
            if (!startDir.empty()) script += "$d.InitialDirectory='" + startDir + "';";
            script += "if($d.ShowDialog() -eq 'OK'){$d.FileName}";
            cmd << "powershell -NoProfile -Command " << Quote(script);
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
        case Backend::PowerShell: {
            std::string script = "Add-Type -AssemblyName System.Windows.Forms;";
            script += "$d=New-Object System.Windows.Forms.FolderBrowserDialog;";
            script += "$d.Description='" + title + "';";
            if (!startDir.empty()) script += "$d.SelectedPath='" + startDir + "';";
            script += "if($d.ShowDialog() -eq 'OK'){$d.SelectedPath}";
            cmd << "powershell -NoProfile -Command " << Quote(script);
            break;
        }
        default:
            return false;
    }
    return Accept(RunAndRead(cmd.str()), out);
}

} // namespace d3d::filedialog
