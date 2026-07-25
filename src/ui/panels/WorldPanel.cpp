#include "ui/panels/WorldPanel.h"

#include <cstdio>
#include <filesystem>

#include "imgui.h"

#include "sage/render/Skybox.h"
#include "sage/scene/Scene.h"
#include "ui/DirectorHost.h"
#include "ui/Icons.h"
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
    if (dir.empty()) { outProblem = "путь пуст"; return false; }
    if (!fs::is_directory(dir, ec)) { outProblem = "это не каталог"; return false; }

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
        outProblem = "не хватает граней: " + missing;
        return false;
    }
    return true;
}

} // namespace

void WorldPanel::Draw(DirectorHost& host) {
    ImGui::Begin("World");

    Scene& scene = host.CurrentScene();
    LightingEnvironment& env = scene.Lighting;

    // Буфер пути синхронизируем со сценой один раз и после загрузки проекта.
    if (!m_skyDirLoaded || (env.Skybox.CubemapDir != m_skyDir && !ImGui::IsAnyItemActive())) {
        std::snprintf(m_skyDir, sizeof(m_skyDir), "%s", env.Skybox.CubemapDir.c_str());
        m_skyDirLoaded = true;
    }

    // --- Небо ---
    if (SectionHeader("Sky")) {
        if (ImGui::Checkbox("Показывать небо", &env.Skybox.Enabled)) host.PushUndo();
        if (!env.Skybox.Enabled) {
            ImGui::TextDisabled("Фон заливается сплошным цветом.");
        }

        ImGui::BeginDisabled(!env.Skybox.Enabled);
        ImGui::Spacing();

        const bool cubemap = env.Skybox.HasCubemap();
        ImGui::TextDisabled(cubemap ? "Режим: кубическая текстура"
                                    : "Режим: процедурный градиент");
        ImGui::Spacing();

        // --- Процедурный градиент ---
        ImGui::BeginDisabled(cubemap);
        if (ImGui::ColorEdit3("Зенит", &env.Skybox.TopColor.x, ImGuiColorEditFlags_Float))
            host.TrackLastItem();
        host.TrackLastItem();
        if (ImGui::ColorEdit3("Горизонт", &env.Skybox.HorizonColor.x, ImGuiColorEditFlags_Float))
            host.TrackLastItem();
        host.TrackLastItem();
        ImGui::EndDisabled();
        if (cubemap) ImGui::TextDisabled("(цвета градиента не действуют, пока задана текстура)");

        ImGui::Spacing();
        ImGui::SeparatorText("Кубическая текстура");
        ImGui::TextWrapped("Каталог с шестью гранями: px, nx, py, ny, pz, nz "
                           "(png, jpg, jpeg, tga или bmp).");

        ImGui::SetNextItemWidth(-90.0f);
        ImGui::InputText("##skydir", m_skyDir, sizeof(m_skyDir));
        ImGui::SameLine();
        if (ImGui::Button("Применить", ImVec2(-1.0f, 0.0f))) {
            std::string problem;
            if (m_skyDir[0] == '\0') {
                host.PushUndo();
                env.Skybox.CubemapDir.clear();
                m_lastError.clear();
                host.SetStatus("Небо: процедурный градиент");
            } else if (CheckSkyDirectory(m_skyDir, problem)) {
                host.PushUndo();
                env.Skybox.CubemapDir = m_skyDir;
                m_lastError.clear();
                host.SetStatus("Небо загружено из текстуры");
            } else {
                m_lastError = "Не подходит: " + problem;
            }
        }

        if (!m_lastError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Record);
            ImGui::TextWrapped("%s", m_lastError.c_str());
            ImGui::PopStyleColor();
        }
        if (cubemap) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Good);
            ImGui::TextWrapped("Активна: %s", env.Skybox.CubemapDir.c_str());
            ImGui::PopStyleColor();
            if (ImGui::SmallButton("Убрать текстуру")) {
                host.PushUndo();
                env.Skybox.CubemapDir.clear();
                m_skyDir[0] = '\0';
                host.SetStatus("Небо: процедурный градиент");
            }
        }

        ImGui::Spacing();
        ImGui::BeginDisabled(!cubemap);
        ImGui::SetNextItemWidth(-120.0f);
        if (ImGui::SliderFloat("Яркость неба", &env.Skybox.Intensity, 0.0f, 4.0f, "%.2f"))
            host.TrackLastItem();
        host.TrackLastItem();
        ImGui::SetNextItemWidth(-120.0f);
        if (ImGui::SliderFloat("Поворот", &env.Skybox.RotationDeg, 0.0f, 360.0f, "%.0f°"))
            host.TrackLastItem();
        host.TrackLastItem();
        if (cubemap) {
            ImGui::TextDisabled("Поворотом совмещают солнце на текстуре с солнцем сцены.");
        }
        ImGui::EndDisabled();

        ImGui::EndDisabled(); // !Enabled
    }

    // --- Солнце ---
    if (SectionHeader("Sun")) {
        if (ImGui::ColorEdit3("Цвет##sun", &env.Sun.Color.x, ImGuiColorEditFlags_Float)) {}
        host.TrackLastItem();
        ImGui::SetNextItemWidth(-120.0f);
        ImGui::DragFloat("Интенсивность##sun", &env.Sun.Intensity, 0.01f, 0.0f, 10.0f, "%.2f");
        host.TrackLastItem();
        ImGui::SetNextItemWidth(-120.0f);
        if (ImGui::DragFloat3("Направление", &env.Sun.Direction.x, 0.01f, -1.0f, 1.0f, "%.2f")) {
            // Нулевой вектор направления даёт NaN в нормализации и чёрный кадр.
            if (glm::length(env.Sun.Direction) < 1e-4f) env.Sun.Direction = {0.0f, -1.0f, 0.0f};
        }
        host.TrackLastItem();
        ImGui::TextDisabled("Направление — КУДА светит солнце, а не откуда.");
    }

    // --- Фоновая засветка ---
    if (SectionHeader("Ambient")) {
        if (env.Skybox.Enabled) {
            ImGui::TextDisabled("Небо включено — засветка берётся из его цветов.");
        }
        ImGui::BeginDisabled(env.Skybox.Enabled);
        if (ImGui::ColorEdit3("Свет неба", &env.SkyColor.x, ImGuiColorEditFlags_Float)) {}
        host.TrackLastItem();
        if (ImGui::ColorEdit3("Отражённый от земли", &env.GroundColor.x, ImGuiColorEditFlags_Float)) {}
        host.TrackLastItem();
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(-120.0f);
        ImGui::SliderFloat("Сила", &env.AmbientStrength, 0.0f, 2.0f, "%.2f");
        host.TrackLastItem();
    }

    // --- Туман ---
    if (SectionHeader("Fog", /*defaultOpen=*/false)) {
        if (ImGui::Checkbox("Туман", &env.Fog.Enabled)) host.PushUndo();
        ImGui::BeginDisabled(!env.Fog.Enabled);
        if (ImGui::ColorEdit3("Цвет##fog", &env.Fog.Color.x, ImGuiColorEditFlags_Float)) {}
        host.TrackLastItem();
        ImGui::SetNextItemWidth(-120.0f);
        ImGui::DragFloat("Начало", &env.Fog.Start, 0.1f, 0.0f, 1000.0f, "%.1f");
        host.TrackLastItem();
        ImGui::SetNextItemWidth(-120.0f);
        ImGui::DragFloat("Конец", &env.Fog.End, 0.1f, 0.1f, 5000.0f, "%.1f");
        host.TrackLastItem();
        // Конец ближе начала переворачивает формулу тумана — чиним молча.
        if (env.Fog.End <= env.Fog.Start) env.Fog.End = env.Fog.Start + 1.0f;
        ImGui::EndDisabled();
    }

    ImGui::End();
}

} // namespace d3d
