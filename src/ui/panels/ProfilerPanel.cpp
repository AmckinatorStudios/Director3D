#include "ui/panels/ProfilerPanel.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"

#include "sage/core/Profiler.h"
#include "ui/DirectorHost.h"
#include "ui/Localization.h"

namespace d3d {

namespace {

// Цвет полосы по доле от самого дорогого прохода. Красный не означает «плохо»:
// что-то в кадре всегда самое дорогое. Он означает «вот сюда смотреть первым».
ImU32 BarColor(float share) {
    const float r = 0.30f + 0.60f * share;
    const float g = 0.75f - 0.45f * share;
    return ImGui::GetColorU32(ImVec4(r, g, 0.35f, 0.55f));
}

void MsCell(double ms, double peak, bool available) {
    if (!available) {
        ImGui::TextDisabled("—");
        return;
    }
    // Полоса рисуется ПОД текстом, на всю ячейку: глазом сравнивать длины полос
    // на порядок быстрее, чем читать столбик чисел с двумя знаками.
    const float share = peak > 0.0 ? (float)std::min(ms / peak, 1.0) : 0.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetTextLineHeight();
    if (share > 0.0f) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            p0, ImVec2(p0.x + width * share, p0.y + height), BarColor(share), 2.0f);
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", ms);
    ImGui::TextUnformatted(text);
}

} // namespace

void ProfilerPanel::Draw(DirectorHost& host) {
    (void)host;
    // Включаем и выключаем профилировщик ВМЕСТЕ с панелью: метки времени GPU
    // стоят денег, и держать их включёнными, когда окно закрыто, незачем.
    if (m_visible != m_wasVisible) {
        sage::profile::SetEnabled(m_visible);
        m_wasVisible = m_visible;
    }
    if (!m_visible) return;

    ImGui::SetNextWindowSize(ImVec2(420.0f, 340.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin((std::string(T("Профилировщик")) + "###Profiler").c_str(), &m_visible)) {
        ImGui::End();
        return;
    }

    const bool gpu = sage::profile::GpuTimersAvailable();

    // --- Итог по кадру ---
    char header[96];
    std::snprintf(header, sizeof(header), "%s %.2f  |  GPU %.2f",
                  T("кадр, мс:  CPU"), sage::profile::FrameCpuMs(), sage::profile::FrameGpuMs());
    if (gpu) {
        ImGui::TextUnformatted(header);
    } else {
        std::snprintf(header, sizeof(header), "%s %.2f", T("кадр, мс:  CPU"),
                      sage::profile::FrameCpuMs());
        ImGui::TextUnformatted(header);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T("(таймеров GPU нет)"));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Драйвер не даёт меток времени GPU — показано только время CPU."));
        }
    }

    ImGui::Checkbox(T("Сглаженные значения"), &m_average);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s",
                          T("Покадровые числа скачут на десятки процентов из-за планировщика ОС.\n"
                            "Смотреть надо на сглаженные; покадровые нужны, чтобы поймать рывок."));
    }

    ImGui::Separator();

    const std::vector<sage::profile::Entry>& rows =
        m_average ? sage::profile::Average() : sage::profile::Frame();

    if (rows.empty()) {
        ImGui::TextDisabled("%s", T("Нет данных: кадры ещё не собраны."));
        ImGui::End();
        return;
    }

    // Максимум ищем по проходам ВЕРХНЕГО уровня: вложенные уже посчитаны внутри
    // родителя, и сравнивать ребёнка с родителем по длине полосы бессмысленно.
    double peakCpu = 0.0, peakGpu = 0.0;
    for (const sage::profile::Entry& e : rows) {
        if (e.Depth != 0) continue;
        peakCpu = std::max(peakCpu, e.CpuMs);
        peakGpu = std::max(peakGpu, e.GpuMs);
    }

    if (ImGui::BeginTable("##profiler", 3,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn(T("Проход"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T("CPU, мс"), ImGuiTableColumnFlags_WidthFixed, 84.0f);
        ImGui::TableSetupColumn(T("GPU, мс"), ImGuiTableColumnFlags_WidthFixed, 84.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const sage::profile::Entry& e : rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            // Вложенность показываем отступом: «Тени внутри Отрисовки» иначе
            // читается как ещё один проход верхнего уровня, и сумма не сходится.
            if (e.Depth > 0) ImGui::Indent(14.0f * (float)e.Depth);
            ImGui::TextUnformatted(e.Name.c_str());
            if (e.Depth > 0) ImGui::Unindent(14.0f * (float)e.Depth);

            ImGui::TableSetColumnIndex(1);
            MsCell(e.CpuMs, peakCpu, true);
            ImGui::TableSetColumnIndex(2);
            MsCell(e.GpuMs, peakGpu, gpu);
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::TextDisabled("%s", T("Узкое место — максимум из CPU и GPU, а не их сумма:\n"
                                "процессор собирает следующий кадр, пока видеокарта рисует прошлый."));

    ImGui::End();
}

} // namespace d3d
