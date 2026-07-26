#include "DirectorLayer.h"

#include "ui/Localization.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include <glm/gtc/matrix_transform.hpp>

#include "ImGuizmo.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h" // DockBuilder — раскладка панелей по умолчанию

#include "anim/BonePose.h"
#include "anim/DirectorComponents.h"
#include "export/GltfExporter.h"
#include "project/Project.h"
#include "render/VideoWriter.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/audio/AudioEngine.h"
#include "sage/core/Application.h"
#include "sage/core/Log.h"
#include "sage/ecs/LightSystem.h"
#include "sage/render/ParticleECS.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Screenshot.h"
#include "sage/render/SkinnedModel.h"
#include "sage/scene/Components.h"
#include "ui/Theme.h"

namespace fs = std::filesystem;

namespace d3d {

namespace {

// Луч из точки вьюпорта в мир — для выбора объекта кликом.
void ScreenRay(const glm::mat4& view, const glm::mat4& proj, float u, float v,
               glm::vec3& outOrigin, glm::vec3& outDir) {
    // NDC: x вправо, y ВВЕРХ (в экранных координатах вниз — отсюда знак).
    const glm::vec4 nearPoint(u * 2.0f - 1.0f, 1.0f - v * 2.0f, -1.0f, 1.0f);
    const glm::vec4 farPoint(nearPoint.x, nearPoint.y, 1.0f, 1.0f);
    const glm::mat4 inv = glm::inverse(proj * view);
    glm::vec4 a = inv * nearPoint;
    glm::vec4 b = inv * farPoint;
    a /= a.w;
    b /= b.w;
    outOrigin = glm::vec3(a);
    outDir = glm::normalize(glm::vec3(b) - glm::vec3(a));
}

// Пересечение луча с AABB в локальном пространстве (slab-тест). Возвращает
// расстояние входа или -1 при промахе.
float RayBox(const glm::vec3& ro, const glm::vec3& rd, const glm::vec3& bmin, const glm::vec3& bmax) {
    const glm::vec3 inv = 1.0f / rd; // нулевая компонента даёт inf — slab-тест это переживает
    const glm::vec3 t0 = (bmin - ro) * inv;
    const glm::vec3 t1 = (bmax - ro) * inv;
    const glm::vec3 tmin = glm::min(t0, t1), tmax = glm::max(t0, t1);
    const float near = std::max({tmin.x, tmin.y, tmin.z});
    const float far = std::min({tmax.x, tmax.y, tmax.z});
    if (near > far || far < 0.0f) return -1.0f;
    return near >= 0.0f ? near : far;
}

} // namespace

DirectorLayer::DirectorLayer(std::string startupProject, RenderJob job)
    : sage::Layer("Director3D"), m_startupProject(std::move(startupProject)), m_job(std::move(job)) {}
DirectorLayer::~DirectorLayer() = default;

// ============================================================================
//  Жизненный цикл
// ============================================================================

void DirectorLayer::OnAttach() {
    sage::Application& app = sage::Application::Get();

    // --- ImGui: доки + вытаскивание панелей в отдельные окна ОС ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    // Своё имя ini-файла: раскладка Director 3D не должна пересекаться с
    // раскладкой редактора SAGE, который тоже живёт на ImGui.
    io.IniFilename = "director3d_imgui.ini";
    Theme::LoadFonts();
    Theme::Apply();
    ImGui_ImplGlfw_InitForOpenGL(app.GetWindow().Handle(), true);
    ImGui_ImplOpenGL3_Init("#version 330");
    m_imguiReady = true;

    // Сглаживание буфера сцены. Переменной окружения нет ради удобства: она
    // нужна проверкам, которые обязаны сравнивать кадры с ним и без него.
    int sceneMsaa = 4;
    if (const char* msaa = std::getenv("D3D_SCENE_MSAA")) sceneMsaa = std::atoi(msaa);
    m_renderer.Init(sceneMsaa);

    // Словарь лежит рядом с бинарником, в assets/i18n. Отсутствие файла — не
    // авария: интерфейс просто останется русским.
    i18n::LoadDictionary((fs::current_path() / "assets" / "i18n").string());
    i18n::LoadPreference();
    // Переменная окружения бьёт сохранённый выбор: так снимаются скриншоты и
    // гоняется CI, не трогая настройку живого пользователя.
    if (const char* lang = std::getenv("D3D_LANG")) i18n::SetLanguageByCode(lang);
    m_assetsDir = fs::current_path();

    BuildDefaultScene();

    m_camera.Position = {7.0f, 4.5f, 9.0f};
    m_camera.Yaw = -108.0f;
    m_camera.Pitch = -16.0f;
    m_camera.ProcessMouse(0.0f, 0.0f);

    m_stage.RequestFocus();
    LOG_INFO("Director") << "Director 3D запущен (объектов в сцене: " << m_scene->Count() << ")";

    // Автоскриншот для проверок без человека за монитором: на заданном кадре
    // окно сохраняется в PNG, и приложение закрывается. Кадр не нулевой,
    // потому что ImGui первым кадром только строит раскладку дока — снимок с
    // него показал бы пустоту вместо интерфейса.
    if (const char* frame = std::getenv("D3D_SCREENSHOT_AT_FRAME")) {
        m_autoScreenshotFrame = std::atoi(frame);
        if (const char* path = std::getenv("D3D_SCREENSHOT_PATH")) m_autoScreenshotPath = path;
        LOG_INFO("Director") << "Автоскриншот на кадре " << m_autoScreenshotFrame
                             << " -> " << m_autoScreenshotPath;
    }
    // Проект из командной строки открываем ПОСЛЕ стартовой сцены: она уже
    // построена, и неудачное открытие оставляет пользователя в рабочем
    // состоянии, а не в пустоте.
    if (!m_startupProject.empty()) {
        std::string err;
        if (OpenProject(m_startupProject, err)) {
            SetStatus("Проект открыт: " + m_startupProject);
        } else {
            LOG_ERROR("Director") << "Не удалось открыть " << m_startupProject << ": " << err;
            SetStatus("Не удалось открыть проект: " + err);
        }
    }

    if (std::getenv("D3D_ADVANCED")) m_simpleMode = false;
    if (const char* tab = std::getenv("D3D_TIMELINE_TAB")) m_timeline.SetTab(std::atoi(tab));
    if (std::getenv("D3D_DEMO")) BuildDemoAnimation();
    // Персонаж с выбранной костью — для снимков интерфейса без человека за
    // мышью: иначе панель Bone и скелет во вьюпорте нечем показать.
    if (const char* boneEnv = std::getenv("D3D_CHARACTER")) {
        const int character = Create(CreateKind::Character);
        RenameObject(character, "Character");
        SetSelectedId(character);
        const int joint = std::atoi(boneEnv);
        if (joint > 0) m_pendingBoneSelect = joint;
        // Пара ключей на кости — чтобы на снимке было видно и костные дорожки
        // таймлайна, а не только пустую сетку.
        if (std::getenv("D3D_CHARACTER_KEYS")) m_pendingBoneKeys = true;
    }
    if (std::getenv("D3D_SMOKE_TEST")) StartSmokeTest();
    // Пакетный рендер запускается ПОСЛЕДНИМ: к этому моменту проект открыт (или
    // построена демо-постановка), и снимать уже есть что.
    if (m_job.Active) {
        if (m_job.Showcase) BuildShowcase();
        if (!m_job.GltfOutput.empty()) m_gltfWaitFrames = 0;
        else StartRenderJob();
    }
    if (std::getenv("D3D_BONE_TEST")) {
        // Персонаж создаётся тем же вызовом, что и по кнопке Create > Character,
        // а модель грузится лениво — отсюда ожидание в кадрах.
        SetSelectedId(Create(CreateKind::Character));
        m_boneCheckFrames = 0;
    }
}

void DirectorLayer::OnDetach() {
    if (m_imguiReady) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        m_imguiReady = false;
    }
    // Кэш ресурсов — глобальный синглтон, и его деструктор сработал бы уже
    // ПОСЛЕ разрушения окна, то есть без GL-контекста: удаление буферов там
    // падает. OnDetach вызывается движком, пока контекст жив, — чистим здесь.
    ResourceManager::Instance().Clear();
}

void DirectorLayer::BuildDefaultScene() {
    m_scene = std::make_unique<Scene>("Director Scene");
    m_doc.ClearContent();
    m_doc.Name = "My_Animation_Project";
    m_doc.Fps = 24.0f;
    m_doc.Duration = 20.0f;
    m_selection.clear();
    m_bone.Clear();
    m_undo.Clear();
    m_playback.Stop();
    m_dirty = false;
    m_projectPath.clear();

    // Стартовая сцена — минимальный съёмочный павильон: камера, свет, пол и
    // объект. Пустая сцена технически честна, но с ней нечего анимировать, и
    // первый запуск превращается в расстановку сцены вместо знакомства с
    // инструментом.
    m_scene->Lighting.Sun.Direction = glm::normalize(glm::vec3(-0.45f, -1.0f, -0.35f));
    m_scene->Lighting.Sun.Intensity = 1.1f;
    m_scene->Lighting.SkyColor = {0.42f, 0.50f, 0.66f};
    m_scene->Lighting.GroundColor = {0.20f, 0.18f, 0.16f};
    m_scene->Lighting.AmbientStrength = 0.35f;
    m_scene->Lighting.Skybox.Enabled = true;

    const int cameraId = Create(CreateKind::Camera);
    RenameObject(cameraId, "Camera_Main");
    if (GameObject cam = m_scene->Get(cameraId); cam.Valid()) {
        cam.GetTransform().Position = {0.0f, 2.4f, 8.0f};
        cam.GetTransform().Rotation = {-6.0f, 0.0f, 0.0f};
    }
    m_activeCameraId = cameraId;

    const int keyLight = Create(CreateKind::SpotLight);
    RenameObject(keyLight, "Key_Light");
    if (GameObject light = m_scene->Get(keyLight); light.Valid()) {
        light.GetTransform().Position = {4.0f, 5.5f, 4.0f};
        light.GetTransform().Rotation = {-45.0f, 40.0f, 0.0f};
    }

    const int ground = Create(CreateKind::Plane);
    RenameObject(ground, "Ground");
    if (GameObject plane = m_scene->Get(ground); plane.Valid()) {
        plane.GetTransform().Scale = {20.0f, 1.0f, 20.0f};
        plane.Renderer().Color = {0.32f, 0.33f, 0.35f};
    }

    const int cube = Create(CreateKind::Cube);
    RenameObject(cube, "Object");
    if (GameObject obj = m_scene->Get(cube); obj.Valid()) {
        obj.GetTransform().Position = {0.0f, 0.5f, 0.0f};
        obj.Renderer().Color = {0.78f, 0.52f, 0.28f};
    }

    m_selection = {cube};
    m_undo.Clear();  // стартовая сцена — это НЕ правка пользователя
    m_dirty = false;
    SetCurrentTime(0.0f);
}

void DirectorLayer::BuildShowcase() {
    // Постановка для готового ролика. От BuildDemoAnimation отличается задачей:
    // та обязана рендериться мгновенно (её гоняет сквозная проверка в CI), эта
    // должна ПОКАЗЫВАТЬ — движение камеры, перевод фокуса, скелетный клип,
    // ручную позу поверх него, блендшейпы, свет и оптику камеры одновременно.
    //
    // Собирается ровно теми же вызовами, что доступны из интерфейса: дорожка +
    // ключи. Отдельного «режима демонстрации» в рендере нет — если ролик
    // получился, значит работает и ручная анимация.
    m_doc.ClearContent();
    m_doc.Name = "Director3D_Showcase";
    m_doc.Fps = 30.0f;
    m_doc.Duration = 10.0f;

    const int camera = m_activeCameraId;
    const int ground = m_scene->FindByName("Ground").Valid() ? m_scene->FindByName("Ground").Id() : -1;
    const int cube = m_scene->FindByName("Object").Valid() ? m_scene->FindByName("Object").Id() : -1;
    const int keyLight = m_scene->FindByName("Key_Light").Valid() ? m_scene->FindByName("Key_Light").Id() : -1;

    // Сетка с радиусом: у постановки есть площадка, и её край в кадре читается
    // как граница сцены, а не как обрыв мира.
    m_overlays.GridConfig.Mode = sage::render::GridSettings::Extent::Radius;
    m_overlays.GridConfig.Radius = 14.0f;
    m_overlays.GridConfig.CellSize = 1.0f;

    m_scene->Lighting.Sun.Direction = glm::normalize(glm::vec3(-0.5f, -0.85f, -0.4f));
    m_scene->Lighting.Sun.Intensity = 1.35f;
    m_scene->Lighting.Sun.Color = {1.0f, 0.95f, 0.86f};
    m_scene->Lighting.SkyColor = {0.34f, 0.44f, 0.62f};
    m_scene->Lighting.GroundColor = {0.16f, 0.14f, 0.13f};
    m_scene->Lighting.AmbientStrength = 0.40f;
    // Туман — не «атмосферность ради атмосферности»: на плоском полу без него
    // нет ни одного признака глубины, и дальний реквизит читается как ближний
    // того же размера. Начало за площадкой, чтобы сама постановка осталась
    // чистой, а уходил в цвет неба — иначе горизонт получается склейкой.
    m_scene->Lighting.Fog.Enabled = true;
    m_scene->Lighting.Fog.Color = {0.62f, 0.70f, 0.80f};
    m_scene->Lighting.Fog.Start = 16.0f;
    m_scene->Lighting.Fog.End = 62.0f;

    if (ground >= 0) {
        GameObject g = m_scene->Get(ground);
        g.GetTransform().Scale = {70.0f, 1.0f, 70.0f};
        g.Renderer().Color = {0.28f, 0.29f, 0.32f};
    }

    // --- Персонаж: клип + ручная поза поверх него -----------------------------
    const int hero = Create(CreateKind::Character);
    RenameObject(hero, "Hero");
    if (GameObject h = m_scene->Get(hero); h.Valid()) {
        h.GetTransform().Position = {0.0f, 0.0f, 0.0f};
        h.GetTransform().Scale = {1.6f, 1.6f, 1.6f};
    }

    // Дорожка клипов: Wave переходит в Curl кросс-фейдом. Именно это отличает
    // монтаж анимации от «включить один клип на весь ролик».
    ClipTrack& clips = m_doc.EnsureClipTrack(hero);
    clips.Blocks.push_back(ClipBlock{"Wave", 0, 0.0f, 5.5f, 1.0f, 0.0f, true});
    clips.Blocks.push_back(ClipBlock{"Curl", 1, 5.0f, 5.0f, 0.9f, 0.8f, true});

    // Блендшейп: форма меняется поверх скелета, а не вместо него.
    Track& morph = m_doc.EnsureTrack(hero, Property::MorphWeight, 0);
    morph.Channels[0].SetKey(1.0f, 0.0f, Interp::EaseInOut);
    morph.Channels[0].SetKey(3.5f, 1.0f, Interp::EaseInOut);
    morph.Channels[0].SetKey(6.0f, 0.0f, Interp::EaseInOut);

    // --- Реквизит: три объекта разной высоты ----------------------------------
    if (cube >= 0) {
        GameObject c = m_scene->Get(cube);
        c.GetTransform().Position = {-3.9f, 0.5f, 1.6f};
        c.Renderer().Color = {0.82f, 0.45f, 0.20f};
        Track& spin = m_doc.EnsureTrack(cube, Property::Rotation);
        spin.Channels[1].SetKey(0.0f, 0.0f, Interp::Linear);
        spin.Channels[1].SetKey(10.0f, 360.0f, Interp::Linear);
        Track& hop = m_doc.EnsureTrack(cube, Property::Position);
        hop.Channels[1].SetKey(0.0f, 0.5f, Interp::EaseOut);
        hop.Channels[1].SetKey(1.2f, 2.1f, Interp::EaseInOut);
        hop.Channels[1].SetKey(2.4f, 0.5f, Interp::EaseIn);
        hop.Channels[1].SetKey(3.6f, 1.7f, Interp::EaseInOut);
        hop.Channels[1].SetKey(4.8f, 0.5f, Interp::EaseIn);
        hop.Channels[1].SetKey(7.0f, 2.4f, Interp::EaseInOut);
        hop.Channels[1].SetKey(9.2f, 0.5f, Interp::EaseIn);
    }

    const int pillar = Create(CreateKind::Cylinder);
    RenameObject(pillar, "Pillar");
    if (GameObject p = m_scene->Get(pillar); p.Valid()) {
        p.GetTransform().Position = {3.6f, 1.3f, -1.6f};
        p.GetTransform().Scale = {0.5f, 2.6f, 0.5f};
        p.Renderer().Color = {0.76f, 0.76f, 0.80f};
    }

    const int orb = Create(CreateKind::Sphere);
    RenameObject(orb, "Orb");
    if (GameObject o = m_scene->Get(orb); o.Valid()) {
        o.GetTransform().Position = {2.4f, 0.9f, 3.2f};
        o.GetTransform().Scale = {0.9f, 0.9f, 0.9f};
        o.Renderer().Color = {0.24f, 0.52f, 0.86f};
    }
    // Цвет анимируется наравне с геометрией — это отдельная дорожка свойства.
    // Все три канала ключуются независимо — иначе цвет уезжает в серое:
    // анимировать только красный и синий при неподвижном зелёном значит вести
    // цвет через середину куба RGB, а не по дуге между двумя насыщенными.
    Track& orbColor = m_doc.EnsureTrack(orb, Property::Color);
    orbColor.Channels[0].SetKey(0.0f, 0.20f); orbColor.Channels[0].SetKey(5.0f, 0.92f);
    orbColor.Channels[0].SetKey(10.0f, 0.20f);
    orbColor.Channels[1].SetKey(0.0f, 0.52f); orbColor.Channels[1].SetKey(5.0f, 0.30f);
    orbColor.Channels[1].SetKey(10.0f, 0.52f);
    orbColor.Channels[2].SetKey(0.0f, 0.90f); orbColor.Channels[2].SetKey(5.0f, 0.22f);
    orbColor.Channels[2].SetKey(10.0f, 0.90f);

    // Заполняющий свет с другой стороны: одним источником объект получает
    // чёрную теневую сторону, и форма в ней теряется. Это стандартная схема
    // «ключ + заполнение», ради неё в инструменте и есть заготовка света.
    const int fill = Create(CreateKind::PointLight);
    RenameObject(fill, "Fill_Light");
    if (GameObject f = m_scene->Get(fill); f.Valid()) {
        f.GetTransform().Position = {-4.5f, 3.2f, 3.0f};
        if (LightComponent* lc = m_scene->Registry().try_get<LightComponent>(f.Entity())) {
            lc->Color = {0.55f, 0.68f, 1.0f};
            lc->Intensity = 2.2f;
            lc->Range = 18.0f;
        }
    }

    // Частицы — ещё одна подсистема движка, которая должна попасть в кадр:
    // в отличие от геометрии они живут своим временем и проверяют, что экспорт
    // ставит время точно (иначе поток частиц дёргался бы между кадрами).
    const int sparks = Create(CreateKind::ParticleEffect);
    RenameObject(sparks, "Sparks");
    if (GameObject sp = m_scene->Get(sparks); sp.Valid()) {
        sp.GetTransform().Position = {1.5f, 0.7f, 2.4f};
        if (ParticleEmitterComponent* pe =
                m_scene->Registry().try_get<ParticleEmitterComponent>(sp.Entity())) {
            // Дефолты эмиттера рассчитаны на игру, где камера в двух метрах: с
            // десяти метров частица размером 0.1 м — это пара пикселей, то есть
            // ничего. Для кадра нужны и крупнее, и живут дольше.
            ParticleEmitterConfig& c = pe->Config;
            c.DirectionMin = {-0.7f, 0.5f, -0.7f};
            c.DirectionMax = {0.7f, 1.5f, 0.7f};
            c.SpeedMin = 0.6f;
            c.SpeedMax = 1.4f;
            c.Gravity = 0.25f;          // вверх: искры всплывают, а не падают
            c.LifetimeMin = 1.6f;
            c.LifetimeMax = 2.6f;
            c.StartSizeMin = 0.14f;
            c.StartSizeMax = 0.26f;
            c.EndSizeMin = 0.0f;
            c.EndSizeMax = 0.03f;
            c.StartColor = {1.0f, 0.82f, 0.42f, 0.95f};
            c.EndColor = {1.0f, 0.35f, 0.10f, 0.0f};
            c.EmissionRate = 38.0f;
        }
    }

    // --- Свет: пульсация ключевого источника ----------------------------------
    if (keyLight >= 0) {
        Track& intensity = m_doc.EnsureTrack(keyLight, Property::LightIntensity);
        intensity.Channels[0].SetKey(0.0f, 1.6f, Interp::Smooth);
        intensity.Channels[0].SetKey(4.0f, 3.4f, Interp::Smooth);
        intensity.Channels[0].SetKey(7.0f, 1.8f, Interp::Smooth);
        intensity.Channels[0].SetKey(10.0f, 2.6f, Interp::Smooth);
    }

    // --- Камера: облёт с переводом фокуса --------------------------------------
    if (camera >= 0) {
        if (GameObject cam = m_scene->Get(camera); cam.Valid()) {
            cam.GetTransform().Position = {6.5f, 3.0f, 7.5f};
        }
        // Дуга вокруг персонажа: ключи по X и Z ставятся независимо, поэтому
        // траектория — не окружность из формулы, а то, что нарисовал аниматор.
        Track& camPos = m_doc.EnsureTrack(camera, Property::Position);
        camPos.Channels[0].SetKey(0.0f, 7.2f, Interp::EaseInOut);
        camPos.Channels[0].SetKey(5.0f, 0.0f, Interp::Smooth);
        camPos.Channels[0].SetKey(10.0f, -6.4f, Interp::EaseInOut);
        camPos.Channels[1].SetKey(0.0f, 3.4f, Interp::EaseInOut);
        camPos.Channels[1].SetKey(5.0f, 2.6f, Interp::EaseInOut);
        camPos.Channels[1].SetKey(10.0f, 3.0f, Interp::EaseInOut);
        camPos.Channels[2].SetKey(0.0f, 7.0f, Interp::EaseInOut);
        camPos.Channels[2].SetKey(5.0f, 8.6f, Interp::Smooth);
        camPos.Channels[2].SetKey(10.0f, 6.2f, Interp::EaseInOut);

        Track& camRot = m_doc.EnsureTrack(camera, Property::Rotation);
        camRot.Channels[0].SetKey(0.0f, -12.0f, Interp::EaseInOut);
        camRot.Channels[0].SetKey(5.0f, -8.0f, Interp::EaseInOut);
        camRot.Channels[0].SetKey(10.0f, -10.0f, Interp::EaseInOut);
        camRot.Channels[1].SetKey(0.0f, 42.0f, Interp::EaseInOut);
        camRot.Channels[1].SetKey(5.0f, 0.0f, Interp::Smooth);
        camRot.Channels[1].SetKey(10.0f, -40.0f, Interp::EaseInOut);

        Track& fov = m_doc.EnsureTrack(camera, Property::CameraFov);
        fov.Channels[0].SetKey(0.0f, 52.0f, Interp::EaseInOut);
        fov.Channels[0].SetKey(5.0f, 38.0f, Interp::EaseInOut);
        fov.Channels[0].SetKey(10.0f, 46.0f, Interp::EaseInOut);

        // Перевод фокуса: сначала резок реквизит на переднем плане, потом фокус
        // приходит на персонажа. Ради этого глубина резкости и делалась
        // анимируемой — статичная камера показала бы её как простое размытие.
        //
        // Числа не «на глаз»: это РАССТОЯНИЯ до того, что должно быть резким в
        // этот момент, посчитанные от ключей позиции камеры. Кадр с фокусом
        // мимо всей геометрии выглядит не как малая глубина резкости, а как
        // испорченный рендер — что и получилось с первого раза.
        Track& focus = m_doc.EnsureTrack(camera, Property::CameraFocusDistance);
        focus.Channels[0].SetKey(0.0f, 7.6f, Interp::EaseInOut);   // куб на переднем плане
        focus.Channels[0].SetKey(2.5f, 10.4f, Interp::EaseInOut);  // фокус уходит на персонажа
        focus.Channels[0].SetKey(5.5f, 8.8f, Interp::EaseInOut);   // камера подошла ближе
        focus.Channels[0].SetKey(10.0f, 9.4f, Interp::EaseInOut);

        // Оптика камеры включается явно: без этого дорожки фокуса и диафрагмы
        // были бы «красивыми кривыми ни о чём».
        if (GameObject cam = m_scene->Get(camera); cam.Valid()) {
            if (CineCameraComponent* cine =
                    m_scene->Registry().try_get<CineCameraComponent>(cam.Entity())) {
                // ГЛУБИНА РЕЗКОСТИ ВЫКЛЮЧЕНА, и это осознанно.
                //
                // Реализация в движке — экранное размытие по кругу нерезкости, и
                // на сцене из гладких primitive'ов без текстур оно съедает
                // единственное, за что цепляется глаз, — чёткую кромку силуэта.
                // В кадре с настоящей моделью и текстурами размытие фона
                // читается как оптика; здесь оно читается как «нерезкий рендер».
                // Демонстрационный ролик должен показывать, что инструмент даёт
                // ЧЁТКУЮ картинку, а перевод фокуса всё равно виден по дорожке.
                //
                // Включить и посмотреть: D3D_SHOWCASE_DOF=1.
                cine->DepthOfField = std::getenv("D3D_SHOWCASE_DOF") != nullptr;
                cine->AutoFocus = false;
                // Диафрагма — это f-число: чем больше, тем ГЛУБЖЕ резкость.
                cine->Aperture = 8.0f;
                cine->Bloom = true;
                cine->BloomIntensity = 0.35f;
                cine->Vignette = true;
                cine->VignetteAmount = 0.22f;
                cine->ColorGrading = true;
                // Смаз движения тоже приглушён: камера идёт по дуге весь ролик,
                // и на каждом кадре он размазывал ровно то, что должно быть
                // резким. Хроматическая аберрация выключена совсем — цветная
                // кайма по краям и есть та «пиксельность» на кромках.
                cine->MotionBlur = true;
                cine->MotionBlurAmount = 0.06f;
                cine->ChromaticAberration = false;
            }
        }
    }

    // --- Монтаж: вторая камера и две склейки ---------------------------------
    //
    // Демонстрация собирается теми же вызовами, что доступны из интерфейса, и
    // монтаж здесь не украшение: он гоняет тот самый путь, на котором камера
    // выбирается ПОКАДРОВО в экспорте. Без него склейки проверялись бы только
    // на CPU, а ошибка в связке «дорожка -> рендер» вылезла бы уже в ролике.
    const int closeUp = Create(CreateKind::Camera);
    RenameObject(closeUp, "Cam_CloseUp");
    if (GameObject c = m_scene->Get(closeUp); c.Valid()) {
        // Второй ракурс намеренно с ДРУГОЙ стороны площадки: склейка должна
        // читаться как смена плана, а не как лёгкий сдвиг.
        // Углы посчитаны из геометрии, а не подобраны на глаз: камера в
        // (-4.2, 2.4, -3.4) смотрит на персонажа у начала координат.
        //
        // ЗНАК ТАНГАЖА. Transform::GetMatrix крутит X, потом Y, потом Z, а
        // направление взгляда — это (0,0,-1) через эту матрицу. При
        // ПОЛОЖИТЕЛЬНОМ угле X y-компонента направления становится
        // ОТРИЦАТЕЛЬНОЙ, то есть камера смотрит ВНИЗ. Привычное
        // «pitch = atan2(dy, расстояние)» даёт здесь противоположный знак, и
        // подстановка его напрямую задирает камеру в небо ровно на столько,
        // на сколько собирался наклонить, — что и произошло. Правильно так:
        //     yaw   = atan2(-dx, -dz) = -129°
        //     pitch = -asin(dy / |d|) = +17.5°
        c.GetTransform().Position = {-4.2f, 2.4f, -3.4f};
        c.GetTransform().Rotation = {17.5f, -129.0f, 0.0f};
        if (CameraComponent* cam = m_scene->Registry().try_get<CameraComponent>(c.Entity())) {
            cam->Primary = false;
            cam->Fov = 38.0f; // длиннее основной — крупнее план
        }
    }
    if (camera >= 0 && closeUp >= 0) {
        // Оптика второго ракурса КОПИРУЕТСЯ с основного, а не выставляется
        // заново. Дело не в экономии строк: два ракурса одного фильма обязаны
        // выглядеть одинаково, а два набора настроек рядом неизбежно разъезжаются.
        // Первая версия оставила второй камере значения по умолчанию — а там
        // глубина резкости на f/2.8 с автофокусом и хроматическая аберрация,
        // и весь крупный план вышел мыльным.
        if (GameObject mainCam = m_scene->Get(camera), closeCam = m_scene->Get(closeUp);
            mainCam.Valid() && closeCam.Valid()) {
            auto& reg = m_scene->Registry();
            if (const CineCameraComponent* from = reg.try_get<CineCameraComponent>(mainCam.Entity())) {
                CineCameraComponent& to = reg.get_or_emplace<CineCameraComponent>(closeCam.Entity());
                const float focus = to.FocusDistance;
                to = *from;
                to.FocusDistance = focus; // дистанция фокуса у своего плана своя
            }
        }

        m_doc.SetCut(0.0f, camera);   // общий план с начала
        m_doc.SetCut(5.0f, closeUp);  // на смене клипа уходим на крупный
        m_doc.SetCut(7.5f, camera);   // и возвращаемся к общему
    }

    m_doc.Markers.push_back(Marker{"Смена клипа", 5.0f, 0xFF3FC8E8u});
    m_selection = {hero};
    m_undo.Clear();
    SetCurrentTime(0.0f);
    LOG_INFO("Render") << "Постановка собрана: дорожек " << m_doc.Tracks.size()
                       << ", дорожек клипов " << m_doc.ClipTracks.size()
                       << ", длительность " << m_doc.Duration << " c";
}

void DirectorLayer::BuildDemoAnimation() {
    // Демонстрация собирается ИЗ ТЕХ ЖЕ вызовов, что доступны пользователю:
    // дорожка + ключи. Никаких «специальных» путей — если демо работает,
    // работает и ручная анимация.
    const int object = m_scene->FindByName("Object").Valid() ? m_scene->FindByName("Object").Id() : -1;
    const int camera = m_activeCameraId;
    const int light = m_scene->FindByName("Key_Light").Valid() ? m_scene->FindByName("Key_Light").Id() : -1;
    if (object < 0) return;

    m_doc.Duration = 6.0f;

    // Объект: прыжок по дуге + оборот вокруг своей оси + смена цвета.
    Track& position = m_doc.EnsureTrack(object, Property::Position);
    position.Channels[0].SetKey(0.0f, -3.0f, Interp::EaseInOut);
    position.Channels[0].SetKey(6.0f, 3.0f, Interp::EaseInOut);
    position.Channels[1].SetKey(0.0f, 0.5f, Interp::EaseOut);
    position.Channels[1].SetKey(1.5f, 2.6f, Interp::EaseInOut);
    position.Channels[1].SetKey(3.0f, 0.5f, Interp::EaseIn);
    position.Channels[1].SetKey(4.5f, 2.0f, Interp::EaseInOut);
    position.Channels[1].SetKey(6.0f, 0.5f, Interp::EaseIn);

    Track& rotation = m_doc.EnsureTrack(object, Property::Rotation);
    rotation.Channels[1].SetKey(0.0f, 0.0f, Interp::Linear);
    rotation.Channels[1].SetKey(6.0f, 720.0f, Interp::Linear);

    Track& color = m_doc.EnsureTrack(object, Property::Color);
    color.Channels[0].SetKey(0.0f, 0.78f); color.Channels[0].SetKey(3.0f, 0.30f); color.Channels[0].SetKey(6.0f, 0.78f);
    color.Channels[1].SetKey(0.0f, 0.52f); color.Channels[1].SetKey(3.0f, 0.68f); color.Channels[1].SetKey(6.0f, 0.52f);
    color.Channels[2].SetKey(0.0f, 0.28f); color.Channels[2].SetKey(3.0f, 0.85f); color.Channels[2].SetKey(6.0f, 0.28f);

    // Камера: медленный наезд.
    if (camera >= 0) {
        Track& camPos = m_doc.EnsureTrack(camera, Property::Position);
        camPos.Channels[2].SetKey(0.0f, 9.5f, Interp::EaseInOut);
        camPos.Channels[2].SetKey(6.0f, 6.0f, Interp::EaseInOut);
        Track& fov = m_doc.EnsureTrack(camera, Property::CameraFov);
        fov.Channels[0].SetKey(0.0f, 55.0f, Interp::EaseInOut);
        fov.Channels[0].SetKey(6.0f, 42.0f, Interp::EaseInOut);

        // Перевод фокуса: сначала резок дальний план, затем фокус приходит на
        // объект и к концу снова уходит вглубь.
        Track& focus = m_doc.EnsureTrack(camera, Property::CameraFocusDistance);
        focus.Channels[0].SetKey(0.0f, 18.0f, Interp::EaseInOut);
        focus.Channels[0].SetKey(2.0f, 7.5f, Interp::EaseInOut);
        focus.Channels[0].SetKey(4.0f, 7.5f, Interp::EaseInOut);
        focus.Channels[0].SetKey(6.0f, 16.0f, Interp::EaseInOut);
    }

    // Свет: пульсация интенсивности.
    if (light >= 0) {
        Track& intensity = m_doc.EnsureTrack(light, Property::LightIntensity);
        intensity.Channels[0].SetKey(0.0f, 1.2f, Interp::Smooth);
        intensity.Channels[0].SetKey(3.0f, 3.0f, Interp::Smooth);
        intensity.Channels[0].SetKey(6.0f, 1.2f, Interp::Smooth);
    }

    // Включаем киношные эффекты на активной камере: демонстрация должна
    // показывать не только движение, но и то, как выглядит кадр.
    if (camera >= 0) {
        GameObject cam = m_scene->Get(camera);
        if (cam.Valid()) {
            CineCameraComponent& cine =
                m_scene->Registry().get_or_emplace<CineCameraComponent>(cam.Entity());
            cine.DepthOfField = true;
            // Автофокус выключен намеренно: ниже дистанция фокуса КЛЮЧУЕТСЯ.
            // Перевод фокуса с одного плана на другой — классический приём, и
            // заодно это проверка того, что анимируемое свойство действительно
            // управляет эффектом, а не просто лежит в файле.
            cine.AutoFocus = false;
            cine.Aperture = 2.2f;       // умеренно открытая: фон мягкий, объект резкий
            cine.MotionBlur = true;
            cine.MotionBlurAmount = 0.45f;
            cine.ChromaticAberration = true;
            cine.ChromaticAmount = 0.35f;
        }
    }

    m_doc.Markers.push_back(Marker{"Пик", 1.5f, 0xFF3FC8E8});
    m_doc.Markers.push_back(Marker{"Смена цвета", 3.0f, 0xFF5CC85C});

    m_selection = {object};
    m_undo.Clear();
    m_dirty = false;
    SetCurrentTime(0.0f);
}

void DirectorLayer::StartRenderJob() {
    m_simpleMode = false;

    const fs::path out(m_job.Output);
    const bool mp4 = out.extension() == ".mp4";
    if (mp4 && !VideoWriter::FfmpegAvailable()) {
        LOG_ERROR("Render") << "Для .mp4 нужен ffmpeg в PATH. Укажите каталог вместо файла — "
                               "получится секвенция PNG.";
        sage::Application::Get().Close();
        return;
    }

    m_renderSettings.OutputFormat = mp4 ? SequenceExporter::Format::Mp4
                                        : SequenceExporter::Format::PngSequence;
    // У ролика имя задаёт файл, у секвенции — каталог: имена кадров внутри него
    // складываются из BaseName и номера.
    m_renderSettings.OutputDir = mp4 ? out.parent_path().string() : out.string();
    if (m_renderSettings.OutputDir.empty()) m_renderSettings.OutputDir = ".";
    m_renderSettings.BaseName = mp4 ? out.stem().string() : "frame";
    m_renderSettings.Width = m_job.Width;
    m_renderSettings.Height = m_job.Height;
    m_renderSettings.Samples = std::max(1, m_job.Samples);
    m_renderSettings.Quality = m_job.Quality;
    m_renderSettings.StartTime = m_job.StartTime;
    m_renderSettings.EndTime = m_job.EndTime;
    m_renderSettings.IncludeAudio = m_doc.Audio.Loaded();
    // Пакетному рендеру некому показывать прогресс, и растягивать его по кадрам
    // приложения незачем: пишем помногу за раз.
    m_renderSettings.FramesPerStep = 8;
    if (m_job.Fps > 0.0f) m_doc.Fps = m_job.Fps;

    StartRender();
    if (!m_exporter.Active()) {
        LOG_ERROR("Render") << "Рендер не запустился: " << m_status;
        sage::Application::Get().Close();
        return;
    }
    m_jobStarted = true;
    LOG_INFO("Render") << "Пакетный рендер: " << m_exporter.TotalFrames() << " кадр(ов) "
                       << m_job.Width << "x" << m_job.Height << " при " << m_doc.Fps
                       << " fps, сглаживание x" << m_renderSettings.Samples << " -> "
                       << m_job.Output;
}

void DirectorLayer::FinishRenderJob() {
    std::error_code ec;
    if (m_exporter.Failed()) {
        LOG_ERROR("Render") << "Рендер провален: " << m_exporter.Error();
    } else if (m_renderSettings.OutputFormat == SequenceExporter::Format::Mp4) {
        const uintmax_t size = fs::file_size(m_exporter.ResultPath(), ec);
        if (ec || size == 0) {
            LOG_ERROR("Render") << "Файл ролика пуст или не создан: " << m_exporter.ResultPath();
        } else {
            LOG_INFO("Render") << "Готово: " << m_exporter.ResultPath() << " ("
                               << (size / 1024) << " КБ)";
        }
    } else {
        LOG_INFO("Render") << "Готово: секвенция в " << m_renderSettings.OutputDir;
    }
    sage::Application::Get().Close();
}

void DirectorLayer::StartSmokeTest() {
    m_smokeTest = true;
    m_simpleMode = false; // проверяем и продвинутый интерфейс (граф, дорожки)
    BuildDemoAnimation();

    // Маленькое разрешение и всего несколько кадров: цель — доказать, что
    // конвейер «документ -> сцена -> кадр -> файл» работает целиком, а не
    // померить скорость.
    m_renderSettings.Width = 320;
    m_renderSettings.Height = 180;
    m_renderSettings.StartTime = 0.0f;
    // Полсекунды: кодировщику нужно больше одного кадра, чтобы проявились
    // ошибки размера и порядка строк, но ролик всё равно рендерится мгновенно.
    m_renderSettings.EndTime = 11.0f / m_doc.Fps; // двенадцать кадров: 0…11
    m_renderSettings.OutputDir = "smoke_render";
    m_renderSettings.BaseName = "smoke";

    // Проверяем ровно тот путь вывода, которым программа пользуется по
    // умолчанию, — то есть MP4. Там, где кодировщика нет (часть CI-раннеров),
    // откатываемся на секвенцию: без этого проверка падала бы не из-за нашего
    // кода. Переменная позволяет прогнать оба пути вручную.
    const char* format = std::getenv("D3D_SMOKE_FORMAT");
    const bool wantMp4 = format ? std::strcmp(format, "mp4") == 0 : VideoWriter::FfmpegAvailable();
    m_renderSettings.OutputFormat = wantMp4 ? SequenceExporter::Format::Mp4
                                            : SequenceExporter::Format::PngSequence;
    m_renderSettings.IncludeAudio = false; // в демо-проекте звуковой дорожки нет
    if (const char* samples = std::getenv("D3D_SMOKE_SAMPLES")) {
        m_renderSettings.Samples = std::atoi(samples);
    }
    StartRender();

    // Если рендер не запустился, ждать его завершения бессмысленно: без этой
    // проверки проверка не падала, а висела до таймаута CI — и причина
    // («не нашёлся кодировщик») терялась в общем логе.
    if (!m_exporter.Active()) {
        LOG_ERROR("Smoke") << "Сквозная проверка ПРОВАЛЕНА: рендер не запустился — " << m_status;
        m_smokeTest = false;
        sage::Application::Get().Close();
    }
}

void DirectorLayer::FinishSmokeTest() {
    const bool mp4 = m_renderSettings.OutputFormat == SequenceExporter::Format::Mp4;
    const int expected = m_exporter.TotalFrames();
    int found = 0;
    std::error_code ec;

    // Кадры секвенции сверяем ПОБАЙТОВО между собой. «Файл есть и не пуст» —
    // проверка слабее, чем кажется: она пройдёт и тогда, когда дорожки перестали
    // применяться к сцене и все двенадцать кадров вышли одинаковыми. А это как
    // раз самая опасная поломка — молчаливая. PNG детерминирован, поэтому
    // одинаковая картинка даёт одинаковые байты, и различие файлов означает
    // различие кадров.
    int distinctFrames = 0;
    bool framesMove = false;
    if (mp4) {
        // У ролика проверяем не «файл есть», а «файл не пуст»: оборванный
        // кодировщик оставляет нулевой файл, который выглядит как результат.
        const uintmax_t size = fs::file_size(m_exporter.ResultPath(), ec);
        if (!ec && size > 0) {
            found = expected;
            LOG_INFO("Smoke") << "Ролик записан: " << m_exporter.ResultPath() << " (" << size
                              << " байт)";
        }
    } else if (fs::is_directory(m_renderSettings.OutputDir, ec)) {
        std::vector<fs::path> frames;
        for (const fs::directory_entry& entry : fs::directory_iterator(m_renderSettings.OutputDir, ec)) {
            // Пустой файл — это не отрендеренный кадр, а следы падения на
            // середине записи, поэтому проверяем и размер.
            if (entry.path().extension() == ".png" && entry.file_size(ec) > 0) {
                ++found;
                frames.push_back(entry.path());
            }
        }
        std::sort(frames.begin(), frames.end()); // имена нумерованные — порядок по имени и есть порядок кадров

        std::vector<size_t> hashes;
        hashes.reserve(frames.size());
        for (const fs::path& frame : frames) {
            std::ifstream file(frame, std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
            hashes.push_back(std::hash<std::string>{}(bytes));
        }
        std::vector<size_t> unique = hashes;
        std::sort(unique.begin(), unique.end());
        unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
        distinctFrames = (int)unique.size();

        // Требуем, чтобы РАЗНЫМИ были почти все кадры, а не только первый с
        // последним: одного отличия хватило бы и сцене, где что-то дёрнулось
        // единственный раз, а дорожки при этом стоят.
        framesMove = distinctFrames >= (int)frames.size() - 1 && frames.size() > 2;
        LOG_INFO("Smoke") << "Различных кадров: " << distinctFrames << " из " << frames.size();
    }

    if (found >= expected && !m_exporter.Failed() && (mp4 || framesMove)) {
        LOG_INFO("Smoke") << "Сквозная проверка пройдена (" << (mp4 ? "MP4" : "PNG")
                          << "): кадров записано " << found << " из " << expected << ", дорожек "
                          << m_doc.Tracks.size();
    } else if (found >= expected && !m_exporter.Failed() && !framesMove) {
        LOG_ERROR("Smoke") << "Сквозная проверка ПРОВАЛЕНА: кадры записаны, но не отличаются друг "
                              "от друга (" << distinctFrames << " различных) — дорожки не двигают сцену";
    } else {
        LOG_ERROR("Smoke") << "Сквозная проверка ПРОВАЛЕНА (" << (mp4 ? "MP4" : "PNG")
                           << "): кадров " << found << " из " << expected
                           << (m_exporter.Failed() ? (", ошибка: " + m_exporter.Error()) : "");
    }
    // Скриншот интерфейса с наполненным таймлайном снимаем ПОСЛЕ рендера —
    // так на нём видно и дорожки, и результат.
    if (m_autoScreenshotFrame <= 0) sage::Application::Get().Close();
    m_smokeTest = false;
}

// ============================================================================
//  Кадр
// ============================================================================

void DirectorLayer::OnUpdate(float dt) {
    // Выбор кости из переменной окружения откладывается до загрузки модели:
    // раньше скелета ещё нет и выбирать нечего.
    if (m_pendingBoneSelect > 0 && SkeletonOf(*m_scene, SelectedId())) {
        SelectBone(SelectedId(), m_pendingBoneSelect);
        m_pendingBoneSelect = -1;
        if (m_pendingBoneKeys) {
            m_pendingBoneKeys = false;
            const int id = SelectedId(), joint = m_bone.Joint;
            SetCurrentTime(0.0f);
            const float a[3] = {0.0f, 0.0f, 0.0f};
            WriteBoneChannel(*m_scene, id, joint, BoneChannel::Rotation, a);
            m_doc.KeyBone(*m_scene, id, joint, 0.0f);
            const float b[3] = {0.0f, 0.0f, 55.0f};
            WriteBoneChannel(*m_scene, id, joint, BoneChannel::Rotation, b);
            m_doc.KeyBone(*m_scene, id, joint, 2.0f);
            SetCurrentTime(1.0f);
        }
    }

    // Выгрузка в glTF из командной строки — после ожидания загрузки моделей.
    if (m_gltfWaitFrames >= 0 && ++m_gltfWaitFrames > 4) {
        m_gltfWaitFrames = -1;
        std::string err;
        const bool ok = ExportAnimationToGltf(m_job.GltfOutput, err);
        if (!ok) LOG_ERROR("glTF") << "Экспорт не удался: " << err;
        RequestQuit();
        return;
    }

    // Проверка костей ждёт, пока движок догрузит модель персонажа: скелет
    // появляется только после этого. Пара кадров — и он на месте.
    if (m_boneCheckFrames >= 0 && ++m_boneCheckFrames > 3) {
        m_boneCheckFrames = -1;
        RunBoneCheck();
        return;
    }

    if (m_statusTimer > 0.0f) {
        m_statusTimer -= dt;
        if (m_statusTimer <= 0.0f) m_status.clear();
    }

    // Экспорт секвенции забирает кадр целиком: сцена в это время ставится на
    // каждый экспортируемый момент, и обычное проигрывание туда лезть не должно.
    if (m_exporter.Active()) {
        if (!m_exporter.Step(*m_scene, m_renderer, m_doc)) {
            if (m_exporter.Failed()) SetStatus("Рендер прерван: " + m_exporter.Error());
            else SetStatus("Рендер завершён: " + m_exporter.ResultPath());
            m_playback.SetTime(m_timeBeforeRender, m_doc.Duration);
            ApplyDocument(true);
            if (m_smokeTest) FinishSmokeTest();
            if (m_jobStarted) { m_jobStarted = false; FinishRenderJob(); }
            // Очередь подхватывает следующее задание сразу: пауза между ними
            // означала бы, что оператор снова ждёт у экрана — ровно то, ради
            // чего очередь и заводилась.
            if (m_queue.Running()) {
                m_queue.FinishCurrent(m_exporter.Failed() ? m_exporter.Error() : std::string());
                if (!StartNextQueued()) {
                    SetStatus("Очередь выполнена: заданий " + std::to_string(m_queue.Jobs.size()));
                }
            }
        }
        return;
    }

    // Проигрывание: сдвигаем головку и переприменяем документ.
    const bool moved = m_playback.Advance(dt, m_doc.Duration);
    if (moved) ApplyDocument(m_playback.Seeking());

    // Скелетные аниматоры движка тикают ПОСЛЕ применения документа: документ
    // говорит, ЧТО играет, а движок продвигает позу и ведёт кросс-фейд.
    sage::anim::UpdateAnimators(*m_scene, m_playback.Playing() ? dt : 0.0f);
    sage::fx::UpdateEmitters(*m_scene, m_renderer.Particles(), dt);

    // Звук ведём за головкой: старт проигрывания запускает дорожку, остановка
    // глушит её. Посэмпловой синхронизации с головкой движок не даёт, поэтому
    // звук честно живёт «примерно с картинкой» — для расстановки ключей по
    // ударам этого хватает, а волна на таймлайне точна до сэмпла.
    if (m_playback.Playing() != m_audioWasPlaying) {
        if (m_playback.Playing() && m_doc.Audio.Loaded() && !m_doc.Audio.Muted) {
            m_audio.PlayMusic(m_doc.Audio.Path(), m_doc.Audio.Volume, /*loop=*/false);
        } else {
            m_audio.StopMusic();
        }
        m_audioWasPlaying = m_playback.Playing();
    }
    m_audio.Update();
}

void DirectorLayer::OnRender() {
    if (m_exporter.Active()) return; // кадр занят экспортом

    LightingEnvironment env = CollectVisibleLighting(*m_scene);
#ifdef D3D_PROFILE_FRAME
    auto t0 = std::chrono::steady_clock::now();
#endif
    m_renderer.RenderShadow(*m_scene, env);
#ifdef D3D_PROFILE_FRAME
    auto t1 = std::chrono::steady_clock::now();
#endif
    // Кадр снимается ЭФФЕКТИВНОЙ камерой: если на этом времени стоит склейка,
    // берётся она, иначе — ручной выбор. Так перемотка показывает готовый
    // монтаж, ничего не записывая в m_activeCameraId: ручной выбор человека
    // остаётся его выбором.
    const int shotCamera = EffectiveCameraId();
    m_renderer.RenderStage(*m_scene, m_camera, env, m_shading, m_preset, shotCamera,
                           m_overlays, m_selection, m_view, m_proj);
#ifdef D3D_PROFILE_FRAME
    auto t2 = std::chrono::steady_clock::now();
#endif
    m_renderer.RenderCameraView(*m_scene, env, shotCamera);

    // Шаг времени закрыт: оба вида этого момента нарисованы, и положение
    // объектов можно запомнить как «прошлое» для смаза движения. Раньше —
    // нельзя: второй вид сравнил бы мир сам с собой и получил нули.
    m_renderer.EndTimeStep();

    // Экранный буфер очищаем сами: превью-кадры ушли в свои FBO, а окно под
    // интерфейс надо привести в известное состояние (и вернуть ему viewport,
    // который сдвигали проходы теней и пост-обработки).
    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();
    Window& window = sage::Application::Get().GetWindow();
    device.BindDefaultFramebuffer();
    device.SetViewport(0, 0, window.Width(), window.Height());
    device.SetClearColor(0.055f, 0.060f, 0.066f, 1.0f);
    device.Clear();

    DrawUI();
#ifdef D3D_PROFILE_FRAME
    auto t3 = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    LOG_INFO("Prof") << "shadow " << ms(t0, t1) << " stage " << ms(t1, t2)
                     << " view+ui " << ms(t2, t3) << " (stage " << m_renderer.StageWidth()
                     << "x" << m_renderer.StageHeight() << ")";
#endif
}

void DirectorLayer::DrawUI() {
    if (!m_imguiReady) return;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();

    HandleShortcuts();

    // --- Каркас: полноэкранное окно с меню, тулбаром, доком и статус-баром ---
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##DirectorRoot", nullptr,
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar);
    ImGui::PopStyleVar(3);

    m_menuBar.Draw(*this);

    // --- Тулбар ---
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
    ImGui::BeginChild("##toolbar", ImVec2(0.0f, Theme::kToolbarHeight), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetCursorPos(ImVec2(6.0f, 4.0f));
    m_toolbar.Draw(*this);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // --- Док ---
    const ImGuiID dockspaceId = ImGui::GetID("D3DDockspace");
    const float dockHeight = ImGui::GetContentRegionAvail().y - Theme::kStatusBarHeight;
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, dockHeight), ImGuiDockNodeFlags_None);
    if (!m_dockBuilt) {
        m_dockBuilt = true;
        // Раскладку строим только при первом запуске: если ini уже есть,
        // пользователь свою раскладку настроил, и перетирать её нельзя.
        if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr ||
            ImGui::DockBuilderGetNode(dockspaceId)->IsEmpty()) {
            BuildDockLayout(dockspaceId);
        }
    }

    // --- Статус-бар ---
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
    ImGui::BeginChild("##statusbar", ImVec2(0.0f, Theme::kStatusBarHeight), false,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2(8.0f, 2.0f));
    m_statusBar.Draw(*this);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::End(); // ##DirectorRoot

    // --- Панели ---
    m_scenePanel.Draw(*this);
    m_assetsPanel.Draw(*this);
    m_stage.DrawViewport(*this);
    m_stage.DrawRenderView(*this);
    m_properties.Draw(*this);
    m_world.Draw(*this);

    m_timeline.Draw(*this);
    m_dialogs.Draw(*this);

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Снимок окна берём ЗДЕСЬ: кадр уже целиком в back buffer, но ещё не
    // отдан на экран (SwapBuffers делает Application после всех слоёв).
    ++m_frameCounter;
    if (m_autoScreenshotFrame > 0 && m_frameCounter >= m_autoScreenshotFrame) {
        m_autoScreenshotFrame = 0; // один снимок за запуск
        Window& window = sage::Application::Get().GetWindow();
        SaveScreenshot(m_autoScreenshotPath, window.Width(), window.Height());
        sage::Application::Get().Close();
    }

    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* backup = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backup);
    }
}

void DirectorLayer::BuildDockLayout(unsigned int dockspaceId) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    // Раскладка повторяет референс: слева дерево сцены и ассеты, справа
    // свойства, в центре вьюпорт, под ним таймлайн (транспорт — его первая
    // строка, а не отдельное окно: ради одного ряда кнопок он забирал целую
    // вкладку с заголовком и полоской прокрутки).
    // ВАЖНО: у каждого сплита забираем ОБА узла. Если не забрать «остаток»
    // (последний параметр), переменная продолжает указывать на узел, который
    // после сплита стал РОДИТЕЛЬСКИМ, и окно, пристыкованное к нему, накрывает
    // собой всю область — вместо своей половины.
    ImGuiID center = dockspaceId;
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.36f, nullptr, &center);

    ImGuiID leftTop = left;
    const ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.48f, nullptr, &leftTop);

    ImGui::DockBuilderDockWindow("Scene", leftTop);
    ImGui::DockBuilderDockWindow("Assets", leftBottom);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderDockWindow("Render View", center);
    ImGui::DockBuilderDockWindow("Properties", right);
    ImGui::DockBuilderDockWindow("World", right);
    ImGui::DockBuilderDockWindow("Timeline", bottom);
    ImGui::DockBuilderFinish(dockspaceId);
}

// ============================================================================
//  Горячие клавиши
// ============================================================================

void DirectorLayer::HandleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    // Пока набирают текст или открыта модалка — клавиши принадлежат им.
    if (io.WantTextInput || m_dialogs.AnyOpen()) return;

    const bool ctrl = io.KeyCtrl;
    const bool shift = io.KeyShift;

    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N)) OpenDialog(Dialog::NewProject);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O)) OpenDialog(Dialog::OpenProject);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
        if (shift || m_projectPath.empty()) {
            OpenDialog(Dialog::SaveProjectAs);
        } else {
            std::string err;
            if (SaveProject(m_projectPath, err)) m_statusBar.NoteSaved();
            else SetStatus("Не сохранилось: " + err);
        }
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_I)) OpenDialog(Dialog::ImportAsset);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) Undo();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) Redo();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D)) DuplicateSelected();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_K)) {
        m_autoKey = !m_autoKey;
        SetStatus(m_autoKey ? "Авто-ключ включён" : "Авто-ключ выключен");
    }

    if (!ctrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
            if (shift) { m_playback.Stop(); SetCurrentTime(0.0f); }
            else m_playback.TogglePlay();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) StepFrames(-1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) StepFrames(1);
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) SetCurrentTime(0.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_End)) SetCurrentTime(m_doc.Duration);
        if (ImGui::IsKeyPressed(ImGuiKey_Comma)) {
            float t = 0.0f;
            if (m_doc.PrevKeyTime(CurrentTime(), t)) SetCurrentTime(t);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Period)) {
            float t = 0.0f;
            if (m_doc.NextKeyTime(CurrentTime(), t)) SetCurrentTime(t);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_K)) KeySelected();
        if (ImGui::IsKeyPressed(ImGuiKey_L)) m_playback.Loop = !m_playback.Loop;
        if (ImGui::IsKeyPressed(ImGuiKey_M)) {
            PushUndo();
            Marker marker;
            marker.Time = CurrentTime();
            marker.Name = "Marker " + std::to_string(m_doc.Markers.size() + 1);
            m_doc.Markers.push_back(marker);
            SetStatus("Метка поставлена");
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F12)) StartRender();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            // Esc уходит на один уровень вверх: сперва снимает кость, и только
            // потом — сам объект. Иначе выйти из правки скелета, не потеряв
            // персонажа, было бы нечем.
            if (m_bone.Valid()) SelectBone(m_bone.EntityId, -1);
            else ClearSelection();
        }
        // Delete во вьюпорте/дереве удаляет объект; в таймлайне его перехватывает
        // сама панель (там Delete убирает ключи).
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow)) {
            DeleteSelected();
        }

        if (ImGui::IsKeyPressed(ImGuiKey_1)) m_preset = ViewPreset::Perspective;
        if (ImGui::IsKeyPressed(ImGuiKey_2)) m_preset = ViewPreset::Front;
        if (ImGui::IsKeyPressed(ImGuiKey_3)) m_preset = ViewPreset::Side;
        if (ImGui::IsKeyPressed(ImGuiKey_4)) m_preset = ViewPreset::Top;
        if (ImGui::IsKeyPressed(ImGuiKey_0)) m_preset = ViewPreset::SceneCamera;
    }
}

// ============================================================================
//  Выбор
// ============================================================================

void DirectorLayer::SetSelectedId(int id) {
    m_selection.clear();
    if (id >= 0 && m_scene->Get(id).Valid()) m_selection.push_back(id);
}

GameObject DirectorLayer::SelectedObject() {
    return m_selection.empty() ? GameObject() : m_scene->Get(m_selection.back());
}

bool DirectorLayer::IsSelected(int id) const {
    return std::find(m_selection.begin(), m_selection.end(), id) != m_selection.end();
}

void DirectorLayer::ToggleSelection(int id) {
    auto it = std::find(m_selection.begin(), m_selection.end(), id);
    if (it != m_selection.end()) m_selection.erase(it);
    else if (m_scene->Get(id).Valid()) m_selection.push_back(id);
}

void DirectorLayer::ValidateSelection() {
    m_selection.erase(std::remove_if(m_selection.begin(), m_selection.end(),
                                     [this](int id) { return !m_scene->Get(id).Valid(); }),
                      m_selection.end());
    // Выбранная кость могла пережить замену сцены (загрузка проекта, откат),
    // где персонажа уже нет или у него другой скелет.
    if (m_bone.Valid()) {
        const sage::anim::Skeleton* sk = SkeletonOf(*m_scene, m_bone.EntityId);
        // Скелета может ещё не быть (модель грузится) — это не повод терять
        // выбор; сбрасываем только заведомо неверное.
        if (!m_scene->Get(m_bone.EntityId).Valid() || (sk && m_bone.Joint >= sk->Count())) {
            m_bone.Clear();
        }
    }
    if (!m_scene->Get(m_activeCameraId).Valid()) {
        // Активная камера пропала — берём любую другую, иначе Render View и
        // экспорт остались бы «без глаз» без объяснения.
        m_activeCameraId = -1;
        auto view = m_scene->Registry().view<CameraComponent, IdComponent>();
        for (auto e : view) {
            m_activeCameraId = view.get<IdComponent>(e).Id;
            break;
        }
    }
}

void DirectorLayer::PickAtStage(float u, float v, bool additive) {
    glm::vec3 origin, dir;
    ScreenRay(m_view, m_proj, u, v, origin, dir);

    int bestId = -1;
    float bestDistance = 1e30f;

    auto& reg = m_scene->Registry();
    auto view = reg.view<Transform, IdComponent>();
    for (auto e : view) {
        if (HiddenObjects::IsHidden(*m_scene, e)) continue; // скрытое не кликается

        const glm::mat4 world = m_scene->WorldMatrix(e);
        const glm::mat4 inv = glm::inverse(world);
        const glm::vec3 localOrigin = glm::vec3(inv * glm::vec4(origin, 1.0f));
        const glm::vec3 localDir = glm::normalize(glm::vec3(inv * glm::vec4(dir, 0.0f)));

        float hit = -1.0f;
        const MeshRendererComponent* mr = reg.try_get<MeshRendererComponent>(e);
        if (mr && mr->MeshPtr) {
            // Оболочка меша: центр и радиус превращаем в куб — точного
            // пересечения с треугольниками для выбора мышью не нужно, а
            // считается это в тысячи раз дешевле.
            const glm::vec3 c = mr->MeshPtr->BoundsCenter();
            const float r = mr->MeshPtr->BoundsRadius();
            hit = RayBox(localOrigin, localDir, c - glm::vec3(r), c + glm::vec3(r));
        } else if (reg.any_of<CameraComponent, LightComponent, ParticleEmitterComponent,
                              AnimatedModelComponent>(e)) {
            // У камеры, света и эмиттера меша нет — кликаем по их маркеру.
            hit = RayBox(localOrigin, localDir, glm::vec3(-0.45f), glm::vec3(0.45f));
        }
        if (hit < 0.0f) continue;

        // Расстояние считаем в МИРОВЫХ единицах: масштаб объекта иначе делал бы
        // мелкие объекты «ближе» крупных.
        const glm::vec3 worldHit = glm::vec3(world * glm::vec4(localOrigin + localDir * hit, 1.0f));
        const float distance = glm::length(worldHit - origin);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestId = view.get<IdComponent>(e).Id;
        }
    }

    if (bestId < 0) {
        if (!additive) ClearSelection();
        return;
    }
    if (additive) ToggleSelection(bestId);
    else SetSelectedId(bestId);
}

// ============================================================================
//  Время и ключи
// ============================================================================

void DirectorLayer::ApplyDocument(bool seeking) {
    // Костные дорожки пересаживаются на актуальные индексы костей, как только
    // движок догрузит модель, — но не раньше, поэтому пробуем каждый кадр,
    // пока есть кого ждать. Перебор по именам недёшев, и держать его вечно
    // незачем: как только ждать некого, флаг снимается.
    if (m_rebindBones && m_doc.RebindBoneTracks(*m_scene) == 0) m_rebindBones = false;

    m_doc.Apply(*m_scene, m_playback.Time(), seeking);
    if (seeking) {
        // Перемотка должна показать позу СРАЗУ, не дожидаясь следующего
        // OnUpdate: иначе при паузе картинка отстаёт на кадр от головки.
        sage::anim::UpdateAnimators(*m_scene, 0.0f);
    }
}

void DirectorLayer::SetCurrentTime(float seconds) {
    m_playback.SetTime(Playback::SnapToFrame(seconds, m_doc.Fps), m_doc.Duration);
    ApplyDocument(true);
    m_playback.ClearSeeking();
    // Перемотка — разрыв непрерывности движения. Без сброса истории смаз
    // движения сравнил бы новый кадр с камерой из совсем другого места ролика
    // и размазал бы картинку через весь экран. На экспорт это не влияет: там
    // кадры идут подряд и SetCurrentTime не вызывается.
    m_renderer.ResetMotionHistory();
}

void DirectorLayer::StepFrames(int frames) {
    const float step = frames / std::max(m_doc.Fps, 1.0f);
    SetCurrentTime(m_playback.Time() + step);
}

bool DirectorLayer::KeyProperty(int entityId, Property prop) {
    if (!m_doc.KeyFromScene(*m_scene, entityId, prop, CurrentTime())) return false;
    m_dirty = true;
    return true;
}

// ============================================================================
//  Кости персонажа
// ============================================================================

// Сквозная проверка ручной анимации костей. Проверяется ровно та цепочка,
// которой пользуется аниматор: выбрать кость -> согнуть -> убедиться, что
// поехал ребёнок -> поставить два ключа -> перемотать между ними -> убедиться,
// что поза интерполируется. Всё через те же вызовы, что и интерфейс.
void DirectorLayer::RunBoneCheck() {
    const int id = SelectedId();
    int passed = 0, failed = 0;
    auto check = [&](bool ok, const char* what) {
        if (ok) { ++passed; return; }
        ++failed;
        LOG_ERROR("BoneCheck") << "ПРОВАЛ: " << what;
    };

    const sage::anim::Skeleton* sk = SkeletonOf(*m_scene, id);
    if (!sk || sk->Count() < 3) {
        LOG_ERROR("BoneCheck") << "ПРОВАЛЕНА: у персонажа нет скелета";
        sage::Application::Get().Close();
        return;
    }
    LOG_INFO("BoneCheck") << "Скелет персонажа: костей " << sk->Count();

    // Кость 1 — первая после корня; её ребёнок покажет, что поворот разошёлся
    // по цепочке, а не остался «в себе».
    const int joint = 1;
    int child = -1;
    for (int i = 0; i < sk->Count(); ++i) {
        if (sk->Joints[(size_t)i].Parent == joint) { child = i; break; }
    }
    check(child > 0, "у кости есть ребёнок (иначе проверять нечего)");
    if (child < 0) { sage::Application::Get().Close(); return; }

    SelectBone(id, joint);
    check(SelectedBone().Is(id, joint), "кость выбралась");
    check(SelectedId() == id, "выбор кости не потерял персонажа");

    // Поза, которую даёт КЛИП, — эталон для проверки сброса в конце. Ноль тут
    // не годится: демо-клип гнёт эту кость, и «вернуться к клипу» означает
    // вернуться именно к его значению, а не выпрямиться.
    float clipPose[3] = {0, 0, 0};
    check(ReadBoneChannel(*m_scene, id, joint, BoneChannel::Rotation, clipPose),
          "поза от клипа читается до правки");
    LOG_INFO("BoneCheck") << "Поворот от клипа: " << clipPose[2] << " градусов";

    glm::mat4 before;
    check(BoneWorldMatrix(*m_scene, id, child, before), "мировая матрица ребёнка читается");
    const glm::vec3 childBefore(before[3]);

    // --- Гнём кость ---
    const float bend[3] = {0.0f, 0.0f, 70.0f};
    check(WriteBoneChannel(*m_scene, id, joint, BoneChannel::Rotation, bend), "поворот кости записан");

    glm::mat4 after;
    check(BoneWorldMatrix(*m_scene, id, child, after), "матрица ребёнка читается после правки");
    const glm::vec3 childAfter(after[3]);
    const float moved = glm::length(childAfter - childBefore);
    check(moved > 0.05f, "поворот кости сдвинул её ребёнка");
    LOG_INFO("BoneCheck") << "Ребёнок сместился на " << moved << " м";

    // Прочитанное обратно значение должно совпасть с записанным — иначе круг
    // «углы -> кватернион -> поза -> углы» где-то теряет данные.
    float readBack[3] = {0, 0, 0};
    check(ReadBoneChannel(*m_scene, id, joint, BoneChannel::Rotation, readBack),
          "поворот кости читается обратно");
    check(std::fabs(readBack[2] - 70.0f) < 0.5f, "прочитанный поворот совпал с записанным");

    // Корень трогать не просили — он не должен был поехать.
    glm::mat4 rootWorld;
    check(BoneWorldMatrix(*m_scene, id, 0, rootWorld), "матрица корня читается");
    check(glm::length(glm::vec3(rootWorld[3]) - glm::vec3(m_scene->WorldMatrix(
              m_scene->Get(id).Entity())[3])) < 0.01f,
          "нетронутая корневая кость осталась на месте");

    // --- Ключи и перемотка ---
    SetCurrentTime(0.0f);
    const float straight[3] = {0.0f, 0.0f, 0.0f};
    WriteBoneChannel(*m_scene, id, joint, BoneChannel::Rotation, straight);
    check(KeyBone() > 0, "ключ на кости поставлен в начале");

    SetCurrentTime(1.0f);
    const float bent[3] = {0.0f, 0.0f, 80.0f};
    WriteBoneChannel(*m_scene, id, joint, BoneChannel::Rotation, bent);
    check(KeyBone() > 0, "ключ на кости поставлен в конце");
    check(m_doc.HasBoneTracks(id), "костные дорожки появились в документе");

    // Перемотка на середину: документ обязан САМ поставить позу между ключами.
    SetCurrentTime(0.5f);
    float middle[3] = {0, 0, 0};
    check(ReadBoneChannel(*m_scene, id, joint, BoneChannel::Rotation, middle),
          "поза читается на середине");
    check(middle[2] > 5.0f && middle[2] < 75.0f,
          "на середине поза между ключами, а не на одном из них");
    LOG_INFO("BoneCheck") << "Поворот в середине: " << middle[2] << " градусов";

    // Возврат в начало должен дать ровно первый ключ.
    SetCurrentTime(0.0f);
    float atStart[3] = {0, 0, 0};
    ReadBoneChannel(*m_scene, id, joint, BoneChannel::Rotation, atStart);
    check(std::fabs(atStart[2]) < 1.0f, "в начале поза вернулась к первому ключу");

    // --- Блендшейпы ---
    // Морфы идут той же дорогой, что и кости: свойство -> дорожка -> ключ.
    // Проверяем ту же цепочку, потому что ломается она так же тихо.
    {
        AnimatedModelComponent* am =
            m_scene->Registry().try_get<AnimatedModelComponent>(m_scene->Get(id).Entity());
        check(am && am->Model && am->Model->MorphCount() > 0, "у демо-модели есть блендшейпы");
        if (am && am->Model && am->Model->MorphCount() > 0) {
            check((int)am->MorphWeights.size() == am->Model->MorphCount(),
                  "веса блендшейпов заведены по числу целей");

            SetCurrentTime(2.0f);
            const float zero[1] = {0.0f};
            check(WriteProperty(*m_scene, id, Property::MorphWeight, zero, 0),
                  "вес блендшейпа записывается");
            check(m_doc.KeyFromScene(*m_scene, id, Property::MorphWeight, 2.0f, 0),
                  "ключ на блендшейпе ставится");

            SetCurrentTime(3.0f);
            const float full[1] = {1.0f};
            WriteProperty(*m_scene, id, Property::MorphWeight, full, 0);
            m_doc.KeyFromScene(*m_scene, id, Property::MorphWeight, 3.0f, 0);

            SetCurrentTime(2.5f);
            float mid[1] = {-1.0f};
            check(ReadProperty(*m_scene, id, Property::MorphWeight, mid, 0),
                  "вес блендшейпа читается");
            check(mid[0] > 0.1f && mid[0] < 0.9f,
                  "на середине вес блендшейпа между ключами");
            LOG_INFO("BoneCheck") << "Вес блендшейпа в середине: " << mid[0];

            // Выход за 0..1 выворачивает геометрию — запись обязана его зажать.
            const float over[1] = {5.0f};
            WriteProperty(*m_scene, id, Property::MorphWeight, over, 0);
            float clamped[1] = {0.0f};
            ReadProperty(*m_scene, id, Property::MorphWeight, clamped, 0);
            check(clamped[0] <= 1.0f, "вес блендшейпа зажимается в допустимый диапазон");
        }
    }

    // --- Обратная кинематика ---
    // Проверяем ровно тот путь, по которому идёт кнопка «Дотянуться»: цель
    // задаётся в МИРЕ, солвер работает в пространстве модели, результат ложится
    // в те же переопределения позы. Юнит-тесты движка проверяют сам солвер на
    // синтетическом скелете; здесь важно другое — что перевод миру↔модели не
    // теряется и что конец цепочки действительно приезжает в указанную точку.
    {
        const int end = sk->Count() - 1; // кончик щупальца — самая длинная цепочка

        // Корень цепочки заданной длины: столько же шагов вверх по родителям,
        // сколько костей в цепочке (минус сама конечная).
        auto chainRootOf = [&](int length) {
            int j = end;
            for (int i = 1; i < length && sk->Joints[(size_t)j].Parent >= 0; ++i) {
                j = sk->Joints[(size_t)j].Parent;
            }
            return j;
        };

        // Цель, которая ГАРАНТИРОВАННО достижима: поворачиваем текущее положение
        // кончика вокруг корня цепочки. Расстояние до корня остаётся тем же, а
        // раз поза его уже держит — солверу есть куда прийти. Отодвигать точку
        // «на глазок» нельзя: у трёхзвенной цепочки вылет короткий, и цель за её
        // пределами провалила бы проверку, ничего не сказав о самом солвере.
        auto reachableTarget = [&](int chainRoot, const glm::vec3& tip) {
            glm::vec3 base(0.0f);
            BoneWorldPosition(*m_scene, id, chainRoot, base);
            const glm::vec4 turned =
                glm::rotate(glm::mat4(1.0f), glm::radians(25.0f), glm::vec3(0, 1, 0)) *
                glm::vec4(tip - base, 1.0f);
            return base + glm::vec3(turned);
        };

        glm::vec3 tip(0.0f);
        check(BoneWorldPosition(*m_scene, id, end, tip), "мировая позиция конца цепочки читается");
        const int root3 = chainRootOf(3);
        glm::vec3 base3(0.0f);
        BoneWorldPosition(*m_scene, id, root3, base3);
        const float span = glm::length(tip - base3);
        check(span > 0.01f, "цепочка не выродилась в точку");

        const glm::vec3 target = reachableTarget(root3, tip);
        bool reached = false;
        check(SolveBoneIK(*m_scene, id, end, 3, target, nullptr, 1.0f, reached),
              "IK отработал на трёхзвенной цепочке");
        check(reached, "IK дотянулся до цели в пределах досягаемости");

        glm::vec3 afterIk(0.0f);
        BoneWorldPosition(*m_scene, id, end, afterIk);
        const float miss = glm::length(afterIk - target);
        LOG_INFO("BoneCheck") << "Промах IK: " << miss << " м (вылет цепочки " << span << " м)";
        check(miss < span * 0.05f, "конец цепочки встал в цель");

        // Недостижимая цель: конечность обязана вытянуться в её сторону, а не
        // сложиться или улететь. Признак — конец стал ДАЛЬШЕ от корня.
        const glm::vec3 far = base3 + glm::normalize(target - base3) * (span * 10.0f);
        bool farReached = true;
        check(SolveBoneIK(*m_scene, id, end, 3, far, nullptr, 1.0f, farReached),
              "IK отработал на недостижимой цели");
        check(!farReached, "недостижимая цель помечена как недостигнутая");
        glm::vec3 stretched(0.0f);
        BoneWorldPosition(*m_scene, id, end, stretched);
        check(glm::length(stretched - base3) > span, "цепочка вытянулась в сторону цели");

        // Длинная цепочка идёт через FABRIK — другой код солвера, тот же мост.
        if (sk->Count() >= 5) {
            ResetPose(id);
            ApplyDocument(true);
            glm::vec3 tip5(0.0f);
            BoneWorldPosition(*m_scene, id, end, tip5);
            const int root5 = chainRootOf(5);
            glm::vec3 base5(0.0f);
            BoneWorldPosition(*m_scene, id, root5, base5);
            const glm::vec3 target5 = reachableTarget(root5, tip5);
            bool ok5 = false;
            check(SolveBoneIK(*m_scene, id, end, 5, target5, nullptr, 1.0f, ok5),
                  "IK отработал на пятизвенной цепочке (FABRIK)");
            glm::vec3 afterFabrik(0.0f);
            BoneWorldPosition(*m_scene, id, end, afterFabrik);
            const float miss5 = glm::length(afterFabrik - target5);
            LOG_INFO("BoneCheck") << "Промах FABRIK: " << miss5 << " м";
            check(miss5 < glm::length(tip5 - base5) * 0.05f,
                  "FABRIK привёл конец цепочки к цели");
        }
    }
    ResetPose(id); // IK оставил свою позу — дальше проверяем сброс с чистого листа
    ApplyDocument(true);

    // --- Сброс позы ---
    // Ручная поза снимается, дорожки остаются: следующий Apply снова наложит
    // ключи. Поэтому сверяем СРАЗУ, до применения документа, — и именно с позой
    // клипа, а не с нулём.
    ResetPose(id);
    float afterReset[3] = {0, 0, 0};
    ReadBoneChannel(*m_scene, id, joint, BoneChannel::Rotation, afterReset);
    LOG_INFO("BoneCheck") << "Поворот после сброса: " << afterReset[2] << " градусов";
    check(std::fabs(afterReset[2] - clipPose[2]) < 0.5f, "после сброса кость вернулась к клипу");
    check(std::fabs(afterReset[2]) > 1.0f, "поза клипа не выродилась в ноль (иначе проверка пустая)");

    if (failed == 0) {
        LOG_INFO("BoneCheck") << "Проверка костей ПРОЙДЕНА: " << passed << " из "
                              << (passed + failed);
    } else {
        LOG_ERROR("BoneCheck") << "Проверка костей ПРОВАЛЕНА: провалов " << failed << " из "
                               << (passed + failed);
    }
    sage::Application::Get().Close();
}

void DirectorLayer::SelectBone(int entityId, int joint) {
    if (joint < 0) {
        m_bone.Clear();
        return;
    }
    // Кость выбирается ВНУТРИ персонажа, поэтому сам персонаж тоже становится
    // выбранным: иначе инспектор показывал бы чужой объект, а «навести камеру»
    // летело бы не туда.
    if (!IsSelected(entityId)) m_selection = {entityId};
    m_bone.EntityId = entityId;
    m_bone.Joint = joint;
}

int DirectorLayer::KeyBone() {
    if (!m_bone.Valid()) return 0;
    PushUndo();
    const int keyed = m_doc.KeyBone(*m_scene, m_bone.EntityId, m_bone.Joint, CurrentTime());
    if (keyed > 0) {
        m_dirty = true;
        SetStatus("Ключ на кости " + JointName(*m_scene, m_bone.EntityId, m_bone.Joint));
    } else {
        SetStatus("Скелет ещё не готов — ключить нечего");
    }
    return keyed;
}

int DirectorLayer::KeyWholePose(int entityId) {
    PushUndo();
    const int keyed = m_doc.KeyPose(*m_scene, entityId, CurrentTime());
    if (keyed > 0) {
        m_dirty = true;
        SetStatus("Поза заключена: дорожек " + std::to_string(keyed));
    } else {
        SetStatus("Нечего ключить: поза не тронута");
    }
    return keyed;
}

void DirectorLayer::ResetPose(int entityId) {
    PushUndo();
    ClearPose(*m_scene, entityId);
    m_dirty = true;
    SetStatus("Ручная поза снята — персонаж вернулся к клипу");
}

int DirectorLayer::KeySelected() {
    // Кость выбрана — ключим её, а не персонажа целиком: на то она и выбрана.
    if (m_bone.Valid()) return KeyBone();
    if (m_selection.empty()) return 0;
    PushUndo();

    int keyed = 0;
    for (int id : m_selection) {
        const int existing = m_doc.KeyExistingTracks(*m_scene, id, CurrentTime());
        if (existing > 0) {
            keyed += existing;
            continue;
        }
        // Дорожек ещё нет — заводим самые ожидаемые (трансформ): «поставить
        // ключ» на объекте без дорожек должно что-то делать, а не молчать.
        for (Property prop : {Property::Position, Property::Rotation, Property::Scale}) {
            if (PropertyApplies(*m_scene, id, prop) && KeyProperty(id, prop)) ++keyed;
        }
    }
    m_dirty = true;
    SetStatus(keyed > 0 ? "Ключи поставлены" : "Ключить нечего");
    return keyed;
}

void DirectorLayer::NotifyObjectEdited(int entityId) {
    m_dirty = true;
    if (!m_autoKey) return;
    // Правим кость — авто-ключ идёт на кость, а не на трансформ персонажа:
    // персонаж-то как раз не двигался.
    if (m_bone.Valid() && m_bone.EntityId == entityId) {
        m_doc.KeyBone(*m_scene, entityId, m_bone.Joint, CurrentTime());
        return;
    }
    // Авто-ключ пишет только в СУЩЕСТВУЮЩИЕ дорожки: иначе каждое случайное
    // касание ползунка заводило бы новую дорожку и засоряло проект.
    if (m_doc.KeyExistingTracks(*m_scene, entityId, CurrentTime()) == 0) {
        for (Property prop : {Property::Position, Property::Rotation, Property::Scale}) {
            if (PropertyApplies(*m_scene, entityId, prop)) m_doc.KeyFromScene(*m_scene, entityId, prop, CurrentTime());
        }
    }
}

// ============================================================================
//  Отмена
// ============================================================================

std::string DirectorLayer::Snapshot() {
    return ProjectFile::SnapshotToString(*m_scene, m_doc);
}

void DirectorLayer::RestoreSnapshot(const std::string& snapshot) {
    std::unique_ptr<Scene> restored;
    std::string err;
    if (!ProjectFile::RestoreFromString(snapshot, restored, m_doc, err)) {
        LOG_ERROR("Director") << "Не удалось восстановить состояние: " << err;
        SetStatus("Отмена не удалась: " + err);
        return;
    }
    m_scene = std::move(restored);
    // Сцену подменили целиком — модели персонажей загрузятся заново, и
    // костные дорожки снова должны найти свои кости.
    m_rebindBones = true;
    ValidateSelection();
    ApplyDocument(true);
    m_renderer.ResetMotionHistory(); // сцену подменили — старая камера не в счёт
    m_dirty = true;
}

void DirectorLayer::PushUndo() {
    m_undo.Push(Snapshot());
    m_dirty = true;
}

void DirectorLayer::CaptureUndo() { m_undo.Capture(Snapshot()); }
void DirectorLayer::CommitUndo() { m_undo.Commit(); m_dirty = true; }

void DirectorLayer::TrackLastItem() {
    // Виджет стал активным (мышь на нём зажали) — запоминаем состояние «до»,
    // но в стек ещё не кладём: жест мог закончиться без изменений.
    if (ImGui::IsItemActivated()) CaptureUndo();
    if (ImGui::IsItemEdited()) CommitUndo();
    if (ImGui::IsItemDeactivated() && m_undo.HasPending()) m_undo.DropPending();
}

void DirectorLayer::Undo() {
    std::string snapshot;
    if (!m_undo.Undo(Snapshot(), snapshot)) {
        SetStatus("Отменять нечего");
        return;
    }
    RestoreSnapshot(snapshot);
    SetStatus("Отменено");
}

void DirectorLayer::Redo() {
    std::string snapshot;
    if (!m_undo.Redo(Snapshot(), snapshot)) {
        SetStatus("Повторять нечего");
        return;
    }
    RestoreSnapshot(snapshot);
    SetStatus("Повторено");
}

// ============================================================================
//  Объекты сцены
// ============================================================================

int DirectorLayer::Create(CreateKind kind) {
    auto& reg = m_scene->Registry();
    const char* name = "Object";
    switch (kind) {
        case CreateKind::Camera:         name = "Camera"; break;
        case CreateKind::PointLight:     name = "Point_Light"; break;
        case CreateKind::SpotLight:      name = "Spot_Light"; break;
        case CreateKind::Cube:           name = "Cube"; break;
        case CreateKind::Sphere:         name = "Sphere"; break;
        case CreateKind::Plane:          name = "Plane"; break;
        case CreateKind::Cylinder:       name = "Cylinder"; break;
        case CreateKind::Cone:           name = "Cone"; break;
        case CreateKind::Character:      name = "Character"; break;
        case CreateKind::ParticleEffect: name = "Effect"; break;
        case CreateKind::Group:          name = "Group"; break;
    }

    GameObject obj = m_scene->CreateObject(name);
    const entt::entity e = obj.Entity();
    reg.emplace<StageItemComponent>(e);

    // Новый объект ставим ПЕРЕД камерой вида, а не в начало координат: иначе он
    // появляется вне кадра и его приходится искать.
    obj.GetTransform().Position = m_camera.Position + m_camera.Front * 6.0f;

    switch (kind) {
        case CreateKind::Camera: {
            CameraComponent cam;
            cam.Primary = !m_scene->Registry().view<CameraComponent>().empty() ? false : true;
            reg.emplace<CameraComponent>(e, cam);
            reg.emplace<CineCameraComponent>(e);
            if (m_activeCameraId < 0) m_activeCameraId = obj.Id();
            break;
        }
        case CreateKind::PointLight:
        case CreateKind::SpotLight: {
            LightComponent light;
            light.Kind = kind == CreateKind::SpotLight ? LightComponent::Type::Spot
                                                       : LightComponent::Type::Point;
            reg.emplace<LightComponent>(e, light);
            break;
        }
        case CreateKind::Character: {
            // Без ассета ставим встроенную демо-модель движка: у неё есть
            // скелет и клип, поэтому дорожку клипов можно попробовать сразу.
            AnimatedModelComponent anim;
            anim.Path.clear();
            reg.emplace<AnimatedModelComponent>(e, std::move(anim));
            obj.GetTransform().Position.y = 0.0f;
            break;
        }
        case CreateKind::ParticleEffect: {
            reg.emplace<ParticleEmitterComponent>(e);
            break;
        }
        case CreateKind::Group:
            break; // пустая сущность — просто узел иерархии
        default: {
            MeshRef::Type type = MeshRef::Type::Cube;
            if (kind == CreateKind::Sphere) type = MeshRef::Type::Sphere;
            else if (kind == CreateKind::Plane) type = MeshRef::Type::Plane;
            else if (kind == CreateKind::Cylinder) type = MeshRef::Type::Cylinder;
            else if (kind == CreateKind::Cone) type = MeshRef::Type::Cone;
            MeshRendererComponent& mr = obj.Renderer();
            mr.Ref.type = type;
            mr.MeshPtr = ResourceManager::Instance().GetPrimitive(type);
            break;
        }
    }

    m_selection = {obj.Id()};
    m_dirty = true;
    return obj.Id();
}

void DirectorLayer::DeleteSelected() {
    if (m_selection.empty()) return;
    PushUndo();
    for (int id : m_selection) {
        // Вместе с объектом уходят и его дорожки: иначе в проекте копились бы
        // «висячие» дорожки, которые никуда не пишут.
        m_doc.RemoveTracksOf(id);
        m_scene->RemoveObject(id);
    }
    m_selection.clear();
    ValidateSelection();
    m_dirty = true;
    SetStatus("Удалено");
}

void DirectorLayer::DuplicateSelected() {
    if (m_selection.empty()) return;
    PushUndo();

    auto& reg = m_scene->Registry();
    std::vector<int> created;
    for (int id : m_selection) {
        GameObject src = m_scene->Get(id);
        if (!src.Valid()) continue;
        const entt::entity from = src.Entity();

        GameObject copy = m_scene->CreateObject(src.Name() + "_copy");
        const entt::entity to = copy.Entity();
        copy.GetTransform() = src.GetTransform();
        copy.GetTransform().Position.x += 1.0f; // чтобы копия не пряталась в оригинале

        // Копируем компоненты поштучно: entt не умеет «скопировать сущность»
        // сам, а перечисление здесь держит поведение явным.
        if (const MeshRendererComponent* mr = reg.try_get<MeshRendererComponent>(from)) {
            copy.Renderer() = *mr;
        }
        if (const CameraComponent* c = reg.try_get<CameraComponent>(from)) {
            CameraComponent copyCam = *c;
            copyCam.Primary = false; // главной остаётся оригинал
            reg.emplace<CameraComponent>(to, copyCam);
        }
        if (const CineCameraComponent* c = reg.try_get<CineCameraComponent>(from)) reg.emplace<CineCameraComponent>(to, *c);
        if (const LightComponent* c = reg.try_get<LightComponent>(from)) reg.emplace<LightComponent>(to, *c);
        if (const ParticleEmitterComponent* c = reg.try_get<ParticleEmitterComponent>(from)) reg.emplace<ParticleEmitterComponent>(to, *c);
        if (const StageItemComponent* c = reg.try_get<StageItemComponent>(from)) reg.emplace<StageItemComponent>(to, *c);
        else reg.emplace<StageItemComponent>(to);
        if (const SourceAssetComponent* c = reg.try_get<SourceAssetComponent>(from)) reg.emplace<SourceAssetComponent>(to, *c);
        if (const AnimatedModelComponent* c = reg.try_get<AnimatedModelComponent>(from)) {
            // Копируем ОПИСАНИЕ, а не рантайм: у копии свой Animator и своя
            // загрузка, иначе две сущности делили бы одно состояние позы.
            AnimatedModelComponent anim;
            anim.Path = c->Path;
            anim.DemoSegments = c->DemoSegments;
            anim.Clip = c->Clip;
            anim.Speed = c->Speed;
            anim.Loop = c->Loop;
            anim.Playing = c->Playing;
            anim.BlendTime = c->BlendTime;
            reg.emplace<AnimatedModelComponent>(to, std::move(anim));
        }

        // Родитель у копии тот же — копия остаётся в той же ветке дерева.
        const entt::entity parent = m_scene->ParentOf(from);
        if (parent != entt::null) m_scene->SetParent(to, parent);

        created.push_back(copy.Id());
    }

    if (!created.empty()) {
        m_selection = created;
        m_dirty = true;
        SetStatus("Продублировано");
    }
}

void DirectorLayer::RenameObject(int id, const std::string& name) {
    GameObject obj = m_scene->Get(id);
    if (!obj.Valid() || name.empty()) return;
    PushUndo();
    obj.SetName(name);
    m_dirty = true;
}

void DirectorLayer::SetParentOf(int childId, int parentId) {
    GameObject child = m_scene->Get(childId);
    if (!child.Valid()) return;
    PushUndo();
    if (parentId < 0) {
        m_scene->SetParent(child.Entity(), entt::null);
    } else {
        GameObject parent = m_scene->Get(parentId);
        if (parent.Valid()) m_scene->SetParent(child.Entity(), parent.Entity());
    }
    m_dirty = true;
}

void DirectorLayer::FocusOnSelected() {
    GameObject obj = SelectedObject();
    if (!obj.Valid()) return;

    const glm::vec3 target = glm::vec3(m_scene->WorldMatrix(obj.Entity())[3]);
    // Дистанцию берём по размеру объекта, чтобы и мелкая лампа, и большая
    // модель занимали в кадре примерно одинаковую долю.
    float radius = 1.0f;
    if (const MeshRendererComponent* mr = m_scene->Registry().try_get<MeshRendererComponent>(obj.Entity())) {
        if (mr->MeshPtr) {
            const glm::vec3 scale = glm::abs(obj.GetTransform().Scale);
            radius = mr->MeshPtr->BoundsRadius() * std::max({scale.x, scale.y, scale.z});
        }
    }
    const float distance = std::max(radius * 3.2f, 2.0f);
    m_camera.Position = target - m_camera.Front * distance;
    SetStatus("Камера наведена на " + obj.Name());
}

int DirectorLayer::ImportAsset(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return -1;

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    PushUndo();
    auto& reg = m_scene->Registry();

    if (ext == ".glb" || ext == ".gltf") {
        // Скелетная модель: проверяем загрузку ДО создания сущности, иначе в
        // сцене оставался бы пустой объект после неудачного импорта.
        try {
            std::unique_ptr<sage::render::SkinnedModel> probe = sage::render::SkinnedModel::Load(path.string());
            if (!probe) return -1;
        } catch (const std::exception& e) {
            LOG_ERROR("Director") << "Импорт не удался (" << path.string() << "): " << e.what();
            return -1;
        }

        GameObject obj = m_scene->CreateObject(path.stem().string());
        const entt::entity e = obj.Entity();
        AnimatedModelComponent anim;
        anim.Path = path.string();
        reg.emplace<AnimatedModelComponent>(e, std::move(anim));
        reg.emplace<StageItemComponent>(e);
        reg.emplace<SourceAssetComponent>(e, SourceAssetComponent{path.string()});
        m_selection = {obj.Id()};
        m_dirty = true;
        return obj.Id();
    }

    if (ext == ".obj") {
        std::shared_ptr<Mesh> mesh = ResourceManager::Instance().GetModel(path.string());
        if (!mesh) return -1;

        GameObject obj = m_scene->CreateObject(path.stem().string());
        MeshRendererComponent& mr = obj.Renderer();
        mr.Ref.type = MeshRef::Type::Model;
        mr.Ref.path = path.string();
        mr.MeshPtr = std::move(mesh);
        reg.emplace<StageItemComponent>(obj.Entity());
        reg.emplace<SourceAssetComponent>(obj.Entity(), SourceAssetComponent{path.string()});
        m_selection = {obj.Id()};
        m_dirty = true;
        return obj.Id();
    }

    LOG_ERROR("Director") << "Формат не поддерживается для импорта: " << ext;
    return -1;
}

// ============================================================================
//  Файлы проекта
// ============================================================================

void DirectorLayer::NewProject() {
    BuildDefaultScene();
    SetStatus("Новый проект");
}

bool DirectorLayer::OpenProject(const fs::path& path, std::string& err) {
    std::unique_ptr<Scene> loaded;
    float playhead = 0.0f;
    if (!ProjectFile::Load(path.string(), loaded, m_doc, playhead, err, &m_overlays)) return false;

    m_scene = std::move(loaded);
    m_projectPath = path;
    m_selection.clear();
    m_bone.Clear();
    m_undo.Clear();
    m_playback.Stop();
    m_dirty = false;
    // Скелеты появятся только когда движок догрузит модели — до тех пор
    // костные дорожки не знают своих индексов (см. ApplyDocument).
    m_rebindBones = true;

    // Активной делаем первую камеру загруженной сцены.
    m_activeCameraId = -1;
    ValidateSelection();

    m_assetsDir = path.has_parent_path() ? path.parent_path() : fs::current_path();
    m_assetsPanel.Invalidate();
    SetCurrentTime(playhead);
    m_stage.RequestFocus();
    return true;
}

bool DirectorLayer::SaveProject(const fs::path& path, std::string& err) {
    if (!ProjectFile::Save(path.string(), *m_scene, m_doc, CurrentTime(), err, &m_overlays)) return false;
    m_projectPath = path;
    m_dirty = false;
    m_statusBar.NoteSaved();
    m_assetsPanel.Invalidate();
    return true;
}

bool DirectorLayer::ExportSceneToEngine(const fs::path& path, std::string& err) {
    return ProjectFile::SaveSceneOnly(path.string(), *m_scene, err);
}

bool DirectorLayer::ExportAnimationToGltf(const fs::path& path, std::string& err) {
    gltf::Options options;
    gltf::Result result;
    const bool ok = gltf::Export(path.string(), *m_scene, m_doc, options, result, err);

    // Экспортёр гоняет сцену по времени и оставляет её на последнем снятом
    // кадре — возвращаем на головку САМИ, и в любом случае, включая ошибку:
    // прерваться на середине и бросить сцену в чужом моменте хуже всего.
    SetCurrentTime(CurrentTime());

    if (ok) {
        SetStatus(std::string(T("Анимация экспортирована: дорожек ")) +
                  std::to_string(result.Channels) + T(", кадров ") + std::to_string(result.Samples));
    }
    return ok;
}

// ============================================================================
//  Рендер и прочее
// ============================================================================

void DirectorLayer::StartRender() {
    if (m_exporter.Active()) {
        SetStatus("Рендер уже идёт");
        return;
    }
    m_renderSettings.CameraId = m_activeCameraId;
    m_timeBeforeRender = CurrentTime();
    m_playback.Pause(); // проигрывание и покадровый экспорт одновременно бессмысленны

    std::string err;
    if (!m_exporter.Begin(m_renderSettings, m_doc, *m_scene, err)) {
        SetStatus("Рендер не запущен: " + err);
        return;
    }
    SetStatus("Рендер запущен: " + std::to_string(m_exporter.TotalFrames()) + " кадр(ов)");
}

void DirectorLayer::StartQueue() {
    if (m_queue.Jobs.empty()) {
        SetStatus("Очередь пуста — добавьте задания в настройках рендера");
        return;
    }
    m_queue.Reset(); // прогон с начала: «запустить» значит снять всё заново
    if (!StartNextQueued()) SetStatus("Очередь не запустилась");
}

bool DirectorLayer::StartNextQueued() {
    SequenceExporter::Settings settings;
    if (!m_queue.TakeNext(settings)) return false;

    m_renderSettings = settings;
    m_timeBeforeRender = CurrentTime();
    m_playback.Pause();

    std::string err;
    if (!m_exporter.Begin(m_renderSettings, m_doc, *m_scene, err)) {
        // Провал ОДНОГО задания не должен ронять очередь: следующее может быть
        // в другом формате или с другой камерой и вполне рабочим.
        LOG_ERROR("Queue") << "Задание пропущено: " << err;
        m_queue.FinishCurrent(err);
        return StartNextQueued();
    }
    SetStatus("Очередь: осталось заданий " + std::to_string(m_queue.Remaining()));
    return true;
}

void DirectorLayer::SetStatus(const std::string& message) {
    m_status = message;
    m_statusTimer = 5.0f; // сообщение живёт пять секунд и уходит само
}

void DirectorLayer::RequestQuit() {
    sage::Application::Get().Close();
}

} // namespace d3d
