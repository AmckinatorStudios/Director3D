#include "ui/panels/MenuBarPanel.h"

#include "imgui.h"

#include "ui/DirectorHost.h"

namespace d3d {

void MenuBarPanel::Draw(DirectorHost& host) {
    if (!ImGui::BeginMenuBar()) return;

    AnimationDocument& doc = host.Document();
    const bool hasSelection = host.SelectedId() >= 0;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", "Ctrl+N")) host.OpenDialog(Dialog::NewProject);
        if (ImGui::MenuItem("Open...", "Ctrl+O")) host.OpenDialog(Dialog::OpenProject);
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S")) {
            // Проект без имени сохранять некуда — спрашиваем путь, как и любой
            // редактор при первом сохранении.
            if (host.ProjectPath().empty()) {
                host.OpenDialog(Dialog::SaveProjectAs);
            } else {
                std::string err;
                if (!host.SaveProject(host.ProjectPath(), err)) host.SetStatus("Не сохранилось: " + err);
            }
        }
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) host.OpenDialog(Dialog::SaveProjectAs);
        ImGui::Separator();
        if (ImGui::MenuItem("Import...", "Ctrl+I")) host.OpenDialog(Dialog::ImportAsset);
        if (ImGui::MenuItem("Export Scene (.sage)...")) host.OpenDialog(Dialog::ExportScene);
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) host.RequestQuit();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, host.CanUndo())) host.Undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, host.CanRedo())) host.Redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSelection)) host.DuplicateSelected();
        if (ImGui::MenuItem("Delete", "Del", false, hasSelection)) host.DeleteSelected();
        ImGui::Separator();
        if (ImGui::MenuItem("Select None", "Esc", false, hasSelection)) host.ClearSelection();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Create")) {
        if (ImGui::MenuItem("Camera")) host.Create(CreateKind::Camera);
        if (ImGui::BeginMenu("Light")) {
            if (ImGui::MenuItem("Point Light")) host.Create(CreateKind::PointLight);
            if (ImGui::MenuItem("Spot Light")) host.Create(CreateKind::SpotLight);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Mesh")) {
            if (ImGui::MenuItem("Cube")) host.Create(CreateKind::Cube);
            if (ImGui::MenuItem("Sphere")) host.Create(CreateKind::Sphere);
            if (ImGui::MenuItem("Plane")) host.Create(CreateKind::Plane);
            if (ImGui::MenuItem("Cylinder")) host.Create(CreateKind::Cylinder);
            if (ImGui::MenuItem("Cone")) host.Create(CreateKind::Cone);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Character")) host.Create(CreateKind::Character);
        if (ImGui::MenuItem("Particle Effect")) host.Create(CreateKind::ParticleEffect);
        ImGui::Separator();
        if (ImGui::MenuItem("Group")) host.Create(CreateKind::Group);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        if (ImGui::BeginMenu("Camera View")) {
            ViewPreset& preset = host.Preset();
            if (ImGui::MenuItem("Perspective", "1", preset == ViewPreset::Perspective)) preset = ViewPreset::Perspective;
            if (ImGui::MenuItem("Front", "2", preset == ViewPreset::Front)) preset = ViewPreset::Front;
            if (ImGui::MenuItem("Side", "3", preset == ViewPreset::Side)) preset = ViewPreset::Side;
            if (ImGui::MenuItem("Top", "4", preset == ViewPreset::Top)) preset = ViewPreset::Top;
            if (ImGui::MenuItem("Scene Camera", "0", preset == ViewPreset::SceneCamera)) preset = ViewPreset::SceneCamera;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Shading")) {
            ShadingMode& mode = host.Shading();
            if (ImGui::MenuItem("Shaded", nullptr, mode == ShadingMode::Shaded)) mode = ShadingMode::Shaded;
            if (ImGui::MenuItem("Wireframe", nullptr, mode == ShadingMode::Wireframe)) mode = ShadingMode::Wireframe;
            if (ImGui::MenuItem("Unlit", nullptr, mode == ShadingMode::Unlit)) mode = ShadingMode::Unlit;
            if (ImGui::MenuItem("Normals", nullptr, mode == ShadingMode::Normals)) mode = ShadingMode::Normals;
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ViewportOverlays& ov = host.Overlays();
        ImGui::MenuItem("Grid", "G", &ov.Grid);
        ImGui::MenuItem("Gizmos", nullptr, &ov.Gizmos);
        ImGui::MenuItem("Camera Frame", nullptr, &ov.CameraFrame);
        ImGui::MenuItem("Safe Area", nullptr, &ov.SafeArea);
        ImGui::MenuItem("Rule of Thirds", nullptr, &ov.Thirds);
        ImGui::MenuItem("Selection Outline", nullptr, &ov.Outline);
        ImGui::MenuItem("Skeleton", nullptr, &ov.Skeleton);
        ImGui::Separator();
        if (ImGui::MenuItem("Frame Selected", "F", false, hasSelection)) host.FocusOnSelected();
        ImGui::Separator();
        ImGui::TextDisabled("Небо, солнце и туман — в панели World");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Tools")) {
        int& op = host.GizmoOp();
        // Числа — это ImGuizmo::OPERATION; сравниваем с ними напрямую, чтобы не
        // тащить заголовок ImGuizmo в меню (см. ToolbarPanel, там же и константы).
        if (ImGui::MenuItem("Select", "Q", op == 0)) op = 0;
        if (ImGui::MenuItem("Move", "W")) op = 7;      // TRANSLATE
        if (ImGui::MenuItem("Rotate", "E")) op = 120;  // ROTATE
        if (ImGui::MenuItem("Scale", "R")) op = 896;   // SCALE
        ImGui::Separator();
        ImGui::MenuItem("Snap", "X", &host.GizmoSnap());
        GizmoSpace& space = host.GizmoSpaceRef();
        bool world = space == GizmoSpace::World;
        if (ImGui::MenuItem("World Space", nullptr, world)) space = world ? GizmoSpace::Local : GizmoSpace::World;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Animation")) {
        if (ImGui::MenuItem(host.Transport().Playing() ? "Pause" : "Play", "Space")) {
            host.Transport().TogglePlay();
        }
        if (ImGui::MenuItem("Stop", "Shift+Space")) {
            host.Transport().Stop();
            host.SetCurrentTime(0.0f);
        }
        ImGui::MenuItem("Loop", "L", &host.Transport().Loop);
        ImGui::Separator();
        if (ImGui::MenuItem("Set Key", "K", false, hasSelection)) host.KeySelected();
        ImGui::MenuItem("Auto Key", "Ctrl+K", &host.AutoKey());
        ImGui::Separator();

        // --- Поза персонажа ---
        const BoneSelection& bone = host.SelectedBone();
        if (ImGui::MenuItem("Key Bone", "K", false, bone.Valid())) host.KeyBone();
        if (ImGui::MenuItem("Key Whole Pose", nullptr, false, hasSelection)) {
            host.KeyWholePose(host.SelectedId());
        }
        if (ImGui::MenuItem("Reset Pose", nullptr, false, hasSelection)) {
            host.ResetPose(host.SelectedId());
        }
        if (ImGui::MenuItem("Deselect Bone", "Esc", false, bone.Valid())) {
            host.SelectBone(bone.EntityId, -1);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Add Marker", "M")) {
            Marker marker;
            marker.Time = host.CurrentTime();
            marker.Name = "Marker " + std::to_string(doc.Markers.size() + 1);
            host.PushUndo();
            doc.Markers.push_back(marker);
        }
        if (ImGui::MenuItem("Fit Duration To Content")) {
            const float end = doc.ContentEnd();
            if (end > 0.0f) {
                host.PushUndo();
                doc.Duration = end;
                host.SetStatus("Длительность подогнана под содержимое");
            }
        }
        if (ImGui::MenuItem("Timeline Settings...")) host.OpenDialog(Dialog::TimelineSettings);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Render")) {
        if (ImGui::MenuItem("Render Settings...")) host.OpenDialog(Dialog::RenderSettings);
        if (ImGui::MenuItem("Render Sequence", "F12", false, !host.Exporter().Active())) host.StartRender();
        if (ImGui::MenuItem("Cancel Render", nullptr, false, host.Exporter().Active())) host.Exporter().Cancel();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Window")) {
        // Простой/продвинутый — здесь же, чтобы переключатель нашёлся и без
        // тулбара (панель можно спрятать, меню — нет).
        ImGui::MenuItem("Simple Mode", nullptr, &host.SimpleMode());
        ImGui::Separator();
        ImGui::TextDisabled("Раскладку панелей меняйте перетаскиванием");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Keyboard Shortcuts")) host.OpenDialog(Dialog::Shortcuts);
        if (ImGui::MenuItem("About Director 3D")) host.OpenDialog(Dialog::About);
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

} // namespace d3d
