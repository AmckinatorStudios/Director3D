#include "ui/panels/WorldPanel.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "imgui.h"

#include "sage/render/Skybox.h"
#include "sage/scene/Scene.h"
#include "ui/DirectorHost.h"
#include "ui/Icons.h"
#include "ui/Localization.h"
#include "ui/Theme.h"

namespace fs = std::filesystem;

namespace d3d {

namespace {

bool SectionHeader(const char* label, bool defaultOpen = true) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 5));
    const bool open = ImGui::CollapsingHeader(
        label, ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding |
                   (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0));
    ImGui::PopStyleVar();
    return open;
}

// Проверяет каталог неба ДО применения и объясняет, чего именно не хватает.
// «Не загрузилось» без причины — самая бесполезная ошибка, какую можно выдать
// на набор из шести файлов с обязательными именами.
bool CheckSkyDirectory(const std::string& dir, std::string& outProblem) {
    std::error_code ec;
    if (dir.empty()) { outProblem = T("путь пуст"); return false; }
    if (!fs::is_directory(dir, ec)) { outProblem = T("это не каталог"); return false; }

    static const char* kExtensions[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp"};
    std::string missing;
    for (const char* face : Skybox::FaceNames()) {
        bool found = false;
        for (const char* ext : kExtensions) {
            if (fs::exists(fs::path(dir) / (std::string(face) + ext), ec)) { found = true; break; }
        }
        if (!found) {
            if (!missing.empty()) missing += ", ";
            missing += face;
        }
    }
    if (!missing.empty()) {
        outProblem = T("не хватает граней: ") + missing;
        return false;
    }
    return true;
}

} // namespace

void WorldPanel::Draw(DirectorHost& host) {
    ImGui::Begin((std::string(T("Мир")) + "###World").c_str());

    // --- Сетка ---
    // Сетка живёт в World вместе с небом и туманом: это всё «как выглядит
    // сцена», а не свойство объекта.
    if (SectionHeader(T("Сетка"))) {
        ViewportOverlays& ov = host.Overlays();
        sage::render::GridSettings& grid = ov.GridConfig;

        ImGui::Spacing();
        ImGui::Checkbox(T("Показывать сетку"), &ov.Grid);

        ImGui::BeginDisabled(!ov.Grid);
        ImGui::Spacing();

        int mode = grid.Mode == sage::render::GridSettings::Extent::Infinite ? 0 : 1;
        ImGui::TextUnformatted(T("Протяжённость"));
        if (ImGui::RadioButton(T("Бесконечная"), mode == 0)) {
            grid.Mode = sage::render::GridSettings::Extent::Infinite;
        }
        ImGui::SameLine(0.0f, 16.0f);
        if (ImGui::RadioButton(T("Радиус"), mode == 1)) {
            grid.Mode = sage::render::GridSettings::Extent::Radius;
        }
        if (grid.Mode == sage::render::GridSettings::Extent::Radius) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat("##radius", &grid.Radius, 0.5f, 1.0f, 5000.0f, T("%.1f м"));
        } else {
            ImGui::TextDisabled("%s", T("Уходит за горизонт с затуханием по высоте камеры."));
        }

        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat("##cell", &grid.CellSize, 0.01f, 0.01f, 100.0f, T("клетка %.2f м"))) {
            grid.CellSize = std::max(grid.CellSize, 0.01f);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Шаг клетки. Он же — шаг привязки гизмо (Snap),\n"
                              "поэтому объект прилипает ровно к видимым линиям."));
        }
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt("##major", &grid.MajorEvery, 2, 20, T("крупная линия каждые %d"));

        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat("##height", &grid.Height, 0.05f, -100.0f, 100.0f, T("высота %.2f м"));
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##opacity", &grid.Opacity, 0.0f, 1.0f, T("прозрачность %.2f"));

        ImGui::Spacing();
        ImGui::Checkbox(T("Выделять оси X и Z"), &grid.ShowAxes);
        ImGui::ColorEdit3(T("Мелкая линия"), &grid.MinorColor.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::ColorEdit3(T("Крупная"), &grid.MajorColor.x, ImGuiColorEditFlags_NoInputs);

        ImGui::Spacing();
        if (ImGui::Button(T("Сантиметры"))) { grid.CellSize = 0.01f; grid.MajorEvery = 10; }
        ImGui::SameLine();
        if (ImGui::Button(T("Метры"))) { grid.CellSize = 1.0f; grid.MajorEvery = 10; }
        ImGui::SameLine();
        if (ImGui::Button(T("Десятки"))) { grid.CellSize = 10.0f; grid.MajorEvery = 10; }

        ImGui::EndDisabled();
        ImGui::Spacing();
    }


    Scene& scene = host.CurrentScene();
    LightingEnvironment& env = scene.Lighting;

    // Буфер пути синхронизируем со сценой один раз и после загрузки проекта.
    if (!m_skyDirLoaded || (env.Skybox.CubemapDir != m_skyDir && !ImGui::IsAnyItemActive())) {
        std::snprintf(m_skyDir, sizeof(m_skyDir), "%s", env.Skybox.CubemapDir.c_str());
        m_skyDirLoaded = true;
    }

    // --- Небо ---
    if (SectionHeader(T("Небо"))) {
        if (ImGui::Checkbox(T("Показывать небо"), &env.Skybox.Enabled)) host.PushUndo();
        if (!env.Skybox.Enabled) {
            ImGui::TextDisabled("%s", T("Фон заливается сплошным цветом."));
        }

        ImGui::BeginDisabled(!env.Skybox.Enabled);
        ImGui::Spacing();

        const bool cubemap = env.Skybox.HasCubemap();
        ImGui::TextDisabled("%s", cubemap ? T("Режим: кубическая текстура")
                                          : T("Режим: процедурный градиент"));
        ImGui::Spacing();

        // --- Процедурный градиент ---
        ImGui::BeginDisabled(cubemap);
        if (ImGui::ColorEdit3(T("Зенит"), &env.Skybox.TopColor.x, ImGuiColorEditFlags_Float))
            host.TrackLastItem();
        host.TrackLastItem();
        if (ImGui::ColorEdit3(T("Горизонт"), &env.Skybox.HorizonColor.x, ImGuiColorEditFlags_Float))
            host.TrackLastItem();
        host.TrackLastItem();
        ImGui::EndDisabled();
        if (cubemap) ImGui::TextDisabled("%s", T("(цвета градиента не действуют, пока задана текстура)"));

        ImGui::Spacing();
        ImGui::SeparatorText(T("Кубическая текстура"));
        ImGui::TextWrapped("%s", T("Каталог с шестью гранями: px, nx, py, ny, pz, nz "
                           "(png, jpg, jpeg, tga или bmp)."));

        ImGui::SetNextItemWidth(-90.0f);
        ImGui::InputText("##skydir", m_skyDir, sizeof(m_skyDir));
        ImGui::SameLine();
        if (ImGui::Button(T("Применить"), ImVec2(-1.0f, 0.0f))) {
            std::string problem;
            if (m_skyDir[0] == '\0') {
                host.PushUndo();
                env.Skybox.CubemapDir.clear();
                m_lastError.clear();
                host.SetStatus(T("Небо: процедурный градиент"));
            } else if (CheckSkyDirectory(m_skyDir, problem)) {
                host.PushUndo();
                env.Skybox.CubemapDir = m_skyDir;
                m_lastError.clear();
                host.SetStatus(T("Небо загружено из текстуры"));
            } else {
                m_lastError = T("Не подходит: ") + problem;
            }
        }

        if (!m_lastError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Record);
            ImGui::TextWrapped("%s", m_lastError.c_str());
            ImGui::PopStyleColor();
        }
        if (cubemap) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Good);
            ImGui::TextWrapped(T("Активна: %s"), env.Skybox.CubemapDir.c_str());
            ImGui::PopStyleColor();
            if (ImGui::SmallButton(T("Убрать текстуру"))) {
                host.PushUndo();
                env.Skybox.CubemapDir.clear();
                m_skyDir[0] = '\0';
                host.SetStatus(T("Небо: процедурный градиент"));
            }
        }

        ImGui::Spacing();
        ImGui::BeginDisabled(!cubemap);
        ImGui::SetNextItemWidth(-120.0f);
        if (ImGui::SliderFloat(T("Яркость неба"), &env.Skybox.Intensity, 0.0f, 4.0f, "%.2f"))
            host.TrackLastItem();
        host.TrackLastItem();
        ImGui::SetNextItemWidth(-120.0f);
        if (ImGui::SliderFloat(T("Поворот"), &env.Skybox.RotationDeg, 0.0f, 360.0f, "%.0f°"))
            host.TrackLastItem();
        host.TrackLastItem();
        if (cubemap) {
            ImGui::TextDisabled("%s", T("Поворотом совмещают солнце на текстуре с солнцем сцены."));
        }
        ImGui::EndDisabled();

        ImGui::EndDisabled(); // !Enabled
    }

    // --- Солнце ---
    if (SectionHeader(T("Солнце"))) {
        if (ImGui::ColorEdit3(T("Цвет##sun"), &env.Sun.Color.x, ImGuiColorEditFlags_Float)) {}
        host.TrackLastItem();
        ImGui::SetNextItemWidth(-120.0f);
        ImGui::DragFloat(T("Интенсивность##sun"), &env.Sun.Intensity, 0.01f, 0.0f, 10.0f, "%.2f");
        host.TrackLastItem();
        ImGui::SetNextItemWidth(-120.0f);
        if (ImGui::DragFloat3(T("Направление"), &env.Sun.Direction.x, 0.01f, -1.0f, 1.0f, "%.2f")) {
            // Нулевой вектор направления даёт NaN в нормализации и чёрный кадр.
            if (glm::length(env.Sun.Direction) < 1e-4f) env.Sun.Direction = {0.0f, -1.0f, 0.0f};
        }
        host.TrackLastItem();
        ImGui::TextDisabled("%s", T("Направление — КУДА светит солнце, а не откуда."));
    }

    // --- Фоновая засветка ---
    if (SectionHeader(T("Окружающий свет"))) {
        if (env.Skybox.Enabled) {
            ImGui::TextDisabled("%s", T("Небо включено — засветка берётся из его цветов."));
        }
        ImGui::BeginDisabled(env.Skybox.Enabled);
        if (ImGui::ColorEdit3(T("Свет неба"), &env.SkyColor.x, ImGuiColorEditFlags_Float)) {}
        host.TrackLastItem();
        if (ImGui::ColorEdit3(T("Отражённый от земли"), &env.GroundColor.x, ImGuiColorEditFlags_Float)) {}
        host.TrackLastItem();
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(-120.0f);
        ImGui::SliderFloat(T("Сила"), &env.AmbientStrength, 0.0f, 2.0f, "%.2f");
        host.TrackLastItem();
    }

    // --- Туман ---
    if (SectionHeader(T("Туман"), /*defaultOpen=*/false)) {
        if (ImGui::Checkbox(T("Туман"), &env.Fog.Enabled)) host.PushUndo();
        ImGui::BeginDisabled(!env.Fog.Enabled);
        if (ImGui::ColorEdit3(T("Цвет##fog"), &env.Fog.Color.x, ImGuiColorEditFlags_Float)) {}
        host.TrackLastItem();
        ImGui::SetNextItemWidth(-120.0f);
        ImGui::DragFloat(T("Начало"), &env.Fog.Start, 0.1f, 0.0f, 1000.0f, "%.1f");
        host.TrackLastItem();
        ImGui::SetNextItemWidth(-120.0f);
        ImGui::DragFloat(T("Конец"), &env.Fog.End, 0.1f, 0.1f, 5000.0f, "%.1f");
        host.TrackLastItem();
        // Конец ближе начала переворачивает формулу тумана — чиним молча.
        if (env.Fog.End <= env.Fog.Start) env.Fog.End = env.Fog.Start + 1.0f;
        ImGui::EndDisabled();
    }

    ImGui::End();
}

} // namespace d3d
