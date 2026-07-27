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

// Выбранная кость персонажа.
//
// Кость выбирается ОТДЕЛЬНО от объекта и живёт рядом с ним, а не вместо: сам
// персонаж остаётся выбранным (инспектор показывает его трансформ, дерево
// подсвечивает строку), а кость — это выбор внутри него. Так же устроены все
// программы анимации: выйти из режима правки скелета не значит потерять
// персонажа.
struct BoneSelection {
    int EntityId = -1; // сущность с анимированной моделью
    int Joint = -1;    // индекс кости в её скелете

    bool Valid() const { return EntityId >= 0 && Joint >= 0; }
    bool Is(int entityId, int joint) const { return EntityId == entityId && Joint == joint; }
    void Clear() { EntityId = -1; Joint = -1; }
};

// Модальные окна, которые умеет показывать DialogsPanel.
enum class Dialog { None, NewProject, OpenProject, SaveProjectAs, ImportAsset, ExportScene,
                    ExportGltf,
                    RenderSettings, TimelineSettings, About, Shortcuts };

// Итог последнего рендера — держится, пока человек его не закроет.
//
// Почему не строкой в статус-баре, как раньше: сообщение там живёт пять секунд,
// а рендер идёт минуты. Человек уходит с чашкой чая и возвращается к окну, из
// которого уже всё исчезло — «то ли получилось, то ли нет, и где искать файл».
// Итог обязан дождаться того, кто его заказывал.
struct RenderOutcome {
    bool Shown = false;   // есть что показать
    bool Ok = false;
    std::string Path;     // файл ролика или каталог секвенции
    std::string Message;  // причина, если Ok == false
    int Frames = 0;
    float Seconds = 0.0f;
};

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

    // --- Кости персонажа ---------------------------------------------------
    // Выбранная кость (см. BoneSelection). Пустая — работаем с объектом целиком.
    virtual const BoneSelection& SelectedBone() const = 0;
    // Выбрать кость. joint < 0 — снять выбор кости, оставив объект выбранным.
    virtual void SelectBone(int entityId, int joint) = 0;
    // Ставит ключи на всю позу выбранной кости (перенос+поворот+масштаб).
    // Возвращает число затронутых дорожек; 0 — кости нет или скелет не готов.
    virtual int KeyBone() = 0;
    // Ставит ключи на всю тронутую позу персонажа целиком.
    virtual int KeyWholePose(int entityId) = 0;
    // Снимает ручную позу персонажа — он возвращается к чистому клипу.
    virtual void ResetPose(int entityId) = 0;

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
    // Экспорт анимации в glTF (.glb): движение объектов, камер и костей в
    // формате, который читают Blender, игровые движки и браузер.
    virtual bool ExportAnimationToGltf(const std::filesystem::path& path, std::string& err) = 0;
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
    // Активная камера, выбранная РУКАМИ.
    virtual int ActiveCameraId() const = 0;
    virtual void SetActiveCameraId(int id) = 0;
    // Камера, которой кадр снимается ПРЯМО СЕЙЧАС: монтаж камер, если на этом
    // времени есть склейка, иначе ручной выбор. Именно её берут Render View,
    // рамка кадра и экспорт — всё, что показывает или пишет готовый кадр.
    // Списки «сделать активной» работают с ActiveCameraId: там речь о выборе
    // человека, а не о том, что видно в кадре.
    virtual int EffectiveCameraId() const = 0;

    // --- Режим и обратная связь --------------------------------------------
    // Простой режим прячает всё, что нужно раз в месяц (граф кривых, дорожки
    // ключей, продвинутые параметры), оставляя понятный минимум.
    virtual bool& SimpleMode() = 0;
    // Видимость окна профилировщика. Ссылка, а не «показать/скрыть»: меню
    // рисует галочку по тому же значению, которым окно себя закрывает крестиком,
    // и рассинхронизировать их нечем.
    virtual bool& ShowProfiler() = 0;
    virtual void SetStatus(const std::string& message) = 0;
    virtual const std::string& Status() const = 0;

    // --- Диалоги -----------------------------------------------------------
    virtual void OpenDialog(Dialog dialog) = 0;

    // --- Рендер секвенции ---------------------------------------------------
    virtual SequenceExporter& Exporter() = 0;
    virtual SequenceExporter::Settings& RenderSettings() = 0;
    // Очередь заданий рендера и её запуск. Очередь живёт у приложения, а не у
    // диалога: она переживает закрытие окна настроек и продолжает идти.
    virtual RenderQueue& Queue() = 0;
    virtual void StartQueue() = 0;
    virtual void StartRender() = 0;
    // Останавливает рендер и очередь и записывает итог. Не Exporter().Cancel():
    // тот только гасит экспортёр, оставляя очередь запускать следующее задание,
    // головку — на случайном кадре, а человека — без ответа, что произошло.
    virtual void CancelRender() = 0;
    // Итог последнего рендера: что записалось, куда и за сколько.
    virtual RenderOutcome& LastRender() = 0;

    // --- Панель ассетов ------------------------------------------------------
    virtual std::filesystem::path& AssetsDir() = 0;
    virtual const std::filesystem::path& SelectedAsset() const = 0;
    virtual void SetSelectedAsset(const std::filesystem::path& path) = 0;

    // --- Прочее -------------------------------------------------------------
    virtual void RequestQuit() = 0;
};

} // namespace d3d
