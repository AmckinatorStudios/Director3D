#include "ui/panels/StatusBarPanel.h"

#include <cstdio>
#include <ctime>

#include "imgui.h"

#include "sage/core/Application.h"
#include "sage/core/Version.h"
#include "sage/render/ResourceManager.h"
#include "sage/rhi/GraphicsDevice.h"
#include "ui/DirectorHost.h"
#include "ui/Theme.h"

namespace d3d {

namespace {

// Разделитель между блоками статус-бара — тонкая вертикальная линия с воздухом.
void Divider() {
    ImGui::SameLine(0.0f, 14.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 2.0f), ImVec2(p.x, p.y + 15.0f),
                                        Theme::Colors::Line, 1.0f);
    ImGui::SameLine(0.0f, 14.0f);
}

} // namespace

void StatusBarPanel::NoteSaved() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::snprintf(m_savedAt, sizeof(m_savedAt), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

void StatusBarPanel::Draw(DirectorHost& host) {
    sage::Application& app = sage::Application::Get();

    // Экспоненциальное сглаживание: мгновенный FPS скачет на каждом кадре и
    // читается как мигающий шум, а не как показатель.
    const float fps = app.Fps();
    m_smoothFps = m_smoothFps <= 0.0f ? fps : m_smoothFps * 0.92f + fps * 0.08f;

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                       "Director 3D v0.1.0 — на SAGE Engine %s", kSageEngineVersion);

    Divider();
    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "GPU: %s",
                       sage::rhi::GraphicsDevice::Get().ApiVersion().c_str());

    Divider();
    const ResourceManager::Stats res = ResourceManager::Instance().GetStats();
    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "Текстуры: %.1f МБ",
                       (double)res.TextureBytes / (1024.0 * 1024.0));

    Divider();
    // Цветом показываем, комфортно ли работать: зелёный — да, жёлтый — заметны
    // подтормаживания, красный — работать тяжело.
    const ImU32 fpsColor = m_smoothFps >= 45.0f ? Theme::Colors::Good
                         : m_smoothFps >= 24.0f ? Theme::Colors::Warning
                                                : Theme::Colors::Record;
    ImGui::PushStyleColor(ImGuiCol_Text, fpsColor);
    ImGui::Text("FPS: %.0f", (double)m_smoothFps);
    ImGui::PopStyleColor();

    Divider();
    const sage::ecs::RenderStats& stats = host.Renderer().LastStats();
    ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                       "Отрисовано: %d / %d", stats.Drawn, stats.Total);

    // --- Правая часть: сообщение, проект, автосохранение ---
    const std::string& status = host.Status();
    const AnimationDocument& doc = host.Document();
    char right[256];
    std::snprintf(right, sizeof(right), "Проект: %s%s     Сохранено: %s",
                  doc.Name.c_str(), host.Dirty() ? " *" : "", m_savedAt);
    const float rightWidth = ImGui::CalcTextSize(right).x;

    if (!status.empty()) {
        Divider();
        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", status.c_str());
    }

    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > rightWidth + 12.0f) {
        ImGui::SameLine(0.0f, avail - rightWidth - 6.0f);
        // Несохранённый проект подсвечиваем: звёздочка мелкая, а потерять
        // работу — дорого.
        ImGui::PushStyleColor(ImGuiCol_Text, host.Dirty() ? Theme::Colors::Warning
                                                          : Theme::Colors::TextDim);
        ImGui::TextUnformatted(right);
        ImGui::PopStyleColor();
    }
}

} // namespace d3d
