#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "anim/AnimationDocument.h"
#include "anim/Playback.h"
#include "render/SequenceExporter.h"
#include "render/StageRenderer.h"
#include "sage/render/Camera.h"
#include "sage/scene/Scene.h"

namespace d3d {

// Что создаёт меню Create / кнопки тулбара.
enum class CreateKind { Camera, PointLight, SpotLight, Cube, Sphere, Plane, Cylinder, Cone,
                        Character, ParticleEffect, Group };

// Пространство манипулятора: оси объекта или оси мира.
enum class GizmoSpace { Local, World };

// Модальные окна, которые умеет показывать DialogsPanel.
enum class Dialog { None, NewProject, OpenProject, SaveProjectAs, ImportAsset, ExportScene,
                    RenderSettings, TimelineSettings, About, Shortcuts };

// ---------------------------------------------------------------------------
// DirectorHost — контракт, через который панели интерфейса разговаривают с
// приложением.
//
// Каждая панель (ui/panels/*) — самостоятельный класс, который владеет только
// СВОИМ состоянием (текст поиска, что перетаскивают мышью, какая вкладка
// открыта) и не знает ни про DirectorLayer, ни про соседние панели. Всё общее —
// сцена, документ, выбор, undo, рендер — приходит через этот интерфейс.
//
// Так панель можно читать и править отдельно от остальных двадцати тысяч строк,
// а сам DirectorLayer не превращается в класс-свалку, где таймлайн лезет в поля
// инспектора. Тот же приём, что и у EditorHost в редакторе SAGE.
// ---------------------------------------------------------------------------
class DirectorHost {
public:
    virtual ~DirectorHost() = default;

    // --- Сцена и выбор -----------------------------------------------------
    virtual Scene& CurrentScene() = 0;
    // Первичная выбранная сущность: под неё инспектор и пивот гизмо. -1 — пусто.
    virtual int SelectedId() const = 0;
    virtual void SetSelectedId(int id) = 0;
    virtual GameObject SelectedObject() = 0;
    virtual const std::vector<int>& Selection() const = 0;
    virtual bool IsSelected(int id) const = 0;
    virtual void ToggleSelection(int id) = 0; // Ctrl-клик
    virtual void ClearSelection() = 0;

    // --- Анимация ----------------------------------------------------------
    virtual AnimationDocument& Document() = 0;
    virtual Playback& Transport() = 0;
    // Текущее время головки. SetTime прилипает к сетке кадров и СРАЗУ применяет
    // документ к сцене — всё, что двигает головку, идёт через него.
    virtual float CurrentTime() const = 0;
    virtual void SetCurrentTime(float seconds) = 0;
    virtual void StepFrames(int frames) = 0;

    // Ставит ключ свойства выбранного объекта на текущем времени. Возвращает
    // false, если свойство неприменимо.
    virtual bool KeyProperty(int entityId, Property prop) = 0;
    // Ключ по всем свойствам, у которых уже есть дорожка (кнопка «Key»).
    virtual int KeySelected() = 0;
    // Авто-ключ: правка объекта сама ставит ключи (кнопка записи в референсе).
    virtual bool& AutoKey() = 0;
    // Вызывается всеми, кто изменил объект: при включённом авто-ключе ставит
    // ключи, иначе ничего не делает.
    virtual void NotifyObjectEdited(int entityId) = 0;

    // --- Отмена/повтор -----------------------------------------------------
    virtual void PushUndo() = 0;      // перед разовой правкой
    virtual void CaptureUndo() = 0;   // начало «размазанной» правки (жест мышью)
    virtual void CommitUndo() = 0;    // факт изменения внутри жеста
    // Обёртка над Capture/Commit для только что нарисованного виджета ImGui:
    // зовётся сразу после DragFloat/InputText и сама разбирается с активацией.
    virtual void TrackLastItem() = 0;
    virtual bool CanUndo() const = 0;
    virtual bool CanRedo() const = 0;
    virtual void Undo() = 0;
    virtual void Redo() = 0;

    // --- Объекты сцены -----------------------------------------------------
    virtual int Create(CreateKind kind) = 0;   // возвращает id созданного объекта
    virtual void DeleteSelected() = 0;
    virtual void DuplicateSelected() = 0;
    virtual void RenameObject(int id, const std::string& name) = 0;
    virtual void SetParentOf(int childId, int parentId) = 0; // parentId < 0 — в корень
    virtual void FocusOnSelected() = 0;        // подвести камеру к объекту
    // Импорт модели (.glb/.gltf — скелетная, .obj — статическая). -1 при ошибке.
    virtual int ImportAsset(const std::filesystem::path& path) = 0;

    // --- Файлы проекта -----------------------------------------------------
    virtual void NewProject() = 0;
    virtual bool OpenProject(const std::filesystem::path& path, std::string& err) = 0;
    virtual bool SaveProject(const std::filesystem::path& path, std::string& err) = 0;
    virtual bool ExportSceneToEngine(const std::filesystem::path& path, std::string& err) = 0;
    virtual const std::filesystem::path& ProjectPath() const = 0;
    virtual bool Dirty() const = 0;

    // --- Вид ---------------------------------------------------------------
    virtual Camera& ViewCamera() = 0;
    virtual StageRenderer& Renderer() = 0;
    virtual ShadingMode& Shading() = 0;
    virtual ViewPreset& Preset() = 0;
    virtual ViewportOverlays& Overlays() = 0;
    virtual int& GizmoOp() = 0;            // значение ImGuizmo::OPERATION
    virtual bool& GizmoSnap() = 0;
    virtual GizmoSpace& GizmoSpaceRef() = 0;
    virtual const glm::mat4& ViewMatrix() const = 0;
    virtual const glm::mat4& ProjMatrix() const = 0;
    virtual void SetStageSize(int w, int h) = 0;
    virtual void SetRenderViewSize(int w, int h) = 0;
    // u,v в [0..1] по вьюпорту. additive (Ctrl) — добавить/убрать из набора.
    virtual void PickAtStage(float u, float v, bool additive) = 0;
    // Активная камера: с неё считается Render View и экспорт секвенции.
    virtual int ActiveCameraId() const = 0;
    virtual void SetActiveCameraId(int id) = 0;

    // --- Режим и обратная связь --------------------------------------------
    // Простой режим прячает всё, что нужно раз в месяц (граф кривых, дорожки
    // ключей, продвинутые параметры), оставляя понятный минимум.
    virtual bool& SimpleMode() = 0;
    virtual void SetStatus(const std::string& message) = 0;
    virtual const std::string& Status() const = 0;

    // --- Диалоги -----------------------------------------------------------
    virtual void OpenDialog(Dialog dialog) = 0;

    // --- Рендер секвенции ---------------------------------------------------
    virtual SequenceExporter& Exporter() = 0;
    virtual SequenceExporter::Settings& RenderSettings() = 0;
    virtual void StartRender() = 0;

    // --- Панель ассетов ------------------------------------------------------
    virtual std::filesystem::path& AssetsDir() = 0;
    virtual const std::filesystem::path& SelectedAsset() const = 0;
    virtual void SetSelectedAsset(const std::filesystem::path& path) = 0;

    // --- Прочее -------------------------------------------------------------
    virtual void RequestQuit() = 0;
};

} // namespace d3d
