#pragma once
#include <string>

#include "ui/DirectorHost.h"

namespace d3d {

// Модальные окна инструмента: новый проект, открыть/сохранить, импорт,
// настройки рендера и таймлайна, справка. Панель владеет их состоянием
// (введённые пути, выбранные параметры) и открывается через host.OpenDialog.
//
// Файловые диалоги — свои, а не системные: системный требует платформенной
// библиотеки на каждой ОС, а инструменту достаточно поля пути с проверкой
// существования и подсказкой о том, что не так.
class DialogsPanel {
public:
    void Draw(DirectorHost& host);
    void Open(Dialog dialog);
    bool AnyOpen() const { return m_active != Dialog::None; }

private:
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
};

} // namespace d3d
