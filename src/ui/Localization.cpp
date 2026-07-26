#include "ui/Localization.h"

#include <algorithm>
#include <fstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "sage/core/Log.h"

namespace d3d::i18n {

namespace {

// Словарь и язык — состояние процесса, а не объекта: перевод запрашивается из
// каждой панели на каждой строке, и таскать через них ссылку на словарь
// означало бы протянуть его через весь DirectorHost ради одной функции.
struct State {
    Language Current = Language::Russian;
    std::unordered_map<std::string, std::string> English;
    // Строки, которым перевода не нашлось. set по строке, а не вектор: одна и
    // та же надпись запрашивается каждый кадр, и вектор рос бы бесконечно.
    std::vector<std::string> Missing;
};

State& Get() {
    static State state;
    return state;
}

void NoteMissing(const char* russian) {
    State& s = Get();
    const std::string key(russian);
    if (std::find(s.Missing.begin(), s.Missing.end(), key) != s.Missing.end()) return;
    s.Missing.push_back(key);
}

} // namespace

bool LoadDictionary(const std::string& directory) {
    State& s = Get();
    s.English.clear();

    const std::string path = directory + "/en.json";
    std::ifstream file(path);
    if (!file) {
        LOG_INFO("i18n") << "Словарь не найден (" << path << ") — интерфейс останется русским";
        return false;
    }
    try {
        nlohmann::json root = nlohmann::json::parse(file);
        if (!root.is_object()) {
            LOG_ERROR("i18n") << "Словарь " << path << " не является объектом";
            return false;
        }
        for (auto it = root.begin(); it != root.end(); ++it) {
            if (it.value().is_string()) s.English[it.key()] = it.value().get<std::string>();
        }
    } catch (const std::exception& e) {
        LOG_ERROR("i18n") << "Словарь " << path << " не прочитан: " << e.what();
        return false;
    }
    LOG_INFO("i18n") << "Словарь загружен: " << s.English.size() << " строк";
    return true;
}

void SetLanguage(Language language) { Get().Current = language; }
Language CurrentLanguage() { return Get().Current; }

const char* LanguageCode() { return Get().Current == Language::English ? "en" : "ru"; }

bool SetLanguageByCode(const std::string& code) {
    if (code == "ru") { SetLanguage(Language::Russian); return true; }
    if (code == "en") { SetLanguage(Language::English); return true; }
    return false;
}

namespace {
const char* kPreferencePath = "director3d_lang.txt";
}

void LoadPreference() {
    std::ifstream file(kPreferencePath);
    if (!file) return; // Файла нет — первый запуск, остаёмся на русском.
    std::string code;
    file >> code;
    if (!SetLanguageByCode(code)) {
        LOG_INFO("i18n") << "В " << kPreferencePath << " непонятный код языка: " << code;
    }
}

void SavePreference() {
    std::ofstream file(kPreferencePath, std::ios::trunc);
    // Не записалось — не авария: язык просто не запомнится. Ронять программу
    // из-за настройки, которую можно переключить в меню за секунду, незачем.
    if (file) file << LanguageCode() << '\n';
}

const char* Translate(const char* russian) {
    if (!russian || !*russian) return russian;
    State& s = Get();
    if (s.Current == Language::Russian) return russian;

    auto it = s.English.find(russian);
    if (it == s.English.end()) {
        // Перевода нет — показываем русский. Пустая строка или заглушка вроде
        // «???» были бы хуже: интерфейс с дырами невозможно использовать, а
        // непереведённая надпись остаётся понятной хотя бы части людей.
        NoteMissing(russian);
        return russian;
    }
    // Указатель В КАРТУ: строка живёт столько же, сколько словарь, то есть до
    // конца работы. Возвращать c_str() временной строки нельзя — ImGui хранит
    // переданное до конца кадра.
    return it->second.c_str();
}

int DictionarySize() { return (int)Get().English.size(); }

const std::vector<std::string>& MissingKeys() { return Get().Missing; }

} // namespace d3d::i18n
