#include "ui/panels/MenuBarPanel.h"

#include "imgui.h"

#include "ui/DirectorHost.h"
#include "ui/Localization.h"

namespace d3d {

void MenuBarPanel::Draw(DirectorHost& host) {
    if (!ImGui::BeginMenuBar()) return;

    AnimationDocument& doc = host.Document();
    const bool hasSelection = host.SelectedId() >= 0;

    if (ImGui::BeginMenu(T("Файл"))) {
        if (ImGui::MenuItem(T("Новый"), "Ctrl+N")) host.OpenDialog(Dialog::NewProject);
        if (ImGui::MenuItem(T("Открыть…"), "Ctrl+O")) host.OpenDialog(Dialog::OpenProject);
        ImGui::Separator();
        if (ImGui::MenuItem(T("Сохранить"), "Ctrl+S")) {
            // Проект без имени сохранять некуда — спрашиваем путь, как и любой
            // редактор при первом сохранении.
            if (host.ProjectPath().empty()) {
                host.OpenDialog(Dialog::SaveProjectAs);
            } else {
                std::string err;
                if (!host.SaveProject(host.ProjectPath(), err)) host.SetStatus(T("Не сохранилось: ") + err);
            }
        }
        if (ImGui::MenuItem(T("Сохранить как…"), "Ctrl+Shift+S")) host.OpenDialog(Dialog::SaveProjectAs);
        ImGui::Separator();
        if (ImGui::MenuItem(T("Импорт…"), "Ctrl+I")) host.OpenDialog(Dialog::ImportAsset);
        if (ImGui::MenuItem(T("Экспорт сцены (.sage)…"))) host.OpenDialog(Dialog::ExportScene);
        ImGui::Separator();
        if (ImGui::MenuItem(T("Выход"), "Alt+F4")) host.RequestQuit();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(T("Правка"))) {
        if (ImGui::MenuItem(T("Отменить"), "Ctrl+Z", false, host.CanUndo())) host.Undo();
        if (ImGui::MenuItem(T("Повторить"), "Ctrl+Y", false, host.CanRedo())) host.Redo();
        ImGui::Separator();
        if (ImGui::MenuItem(T("Дублировать"), "Ctrl+D", false, hasSelection)) host.DuplicateSelected();
        if (ImGui::MenuItem(T("Удалить"), "Del", false, hasSelection)) host.DeleteSelected();
        ImGui::Separator();
        if (ImGui::MenuItem(T("Снять выделение"), "Esc", false, hasSelection)) host.ClearSelection();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(T("Создать"))) {
        if (ImGui::MenuItem(T("Камера"))) host.Create(CreateKind::Camera);
        if (ImGui::BeginMenu(T("Свет"))) {
            if (ImGui::MenuItem(T("Точечный свет"))) host.Create(CreateKind::PointLight);
            if (ImGui::MenuItem(T("Прожектор"))) host.Create(CreateKind::SpotLight);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("Примитив"))) {
            if (ImGui::MenuItem(T("Куб"))) host.Create(CreateKind::Cube);
            if (ImGui::MenuItem(T("Сфера"))) host.Create(CreateKind::Sphere);
            if (ImGui::MenuItem(T("Плоскость"))) host.Create(CreateKind::Plane);
            if (ImGui::MenuItem(T("Цилиндр"))) host.Create(CreateKind::Cylinder);
            if (ImGui::MenuItem(T("Конус"))) host.Create(CreateKind::Cone);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(T("Персонаж"))) host.Create(CreateKind::Character);
        if (ImGui::MenuItem(T("Эффект частиц"))) host.Create(CreateKind::ParticleEffect);
        ImGui::Separator();
        if (ImGui::MenuItem(T("Группа"))) host.Create(CreateKind::Group);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(T("Вид"))) {
        if (ImGui::BeginMenu(T("Вид с камеры"))) {
            ViewPreset& preset = host.Preset();
            if (ImGui::MenuItem(T("Перспектива"), "1", preset == ViewPreset::Perspective)) preset = ViewPreset::Perspective;
            if (ImGui::MenuItem(T("Спереди"), "2", preset == ViewPreset::Front)) preset = ViewPreset::Front;
            if (ImGui::MenuItem(T("Сбоку"), "3", preset == ViewPreset::Side)) preset = ViewPreset::Side;
            if (ImGui::MenuItem(T("Сверху"), "4", preset == ViewPreset::Top)) preset = ViewPreset::Top;
            if (ImGui::MenuItem(T("Камера сцены"), "0", preset == ViewPreset::SceneCamera)) preset = ViewPreset::SceneCamera;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("Отображение"))) {
            ShadingMode& mode = host.Shading();
            if (ImGui::MenuItem(T("С затенением"), nullptr, mode == ShadingMode::Shaded)) mode = ShadingMode::Shaded;
            if (ImGui::MenuItem(T("Каркас"), nullptr, mode == ShadingMode::Wireframe)) mode = ShadingMode::Wireframe;
            if (ImGui::MenuItem(T("Без света"), nullptr, mode == ShadingMode::Unlit)) mode = ShadingMode::Unlit;
            if (ImGui::MenuItem(T("Нормали"), nullptr, mode == ShadingMode::Normals)) mode = ShadingMode::Normals;
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ViewportOverlays& ov = host.Overlays();
        ImGui::MenuItem(T("Сетка"), "G", &ov.Grid);
        if (ImGui::BeginMenu(T("Размер сетки"))) {
            using Extent = sage::render::GridSettings::Extent;
            Extent& mode = ov.GridConfig.Mode;
            if (ImGui::MenuItem(T("Бесконечная"), nullptr, mode == Extent::Infinite)) {
                mode = Extent::Infinite;
            }
            if (ImGui::MenuItem(T("Радиус"), nullptr, mode == Extent::Radius)) mode = Extent::Radius;
            ImGui::Separator();
            ImGui::SetNextItemWidth(140.0f);
            ImGui::DragFloat(T("Радиус"), &ov.GridConfig.Radius, 0.5f, 1.0f, 5000.0f, T("%.1f м"));
            ImGui::SetNextItemWidth(140.0f);
            ImGui::DragFloat(T("Клетка"), &ov.GridConfig.CellSize, 0.01f, 0.01f, 100.0f, T("%.2f м"));
            ImGui::TextDisabled("%s", T("Остальное — в панели World"));
            ImGui::EndMenu();
        }
        ImGui::MenuItem(T("Каркасы"), nullptr, &ov.Gizmos);
        ImGui::MenuItem(T("Рамка кадра"), nullptr, &ov.CameraFrame);
        ImGui::MenuItem(T("Безопасная зона"), nullptr, &ov.SafeArea);
        ImGui::MenuItem(T("Сетка третей"), nullptr, &ov.Thirds);
        ImGui::MenuItem(T("Обводка выделения"), nullptr, &ov.Outline);
        ImGui::MenuItem(T("Скелет"), nullptr, &ov.Skeleton);
        ImGui::Separator();
        if (ImGui::MenuItem(T("Навести на выбранное"), "F", false, hasSelection)) host.FocusOnSelected();
        ImGui::Separator();
        ImGui::TextDisabled("%s", T("Небо, солнце и туман — в панели World"));
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(T("Инструменты"))) {
        int& op = host.GizmoOp();
        // Числа — это ImGuizmo::OPERATION; сравниваем с ними напрямую, чтобы не
        // тащить заголовок ImGuizmo в меню (см. ToolbarPanel, там же и константы).
        if (ImGui::MenuItem(T("Выбор"), "Q", op == 0)) op = 0;
        if (ImGui::MenuItem(T("Перемещение"), "W")) op = 7;      // TRANSLATE
        if (ImGui::MenuItem(T("Повернуть"), "E")) op = 120;  // ROTATE
        if (ImGui::MenuItem(T("Масштаб"), "R")) op = 896;   // SCALE
        ImGui::Separator();
        ImGui::MenuItem(T("Привязка"), "X", &host.GizmoSnap());
        GizmoSpace& space = host.GizmoSpaceRef();
        bool world = space == GizmoSpace::World;
        if (ImGui::MenuItem(T("Оси мира"), nullptr, world)) space = world ? GizmoSpace::Local : GizmoSpace::World;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(T("Анимация"))) {
        if (ImGui::MenuItem(host.Transport().Playing() ? T("Пауза") : T("Проиграть"), T("Пробел"))) {
            host.Transport().TogglePlay();
        }
        if (ImGui::MenuItem(T("Стоп"), T("Shift+Пробел"))) {
            host.Transport().Stop();
            host.SetCurrentTime(0.0f);
        }
        ImGui::MenuItem(T("Цикл"), "L", &host.Transport().Loop);
        ImGui::Separator();
        if (ImGui::MenuItem(T("Поставить ключ"), "K", false, hasSelection)) host.KeySelected();
        ImGui::MenuItem(T("Авто-ключ"), "Ctrl+K", &host.AutoKey());
        ImGui::Separator();

        // --- Поза персонажа ---
        const BoneSelection& bone = host.SelectedBone();
        if (ImGui::MenuItem(T("Ключ кости"), "K", false, bone.Valid())) host.KeyBone();
        if (ImGui::MenuItem(T("Ключ всей позы"), nullptr, false, hasSelection)) {
            host.KeyWholePose(host.SelectedId());
        }
        if (ImGui::MenuItem(T("Сбросить позу"), nullptr, false, hasSelection)) {
            host.ResetPose(host.SelectedId());
        }
        if (ImGui::MenuItem(T("Снять выбор с кости"), "Esc", false, bone.Valid())) {
            host.SelectBone(bone.EntityId, -1);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T("Добавить метку"), "M")) {
            Marker marker;
            marker.Time = host.CurrentTime();
            marker.Name = T("Метка ") + std::to_string(doc.Markers.size() + 1);
            host.PushUndo();
            doc.Markers.push_back(marker);
        }
        if (ImGui::MenuItem(T("Подогнать длительность"))) {
            const float end = doc.ContentEnd();
            if (end > 0.0f) {
                host.PushUndo();
                doc.Duration = end;
                host.SetStatus(T("Длительность подогнана под содержимое"));
            }
        }
        if (ImGui::MenuItem(T("Настройки таймлайна…"))) host.OpenDialog(Dialog::TimelineSettings);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(T("Рендер"))) {
        if (ImGui::MenuItem(T("Настройки рендера…"))) host.OpenDialog(Dialog::RenderSettings);
        if (ImGui::MenuItem(T("Рендер секвенции"), "F12", false, !host.Exporter().Active())) host.StartRender();
        if (ImGui::MenuItem(T("Отменить рендер"), nullptr, false, host.Exporter().Active())) host.Exporter().Cancel();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(T("Окно"))) {
        // Простой/продвинутый — здесь же, чтобы переключатель нашёлся и без
        // тулбара (панель можно спрятать, меню — нет).
        ImGui::MenuItem(T("Простой режим"), nullptr, &host.SimpleMode());
        ImGui::Separator();

        // Язык интерфейса. Названия языков НЕ переводятся: человек, открывший
        // программу на незнакомом языке, ищет в списке своё слово — «Русский»
        // или «English», — а не перевод этого слова на текущий язык.
        if (ImGui::BeginMenu("Язык / Language")) {
            const bool ru = i18n::CurrentLanguage() == i18n::Language::Russian;
            if (ImGui::MenuItem("Русский", nullptr, ru)) {
                i18n::SetLanguage(i18n::Language::Russian);
                i18n::SavePreference();
                host.SetStatus("Язык интерфейса: русский");
            }
            if (ImGui::MenuItem("English", nullptr, !ru)) {
                i18n::SetLanguage(i18n::Language::English);
                i18n::SavePreference();
                host.SetStatus("Interface language: English");
            }
            if (i18n::DictionarySize() == 0) {
                ImGui::Separator();
                ImGui::TextDisabled("assets/i18n/en.json не найден");
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", T("Раскладку панелей меняйте перетаскиванием"));
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(T("Справка"))) {
        if (ImGui::MenuItem("Keyboard Shortcuts")) host.OpenDialog(Dialog::Shortcuts);
        if (ImGui::MenuItem("About Director 3D")) host.OpenDialog(Dialog::About);
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

} // namespace d3d
