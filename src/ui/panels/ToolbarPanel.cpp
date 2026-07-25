#include "ui/panels/ToolbarPanel.h"

// imgui.h — ПЕРВЫМ: ImGuizmo.h пользуется его типами (ImVec2/ImU32/ImDrawList)
// и сам их не подключает.
#include "imgui.h"
#include "ImGuizmo.h"

#include "ui/DirectorHost.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace d3d {

using Icons::Icon;

void ToolbarPanel::Draw(DirectorHost& host) {
    const bool hasSelection = host.SelectedId() >= 0;
    const bool exporting = host.Exporter().Active();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3, 0));

    // --- Файл ---
    if (Icons::ToolbarButton(Icon::New, "New", "Новый проект (Ctrl+N)")) host.OpenDialog(Dialog::NewProject);
    ImGui::SameLine();
    if (Icons::ToolbarButton(Icon::Open, "Open", "Открыть проект (Ctrl+O)")) host.OpenDialog(Dialog::OpenProject);
    ImGui::SameLine();
    if (Icons::ToolbarButton(Icon::Save, "Save", "Сохранить проект (Ctrl+S)")) {
        if (host.ProjectPath().empty()) {
            host.OpenDialog(Dialog::SaveProjectAs);
        } else {
            std::string err;
            if (!host.SaveProject(host.ProjectPath(), err)) host.SetStatus("Не сохранилось: " + err);
        }
    }
    Icons::ToolbarSeparator();

    // --- Обмен ---
    if (Icons::ToolbarButton(Icon::Import, "Import", "Импортировать модель или звук (Ctrl+I)"))
        host.OpenDialog(Dialog::ImportAsset);
    ImGui::SameLine();
    if (Icons::ToolbarButton(Icon::Export, "Export", "Экспортировать сцену в формат движка (.sage)"))
        host.OpenDialog(Dialog::ExportScene);
    Icons::ToolbarSeparator();

    // --- Отмена ---
    if (Icons::ToolbarButton(Icon::Undo, "Undo", "Отменить (Ctrl+Z)", false, host.CanUndo())) host.Undo();
    ImGui::SameLine();
    if (Icons::ToolbarButton(Icon::Redo, "Redo", "Повторить (Ctrl+Y)", false, host.CanRedo())) host.Redo();
    Icons::ToolbarSeparator();

    // --- Инструменты манипуляции ---
    // Значения — операции ImGuizmo; 0 означает «только выбор, без манипулятора».
    int& op = host.GizmoOp();
    if (Icons::ToolbarButton(Icon::Select, "Select", "Выбор (Q)", op == 0)) op = 0;
    ImGui::SameLine();
    if (Icons::ToolbarButton(Icon::Move, "Move", "Перемещение (W)", op == (int)ImGuizmo::TRANSLATE))
        op = (int)ImGuizmo::TRANSLATE;
    ImGui::SameLine();
    if (Icons::ToolbarButton(Icon::Rotate, "Rotate", "Поворот (E)", op == (int)ImGuizmo::ROTATE))
        op = (int)ImGuizmo::ROTATE;
    ImGui::SameLine();
    if (Icons::ToolbarButton(Icon::Scale, "Scale", "Масштаб (R)", op == (int)ImGuizmo::SCALE))
        op = (int)ImGuizmo::SCALE;
    Icons::ToolbarSeparator();

    // --- Быстрое создание ---
    if (Icons::ToolbarButton(Icon::Camera, "Camera", "Добавить камеру")) host.Create(CreateKind::Camera);
    ImGui::SameLine();
    if (Icons::ToolbarButton(Icon::Light, "Light", "Добавить источник света")) host.Create(CreateKind::PointLight);
    ImGui::SameLine();
    if (Icons::ToolbarButton(Icon::Cube, "Object", "Добавить объект (куб)")) host.Create(CreateKind::Cube);
    Icons::ToolbarSeparator();

    // --- Транспорт и рендер ---
    Playback& transport = host.Transport();
    if (Icons::ToolbarButton(transport.Playing() ? Icon::Pause : Icon::Play,
                             transport.Playing() ? "Pause" : "Play",
                             "Проигрывание (Пробел)", transport.Playing())) {
        transport.TogglePlay();
    }
    ImGui::SameLine();
    if (Icons::ToolbarButton(Icon::Stop, "Stop", "Стоп и в начало (Shift+Пробел)", false, transport.Playing() || host.CurrentTime() > 0.0f)) {
        transport.Stop();
        host.SetCurrentTime(0.0f);
    }
    ImGui::SameLine();
    if (Icons::ToolbarButton(Icon::Record, "Auto Key",
                             "Авто-ключ: любая правка объекта сама ставит ключ (Ctrl+K)",
                             host.AutoKey())) {
        host.AutoKey() = !host.AutoKey();
        host.SetStatus(host.AutoKey() ? "Авто-ключ включён" : "Авто-ключ выключен");
    }
    ImGui::SameLine();
    if (Icons::ToolbarButton(Icon::Key, "Key", "Поставить ключ выбранному (K)", false, hasSelection)) {
        const int keyed = host.KeySelected();
        host.SetStatus(keyed > 0 ? "Ключи поставлены" : "Нечего ключить: сначала добавьте дорожку");
    }
    ImGui::SameLine();
    if (Icons::ToolbarButton(Icon::Render, "Render", "Отрендерить секвенцию кадров (F12)", exporting, !exporting))
        host.StartRender();

    // --- Переключатель режима (прижат вправо, как в референсе) ---
    // Отступ задаём АБСОЛЮТНОЙ координатой от начала строки, а не «пробелом»
    // после последней кнопки: SameLine(0, spacing) отсчитывает от конца
    // предыдущего элемента, и переключатель уезжал бы за край окна.
    const float switchWidth = 210.0f;
    const float rowWidth = ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
    const float lastX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    if (rowWidth - switchWidth > lastX + 12.0f) {
        ImGui::SameLine(rowWidth - switchWidth);
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(1.0f, (Theme::kToolbarHeight - 34.0f) * 0.5f));

        bool& simple = host.SimpleMode();
        ImGui::TextColored(simple ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
                                  : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                           "Simple Mode");
        ImGui::SameLine();

        // Тумблер: слева «просто», справа «всё». Рисуем руками — штатный
        // Checkbox не читается как переключатель двух режимов.
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 size(38.0f, 18.0f);
        if (ImGui::InvisibleButton("##mode", size)) simple = !simple;
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 track = simple ? Theme::Colors::PanelRaised : Theme::Colors::Accent;
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), track, size.y * 0.5f);
        if (hovered) {
            dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), Theme::Colors::Text, size.y * 0.5f);
        }
        const float knobX = simple ? pos.x + size.y * 0.5f + 1.0f : pos.x + size.x - size.y * 0.5f - 1.0f;
        dl->AddCircleFilled(ImVec2(knobX, pos.y + size.y * 0.5f), size.y * 0.5f - 2.0f,
                            IM_COL32(240, 240, 240, 255), 16);
        if (hovered) {
            ImGui::SetTooltip("Простой режим прячет редактор кривых, ключевые дорожки\n"
                              "и тонкие параметры. Продвинутый показывает всё.");
        }

        ImGui::SameLine();
        ImGui::TextColored(simple ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
                                  : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                           "Advanced");
        ImGui::EndGroup();
    }

    ImGui::PopStyleVar();
}

} // namespace d3d
