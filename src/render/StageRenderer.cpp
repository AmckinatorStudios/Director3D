#include "render/StageRenderer.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/component_wise.hpp>

#include "anim/DirectorComponents.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/core/Application.h"
#include "sage/core/Config.h"
#include "sage/core/Profiler.h"
#include "sage/ecs/CameraView.h"
#include "sage/ecs/LightSystem.h"
#include "sage/render/ResourceManager.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Components.h"

namespace d3d {

// ============================================================================
//  Скрытие объектов (глазок в аутлайнере)
// ============================================================================

bool HiddenObjects::IsHidden(Scene& scene, entt::entity e) {
    const StageItemComponent* item = scene.Registry().try_get<StageItemComponent>(e);
    return item && !item->Visible;
}

HiddenObjects::HiddenObjects(Scene& scene) : m_scene(scene) {
    auto& reg = scene.Registry();
    auto view = reg.view<StageItemComponent>();
    for (auto e : view) {
        if (view.get<StageItemComponent>(e).Visible) continue;

        // Статическая геометрия: забираем GPU-меш — для батча сущность
        // становится неотличима от той, которой меш вообще не назначали.
        if (MeshRendererComponent* mr = reg.try_get<MeshRendererComponent>(e)) {
            if (mr->MeshPtr) {
                m_meshes.push_back({e, std::move(mr->MeshPtr)});
                mr->MeshPtr.reset();
            }
        }
        // Скелетные модели идут своей веткой обхода (sage::anim::DrawAnimatedModels)
        // и проверяют Model, а не MeshPtr. Сам ассет не выгружаем — только
        // «отцепляем» на кадр: перезагрузка .glb на каждое моргание глазком
        // была бы фризом. Заодно ставим на паузу, чтобы невидимый персонаж не
        // проматывал свой клип впустую.
        if (AnimatedModelComponent* am = reg.try_get<AnimatedModelComponent>(e)) {
            if (am->Model) {
                m_animated.push_back({e, std::move(am->Model), am->Playing});
                am->Model.reset();
                am->Playing = false;
            }
        }
    }
}

HiddenObjects::~HiddenObjects() {
    auto& reg = m_scene.Registry();
    for (StashedMesh& s : m_meshes) {
        if (MeshRendererComponent* mr = reg.try_get<MeshRendererComponent>(s.Entity))
            mr->MeshPtr = std::move(s.Mesh_);
    }
    for (StashedModel& s : m_animated) {
        if (AnimatedModelComponent* am = reg.try_get<AnimatedModelComponent>(s.Entity)) {
            am->Model = std::move(s.Model);
            am->Playing = s.WasPlaying;
        }
    }
}

LightingEnvironment CollectVisibleLighting(Scene& scene) {
    // Собираем как движок, затем выбрасываем погашенные глазком источники.
    // Пересобирать с нуля дешевле, чем дублировать логику LightSystem: света
    // в кадре единицы, а совпадение с движком гарантировано.
    LightingEnvironment env = scene.Lighting;
    auto& reg = scene.Registry();
    auto view = reg.view<LightComponent, Transform>();
    for (auto e : view) {
        if (HiddenObjects::IsHidden(scene, e)) continue;
        const LightComponent& lc = view.get<LightComponent>(e);
        const glm::mat4 world = scene.WorldMatrix(e);
        const glm::vec3 wpos = glm::vec3(world[3]);

        if (lc.Kind == LightComponent::Type::Spot) {
            if ((int)env.SpotLights.size() >= LightingEnvironment::MaxSpotLights) continue;
            SpotLight s;
            s.Position = wpos;
            s.Direction = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
            s.Color = lc.Color;
            s.Intensity = lc.Intensity;
            s.Range = lc.Range;
            s.InnerAngleDeg = lc.InnerConeDeg;
            s.OuterAngleDeg = lc.OuterConeDeg;
            env.SpotLights.push_back(s);
        } else {
            if ((int)env.PointLights.size() >= LightingEnvironment::MaxPointLights) continue;
            PointLight p;
            p.Position = wpos;
            p.Color = lc.Color;
            p.Intensity = lc.Intensity;
            p.Range = lc.Range;
            env.PointLights.push_back(p);
        }
    }
    return env;
}

// ============================================================================
//  Шейдер подсветки выделения
// ============================================================================

namespace {

// Силуэт выделенного объекта пишется в масочный буфер, затем расширяется на
// постоянное число ПИКСЕЛЕЙ поверх готового кадра. Ширина каймы не зависит ни
// от размера объекта, ни от расстояния до камеры — маленькая далёкая лампа
// подсвечивается так же читаемо, как большой персонаж на переднем плане.
const char* kMaskVert = R"(#version 330 core
layout (location = 0) in vec3 aPos;
uniform mat4 uModel;
uniform mat4 uViewProj;
void main() { gl_Position = uViewProj * uModel * vec4(aPos, 1.0); }
)";
const char* kMaskFrag = R"(#version 330 core
out vec4 FragColor;
void main() { FragColor = vec4(1.0); }
)";

const char* kEdgeVert = R"(#version 330 core
out vec2 vUV;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
const char* kEdgeFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uMask;
uniform vec2 uTexel;
uniform vec3 uColor;
const int R = 3;
void main() {
    if (texture(uMask, vUV).r > 0.5) discard;       // внутри силуэта каймы нет
    float near = 0.0;
    for (int y = -R; y <= R; ++y)
        for (int x = -R; x <= R; ++x)
            near = max(near, texture(uMask, vUV + vec2(x, y) * uTexel).r);
    if (near < 0.5) discard;                        // рядом ничего нет
    FragColor = vec4(uColor, 1.0);
}
)";

Shader& MaskShader() {
    static Shader* s = new Shader(Shader::FromSource(kMaskVert, kMaskFrag, "D3DOutlineMask"));
    return *s;
}
Shader& EdgeShader() {
    static Shader* s = new Shader(Shader::FromSource(kEdgeVert, kEdgeFrag, "D3DOutlineEdge"));
    return *s;
}

// С какого радиуса сцены переходить на каскады. Ниже него одна карта, севшая
// вплотную по габаритам, даёт тексель не хуже — а стоит один проход вместо
// трёх. Сорок метров — это площадка, на которой одна карта 2048 даёт уже
// четыре сантиметра на тексель, и ступеньки становятся заметны вблизи.
constexpr float kCascadeSceneRadius = 40.0f;

int ShadingCode(ShadingMode mode) {
    // Коды движкового батча: 0 — освещённый, 1 — плоский цвет, 2 — нормали.
    switch (mode) {
        case ShadingMode::Shaded:    return 0;
        case ShadingMode::Wireframe: return 1;
        case ShadingMode::Unlit:     return 1;
        case ShadingMode::Normals:   return 2;
    }
    return 0;
}

} // namespace

// ============================================================================
//  Жизненный цикл
// ============================================================================

void StageRenderer::Init(int sceneMsaa) {
    m_sceneMsaa = std::max(sceneMsaa, 1);
    // Разрешение карты теней берём из настроек движка: оно уже есть в Config
    // (SAGE_SHADOW_RES, качество графики), и прибивать здесь своё число значило
    // бы игнорировать выбор человека.
    m_shadows.emplace(std::max(512, sage::EngineConfig::Get().ShadowResolution),
                      sage::EngineConfig::Get().ShadowCascades);
    // MSAA — только у буферов СЦЕНЫ. Буферы пост-обработки принимают уже
    // готовую картинку, геометрии в них нет, и сглаживать там нечего.
    m_stageFbo.emplace(m_stageW, m_stageH, m_sceneMsaa);
    m_stagePostFbo.emplace(m_stageW, m_stageH);
    m_viewFbo.emplace(m_viewW, m_viewH, m_sceneMsaa);
    m_viewPostFbo.emplace(m_viewW, m_viewH);
    m_outlineMask.emplace(m_stageW, m_stageH);
    m_stagePostfx.emplace();
    m_viewPostfx.emplace();
    m_debug.emplace();
    m_sky.emplace();
    m_particles.emplace();
    m_fullscreenTri = sage::rhi::GraphicsDevice::Get().CreateGeometry(sage::rhi::VertexLayout{});
}

void StageRenderer::SetStageSize(int w, int h) {
    m_stageW = std::max(w, 8);
    m_stageH = std::max(h, 8);
}

void StageRenderer::SetRenderViewSize(int w, int h) {
    m_viewW = std::max(w, 8);
    m_viewH = std::max(h, 8);
}

// ============================================================================
//  Проходы
// ============================================================================

void StageRenderer::DrawSky(const LightingEnvironment& env, const glm::mat4& view,
                            const glm::mat4& proj) {
    if (!env.Skybox.Enabled) return;
    if (env.Skybox.HasCubemap()) {
        if (std::shared_ptr<Skybox> sky = ResourceManager::Instance().GetSkybox(env.Skybox.CubemapDir)) {
            sky->Draw(view, proj, env.Skybox.Intensity, env.Skybox.RotationDeg);
            return;
        }
        // Каталог указан, но не читается — остаёмся на градиенте, чтобы кадр
        // не оказался пустым. Причина уже записана в лог.
    }
    m_sky->Draw(view, proj, env.Skybox.TopColor, env.Skybox.HorizonColor);
}

// Габариты того, что реально отбрасывает тень. Считаются каждый кадр: объекты
// двигаются, и коробка, подогнанная один раз, назавтра оказалась бы не той.
static bool ShadowBounds(Scene& scene, glm::vec3& center, float& radius) {
    auto& reg = scene.Registry();
    glm::vec3 lo(1e9f), hi(-1e9f);
    bool any = false;

    auto add = [&](entt::entity e) {
        if (HiddenObjects::IsHidden(scene, e)) return;
        const glm::vec3 p = glm::vec3(scene.WorldMatrix(e)[3]);
        // Габарит объекта берём по масштабу: точные границы меша потребовали бы
        // держать их рядом с геометрией, а промах здесь стоит дёшево — коробка
        // чуть больше нужного, тень чуть крупнее текселем.
        const Transform& tf = reg.get<Transform>(e);
        const float extent = glm::compMax(glm::abs(tf.Scale)) * 1.75f + 0.5f;
        lo = glm::min(lo, p - extent);
        hi = glm::max(hi, p + extent);
        any = true;
    };
    for (auto e : reg.view<MeshRendererComponent, Transform>()) add(e);
    for (auto e : reg.view<AnimatedModelComponent, Transform>()) add(e);
    if (!any) return false;

    center = (lo + hi) * 0.5f;
    radius = glm::length(hi - lo) * 0.5f;
    return radius > 0.01f;
}

bool StageRenderer::CascadeViewOf(Scene& scene, int cameraEntityId, float aspect,
                                  ShadowMap::CameraView& out) const {
    const CameraFrameInfo frame = CameraFrameOf(scene, cameraEntityId, aspect);
    if (!frame.HasCamera) return false;

    out.Position = frame.Position;
    // Направление и «верх» достаём из матрицы вида: камера сцены — это
    // компонент с трансформом, готового объекта Camera у неё нет.
    const glm::mat3 basis = glm::mat3(frame.View);
    out.Forward = -glm::vec3(basis[0][2], basis[1][2], basis[2][2]);
    out.Up = glm::vec3(basis[0][1], basis[1][1], basis[2][1]);
    out.FovY = glm::radians(frame.Fov);
    out.Aspect = frame.Aspect;
    out.Near = 0.1f;
    out.ShadowDistance = sage::EngineConfig::Get().ShadowDistance;
    return true;
}

void StageRenderer::RenderShadow(Scene& scene, const LightingEnvironment& env,
                                 const ShadowMap::CameraView* camera) {
    SAGE_PROFILE("Тени");
    Window& window = sage::Application::Get().GetWindow();
    HiddenObjects hidden(scene); // спрятанное не отбрасывает тень

    // ПОДГОНКА КОРОБКИ ТЕНЕЙ ПОД СЦЕНУ.
    //
    // Раньше здесь стояло «центр в начале координат, радиус 28 метров» — при
    // карте 2048 это 2.7 сантиметра на тексель НЕЗАВИСИМО от того, что в
    // сцене. Комнате из трёх предметов доставалась точность, рассчитанная на
    // стометровую площадку, и тень выглядела крупноблочной именно поэтому.
    //
    // Теперь радиус — по фактическим габаритам. Сцена из демонстрации
    // укладывается примерно в 12 метров, то есть тексель становится втрое
    // мельче на том же разрешении. Ограничение снизу не даёт коробке
    // схлопнуться на одиноком объекте (тень стала бы резче геометрии и
    // проявила бы ступеньки самой карты), сверху — не даёт уползти в
    // бессмысленную точность на огромной сцене.
    glm::vec3 center(0.0f);
    float radius = 28.0f;
    if (ShadowBounds(scene, center, radius)) {
        radius = glm::clamp(radius * 1.15f, 4.0f, 120.0f);
    } else {
        center = glm::vec3(0.0f);
        radius = 28.0f;
    }
    // КОГДА ВКЛЮЧАТЬ КАСКАДЫ. Только на большой сцене и только если известна
    // камера. На павильонной постановке (а это обычный случай инструмента)
    // коробка выше и так садится вплотную, тексель выходит мелким, и каскады
    // означали бы три прохода геометрии ради той же картинки. Каскады нужны
    // ровно там, где одной карте не хватает разрешения на всю даль.
    const bool bigScene = radius > kCascadeSceneRadius;
    if (camera && bigScene && m_shadows->CascadeCount() > 1) {
        m_shadows->SetCascades(env.Sun.Direction, *camera);
    } else {
        m_shadows->SetLightMatrix(env.Sun.Direction, center, radius);
    }
    for (int c = 0; c < m_shadows->CascadeCount(); ++c) {
        m_shadows->BeginRender(c);
        m_batch.RenderDepth(scene, m_shadows->LightMatrix(c));
        sage::anim::DrawAnimatedModelsDepth(scene, m_shadows->LightMatrix(c));
        // Одна карта: остальные каскады — её копии, рисовать в них незачем.
        if (m_shadows->ActiveCascades() == 1) break;
    }
    m_shadows->EndRender(window.Width(), window.Height());
}

void StageRenderer::DrawScene(Scene& scene, const LightingEnvironment& env, const glm::mat4& view,
                              const glm::mat4& proj, const glm::vec3& viewPos, ShadingMode shading) {
    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();
    const bool wireframe = (shading == ShadingMode::Wireframe);

    if (wireframe) device.SetPolygonMode(sage::rhi::PolygonMode::Line);
    m_stats = m_batch.RenderColor(scene, view, proj, viewPos, env,
                                  ShadowBinding(*m_shadows, true), ShadingCode(shading));
    if (wireframe) device.SetPolygonMode(sage::rhi::PolygonMode::Fill);

    sage::anim::DrawAnimatedModels(scene, view, proj, viewPos, env,
                                   ShadowBinding(*m_shadows, true));
    m_particles->DrawFromView(view, proj);
}

StageRenderer::CameraFrameInfo StageRenderer::CameraFrameOf(Scene& scene, int cameraEntityId, float aspect) {
    CameraFrameInfo info;
    info.Aspect = aspect;

    GameObject cam = scene.Get(cameraEntityId);
    if (cam.Valid() && scene.Registry().all_of<CameraComponent>(cam.Entity())) {
        // Матрицы строим ровно так же, как движок для игровой камеры: мировая
        // матрица сущности + Fov/Near/Far компонента. Иначе «вид камеры» в
        // инструменте и кадр в рендере разъехались бы.
        const CameraComponent& cc = scene.Registry().get<CameraComponent>(cam.Entity());
        const glm::mat4 world = scene.WorldMatrix(cam.Entity());
        const glm::vec3 pos = glm::vec3(world[3]);
        const glm::vec3 fwd = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
        const glm::vec3 up = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)));
        info.View = glm::lookAt(pos, pos + fwd, up);
        info.Proj = glm::perspective(glm::radians(cc.Fov), aspect, cc.NearClip, cc.FarClip);
        info.Position = pos;
        info.Fov = cc.Fov;
        info.HasCamera = true;
        return info;
    }

    // Явной камеры не передали (или её удалили) — берём первую Primary сцены
    // тем же хелпером, что и движок.
    sage::ecs::CameraFrame frame = sage::ecs::PrimaryCameraFrame(scene, aspect);
    info.View = frame.View;
    info.Proj = frame.Proj;
    info.Position = frame.Position;
    info.HasCamera = frame.HasPrimary;
    return info;
}

float StageRenderer::AutoFocusDistance(Scene& scene, const CameraFrameInfo& frame) {
    // Ищем ближайший объект, попадающий под центр кадра: берём сущности с
    // мешем и проверяем угол между направлением камеры и направлением на
    // объект. Полноценный рейкаст здесь избыточен — фокус наводится на
    // предмет съёмки, а он по построению стоит в центре кадра.
    const glm::vec3 forward = glm::normalize(
        glm::vec3(glm::inverse(frame.View) * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));

    // Выбираем объект, ближайший к ЦЕНТРАЛЬНОМУ ЛУЧУ, а не к камере: фокус
    // наводится на то, на что камера смотрит. Ближайший по расстоянию давал бы
    // осечку каждый раз, когда сбоку от кадра стоит что-то поближе.
    float best = 0.0f;
    float bestAlignment = 0.966f; // косинус 15 градусов — граница «в центре кадра»

    auto& reg = scene.Registry();
    auto view = reg.view<Transform, MeshRendererComponent>();
    for (auto e : view) {
        if (!view.get<MeshRendererComponent>(e).MeshPtr) continue;
        if (HiddenObjects::IsHidden(scene, e)) continue;

        const glm::vec3 toObject = glm::vec3(scene.WorldMatrix(e)[3]) - frame.Position;
        const float distance = glm::length(toObject);
        if (distance < 1e-3f) continue;

        const float alignment = glm::dot(toObject / distance, forward);
        if (alignment > bestAlignment) {
            bestAlignment = alignment;
            best = distance;
        }
    }
    return best; // 0 — под центром кадра ничего нет, дистанцию задаёт ползунок
}

sage::render::PostFXSettings StageRenderer::PostFXOf(Scene& scene, int cameraEntityId,
                                                     const CameraFrameInfo& frame, bool cinematic) {
    sage::render::PostFXSettings fx;
    // База — спокойные значения: инструмент не должен «улучшать» кадр за спиной
    // у оператора. Всё остальное приходит с киношной камеры.
    fx.Exposure = 1.0f;
    fx.Gamma = 2.2f;
    fx.Saturation = 1.05f;
    fx.Contrast = 1.02f;
    fx.AOEnabled = true;
    fx.AOStrength = 0.8f;

    GameObject cam = scene.Get(cameraEntityId);
    if (!cam.Valid()) return fx;
    const CineCameraComponent* cine = scene.Registry().try_get<CineCameraComponent>(cam.Entity());
    if (!cine) return fx;

    fx.BloomEnabled = cine->Bloom;
    fx.BloomIntensity = cine->BloomIntensity;
    fx.Vignette = cine->Vignette ? cine->VignetteAmount : 0.0f;
    if (cine->ColorGrading) {
        // «Грейд» на доступных ручках цепочки движка: насыщенность и контраст
        // ведутся одним ползунком силы — понятная ручка вместо кривых по каналам.
        fx.Saturation = 1.0f + 0.20f * cine->ColorGradingAmount;
        fx.Contrast = 1.0f + 0.10f * cine->ColorGradingAmount;
    }

    // Глубина резкости. Дистанцию фокуса при включённом автофокусе задаёт не
    // ползунок, а расстояние до цели наводки (см. AutoFocusDistance): оператору
    // достаточно выбрать объект, а не подбирать метры вручную.
    fx.DofEnabled = cinematic && cine->DepthOfField;
    fx.FocusDistance = cine->FocusDistance;
    if (cine->AutoFocus) {
        const float autoDistance = AutoFocusDistance(scene, frame);
        if (autoDistance > 0.0f) fx.FocusDistance = autoDistance;
    }
    fx.Aperture = cine->Aperture;

    fx.MotionBlurEnabled = cinematic && cine->MotionBlur;
    fx.MotionBlurAmount = cine->MotionBlurAmount;

    fx.ChromaticAberration =
        (cinematic && cine->ChromaticAberration) ? cine->ChromaticAmount : 0.0f;
    return fx;
}

void StageRenderer::RenderStage(Scene& scene, Camera& camera, const LightingEnvironment& env,
                                ShadingMode shading, ViewPreset preset, int sceneCameraId,
                                const ViewportOverlays& overlays, const std::vector<int>& selection,
                                glm::mat4& outView, glm::mat4& outProj) {
    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();
    const float aspect = (float)m_stageW / (float)std::max(m_stageH, 1);

    // --- Матрицы вида по выбранному пресету ---
    if (preset == ViewPreset::SceneCamera) {
        CameraFrameInfo frame = CameraFrameOf(scene, sceneCameraId, aspect);
        outView = frame.View;
        outProj = frame.Proj;
    } else if (preset == ViewPreset::Perspective) {
        outView = camera.GetViewMatrix();
        outProj = camera.GetProjectionMatrix(aspect);
    } else {
        // Ортогональные виды спереди/сбоку/сверху: расстояние до цели задаёт
        // масштаб окна, поэтому колесо мыши зумит их так же, как перспективу.
        const glm::vec3 target = camera.Position + camera.Front * 8.0f;
        const float half = std::max(glm::length(camera.Position - target) * 0.5f, 0.5f);
        glm::vec3 eye = target;
        glm::vec3 up(0.0f, 1.0f, 0.0f);
        switch (preset) {
            case ViewPreset::Front: eye += glm::vec3(0.0f, 0.0f, 40.0f); break;
            case ViewPreset::Side:  eye += glm::vec3(40.0f, 0.0f, 0.0f); break;
            default:                eye += glm::vec3(0.0f, 40.0f, 0.0f);
                                    up = glm::vec3(0.0f, 0.0f, -1.0f); break;
        }
        outView = glm::lookAt(eye, target, up);
        outProj = glm::ortho(-half * aspect, half * aspect, -half, half, 0.1f, 200.0f);
    }

    const glm::vec3 viewPos = glm::vec3(glm::inverse(outView)[3]);

    m_stageFbo->Resize(m_stageW, m_stageH);
    m_stagePostFbo->Resize(m_stageW, m_stageH);

    FrameDesc d;
    d.View = outView;
    d.Proj = outProj;
    d.ViewPos = viewPos;
    d.Hdr = &*m_stageFbo;
    d.Shading = shading;
    d.HelperAspect = aspect;
    d.Helpers = &overlays;
    if (overlays.Grid) d.Grid = &overlays.GridConfig;
    if (overlays.Outline && !selection.empty()) d.Outline = &selection;

    // Пост-обработка только в полном затенении: отладочные режимы должны
    // показывать сырые данные, а не «красивую картинку». Отсутствие Fx/Output и
    // означает «кадр без пост-обработки» — отдельного флага для этого нет.
    m_stagePostApplied = (shading == ShadingMode::Shaded);
    if (m_stagePostApplied) {
        d.Output = &*m_stagePostFbo;
        d.Fx = &*m_stagePostfx;
        // Оптика камеры (глубина резкости, смаз, аберрация) включается только
        // когда вьюпорт СМОТРИТ камерой сцены: у свободной камеры своя позиция,
        // и чужая дистанция фокуса замылила бы всё рабочее поле.
        d.FxSettings = PostFXOf(scene, sceneCameraId, CameraFrameOf(scene, sceneCameraId, aspect),
                                /*cinematic=*/preset == ViewPreset::SceneCamera);
        d.Velocity = true;
        d.PrevViewProj = &m_prevViewProjStage;
        // Во вьюпорте склейка видна только когда он смотрит камерой сцены; в
        // свободном облёте камера своя, и склейка её не касается.
        BeginShot(preset == ViewPreset::SceneCamera ? sceneCameraId : -1,
                  m_lastCameraStage, m_prevViewProjStage, d, m_stagePostfx);
    }

    RenderFrame(scene, env, d);
}

// ============================================================================
//  Кадр
// ============================================================================

// ЕДИНСТВЕННОЕ место, где записан порядок проходов. Всё, что рисует
// инструмент — рабочий вьюпорт, Render View, кадр экспорта, — проходит здесь;
// отличаются они только описанием (FrameDesc), а не своей копией
// последовательности.
Framebuffer& StageRenderer::RenderFrame(Scene& scene, const LightingEnvironment& env,
                                        const FrameDesc& d) {
    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();
    HiddenObjects hidden(scene); // погашенное глазком не рисуется и не светит

    // --- Проход 1: очистка и небо ---
    {
        SAGE_PROFILE("Небо");
        d.Hdr->Bind();
        device.SetClearColor(d.ClearColor.r, d.ClearColor.g, d.ClearColor.b, d.ClearColor.a);
        device.Clear();
        if (d.Sky) DrawSky(env, d.View, d.Proj);
    }

    // --- Проход 2: геометрия (батч + скелетные модели + частицы) ---
    {
        SAGE_PROFILE("Геометрия");
        DrawScene(scene, env, d.View, d.Proj, d.ViewPos, d.Shading);
    }

    // --- Проход 3: сетка ---
    // После геометрии и ДО служебной графики: она полупрозрачна и должна
    // смешиваться с уже нарисованной сценой, а каркасы камер и светов должны
    // ложиться поверх неё.
    if (d.Grid) {
        SAGE_PROFILE("Сетка");
        sage::render::GridSettings grid = *d.Grid;
        grid.Enabled = true;
        m_grid.Draw(d.View, d.Proj, d.ViewPos, grid);
    }

    // --- Проход 4: служебная графика вьюпорта ---
    // В тот же буфер и с тестом глубины, чтобы объекты корректно её заслоняли.
    if (d.Helpers) {
        SAGE_PROFILE("Служебная графика");
        DrawHelpers(scene, *d.Helpers, d.Outline ? *d.Outline : std::vector<int>{},
                    d.HelperAspect);
        m_debug->Flush(d.View, d.Proj);
    }

    // Многосэмпловое содержимое переносится в обычные текстуры. Строго ЗДЕСЬ:
    // вся геометрия и служебная графика уже нарисованы, а всё, что дальше,
    // читает буфер как текстуру — а многосэмпловую обычный sampler2D не берёт.
    {
        SAGE_PROFILE("Разрешение MSAA");
        d.Hdr->Resolve();
    }

    // --- Проход 5: скорости ---
    // Отдельный проход геометрии, пишущий экранное смещение каждого пикселя за
    // кадр. Нужен только смазу движения, поэтому и рисуется только когда смаз
    // включён: лишний проход по всей видимой геометрии стоит заметно.
    unsigned int velocityTexture = 0;
    if (d.Velocity && d.PrevViewProj && d.FxSettings.MotionBlurEnabled &&
        d.FxSettings.MotionBlurAmount > 0.0f) {
        SAGE_PROFILE("Скорости");
        if (!m_velocityFbo || m_velocityFbo->Width() != d.Hdr->Width() ||
            m_velocityFbo->Height() != d.Hdr->Height()) {
            m_velocityFbo.emplace(d.Hdr->Width(), d.Hdr->Height());
        }
        m_velocityFbo->Bind();
        // Чёрный — нулевая скорость: там, где геометрии нет (небо), смазывать
        // нечего, и фон не должен тянуться за движущимся объектом.
        device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        device.Clear();
        m_batch.RenderVelocity(d.Proj * d.View, *d.PrevViewProj);
        velocityTexture = m_velocityFbo->ColorTexture();
    }
    if (d.PrevViewProj) *d.PrevViewProj = d.Proj * d.View;

    // --- Проход 6: пост-обработка ---
    Framebuffer* result = d.Hdr;
    if (d.Fx && d.Output) {
        SAGE_PROFILE("Пост-обработка");
        d.Fx->Render(d.Hdr->ColorTexture(), d.Hdr->DepthTexture(), d.Hdr->Width(), d.Hdr->Height(),
                     d.Proj, d.View, d.FxSettings, d.Output, 0, 0, d.Output->Width(),
                     d.Output->Height(), velocityTexture);
        result = d.Output;
    }

    // --- Проход 7: подсветка выделения ---
    // Строго последней и поверх результата пост-обработки: обводка — это
    // указание инструмента, а не часть изображения, и тон-маппинг её съел бы.
    if (d.Outline && !d.Outline->empty()) {
        SAGE_PROFILE("Обводка");
        RenderOutline(scene, *d.Outline, d.View, d.Proj, *result);
    }

    device.BindDefaultFramebuffer();
    return *result;
}

bool StageRenderer::RenderCameraView(Scene& scene, const LightingEnvironment& env, int cameraEntityId) {
    m_viewPostApplied = false;

    const float aspect = (float)m_viewW / (float)std::max(m_viewH, 1);
    const CameraFrameInfo frame = CameraFrameOf(scene, cameraEntityId, aspect);
    if (!frame.HasCamera) return false;

    m_viewFbo->Resize(m_viewW, m_viewH);
    m_viewPostFbo->Resize(m_viewW, m_viewH);

    FrameDesc d;
    d.View = frame.View;
    d.Proj = frame.Proj;
    d.ViewPos = frame.Position;
    d.Hdr = &*m_viewFbo;
    d.Output = &*m_viewPostFbo;
    d.Fx = &*m_viewPostfx;
    d.FxSettings = PostFXOf(scene, cameraEntityId, frame, /*cinematic=*/true);
    d.ClearColor = glm::vec4(env.SkyColor * 0.85f, 1.0f);
    d.Velocity = true;
    d.PrevViewProj = &m_prevViewProjView;
    BeginShot(cameraEntityId, m_lastCameraView, m_prevViewProjView, d, m_viewPostfx);
    RenderFrame(scene, env, d);

    m_viewPostApplied = true;
    return true;
}

bool StageRenderer::RenderToTarget(Scene& scene, const LightingEnvironment& env, int cameraEntityId,
                                   Framebuffer& target, glm::vec2 jitterPixels) {
    const int w = target.Width(), h = target.Height();
    const float aspect = (float)w / (float)std::max(h, 1);
    CameraFrameInfo frame = CameraFrameOf(scene, cameraEntityId, aspect);
    if (!frame.HasCamera) return false;

    // Сдвиг проекции на доли пикселя. Правим два элемента матрицы, а не саму
    // камеру: смещать камеру в мире нельзя — изменился бы параллакс, и
    // усреднение дало бы не сглаживание, а лёгкое размытие движения.
    if (jitterPixels.x != 0.0f || jitterPixels.y != 0.0f) {
        frame.Proj[2][0] += 2.0f * jitterPixels.x / (float)w;
        frame.Proj[2][1] += 2.0f * jitterPixels.y / (float)h;
    }

    // Сцена рисуется в СВОЙ HDR-буфер, и только потом пост-обработка пишет
    // результат в target. Раньше сюда рисовалось напрямую, и экспортированный
    // кадр уходил в файл без тон-маппинга, bloom и виньетки — то есть заметно
    // темнее и площе того, что показывал Render View.
    if (!m_exportFbo || m_exportFbo->Width() != w || m_exportFbo->Height() != h) {
        m_exportFbo.emplace(w, h, m_sceneMsaa);
    }
    // Отдельный экземпляр PostFX: у него своя история кадра для motion blur, и
    // экспорт не должен смешиваться с историей интерактивного превью.
    if (!m_exportPostfx) m_exportPostfx.emplace();

    FrameDesc d;
    d.View = frame.View;
    d.Proj = frame.Proj;
    d.ViewPos = frame.Position;
    d.Hdr = &*m_exportFbo;
    d.Output = &target;
    d.Fx = &*m_exportPostfx;
    d.FxSettings = PostFXOf(scene, cameraEntityId, frame, /*cinematic=*/true);
    d.ClearColor = glm::vec4(env.SkyColor * 0.85f, 1.0f);
    d.Velocity = true;
    d.PrevViewProj = &m_prevViewProjExport;
    BeginShot(cameraEntityId, m_lastCameraExport, m_prevViewProjExport, d, m_exportPostfx);
    RenderFrame(scene, env, d);
    return true;
}

void StageRenderer::BeginShot(int cameraId, int& lastCameraId, glm::mat4& prevViewProj,
                              const FrameDesc& desc,
                              std::optional<sage::render::PostFX>& fx) {
    if (cameraId == lastCameraId) return;
    lastCameraId = cameraId;
    // Прошлой матрицей объявляем ТЕКУЩУЮ: скорость камеры на первом кадре
    // плана выходит нулевой, и склейка получается резкой, как ей и положено.
    // Скорости самих объектов при этом остаются настоящими — они никуда не
    // прыгали, и смаз от их движения в новом плане законен.
    prevViewProj = desc.Proj * desc.View;
    if (fx) fx->ResetHistory();
}

void StageRenderer::ResetMotionHistory() {
    // Скачок во времени или смена сцены рвут непрерывность движения: без сброса
    // первый кадр после скачка сравнивался бы с матрицей из другой точки ролика
    // и размазался бы через весь экран.
    if (m_stagePostfx) m_stagePostfx->ResetHistory();
    if (m_viewPostfx) m_viewPostfx->ResetHistory();
    if (m_exportPostfx) m_exportPostfx->ResetHistory();
    // И положение объектов: после скачка во времени они стоят совсем не там,
    // где кадром раньше, и скорость по старому снимку была бы длиной в прыжок.
    m_batch.ResetVelocityHistory();
}

// ============================================================================
//  Служебная графика вьюпорта
// ============================================================================

void StageRenderer::DrawHelpers(Scene& scene, const ViewportOverlays& overlays,
                                const std::vector<int>& selection, float cameraAspect) {
    // Сетка рисуется НЕ здесь: ей нужны матрицы камеры (она считает пересечение
    // луча с плоскостью), а служебная графика работает в мировых координатах.
    // См. DrawGrid, который вызывается из прохода вьюпорта.
    if (!overlays.Gizmos) return;

    auto& reg = scene.Registry();
    auto isSelected = [&](int id) {
        return std::find(selection.begin(), selection.end(), id) != selection.end();
    };

    // Камеры — каркас пирамиды видимости: сразу видно, что попадёт в кадр.
    auto cams = reg.view<CameraComponent, Transform, IdComponent>();
    for (auto e : cams) {
        if (HiddenObjects::IsHidden(scene, e)) continue;
        const bool sel = isSelected(cams.get<IdComponent>(e).Id);
        const glm::mat4 world = scene.WorldMatrix(e);
        const glm::vec3 pos = glm::vec3(world[3]);
        const glm::vec3 fwd = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
        const CameraComponent& cc = cams.get<CameraComponent>(e);
        m_debug->WireFrustum(pos, fwd, cc.Fov, cameraAspect, 0.3f, sel ? 3.5f : 2.0f,
                             sel ? glm::vec3(1.0f, 0.78f, 0.25f) : glm::vec3(0.45f, 0.68f, 0.92f));
    }

    // Ограничения — линия к цели и точки траектории.
    //
    // Связь «этот объект смотрит на тот» существует только в панели свойств, и
    // пока она невидима, сцена выглядит так, будто объект движется сам по себе.
    // Линия делает правило частью КАРТИНКИ: видно, что связано с чем, не
    // выбирая каждый объект по очереди.
    auto constrained = reg.view<ConstraintComponent, Transform, IdComponent>();
    for (auto e : constrained) {
        if (HiddenObjects::IsHidden(scene, e)) continue;
        const ConstraintComponent& cc = constrained.get<ConstraintComponent>(e);
        if (cc.Type == ConstraintType::None || cc.TargetId < 0) continue;
        GameObject target = scene.Get(cc.TargetId);
        if (!target.Valid()) continue;

        const bool sel = isSelected(constrained.get<IdComponent>(e).Id);
        const glm::vec3 from = glm::vec3(scene.WorldMatrix(e)[3]);
        const glm::vec3 to = glm::vec3(scene.WorldMatrix(target.Entity())[3]);
        // Выключенное силой правило рисуется тусклее: ноль в поле «Сила» —
        // рабочее состояние (дорожка ещё не доехала), а не поломка.
        const float w = std::clamp(cc.Influence, 0.0f, 1.0f);
        const glm::vec3 tint = glm::vec3(0.35f + 0.55f * w, 0.75f, 0.45f + 0.35f * w);
        m_debug->Line(from, to, tint * (sel ? 1.3f : 0.8f));
        m_debug->WireSphere(to, sel ? 0.16f : 0.11f, tint * 0.9f, 8);

        if (cc.Type == ConstraintType::Path) {
            // Саму траекторию тоже показываем: без неё «положение 0.37» —
            // число, к которому нечего отнести глазами.
            const std::vector<glm::vec3> points = PathPoints(scene, cc.TargetId);
            glm::vec3 prev, tangent;
            const int steps = 48;
            for (int i = 0; i <= steps; ++i) {
                glm::vec3 p;
                if (!SamplePath(points, (float)i / (float)steps, p, tangent)) break;
                if (i > 0) m_debug->Line(prev, p, glm::vec3(0.95f, 0.72f, 0.30f));
                prev = p;
            }
            for (const glm::vec3& p : points) {
                m_debug->WireSphere(p, 0.13f, glm::vec3(0.95f, 0.72f, 0.30f), 8);
            }
        }
    }

    // Свет — маркер в позиции; у выбранного дополнительно показываем зону
    // действия (сферу радиуса или конус прожектора).
    auto lights = reg.view<LightComponent, Transform, IdComponent>();
    for (auto e : lights) {
        if (HiddenObjects::IsHidden(scene, e)) continue;
        const LightComponent& lc = lights.get<LightComponent>(e);
        const bool sel = isSelected(lights.get<IdComponent>(e).Id);
        const glm::mat4 world = scene.WorldMatrix(e);
        const glm::vec3 pos = glm::vec3(world[3]);
        m_debug->WireSphere(pos, sel ? 0.30f : 0.22f, lc.Color * (sel ? 1.25f : 0.9f), 10);
        if (sel) {
            if (lc.Kind == LightComponent::Type::Spot) {
                const glm::vec3 dir = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
                m_debug->WireCone(pos, dir, lc.Range, lc.OuterConeDeg, lc.Color * 0.85f);
            } else {
                m_debug->WireSphere(pos, lc.Range, lc.Color * 0.7f);
            }
        }
    }

    // Эмиттеры частиц.
    auto fx = reg.view<ParticleEmitterComponent, Transform, IdComponent>();
    for (auto e : fx) {
        if (HiddenObjects::IsHidden(scene, e)) continue;
        const bool sel = isSelected(fx.get<IdComponent>(e).Id);
        const glm::vec3 pos = glm::vec3(scene.WorldMatrix(e)[3]);
        m_debug->WireSphere(pos, 0.18f, sel ? glm::vec3(1.0f, 0.85f, 0.3f) : glm::vec3(0.85f, 0.55f, 0.3f), 8);
    }

    // Оси выбранного объекта — та же система координат, что у гизмо.
    for (int id : selection) {
        GameObject obj = scene.Get(id);
        if (obj.Valid()) m_debug->Axes(scene.WorldMatrix(obj.Entity()), 1.3f);
    }
}

void StageRenderer::RenderOutline(Scene& scene, const std::vector<int>& selection,
                                  const glm::mat4& view, const glm::mat4& proj, Framebuffer& target) {
    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();

    // --- Силуэт в масочный буфер (без теста глубины: подсветка видна целиком,
    //     даже если объект частично закрыт другим) ---
    m_outlineMask->Resize(m_stageW, m_stageH);
    m_outlineMask->Bind();
    device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    device.Clear();

    Shader& mask = MaskShader();
    mask.Use();
    mask.SetMat4("uViewProj", proj * view);
    device.SetDepthTest(false);
    bool anySilhouette = false;
    for (int id : selection) {
        GameObject obj = scene.Get(id);
        if (!obj.Valid()) continue;
        const MeshRendererComponent* mr = scene.Registry().try_get<MeshRendererComponent>(obj.Entity());
        if (!mr || !mr->MeshPtr) continue; // у камеры/света силуэта нет — их выделяет гизмо
        mask.SetMat4("uModel", scene.WorldMatrix(obj.Entity()));
        mr->MeshPtr->Draw();
        anySilhouette = true;
    }
    device.SetDepthTest(true);
    if (!anySilhouette) return;

    // --- Кайма поверх готового кадра ---
    target.Bind();
    device.SetViewport(0, 0, m_stageW, m_stageH);
    device.SetDepthTest(false);
    device.SetBlend(true);
    Shader& edge = EdgeShader();
    edge.Use();
    edge.SetInt("uMask", 0);
    edge.SetVec2("uTexel", glm::vec2(1.0f / (float)m_stageW, 1.0f / (float)m_stageH));
    edge.SetVec3("uColor", glm::vec3(1.0f, 0.62f, 0.14f));
    device.BindTexture2D(0, m_outlineMask->ColorTexture());
    m_fullscreenTri->DrawArrays(3);
    device.SetDepthTest(true);
}

unsigned int StageRenderer::StageTexture() const {
    return m_stagePostApplied ? m_stagePostFbo->ColorTexture() : m_stageFbo->ColorTexture();
}

unsigned int StageRenderer::RenderViewTexture() const {
    return m_viewPostApplied ? m_viewPostFbo->ColorTexture() : m_viewFbo->ColorTexture();
}

} // namespace d3d
