#pragma once
#include <memory>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include "sage/ecs/RenderBatch.h"
#include "sage/render/Camera.h"
#include "sage/render/DebugDraw.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/GridRenderer.h"
#include "sage/render/ParticleSystem.h"
#include "sage/render/PostFX.h"
#include "sage/render/ShadowMap.h"
#include "sage/render/SkyRenderer.h"
#include "sage/scene/Light.h"
#include "sage/scene/Scene.h"

namespace d3d {

// Что показывает вьюпорт (выпадающий список «Shaded» в референсе).
enum class ShadingMode { Shaded, Wireframe, Unlit, Normals };

// Откуда смотрим (выпадающий список «Perspective»).
enum class ViewPreset { Perspective, Front, Side, Top, SceneCamera };

// Вспомогательная графика вьюпорта — переключатели рядом со списком вида.
struct ViewportOverlays {
    bool Grid = true;
    // Настройки сетки живут рядом с флагом её показа: это один и тот же
    // инструмент вида. Шаг клетки заодно задаёт привязку гизмо — иначе объект
    // прилипал бы не к тем линиям, которые видит аниматор.
    sage::render::GridSettings GridConfig;
    bool Gizmos = true;         // каркасы камер/светов/эмиттеров
    bool CameraFrame = true;    // рамка кадра активной камеры
    bool SafeArea = true;       // безопасная зона внутри рамки
    bool Thirds = false;        // сетка третей для композиции кадра
    bool Outline = true;        // подсветка выделенного объекта
    bool Skeleton = true;       // кости персонажа поверх картинки (выбор и поза)
};

// ---------------------------------------------------------------------------
// StageRenderer — весь превью-рендер Director 3D.
//
// Рисует ДВА независимых кадра в offscreen-буферы, которые панели показывают
// как обычные картинки ImGui:
//   • Stage       — рабочий вьюпорт: свободная камера, сетка, гизмо, рамка кадра;
//   • Render View — «чистовой» кадр от активной камеры сцены, без служебной
//                   графики: то, что уйдёт в файл при экспорте секвенции.
// Оба кадра идут через один проход теней и одну и ту же цепочку освещения,
// поэтому картинка во вьюпорте и в рендере совпадает по свету, а не «похожа».
//
// Вся тяжёлая работа — движковая: инстансный батч со отсечением по фрустуму
// (sage::ecs::RenderBatch), карта теней солнца, скиннинг скелетных моделей,
// частицы, пост-обработка. Инструмент добавляет только своё: скрытие объектов
// по глазку в аутлайнере, композиционные направляющие и подсветку выделения.
// ---------------------------------------------------------------------------
class StageRenderer {
public:
    void Init();

    void SetStageSize(int w, int h);
    void SetRenderViewSize(int w, int h);
    int StageWidth() const { return m_stageW; }
    int StageHeight() const { return m_stageH; }

    // Общий для обоих кадров depth-проход солнца. Зовётся раз в кадр до Render*.
    void RenderShadow(Scene& scene, const LightingEnvironment& env);

    // Рабочий вьюпорт. camera — свободная камера инструмента; при preset ==
    // SceneCamera вместо неё берётся кадр активной камеры сцены (cameraEntityId).
    // Возвращает через outView/outProj фактические матрицы — они нужны панели
    // для ImGuizmo и для пикинга мышью.
    void RenderStage(Scene& scene, Camera& camera, const LightingEnvironment& env,
                     ShadingMode shading, ViewPreset preset, int sceneCameraId,
                     const ViewportOverlays& overlays, const std::vector<int>& selection,
                     glm::mat4& outView, glm::mat4& outProj);

    // Чистовой кадр от камеры сцены. false — камеры нет (панель покажет подсказку).
    bool RenderCameraView(Scene& scene, const LightingEnvironment& env, int cameraEntityId);

    unsigned int StageTexture() const;
    unsigned int RenderViewTexture() const;

    // Кадр от камеры сцены в ПРОИЗВОЛЬНЫЙ буфер заданного размера — этим
    // экспорт секвенции получает кадр нужного разрешения, не трогая размер
    // панели. Проходит ТУ ЖЕ пост-обработку, что и Render View, поэтому файл
    // совпадает с превью. Возвращает false, если камеры нет.
    // jitterPixels — сдвиг проекции на доли пикселя. Нужен НАКОПИТЕЛЬНОМУ
    // сглаживанию: чистовой кадр не обязан рисоваться за 16 мс, поэтому его
    // можно снять несколько раз с микросмещениями и усреднить. Это честный
    // сверхсэмплинг — он убирает не только лесенку на кромках, но и мерцание
    // тонких деталей и шум шейдеров, чего экранный FXAA не умеет в принципе.
    bool RenderToTarget(Scene& scene, const LightingEnvironment& env, int cameraEntityId,
                        Framebuffer& target, glm::vec2 jitterPixels = glm::vec2(0.0f));

    // Рвёт непрерывность движения для motion blur: вызывается при скачке
    // головки таймлайна, смене сцены и загрузке проекта.
    void ResetMotionHistory();

    // Закрывает шаг времени сцены: положение объектов запоминается как
    // «прошлое» для смаза движения. Зовётся РОВНО ОДИН РАЗ за шаг, после того
    // как отрисованы все виды (вьюпорт, Render View) или записан кадр экспорта.
    void EndTimeStep() { m_batch.AdvanceVelocityHistory(); }

    const sage::ecs::RenderStats& LastStats() const { return m_stats; }
    ParticleSystem& Particles() { return *m_particles; }

    // Матрицы кадра камеры сцены (для рамки кадра во вьюпорте и для пикинга в
    // режиме «смотрим камерой»). HasCamera == false — камеры нет.
    struct CameraFrameInfo {
        glm::mat4 View{1.0f};
        glm::mat4 Proj{1.0f};
        glm::vec3 Position{0.0f};
        float Fov = 60.0f;
        float Aspect = 1.777f;
        bool HasCamera = false;
    };
    static CameraFrameInfo CameraFrameOf(Scene& scene, int cameraEntityId, float aspect);

private:
    // ------------------------------------------------------------------------
    // Описание кадра: ЧТО в него входит и куда он пишется.
    //
    // Последовательность проходов — небо, геометрия, сетка, служебная графика,
    // пост-обработка, подсветка выделения — задана ОДИН раз, в RenderFrame.
    // Рабочий вьюпорт, Render View и экспорт отличаются только этим описанием.
    //
    // Это не абстракция ради абстракции: раньше последовательность стояла в двух
    // местах (превью камеры и экспорт), и любой новый проход надо было
    // дописывать в оба, помня, что в третьем — вьюпорте — он идёт вперемешку со
    // служебной графикой. Теперь новый проход добавляется одной строкой в
    // RenderFrame и одним полем здесь.
    //
    // Указатели, а не флаги: nullptr значит «этого прохода в кадре нет», и
    // выключить проход нельзя, забыв заполнить его данные.
    struct FrameDesc {
        glm::mat4 View{1.0f};
        glm::mat4 Proj{1.0f};
        glm::vec3 ViewPos{0.0f};

        Framebuffer* Hdr = nullptr;      // куда рисуется сцена (обязателен)
        Framebuffer* Output = nullptr;   // куда пишет пост-обработка
        sage::render::PostFX* Fx = nullptr; // nullptr — кадр без пост-обработки
        sage::render::PostFXSettings FxSettings;

        ShadingMode Shading = ShadingMode::Shaded;
        glm::vec4 ClearColor{0.106f, 0.114f, 0.133f, 1.0f};
        bool Sky = true;

        // Смаз движения от ОБЪЕКТОВ, а не только от камеры. Требует отдельного
        // прохода геометрии в буфер скоростей, поэтому включается только там,
        // где смаз реально нужен, — иначе за него платили бы все кадры.
        bool Velocity = false;
        // Матрица камеры на прошлом шаге ЭТОГО потока кадров. Своя у вьюпорта,
        // Render View и экспорта — RenderFrame читает её и записывает новую.
        glm::mat4* PrevViewProj = nullptr;

        // Служебная графика рабочего вида. Всё nullptr — чистовой кадр.
        const sage::render::GridSettings* Grid = nullptr;
        const ViewportOverlays* Helpers = nullptr;
        const std::vector<int>* Outline = nullptr;
        float HelperAspect = 1.777f;
    };

    // Единственное место, где записана последовательность проходов кадра.
    // Возвращает буфер, в котором лежит результат (Hdr или Output).
    Framebuffer& RenderFrame(Scene& scene, const LightingEnvironment& env, const FrameDesc& desc);

    void DrawScene(Scene& scene, const LightingEnvironment& env, const glm::mat4& view,
                   const glm::mat4& proj, const glm::vec3& viewPos, ShadingMode shading);
    // Небо кадра: кубическая текстура из каталога сцены, если он задан, иначе
    // процедурный градиент. Одна точка на вьюпорт, Render View и экспорт.
    void DrawSky(const LightingEnvironment& env, const glm::mat4& view, const glm::mat4& proj);
    // Дистанция автофокуса: до ближайшего объекта под центром кадра. Оператору
    // достаточно навести камеру, а не подбирать метры руками.
    static float AutoFocusDistance(Scene& scene, const CameraFrameInfo& frame);
    void DrawHelpers(Scene& scene, const ViewportOverlays& overlays,
                     const std::vector<int>& selection, float cameraAspect);
    void RenderOutline(Scene& scene, const std::vector<int>& selection,
                       const glm::mat4& view, const glm::mat4& proj, Framebuffer& target);
    // Настройки пост-обработки с киношной камеры. frame нужен автофокусу:
    // дистанция берётся из того, куда камера реально смотрит.
    //
    // cinematic == false отключает оптику камеры (глубину резкости, смаз
    // движения, хроматическую аберрацию), оставляя только тон-маппинг и грейд.
    // Так рисуется РАБОЧИЙ вьюпорт: он смотрит свободной камерой, у которой
    // своя позиция, и чужая дистанция фокуса замылила бы всё рабочее поле. В
    // любом 3D-пакете рабочий вид остаётся резким, а оптика видна там, где её и
    // оценивают, — в кадре камеры и в рендере.
    static sage::render::PostFXSettings PostFXOf(Scene& scene, int cameraEntityId,
                                                 const CameraFrameInfo& frame, bool cinematic);

    std::optional<ShadowMap> m_shadows;
    std::optional<Framebuffer> m_stageFbo, m_stagePostFbo;
    std::optional<Framebuffer> m_viewFbo, m_viewPostFbo;
    std::optional<Framebuffer> m_outlineMask;
    // Буфер и пост-обработка экспорта: создаются при первом рендере секвенции,
    // размер задаёт настройка экспорта, а не размер панели.
    // Буфер экранных скоростей. Один на все кадры: проходы идут по очереди, а
    // размер подгоняется под текущий кадр перед использованием.
    std::optional<Framebuffer> m_velocityFbo;
    // Прошлая матрица камеры отдельно у каждого потока кадров (см. FrameDesc).
    glm::mat4 m_prevViewProjStage{1.0f}, m_prevViewProjView{1.0f}, m_prevViewProjExport{1.0f};
    std::optional<Framebuffer> m_exportFbo;
    std::optional<sage::render::PostFX> m_exportPostfx;
    std::optional<sage::render::PostFX> m_stagePostfx, m_viewPostfx;
    std::optional<DebugDraw> m_debug;
    sage::render::GridRenderer m_grid;
    std::optional<SkyRenderer> m_sky;
    std::optional<ParticleSystem> m_particles;
    std::unique_ptr<sage::rhi::Geometry> m_fullscreenTri;
    sage::ecs::RenderBatch m_batch;
    sage::ecs::RenderStats m_stats;

    bool m_stagePostApplied = false;
    bool m_viewPostApplied = false;
    int m_stageW = 1280, m_stageH = 720;
    int m_viewW = 1280, m_viewH = 720;
};

// ---------------------------------------------------------------------------
// HiddenObjects — RAII-скрытие объектов на время отрисовки.
//
// Глазок в аутлайнере — свойство ИНСТРУМЕНТА (StageItemComponent), а движковый
// RenderBatch обходит сцену сам и про него не знает. Вместо того чтобы тащить
// понятие видимости в движок (в игре его нет — там объект либо есть, либо нет),
// на время кадра у скрытых сущностей забирается GPU-меш: батч их просто не
// увидит, как не видит сущность без назначенного меша. Деструктор возвращает
// мешы на место, поэтому исключение или ранний выход не оставят сцену калекой.
// ---------------------------------------------------------------------------
class HiddenObjects {
public:
    explicit HiddenObjects(Scene& scene);
    ~HiddenObjects();

    HiddenObjects(const HiddenObjects&) = delete;
    HiddenObjects& operator=(const HiddenObjects&) = delete;

    // Есть ли вообще что-то скрытое (позволяет пропустить лишнюю работу).
    bool Any() const { return !m_meshes.empty() || !m_animated.empty(); }

    // Скрыта ли сущность. Спрашивают и сбор освещения (глазок гасит лампу, а не
    // только её маркер), и отрисовка служебной графики.
    static bool IsHidden(Scene& scene, entt::entity e);

private:
    // Что забрали на время кадра и вернём в деструкторе.
    struct StashedMesh { entt::entity Entity; std::shared_ptr<Mesh> Mesh_; };
    struct StashedModel {
        entt::entity Entity;
        std::shared_ptr<sage::render::SkinnedModel> Model;
        bool WasPlaying = false;
    };

    Scene& m_scene;
    std::vector<StashedMesh> m_meshes;
    std::vector<StashedModel> m_animated;
};

// Освещение кадра с учётом погашенных глазком источников. Обёртка над
// sage::ecs::CollectLighting — та собирает ВСЕ света сцены.
LightingEnvironment CollectVisibleLighting(Scene& scene);

} // namespace d3d
