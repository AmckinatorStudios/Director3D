#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "anim/AnimationDocument.h"
#include "anim/Playback.h"
#include "project/UndoStack.h"
#include "render/SequenceExporter.h"
#include "render/StageRenderer.h"
#include "sage/core/Layer.h"
#include "sage/audio/AudioEngine.h"
#include "sage/render/Camera.h"
#include "sage/scene/Scene.h"
#include "ui/DirectorHost.h"
#include "ui/panels/AssetsPanel.h"
#include "ui/panels/DialogsPanel.h"
#include "ui/panels/MenuBarPanel.h"
#include "ui/panels/PropertiesPanel.h"
#include "ui/panels/ScenePanel.h"
#include "ui/panels/StagePanel.h"
#include "ui/panels/StatusBarPanel.h"
#include "ui/panels/TimelinePanel.h"
#include "ui/panels/ToolbarPanel.h"
#include "ui/panels/WorldPanel.h"

namespace d3d {

// ---------------------------------------------------------------------------
// DirectorLayer — приложение Director 3D как СЛОЙ движка.
//
// SAGE водит слои в главном цикле (OnUpdate → OnRender), а владение
// окном, контекстом и таймингом остаётся у Application. Поэтому здесь нет ни
// своего цикла, ни своей обработки ошибок — только состояние инструмента и
// оркестровка панелей.
//
// Слой реализует DirectorHost: панели видят ровно тот интерфейс, а не этот
// класс. Внутри лежит всё, что действительно общее для инструмента: сцена,
// документ анимации, транспорт, выбор, отмена, превью-рендер и экспорт.
// ---------------------------------------------------------------------------
class DirectorLayer : public sage::Layer, public DirectorHost {
public:
    // startupProject — файл .d3dproj, который надо открыть сразу после запуска
    // (путь из командной строки). Пусто — стартуем с новой сцены.
    explicit DirectorLayer(std::string startupProject = {});
    ~DirectorLayer() override;

    // --- Слой движка ---
    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(float dt) override;
    void OnRender() override;

    // --- DirectorHost: сцена и выбор ---
    Scene& CurrentScene() override { return *m_scene; }
    int SelectedId() const override { return m_selection.empty() ? -1 : m_selection.back(); }
    void SetSelectedId(int id) override;
    GameObject SelectedObject() override;
    const std::vector<int>& Selection() const override { return m_selection; }
    bool IsSelected(int id) const override;
    void ToggleSelection(int id) override;
    void ClearSelection() override { m_selection.clear(); m_bone.Clear(); }

    // --- DirectorHost: кости ---
    const BoneSelection& SelectedBone() const override { return m_bone; }
    void SelectBone(int entityId, int joint) override;
    int KeyBone() override;
    int KeyWholePose(int entityId) override;
    void ResetPose(int entityId) override;

    // --- DirectorHost: анимация ---
    AnimationDocument& Document() override { return m_doc; }
    Playback& Transport() override { return m_playback; }
    float CurrentTime() const override { return m_playback.Time(); }
    void SetCurrentTime(float seconds) override;
    void StepFrames(int frames) override;
    bool KeyProperty(int entityId, Property prop) override;
    int KeySelected() override;
    bool& AutoKey() override { return m_autoKey; }
    void NotifyObjectEdited(int entityId) override;

    // --- DirectorHost: отмена ---
    void PushUndo() override;
    void CaptureUndo() override;
    void CommitUndo() override;
    void TrackLastItem() override;
    bool CanUndo() const override { return m_undo.CanUndo(); }
    bool CanRedo() const override { return m_undo.CanRedo(); }
    void Undo() override;
    void Redo() override;

    // --- DirectorHost: объекты ---
    int Create(CreateKind kind) override;
    void DeleteSelected() override;
    void DuplicateSelected() override;
    void RenameObject(int id, const std::string& name) override;
    void SetParentOf(int childId, int parentId) override;
    void FocusOnSelected() override;
    int ImportAsset(const std::filesystem::path& path) override;

    // --- DirectorHost: файлы ---
    void NewProject() override;
    bool OpenProject(const std::filesystem::path& path, std::string& err) override;
    bool SaveProject(const std::filesystem::path& path, std::string& err) override;
    bool ExportSceneToEngine(const std::filesystem::path& path, std::string& err) override;
    const std::filesystem::path& ProjectPath() const override { return m_projectPath; }
    bool Dirty() const override { return m_dirty; }

    // --- DirectorHost: вид ---
    Camera& ViewCamera() override { return m_camera; }
    StageRenderer& Renderer() override { return m_renderer; }
    ShadingMode& Shading() override { return m_shading; }
    ViewPreset& Preset() override { return m_preset; }
    ViewportOverlays& Overlays() override { return m_overlays; }
    int& GizmoOp() override { return m_gizmoOp; }
    bool& GizmoSnap() override { return m_gizmoSnap; }
    GizmoSpace& GizmoSpaceRef() override { return m_gizmoSpace; }
    const glm::mat4& ViewMatrix() const override { return m_view; }
    const glm::mat4& ProjMatrix() const override { return m_proj; }
    void SetStageSize(int w, int h) override { m_renderer.SetStageSize(w, h); }
    void SetRenderViewSize(int w, int h) override { m_renderer.SetRenderViewSize(w, h); }
    void PickAtStage(float u, float v, bool additive) override;
    int ActiveCameraId() const override { return m_activeCameraId; }
    void SetActiveCameraId(int id) override { m_activeCameraId = id; }

    // --- DirectorHost: режим ---
    bool& SimpleMode() override { return m_simpleMode; }
    void SetStatus(const std::string& message) override;
    const std::string& Status() const override { return m_status; }

    // --- DirectorHost: диалоги, рендер, ассеты ---
    void OpenDialog(Dialog dialog) override { m_dialogs.Open(dialog); }
    SequenceExporter& Exporter() override { return m_exporter; }
    SequenceExporter::Settings& RenderSettings() override { return m_renderSettings; }
    void StartRender() override;
    std::filesystem::path& AssetsDir() override { return m_assetsDir; }
    const std::filesystem::path& SelectedAsset() const override { return m_selectedAsset; }
    void SetSelectedAsset(const std::filesystem::path& path) override { m_selectedAsset = path; }
    void RequestQuit() override;

private:
    // Весь интерфейс за кадр. Зовётся из OnRender ПОСЛЕ превью-рендера:
    // Application водит слои через OnUpdate/OnRender, а хук OnImGui у Layer
    // объявлен «на случай активного ImGui-фрейма» и главным циклом не
    // вызывается — фрейм ImGui открывает и закрывает тот, кто им владеет
    // (здесь — мы, как и редактор движка).
    void DrawUI();
    void BuildDefaultScene();
    // Наполняет проект демонстрационной анимацией (движение, вращение, свет,
    // цвет). Нужна и как «показать, что инструмент делает», и как основа
    // сквозной проверки: по ней прогоняется рендер секвенции.
    void BuildDemoAnimation();
    // Сквозная проверка на живом GPU: демо-анимация -> рендер нескольких
    // кадров в PNG -> сверка, что файлы появились -> выход. Включается
    // переменной D3D_SMOKE_TEST; в CI заменяет человека за монитором.
    void StartSmokeTest();
    void FinishSmokeTest();
    // Сквозная проверка ручной анимации костей на ЖИВОМ скелете и живом GL.
    // Headless-самотест сюда не достаёт: скелет приезжает вместе с моделью, а
    // она требует графического контекста. Включается D3D_BONE_TEST.
    void RunBoneCheck();
    void BuildDockLayout(unsigned int dockspaceId);
    void ApplyDocument(bool seeking);
    void HandleShortcuts();
    std::string Snapshot();
    void RestoreSnapshot(const std::string& snapshot);
    // Сбрасывает выбор от несуществующих сущностей — сцена могла быть заменена
    // загрузкой проекта или откатом.
    void ValidateSelection();

    // --- Состояние ---
    std::unique_ptr<Scene> m_scene;
    AnimationDocument m_doc;
    Playback m_playback;
    UndoStack m_undo;
    StageRenderer m_renderer;
    SequenceExporter m_exporter;
    // Звук — свой экземпляр движкового AudioEngine (движок не держит его
    // синглтоном: звуковым устройством владеет приложение). На машине без
    // звуковой карты он просто недоступен, и все Play* молча ничего не делают.
    AudioEngine m_audio;
    bool m_audioWasPlaying = false;
    SequenceExporter::Settings m_renderSettings;
    Camera m_camera;

    std::vector<int> m_selection;
    BoneSelection m_bone;
    // Костные дорожки ждут, пока движок догрузит модель, чтобы пересесть на
    // индексы костей по именам. Флаг снимается, когда ждать больше некого.
    bool m_rebindBones = false;
    // Проверка костей ждёт, пока движок догрузит модель персонажа; -1 — не идёт.
    int m_boneCheckFrames = -1;
    int m_pendingBoneSelect = -1; // кость из D3D_CHARACTER, ждущая загрузки модели
    bool m_pendingBoneKeys = false; // поставить пару ключей на неё (для снимков)
    int m_activeCameraId = -1;
    bool m_autoKey = false;
    bool m_simpleMode = true; // старт в простом режиме: инструмент должен быть понятен сразу
    bool m_dirty = false;
    bool m_dockBuilt = false;
    // Время, на которое надо вернуть головку после экспорта секвенции.
    float m_timeBeforeRender = 0.0f;

    // Автоскриншот окна (D3D_SCREENSHOT_AT_FRAME) — проверка интерфейса без
    // человека за монитором. 0 — выключен.
    int m_autoScreenshotFrame = 0;
    int m_frameCounter = 0;
    std::string m_autoScreenshotPath = "director3d.png";
    bool m_smokeTest = false;

    ShadingMode m_shading = ShadingMode::Shaded;
    ViewPreset m_preset = ViewPreset::Perspective;
    ViewportOverlays m_overlays;
    int m_gizmoOp = 7; // ImGuizmo::TRANSLATE
    bool m_gizmoSnap = false;
    GizmoSpace m_gizmoSpace = GizmoSpace::World;

    glm::mat4 m_view{1.0f};
    glm::mat4 m_proj{1.0f};

    std::filesystem::path m_projectPath;
    std::filesystem::path m_assetsDir;
    std::filesystem::path m_selectedAsset;
    std::string m_status;
    // Проект из командной строки: открывается один раз в OnAttach.
    std::string m_startupProject;
    float m_statusTimer = 0.0f;

    // --- Панели ---
    MenuBarPanel m_menuBar;
    ToolbarPanel m_toolbar;
    ScenePanel m_scenePanel;
    AssetsPanel m_assetsPanel;
    StagePanel m_stage;
    TimelinePanel m_timeline;
    PropertiesPanel m_properties;
    WorldPanel m_world;
    StatusBarPanel m_statusBar;
    DialogsPanel m_dialogs;

    bool m_imguiReady = false;
};

} // namespace d3d
