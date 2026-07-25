#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace d3d {

class DirectorHost;

// Браузер ассетов: слева дерево каталогов, справа — плитки файлов с типовыми
// иконками. Двойной клик по модели импортирует её в сцену, по звуку — ставит
// на звуковую дорожку.
//
// Содержимое каталога перечитывается не каждый кадр, а по изменению пути и по
// кнопке обновления: обход директории на каждый кадр — это сотни системных
// вызовов в секунду на ровном месте.
class AssetsPanel {
public:
    // Один элемент каталога. Публичный, потому что тип ассета определяет и
    // иконку, и цвет плитки, и что делает двойной клик — это разбирают
    // свободные функции отрисовки в .cpp.
    struct Entry {
        std::filesystem::path Path;
        std::string Name;
        bool Directory = false;
        enum class Kind { Folder, Model, Texture, Audio, Material, Scene, Other } Type = Kind::Other;
        uintmax_t Size = 0;
    };

    void Draw(DirectorHost& host);
    void Invalidate() { m_dirty = true; }

private:
    void Refresh(const std::filesystem::path& dir);
    void DrawTree(DirectorHost& host, const std::filesystem::path& dir, int depth);
    void DrawGrid(DirectorHost& host);
    static Entry::Kind KindOf(const std::filesystem::path& path);

    std::vector<Entry> m_entries;
    std::filesystem::path m_shownDir;
    std::string m_filter;
    bool m_dirty = true;
    // 84 пикселя на плитку — это одна колонка в панели шириной в пятую часть
    // экрана: сетка перестаёт быть сеткой. 64 дают две-три колонки и остаются
    // читаемыми; кому нужно крупнее — ползунок рядом с поиском.
    float m_tileSize = 64.0f;
    int m_tab = 0; // 0 — Assets, 1 — Scene Presets
};

} // namespace d3d
