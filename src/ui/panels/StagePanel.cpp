#include "ui/panels/StagePanel.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>

#include "ImGuizmo.h"
#include "imgui.h"

#include "anim/DirectorComponents.h"
#include "sage/core/Application.h"
#include "sage/scene/Components.h"
#include "ui/DirectorHost.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace d3d {

using Icons::Icon;

namespace {

// Раскладывает мировую матрицу обратно в Transform. Порядок углов ОБЯЗАН
// совпадать с Transform::GetMatrix (T * Rx * Ry * Rz * S), поэтому берём
// glm::extractEulerAngleXYZ, а не декомпозицию ImGuizmo — у неё другой порядок
// осей, и гизмо «прыгал» бы на уже повёрнутом объекте.
void DecomposeToTransform(const glm::mat4& m, Transform& out) {
    out.Position = glm::vec3(m[3]);

    glm::vec3 scale(glm::length(glm::vec3(m[0])),
                    glm::length(glm::vec3(m[1])),
                    glm::length(glm::vec3(m[2])));
    scale = glm::max(scale, glm::vec3(1e-6f)); // вырожденный масштаб ломает inverse
    out.Scale = scale;

    glm::mat4 rot(1.0f);
    rot[0] = glm::vec4(glm::vec3(m[0]) / scale.x, 0.0f);
    rot[1] = glm::vec4(glm::vec3(m[1]) / scale.y, 0.0f);
    rot[2] = glm::vec4(glm::vec3(m[2]) / scale.z, 0.0f);

    float rx, ry, rz;
    glm::extractEulerAngleXYZ(rot, rx, ry, rz);
    out.Rotation = glm::degrees(glm::vec3(rx, ry, rz));
}

const char* PresetName(ViewPreset p) {
    switch (p) {
        case ViewPreset::Perspective: return "Perspective";
        case ViewPreset::Front:       return "Front";
        case ViewPreset::Side:        return "Side";
        case ViewPreset::Top:         return "Top";
        case ViewPreset::SceneCamera: return "Camera";
    }
    return "Perspective";
}

const char* ShadingName(ShadingMode m) {
    switch (m) {
        case ShadingMode::Shaded:    return "Shaded";
        case ShadingMode::Wireframe: return "Wireframe";
        case ShadingMode::Unlit:     return "Unlit";
        case ShadingMode::Normals:   return "Normals";
    }
    return "Shaded";
}

} // namespace

// ============================================================================
//  Панель инструментов вьюпорта
// ============================================================================

void StagePanel::DrawViewToolbar(DirectorHost& host) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));

    ImGui::SetNextItemWidth(130.0f);
    if (ImGui::BeginCombo("##view", PresetName(host.Preset()))) {
        const ViewPreset presets[] = {ViewPreset::Perspective, ViewPreset::Front, ViewPreset::Side,
                                      ViewPreset::Top, ViewPreset::SceneCamera};
        for (ViewPreset p : presets) {
            if (ImGui::Selectable(PresetName(p), host.Preset() == p)) host.Preset() = p;
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo("##shading", ShadingName(host.Shading()))) {
        const ShadingMode modes[] = {ShadingMode::Shaded, ShadingMode::Wireframe,
                                     ShadingMode::Unlit, ShadingMode::Normals};
        for (ShadingMode m : modes) {
            if (ImGui::Selectable(ShadingName(m), host.Shading() == m)) host.Shading() = m;
        }
        ImGui::EndCombo();
    }

    ViewportOverlays& ov = host.Overlays();
    ImGui::SameLine(0.0f, 14.0f);
    if (Icons::IconButton("grid", Icon::Grid, "Сетка (G)", ov.Grid)) ov.Grid = !ov.Grid;
    ImGui::SameLine();
    if (Icons::IconButton("gizmos", Icon::Cube, "Каркасы камер, света и эффектов", ov.Gizmos))
        ov.Gizmos = !ov.Gizmos;
    ImGui::SameLine();
    if (Icons::IconButton("frame", Icon::Camera, "Рамка кадра активной камеры", ov.CameraFrame))
        ov.CameraFrame = !ov.CameraFrame;
    ImGui::SameLine();
    if (Icons::IconButton("safe", Icon::Render, "Безопасная зона", ov.SafeArea)) ov.SafeArea = !ov.SafeArea;
    ImGui::SameLine();
    if (Icons::IconButton("thirds", Icon::Curve, "Сетка третей (композиция кадра)", ov.Thirds))
        ov.Thirds = !ov.Thirds;

    // --- Пространство и привязка гизмо ---
    ImGui::SameLine(0.0f, 14.0f);
    GizmoSpace& space = host.GizmoSpaceRef();
    if (ImGui::SmallButton(space == GizmoSpace::World ? "World" : "Local")) {
        space = space == GizmoSpace::World ? GizmoSpace::Local : GizmoSpace::World;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Пространство манипулятора: оси мира или оси объекта");
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &host.GizmoSnap());

    ImGui::PopStyleVar();
}

// ============================================================================
//  Камера и гизмо
// ============================================================================

void StagePanel::HandleCamera(DirectorHost& host, bool hovered) {
    ImGuiIO& io = ImGui::GetIO();
    Camera& camera = host.ViewCamera();
    const float dt = sage::Application::Get().DeltaTime();

    // ПКМ — осмотр и полёт WASD/QE (как в редакторе SAGE и в любом 3D-пакете).
    // Ведём полёт, пока кнопка зажата, даже если курсор ушёл за край панели —
    // иначе камера «залипала» бы на границе окна.
    const bool rmb = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if ((hovered || m_cameraDriving) && rmb) {
        m_cameraDriving = true;
        camera.ProcessMouse(io.MouseDelta.x, -io.MouseDelta.y);
        const float speed = camera.MovementSpeed * (io.KeyShift ? 3.0f : 1.0f) * dt;
        if (ImGui::IsKeyDown(ImGuiKey_W)) camera.Position += camera.Front * speed;
        if (ImGui::IsKeyDown(ImGuiKey_S)) camera.Position -= camera.Front * speed;
        if (ImGui::IsKeyDown(ImGuiKey_A)) camera.Position -= camera.Right * speed;
        if (ImGui::IsKeyDown(ImGuiKey_D)) camera.Position += camera.Right * speed;
        if (ImGui::IsKeyDown(ImGuiKey_E)) camera.Position += camera.WorldUp * speed;
        if (ImGui::IsKeyDown(ImGuiKey_Q)) camera.Position -= camera.WorldUp * speed;
    } else {
        m_cameraDriving = false;
    }

    // СКМ — панорама: сдвиг камеры в плоскости экрана.
    if (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        const float pan = 0.012f * std::max(1.0f, glm::length(camera.Position) * 0.25f);
        camera.Position -= camera.Right * io.MouseDelta.x * pan;
        camera.Position += camera.Up * io.MouseDelta.y * pan;
    }

    if (hovered && io.MouseWheel != 0.0f) camera.Position += camera.Front * io.MouseWheel * 0.8f;

    // Хоткеи инструментов — только когда не летим и не пишем текст.
    if (hovered && !m_cameraDriving && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Q)) host.GizmoOp() = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_W)) host.GizmoOp() = (int)ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) host.GizmoOp() = (int)ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) host.GizmoOp() = (int)ImGuizmo::SCALE;
        if (ImGui::IsKeyPressed(ImGuiKey_F)) host.FocusOnSelected();
        if (ImGui::IsKeyPressed(ImGuiKey_G)) host.Overlays().Grid = !host.Overlays().Grid;
    }
}

void StagePanel::DrawGizmo(DirectorHost& host, ImVec2 imagePos, ImVec2 imageSize) {
    if (host.GizmoOp() == 0) { m_gizmoWasUsing = false; return; } // режим «только выбор»

    GameObject selected = host.SelectedObject();
    if (!selected.Valid()) { m_gizmoWasUsing = false; return; }

    Scene& scene = host.CurrentScene();
    // Заблокированный объект замком в аутлайнере не двигается: замок для того и
    // ставят, чтобы случайно не сдвинуть выставленный кадр.
    if (const StageItemComponent* item = scene.Registry().try_get<StageItemComponent>(selected.Entity())) {
        if (item->Locked) { m_gizmoWasUsing = false; return; }
    }

    ImGuizmo::SetOrthographic(host.Preset() != ViewPreset::Perspective &&
                              host.Preset() != ViewPreset::SceneCamera);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(imagePos.x, imagePos.y, imageSize.x, imageSize.y);

    Transform& tr = selected.GetTransform();
    const entt::entity parent = scene.ParentOf(selected.Entity());
    const glm::mat4 parentWorld = (parent != entt::null) ? scene.WorldMatrix(parent) : glm::mat4(1.0f);
    glm::mat4 model = scene.WorldMatrix(selected.Entity());

    // Снимок «до» берём, пока гизмо ещё не тащат: первый же кадр перетаскивания
    // уже мутирует Transform, и снимок после него записал бы уже изменённое.
    if (!ImGuizmo::IsUsing() && ImGuizmo::IsOver()) host.CaptureUndo();

    const auto op = (ImGuizmo::OPERATION)host.GizmoOp();
    float snapValues[3];
    const float snapUnit = (op == ImGuizmo::ROTATE) ? 15.0f : (op == ImGuizmo::SCALE ? 0.1f : 0.25f);
    snapValues[0] = snapValues[1] = snapValues[2] = snapUnit;

    // Масштаб ImGuizmo всегда считает локально (WORLD он для scale игнорирует).
    const auto mode = (host.GizmoSpaceRef() == GizmoSpace::World && op != ImGuizmo::SCALE)
                          ? ImGuizmo::WORLD : ImGuizmo::LOCAL;

    const bool usingNow = ImGuizmo::IsUsing();
    if (usingNow && !m_gizmoWasUsing) {
        // Начало жеста: одна запись отмены на всё перетаскивание + снимок
        // мировых матриц всего набора (мультивыделение двигается как целое).
        host.CommitUndo();
        m_dragStartPrimary = scene.WorldMatrix(selected.Entity());
        m_dragStartWorlds.clear();
        for (int id : host.Selection()) {
            GameObject o = scene.Get(id);
            if (o.Valid()) m_dragStartWorlds.push_back({id, scene.WorldMatrix(o.Entity())});
        }
    }

    if (ImGuizmo::Manipulate(glm::value_ptr(host.ViewMatrix()), glm::value_ptr(host.ProjMatrix()),
                             op, mode, glm::value_ptr(model), nullptr,
                             host.GizmoSnap() ? snapValues : nullptr)) {
        const glm::mat4 local = (parent != entt::null) ? glm::inverse(parentWorld) * model : model;
        DecomposeToTransform(local, tr);

        // Мультивыделение: ту же мировую дельту — остальным выбранным.
        if (host.Selection().size() > 1) {
            const glm::mat4 delta = model * glm::inverse(m_dragStartPrimary);
            for (auto& [id, startWorld] : m_dragStartWorlds) {
                if (id == selected.Id()) continue;
                GameObject o = scene.Get(id);
                if (!o.Valid()) continue;
                // Потомков других выбранных пропускаем: их несёт двигающийся
                // родитель, иначе смещение применилось бы дважды.
                bool ancestorSelected = false;
                for (entt::entity a = scene.ParentOf(o.Entity()); a != entt::null; a = scene.ParentOf(a)) {
                    if (host.IsSelected(scene.Registry().get<IdComponent>(a).Id)) {
                        ancestorSelected = true;
                        break;
                    }
                }
                if (ancestorSelected) continue;

                const glm::mat4 newWorld = delta * startWorld;
                const entt::entity p = scene.ParentOf(o.Entity());
                const glm::mat4 pw = (p != entt::null) ? scene.WorldMatrix(p) : glm::mat4(1.0f);
                DecomposeToTransform((p != entt::null) ? glm::inverse(pw) * newWorld : newWorld,
                                     o.GetTransform());
                host.NotifyObjectEdited(id);
            }
        }
        host.NotifyObjectEdited(selected.Id());
    }
    m_gizmoWasUsing = usingNow;
}

// ============================================================================
//  Композиционные направляющие
// ============================================================================

void StagePanel::DrawFramingGuides(DirectorHost& host, ImVec2 imagePos, ImVec2 imageSize) {
    ViewportOverlays& ov = host.Overlays();
    if (!ov.CameraFrame && !ov.SafeArea && !ov.Thirds) return;

    Scene& scene = host.CurrentScene();
    GameObject cam = scene.Get(host.ActiveCameraId());
    if (!cam.Valid()) return;

    // Соотношение сторон кадра берём с киношной камеры: рамка показывает
    // ИМЕННО то, что попадёт в рендер, а не то, какой формы сейчас панель.
    float aspect = 1.85f;
    if (const CineCameraComponent* cine = scene.Registry().try_get<CineCameraComponent>(cam.Entity())) {
        if (!cine->FilmBack) return;
        aspect = cine->AspectRatio > 0.1f ? cine->AspectRatio : aspect;
    }

    // Вписываем прямоугольник нужной пропорции в панель.
    const float panelAspect = imageSize.x / std::max(imageSize.y, 1.0f);
    ImVec2 frameSize = imageSize;
    if (panelAspect > aspect) frameSize.x = imageSize.y * aspect;
    else frameSize.y = imageSize.x / aspect;
    // В режиме «смотрим камерой» рамка совпадает с кадром — тогда прижимаем её
    // с небольшим полем, чтобы линия не сливалась с краем панели.
    const float inset = host.Preset() == ViewPreset::SceneCamera ? 0.0f : 0.08f;
    frameSize.x *= (1.0f - inset);
    frameSize.y *= (1.0f - inset);

    const ImVec2 a(imagePos.x + (imageSize.x - frameSize.x) * 0.5f,
                   imagePos.y + (imageSize.y - frameSize.y) * 0.5f);
    const ImVec2 b(a.x + frameSize.x, a.y + frameSize.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (ov.CameraFrame) {
        // Всё вне кадра затемняем: сразу видно, что в ролик не попадёт.
        const ImU32 dim = IM_COL32(0, 0, 0, 90);
        dl->AddRectFilled(imagePos, ImVec2(imagePos.x + imageSize.x, a.y), dim);
        dl->AddRectFilled(ImVec2(imagePos.x, b.y),
                          ImVec2(imagePos.x + imageSize.x, imagePos.y + imageSize.y), dim);
        dl->AddRectFilled(ImVec2(imagePos.x, a.y), ImVec2(a.x, b.y), dim);
        dl->AddRectFilled(ImVec2(b.x, a.y), ImVec2(imagePos.x + imageSize.x, b.y), dim);
        dl->AddRect(a, b, IM_COL32(235, 235, 235, 170), 0.0f, 0, 1.5f);
    }

    if (ov.SafeArea) {
        // Классические 90% «безопасной зоны действия»: то, что гарантированно
        // видно на любом экране.
        const float mx = frameSize.x * 0.05f, my = frameSize.y * 0.05f;
        dl->AddRect(ImVec2(a.x + mx, a.y + my), ImVec2(b.x - mx, b.y - my),
                    IM_COL32(235, 235, 235, 70), 0.0f, 0, 1.0f);
    }

    if (ov.Thirds) {
        const ImU32 c = IM_COL32(235, 235, 235, 55);
        for (int i = 1; i <= 2; ++i) {
            const float x = a.x + frameSize.x * (float)i / 3.0f;
            const float y = a.y + frameSize.y * (float)i / 3.0f;
            dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), c, 1.0f);
            dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), c, 1.0f);
        }
    }
}

// ============================================================================
//  Вьюпорт
// ============================================================================

void StagePanel::DrawViewport(DirectorHost& host) {
    if (m_focusFrames > 0) {
        ImGui::SetNextWindowFocus();
        --m_focusFrames;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport");
    ImGui::PopStyleVar();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
    ImGui::Indent(6.0f);
    ImGui::Spacing();
    DrawViewToolbar(host);
    ImGui::Unindent(6.0f);
    ImGui::PopStyleVar();
    ImGui::Spacing();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x >= 16.0f && avail.y >= 16.0f) host.SetStageSize((int)avail.x, (int)avail.y);
    const ImVec2 imagePos = ImGui::GetCursorScreenPos();

    // Текстура OpenGL идёт снизу вверх — переворачиваем по V.
    ImGui::Image((ImTextureID)(std::intptr_t)host.Renderer().StageTexture(), avail,
                 ImVec2(0, 1), ImVec2(1, 0));
    const bool hovered = ImGui::IsItemHovered();

    HandleCamera(host, hovered);
    DrawGizmo(host, imagePos, avail);
    DrawFramingGuides(host, imagePos, avail);

    // --- Выбор кликом (не по гизмо и не во время манипуляции) ---
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGuizmo::IsOver() && !ImGuizmo::IsUsing()) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const float u = (mouse.x - imagePos.x) / avail.x;
        const float v = (mouse.y - imagePos.y) / avail.y;
        if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
            host.PickAtStage(u, v, ImGui::GetIO().KeyCtrl);
        }
    }

    // --- Подсказка по управлению в углу ---
    if (ImFont* small = Theme::SmallFont()) ImGui::PushFont(small);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(imagePos.x + 10.0f, imagePos.y + avail.y - 20.0f),
        IM_COL32(255, 255, 255, 90),
        "ПКМ — осмотр и полёт (WASD/QE)   СКМ — панорама   Колесо — приблизить   F — навести");
    if (Theme::SmallFont()) ImGui::PopFont();

    // --- Индикатор авто-ключа: пишущий режим должен быть виден всегда ---
    if (host.AutoKey()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c(imagePos.x + avail.x - 24.0f, imagePos.y + 22.0f);
        dl->AddCircleFilled(c, 7.0f, Theme::Colors::Record, 16);
        dl->AddText(ImVec2(c.x - 74.0f, c.y - 7.0f), Theme::Colors::Record, "AUTO KEY");
    }

    ImGui::End();
}

// ============================================================================
//  Render View
// ============================================================================

void StagePanel::DrawRenderView(DirectorHost& host) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Render View");
    ImGui::PopStyleVar();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 16.0f || avail.y < 16.0f) { ImGui::End(); return; }

    Scene& scene = host.CurrentScene();
    if (!scene.Get(host.ActiveCameraId()).Valid()) {
        ImGui::Spacing();
        ImGui::Indent(12.0f);
        ImGui::TextDisabled("Активной камеры нет.");
        ImGui::Spacing();
        ImGui::TextWrapped("Чистовой кадр снимается с камеры сцены. Создайте камеру "
                           "(Create > Camera) — она станет активной автоматически.");
        if (ImGui::Button("Создать камеру")) host.SetActiveCameraId(host.Create(CreateKind::Camera));
        ImGui::Unindent(12.0f);
        ImGui::End();
        return;
    }

    // Кадр рендерится в соотношении сторон камеры, а не панели: превью должно
    // совпадать с тем, что уйдёт в файл, вплоть до кадрирования.
    float aspect = 1.85f;
    GameObject cam = scene.Get(host.ActiveCameraId());
    if (const CineCameraComponent* cine = scene.Registry().try_get<CineCameraComponent>(cam.Entity())) {
        if (cine->AspectRatio > 0.1f) aspect = cine->AspectRatio;
    }
    ImVec2 size = avail;
    if (avail.x / std::max(avail.y, 1.0f) > aspect) size.x = avail.y * aspect;
    else size.y = avail.x / aspect;

    host.SetRenderViewSize((int)size.x, (int)size.y);
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (avail.x - size.x) * 0.5f,
                               ImGui::GetCursorPosY() + (avail.y - size.y) * 0.5f));
    ImGui::Image((ImTextureID)(std::intptr_t)host.Renderer().RenderViewTexture(), size,
                 ImVec2(0, 1), ImVec2(1, 0));

    // Прогресс экспорта — прямо поверх кадра, который сейчас пишется.
    if (host.Exporter().Active()) {
        SequenceExporter& exporter = host.Exporter();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetItemRectMin();
        const ImVec2 barA(pos.x + 16.0f, pos.y + size.y - 34.0f);
        const ImVec2 barB(pos.x + size.x - 16.0f, barA.y + 16.0f);
        dl->AddRectFilled(barA, barB, IM_COL32(0, 0, 0, 170), 3.0f);
        dl->AddRectFilled(barA, ImVec2(barA.x + (barB.x - barA.x) * exporter.Progress(), barB.y),
                          Theme::Colors::Accent, 3.0f);
        char label[96];
        std::snprintf(label, sizeof(label), "Рендер: кадр %d из %d",
                      exporter.CurrentFrame(), exporter.TotalFrames());
        dl->AddText(ImVec2(barA.x + 8.0f, barA.y + 1.0f), IM_COL32(255, 255, 255, 230), label);
    }

    ImGui::End();
}

} // namespace d3d
