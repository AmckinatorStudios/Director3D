#include "ui/panels/AssetsPanel.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "imgui.h"
#include "imgui_stdlib.h"

#include "ui/DirectorHost.h"
#include "ui/Icons.h"
#include "ui/Localization.h"
#include "ui/Theme.h"

namespace fs = std::filesystem;

namespace d3d {

using Icons::Icon;

namespace {

std::string LowerExt(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

Icon IconFor(AssetsPanel::Entry::Kind kind) {
    switch (kind) {
        case AssetsPanel::Entry::Kind::Folder:   return Icon::Folder;
        case AssetsPanel::Entry::Kind::Model:    return Icon::Model;
        case AssetsPanel::Entry::Kind::Texture:  return Icon::Texture;
        case AssetsPanel::Entry::Kind::Audio:    return Icon::Audio;
        case AssetsPanel::Entry::Kind::Material: return Icon::Material;
        case AssetsPanel::Entry::Kind::Scene:    return Icon::Cube;
        default:                                 return Icon::Model;
    }
}

ImU32 ColorFor(AssetsPanel::Entry::Kind kind) {
    // Тип ассета читается цветом ещё до чтения имени — на плотной сетке плиток
    // это быстрее, чем разбирать расширения.
    switch (kind) {
        case AssetsPanel::Entry::Kind::Folder:   return Theme::Colors::Warning;
        case AssetsPanel::Entry::Kind::Model:    return 0xFFE8A03C;
        case AssetsPanel::Entry::Kind::Texture:  return Theme::Colors::Good;
        case AssetsPanel::Entry::Kind::Audio:    return Theme::Colors::Waveform;
        case AssetsPanel::Entry::Kind::Material: return 0xFFB07AD0;
        case AssetsPanel::Entry::Kind::Scene:    return Theme::Colors::KeySelected;
        default:                                 return Theme::Colors::TextDim;
    }
}

} // namespace

AssetsPanel::Entry::Kind AssetsPanel::KindOf(const fs::path& path) {
    const std::string ext = LowerExt(path);
    if (ext == ".glb" || ext == ".gltf" || ext == ".obj" || ext == ".fbx") return Entry::Kind::Model;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp")
        return Entry::Kind::Texture;
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac") return Entry::Kind::Audio;
    if (ext == ".sagemat") return Entry::Kind::Material;
    if (ext == ".sage" || ext == ".sageprefab" || ext == ".d3dproj") return Entry::Kind::Scene;
    return Entry::Kind::Other;
}

void AssetsPanel::Refresh(const fs::path& dir) {
    m_entries.clear();
    m_shownDir = dir;
    m_dirty = false;

    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;

    for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
        Entry entry;
        entry.Path = de.path();
        entry.Name = de.path().filename().string();
        entry.Directory = de.is_directory(ec);
        entry.Type = entry.Directory ? Entry::Kind::Folder : KindOf(de.path());
        if (!entry.Directory) entry.Size = de.file_size(ec);
        m_entries.push_back(std::move(entry));
    }
    // Каталоги первыми, дальше по имени — привычный порядок любого файлового
    // менеджера; без сортировки порядок задаёт файловая система и он случаен.
    std::sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) {
        if (a.Directory != b.Directory) return a.Directory;
        return a.Name < b.Name;
    });
}

void AssetsPanel::DrawTree(DirectorHost& host, const fs::path& dir, int depth) {
    if (depth > 4) return; // глубже не разворачиваем: дерево слева — навигация, а не обзор

    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;

    std::vector<fs::path> subdirs;
    for (const fs::directory_entry& de : fs::directory_iterator(dir, ec)) {
        if (de.is_directory(ec)) subdirs.push_back(de.path());
    }
    std::sort(subdirs.begin(), subdirs.end());

    for (const fs::path& sub : subdirs) {
        ImGui::PushID(sub.string().c_str());
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
        if (sub == host.AssetsDir()) flags |= ImGuiTreeNodeFlags_Selected;
        const bool open = ImGui::TreeNodeEx("##d", flags, "%s", sub.filename().string().c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            host.AssetsDir() = sub;
            m_dirty = true;
        }
        if (open) {
            DrawTree(host, sub, depth + 1);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void AssetsPanel::DrawGrid(DirectorHost& host) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float step = m_tileSize + ImGui::GetStyle().ItemSpacing.x;
    const int columns = std::max(1, (int)(avail / step));

    int drawn = 0;
    for (const Entry& entry : m_entries) {
        if (!m_filter.empty() && entry.Name.find(m_filter) == std::string::npos) continue;

        if (drawn % columns != 0) ImGui::SameLine();
        ++drawn;

        ImGui::PushID(entry.Path.string().c_str());
        ImGui::BeginGroup();

        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float thumb = m_tileSize;
        const bool selected = host.SelectedAsset() == entry.Path;

        ImGui::InvisibleButton("##tile", ImVec2(thumb, thumb + 20.0f));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) host.SetSelectedAsset(entry.Path);

        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (entry.Directory) {
                host.AssetsDir() = entry.Path;
                m_dirty = true;
            } else if (entry.Type == Entry::Kind::Model) {
                if (host.ImportAsset(entry.Path) < 0) host.SetStatus(T("Не удалось импортировать: ") + entry.Name);
            } else if (entry.Type == Entry::Kind::Audio) {
                host.PushUndo();
                if (host.Document().Audio.Load(entry.Path.string())) {
                    host.SetStatus(T("Звуковая дорожка: ") + entry.Name);
                }
            } else if (entry.Type == Entry::Kind::Scene) {
                std::string err;
                if (!host.OpenProject(entry.Path, err)) host.SetStatus(T("Не открылось: ") + err);
            }
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 bg = selected ? ImGui::GetColorU32(ImGuiCol_HeaderActive)
                       : hovered  ? ImGui::GetColorU32(ImGuiCol_FrameBgHovered)
                                  : Theme::Colors::PanelDeep;
        dl->AddRectFilled(pos, ImVec2(pos.x + thumb, pos.y + thumb), bg, 4.0f);
        Icons::Draw(dl, IconFor(entry.Type), ImVec2(pos.x + thumb * 0.5f, pos.y + thumb * 0.45f),
                    thumb * 0.44f, ColorFor(entry.Type));

        // Подпись обрезаем по ширине плитки: длинные имена иначе наезжают на
        // соседей и ломают сетку.
        if (ImFont* small = Theme::SmallFont()) ImGui::PushFont(small);
        std::string label = entry.Name;
        while (label.size() > 4 && ImGui::CalcTextSize(label.c_str()).x > thumb - 4.0f) {
            label.erase(label.size() - 4, 1);
            label[label.size() - 3] = '.';
            label[label.size() - 2] = '.';
            label[label.size() - 1] = '.';
        }
        const float textW = ImGui::CalcTextSize(label.c_str()).x;
        dl->AddText(ImVec2(pos.x + (thumb - textW) * 0.5f, pos.y + thumb + 3.0f),
                    selected ? Theme::Colors::Text : Theme::Colors::TextDim, label.c_str());
        if (Theme::SmallFont()) ImGui::PopFont();

        if (hovered) {
            if (entry.Directory) ImGui::SetTooltip(T("%s\nДвойной клик — открыть папку"), entry.Name.c_str());
            else ImGui::SetTooltip(T("%s\n%.1f КБ%s"), entry.Name.c_str(), (double)entry.Size / 1024.0,
                                   entry.Type == Entry::Kind::Model ? T("\nДвойной клик — импорт в сцену")
                                 : entry.Type == Entry::Kind::Audio ? T("\nДвойной клик — на звуковую дорожку")
                                                                    : "");
        }

        ImGui::EndGroup();
        ImGui::PopID();
    }

    if (drawn == 0) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", m_filter.empty() ? T("Папка пуста.") : T("Ничего не найдено."));
    }
}

void AssetsPanel::Draw(DirectorHost& host) {
    ImGui::Begin((std::string(T("Ресурсы")) + "###Assets").c_str());

    if (ImGui::BeginTabBar("##assetTabs")) {
        // «Файлы», а не «Assets»: вкладка дока над ними уже называется Assets,
        // и одинаковое слово двумя строками подряд читается как сбой раскладки.
        if (ImGui::BeginTabItem(T("Файлы"))) { m_tab = 0; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(T("Заготовки"))) { m_tab = 1; ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    if (m_tab == 1) {
        // Пресеты — быстрые заготовки сцены: не файлы на диске, а рецепты
        // «поставить и сразу снимать».
        ImGui::Spacing();
        ImGui::TextWrapped("%s", T("Готовые заготовки: собирают базовую сцену за один клик, "
                           "чтобы можно было сразу анимировать, а не расставлять свет."));
        ImGui::Spacing();
        if (ImGui::Button(T("Схема из трёх источников"), ImVec2(-1, 0))) {
            host.PushUndo();
            const int key = host.Create(CreateKind::SpotLight);
            const int fill = host.Create(CreateKind::PointLight);
            const int rim = host.Create(CreateKind::PointLight);
            host.RenameObject(key, "Key_Light");
            host.RenameObject(fill, "Fill_Light");
            host.RenameObject(rim, "Rim_Light");
            Scene& scene = host.CurrentScene();
            // Классическая трёхточечная схема: рисующий сверху-сбоку, заполняющий
            // с противоположной стороны послабее, контровой сзади.
            if (GameObject o = scene.Get(key); o.Valid()) {
                o.GetTransform().Position = {3.5f, 4.0f, 3.5f};
                o.GetTransform().Rotation = {-40.0f, 45.0f, 0.0f};
            }
            if (GameObject o = scene.Get(fill); o.Valid()) o.GetTransform().Position = {-3.5f, 2.0f, 2.5f};
            if (GameObject o = scene.Get(rim); o.Valid()) o.GetTransform().Position = {0.0f, 3.0f, -4.0f};
            host.SetStatus(T("Трёхточечная схема света добавлена"));
        }
        if (ImGui::Button(T("Камера и пол"), ImVec2(-1, 0))) {
            host.PushUndo();
            const int cam = host.Create(CreateKind::Camera);
            const int ground = host.Create(CreateKind::Plane);
            host.RenameObject(ground, "Ground");
            Scene& scene = host.CurrentScene();
            if (GameObject o = scene.Get(ground); o.Valid()) o.GetTransform().Scale = {24.0f, 1.0f, 24.0f};
            host.SetActiveCameraId(cam);
            host.SetStatus(T("Камера и пол добавлены"));
        }
        if (ImGui::Button(T("Turntable (объект крутится 4 c)"), ImVec2(-1, 0))) {
            const int id = host.SelectedId();
            if (id < 0) {
                host.SetStatus(T("Сначала выберите объект"));
            } else {
                host.PushUndo();
                // Оборот вокруг Y за 4 секунды — самый частый показ модели.
                AnimationDocument& doc = host.Document();
                Track& track = doc.EnsureTrack(id, Property::Rotation);
                if (track.ChannelCount() >= 2) {
                    Scene& scene = host.CurrentScene();
                    float values[3] = {0, 0, 0};
                    ReadProperty(scene, id, Property::Rotation, values);
                    track.Channels[1].Clear();
                    track.Channels[1].SetKey(0.0f, values[1], Interp::Linear);
                    track.Channels[1].SetKey(4.0f, values[1] + 360.0f, Interp::Linear);
                }
                host.SetStatus(T("Вращение на 4 секунды добавлено"));
            }
        }
        ImGui::End();
        return;
    }

    // --- Панель пути и поиск ---
    if (m_dirty || m_shownDir != host.AssetsDir()) Refresh(host.AssetsDir());

    if (Icons::IconButton("up", Icon::Open, T("На уровень вверх"))) {
        if (host.AssetsDir().has_parent_path()) {
            host.AssetsDir() = host.AssetsDir().parent_path();
            m_dirty = true;
        }
    }
    ImGui::SameLine();
    {
        // Ползунок размера плиток прячем на узкой панели. Он второстепенный, а
        // отнимал сто пикселей у поиска — в итоге не помещалось ни поле ввода
        // (от подсказки оставалось «Searc»), ни сам ползунок, который без
        // подписи выглядел просто сломанным виджетом.
        const float avail = ImGui::GetContentRegionAvail().x;
        const bool showTileSlider = avail > 300.0f;
        const float searchWidth = avail - (showTileSlider ? 116.0f : 0.0f);

        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemWidth(searchWidth);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(24, 4));
        ImGui::InputTextWithHint("##assetSearch", T("Поиск..."), &m_filter);
        ImGui::PopStyleVar();
        Icons::Draw(ImGui::GetWindowDrawList(), Icon::Search,
                    ImVec2(pos.x + 13.0f, pos.y + ImGui::GetFrameHeight() * 0.5f),
                    13.0f, Theme::Colors::TextFaint);

        if (showTileSlider) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100.0f);
            ImGui::SliderFloat("##tile", &m_tileSize, 48.0f, 140.0f, T("размер"));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Размер плиток"));
        }
    }

    // Путь укорачивается СЛЕВА: конец пути (текущая папка) важнее начала, а
    // целиком он в панель шириной в четверть экрана не помещается никогда.
    {
        const std::string full = host.AssetsDir().string();
        const float maxWidth = ImGui::GetContentRegionAvail().x;
        std::string shown = full;
        if (ImGui::CalcTextSize(full.c_str()).x > maxWidth) {
            size_t cut = 0;
            while (cut < full.size() &&
                   ImGui::CalcTextSize(("…" + full.substr(cut)).c_str()).x > maxWidth) {
                ++cut;
            }
            shown = "…" + full.substr(cut);
        }
        ImGui::TextDisabled("%s", shown.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", full.c_str());
    }
    ImGui::Separator();

    // --- Дерево слева, плитки справа ---
    // Дереву нужна не доля, а минимум под имя папки: при 40% от узкой панели
    // оно получало ~88 пикселей, и «.github» обрезалось на середине. Ниже 130
    // не опускаемся, но и больше половины панели не забираем.
    // На узкой панели дерево ПРЯЧЕТСЯ целиком. Делить двести пикселей между
    // деревом и сеткой бессмысленно: не помещается ни одно, ни другое. Ходить
    // по папкам можно и сеткой — двойным щелчком внутрь, кнопкой слева наверх.
    const float assetsAvail = ImGui::GetContentRegionAvail().x;
    const bool showTree = assetsAvail >= 320.0f;
    if (showTree) {
        const float treeWidth = std::clamp(assetsAvail * 0.45f, 130.0f, 190.0f);
        ImGui::BeginChild("##assetTree", ImVec2(treeWidth, -ImGui::GetFrameHeightWithSpacing()), true);
        DrawTree(host, host.AssetsDir(), 0);
        if (m_entries.empty()) ImGui::TextDisabled("%s", T("Нет вложенных папок"));
        ImGui::EndChild();
        ImGui::SameLine();
    }
    ImGui::BeginChild("##assetGrid", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true);
    DrawGrid(host);
    ImGui::EndChild();

    int shown = 0;
    for (const Entry& e : m_entries) {
        if (m_filter.empty() || e.Name.find(m_filter) != std::string::npos) ++shown;
    }
    ImGui::TextDisabled(T("%d элемент(ов)"), shown);
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 80.0f);
    if (ImGui::SmallButton(T("Обновить"))) m_dirty = true;

    ImGui::End();
}

} // namespace d3d
