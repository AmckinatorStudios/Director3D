#pragma once
#include <string>

#include "ui/DirectorHost.h"

namespace d3d {

// Модальные окна инструмента: новый проект, открыть/сохранить, импорт,
// настройки рендера и таймлайна, справка. Панель владеет их состоянием
// (введённые пути, выбранные параметры) и открывается через host.OpenDialog.
//
// Путь всегда можно ввести строкой, а рядом стоит кнопка «Обзор…», открывающая
// НАСТОЯЩИЙ системный диалог (ui/FileDialog). Поле с проверкой существования
// остаётся основой: оно работает даже там, где системного диалога нет, и
// показывает проблему до нажатия кнопки, а не в виде ошибки после.
//
// Здесь же живут два окна, которые открывает не человек, а ход дела: прогресс
// идущего рендера и его итог.
class DialogsPanel {
public:
    void Draw(DirectorHost& host);
    void Open(Dialog dialog);
    bool AnyOpen() const { return m_active != Dialog::None; }

private:
    // Ход рендера и его итог — не из перечисления Dialog: их открывает не
    // человек, а состояние экспортёра, и закрывать их по Esc нельзя.
    void DrawRenderProgress(DirectorHost& host);
    void DrawRenderOutcome(DirectorHost& host);

    void DrawNewProject(DirectorHost& host);
    void DrawOpenProject(DirectorHost& host);
    void DrawSaveProjectAs(DirectorHost& host);
    void DrawImportAsset(DirectorHost& host);
    void DrawExportScene(DirectorHost& host);
    void DrawExportGltf(DirectorHost& host);
    void DrawRenderSettings(DirectorHost& host);
    void DrawTimelineSettings(DirectorHost& host);
    void DrawAbout(DirectorHost& host);
    void DrawShortcuts(DirectorHost& host);

    // Общая шапка модалки + кнопка закрытия. Возвращает false, если окно закрыли.
    bool BeginModal(const char* title);
    void Close();

    Dialog m_active = Dialog::None;
    Dialog m_requested = Dialog::None; // OpenPopup можно звать только внутри кадра
    std::string m_error;

    char m_projectName[96] = "My_Animation_Project";
    char m_path[512] = "";
    char m_renderDir[512] = "render";
    char m_renderName[96] = "frame";
    // Модалка прогресса открывается один раз на рендер: OpenPopup можно звать
    // только когда попапа ещё нет, иначе ImGui каждый кадр сбрасывает его
    // состояние.
    bool m_progressOpen = false;
    bool m_outcomeOpen = false;
};

} // namespace d3d
