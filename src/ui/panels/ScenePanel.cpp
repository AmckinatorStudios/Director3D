#include "ui/panels/ScenePanel.h"

#include <algorithm>
#include <cstring>

#include "imgui.h"
#include "imgui_stdlib.h"

#include "anim/BonePose.h"
#include "anim/DirectorComponents.h"
#include "sage/scene/Components.h"
#include "ui/DirectorHost.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace d3d {

using Icons::Icon;

// ============================================================================
//  Категории
// ============================================================================

ScenePanel::Category ScenePanel::CategoryOf(Scene& scene, entt::entity e) {
    auto& reg = scene.Registry();
    // Порядок проверок — от более специфичного к общему: у камеры может быть и
    // меш-маркер, но она всё равно камера.
    if (reg.all_of<CameraComponent>(e)) return Category::Cameras;
    if (reg.all_of<LightComponent>(e)) return Category::Lights;
    if (reg.all_of<AnimatedModelComponent>(e)) return Category::Characters;
    if (reg.all_of<ParticleEmitterComponent>(e)) return Category::Effects;
    if (const MeshRendererComponent* mr = reg.try_get<MeshRendererComponent>(e)) {
        if (mr->Ref.type != MeshRef::Type::None) return Category::Environment;
    }
    return Category::Other;
}

const char* ScenePanel::CategoryName(Category c) {
    switch (c) {
        case Category::Cameras:     return "Cameras";
        case Category::Characters:  return "Characters";
        case Category::Environment: return "Environment";
        case Category::Lights:      return "Lights";
        case Category::Effects:     return "Effects";
        default:                    return "Other";
    }
}

namespace {

Icon IconForEntity(Scene& scene, entt::entity e) {
    auto& reg = scene.Registry();
    if (reg.all_of<CameraComponent>(e)) return Icon::Camera;
    if (reg.all_of<LightComponent>(e)) return Icon::Light;
    if (reg.all_of<AnimatedModelComponent>(e)) return Icon::Character;
    if (reg.all_of<ParticleEmitterComponent>(e)) return Icon::Effect;
    if (const MeshRendererComponent* mr = reg.try_get<MeshRendererComponent>(e)) {
        if (mr->Ref.type == MeshRef::Type::Model) return Icon::Model;
        if (mr->Ref.type != MeshRef::Type::None) return Icon::Cube;
    }
    return Icon::Folder; // пустая сущность работает как группа
}

// Регистронезависимое вхождение — поиск в дереве не должен требовать
// попадания в регистр.
bool ContainsNoCase(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower((unsigned char)a) ==
                                                      std::tolower((unsigned char)b); });
    return it != haystack.end();
}

} // namespace

bool ScenePanel::MatchesFilter(Scene& scene, entt::entity e) const {
    if (m_filter.empty()) return true;
    auto& reg = scene.Registry();
    const NameComponent* name = reg.try_get<NameComponent>(e);
    if (name && ContainsNoCase(name->Name, m_filter)) return true;
    // Родитель показывается, если подходит хоть один потомок — иначе поиск
    // «спрятал» бы найденный объект вместе с его веткой.
    const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e);
    if (h) {
        for (entt::entity child : h->Children) {
            if (reg.valid(child) && MatchesFilter(scene, child)) return true;
        }
    }
    return false;
}

// ============================================================================
//  Отрисовка
// ============================================================================

// Ветка костей персонажа. Кости — не сущности сцены: в ECS их нет, они живут
// внутри модели. Поэтому дерево строится прямо по скелету, а выбор кости идёт
// мимо обычного выбора объекта (см. BoneSelection).
void ScenePanel::DrawJoint(DirectorHost& host, Scene& scene, int entityId,
                           const sage::anim::Skeleton& skeleton, int joint,
                           const std::vector<std::vector<int>>& children) {
    const std::vector<int>& kids = children[(size_t)joint];
    const std::string& name = skeleton.Joints[(size_t)joint].Name;

    ImGui::PushID(joint);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (kids.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (host.SelectedBone().Is(entityId, joint)) flags |= ImGuiTreeNodeFlags_Selected;
    if (!m_boneFilter.empty() && !kids.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);

    // Кость с дорожкой подсвечена: по дереву сразу видно, что уже анимировано.
    const AnimationDocument& doc = host.Document();
    const bool animated = doc.FindTrack(entityId, Property::BoneRotation, joint) ||
                          doc.FindTrack(entityId, Property::BonePosition, joint) ||
                          doc.FindTrack(entityId, Property::BoneScale, joint);
    if (animated) ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Accent);

    const bool open = ImGui::TreeNodeEx("##joint", flags, "%s",
                                        name.empty() ? "(без имени)" : name.c_str());
    if (animated) ImGui::PopStyleColor();

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) host.SelectBone(entityId, joint);

    if (ImGui::BeginPopupContextItem("##bonectx")) {
        host.SelectBone(entityId, joint);
        if (ImGui::MenuItem("Поставить ключ на кость", "K")) host.KeyBone();
        if (ImGui::MenuItem("Заключить всю позу")) host.KeyWholePose(entityId);
        ImGui::Separator();
        if (ImGui::MenuItem("Снять выбор кости")) host.SelectBone(entityId, -1);
        ImGui::EndPopup();
    }

    if (open && !kids.empty()) {
        for (int child : kids) DrawJoint(host, scene, entityId, skeleton, child, children);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void ScenePanel::DrawSkeletonTree(DirectorHost& host, Scene& scene, int entityId) {
    const sage::anim::Skeleton* skeleton = SkeletonOf(scene, entityId);
    if (!skeleton) {
        // Модель грузится лениво — это нормальное состояние в первые кадры, и
        // молчать о нём хуже, чем показать строку «скелет ещё не готов».
        ImGui::TreeNodeEx("##nosk", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen,
                          "Skeleton (загружается…)");
        return;
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (host.SelectedBone().EntityId == entityId) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    if (!ImGui::TreeNodeEx("##skeleton", flags, "Skeleton (%d)", skeleton->Count())) return;

    // Список детей каждой кости — строим один раз на кадр: рекурсия по
    // «найти всех, у кого Parent == i» стоила бы O(n²) на каждом узле.
    const int count = skeleton->Count();
    std::vector<std::vector<int>> children((size_t)count);
    std::vector<int> roots;
    for (int i = 0; i < count; ++i) {
        const int parent = skeleton->Joints[(size_t)i].Parent;
        // Родитель вне диапазона или указывающий вперёд по циклу — битый файл;
        // такую кость показываем как корневую, иначе она пропала бы из дерева.
        if (parent >= 0 && parent < count && parent != i) children[(size_t)parent].push_back(i);
        else roots.push_back(i);
    }

    if (count > 12) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##bonesearch", "Поиск кости...", &m_boneFilter);
    }

    if (m_boneFilter.empty()) {
        for (int root : roots) DrawJoint(host, scene, entityId, *skeleton, root, children);
    } else {
        // При поиске иерархия не нужна — нужен список совпавших костей.
        std::string needle = m_boneFilter;
        std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
        int shown = 0;
        for (int i = 0; i < count; ++i) {
            std::string name = skeleton->Joints[(size_t)i].Name;
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            if (name.find(needle) == std::string::npos) continue;
            ImGui::PushID(i);
            const bool selected = host.SelectedBone().Is(entityId, i);
            if (ImGui::Selectable(skeleton->Joints[(size_t)i].Name.c_str(), selected)) {
                host.SelectBone(entityId, i);
            }
            ImGui::PopID();
            ++shown;
        }
        if (shown == 0) ImGui::TextDisabled("Костей с таким именем нет");
    }

    ImGui::TreePop();
}

void ScenePanel::DrawEntity(DirectorHost& host, Scene& scene, entt::entity e, bool insideCategory) {
    auto& reg = scene.Registry();
    if (!reg.valid(e) || !reg.all_of<IdComponent>(e)) return;
    if (!MatchesFilter(scene, e)) return;

    const int id = reg.get<IdComponent>(e).Id;
    const std::string& name = reg.get<NameComponent>(e).Name;
    const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e);
    // Персонаж раскрывается даже без дочерних объектов: внутри у него скелет.
    const bool hasSkeleton = reg.all_of<AnimatedModelComponent>(e);
    const bool hasChildren = (h && !h->Children.empty()) || hasSkeleton;

    ImGui::PushID(id);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_FramePadding;
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (host.IsSelected(id)) flags |= ImGuiTreeNodeFlags_Selected;
    // При активном поиске ветки раскрыты: иначе найденный объект остаётся
    // спрятанным внутри свёрнутого родителя.
    if (!m_filter.empty() && hasChildren) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    // Выбрали кость (например, кликом во вьюпорте) — раскрываем персонажа, иначе
    // непонятно, какая кость выбрана: дерево показывает свёрнутую строку.
    if (host.SelectedBone().EntityId == id) ImGui::SetNextItemOpen(true, ImGuiCond_Always);

    // Иконка рисуется поверх строки узла: у ImGui нет штатной «иконки в
    // TreeNode», а отступ под неё даём пробелами в подписи.
    const ImVec2 rowPos = ImGui::GetCursorScreenPos();
    const bool open = ImGui::TreeNodeEx("##node", flags, "      %s", name.c_str());
    const bool nodeClicked = ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen();

    Icons::Draw(ImGui::GetWindowDrawList(), IconForEntity(scene, e),
                ImVec2(rowPos.x + ImGui::GetTreeNodeToLabelSpacing() + 7.0f,
                       rowPos.y + ImGui::GetTextLineHeight() * 0.5f + 2.0f),
                14.0f, host.IsSelected(id) ? Theme::Colors::Text : Theme::Colors::TextDim);

    if (nodeClicked) {
        if (ImGui::GetIO().KeyCtrl) host.ToggleSelection(id);
        else host.SetSelectedId(id);
    }

    // --- Перенос мышью: смена родителя ---
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
        m_dragSource = id;
        ImGui::SetDragDropPayload("D3D_ENTITY", &m_dragSource, sizeof(int));
        ImGui::Text("Перенести: %s", name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("D3D_ENTITY")) {
            const int childId = *(const int*)payload->Data;
            if (childId != id) host.SetParentOf(childId, id);
        }
        ImGui::EndDragDropTarget();
    }

    // --- Контекстное меню объекта ---
    if (ImGui::BeginPopupContextItem("##ctx")) {
        if (!host.IsSelected(id)) host.SetSelectedId(id);
        if (ImGui::MenuItem("Переименовать", "F2")) {
            m_renaming = id;
            std::snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%s", name.c_str());
        }
        if (ImGui::MenuItem("Дублировать", "Ctrl+D")) host.DuplicateSelected();
        if (ImGui::MenuItem("Удалить", "Del")) host.DeleteSelected();
        ImGui::Separator();
        if (ImGui::MenuItem("Навести камеру", "F")) host.FocusOnSelected();
        if (ImGui::MenuItem("Открепить от родителя")) host.SetParentOf(id, -1);
        ImGui::Separator();
        if (ImGui::MenuItem("Поставить ключ", "K")) host.KeySelected();
        if (reg.all_of<AnimatedModelComponent>(e)) {
            if (ImGui::MenuItem("Заключить всю позу")) host.KeyWholePose(id);
            if (ImGui::MenuItem("Снять ручную позу")) host.ResetPose(id);
        }
        if (reg.all_of<CameraComponent>(e)) {
            if (ImGui::MenuItem("Сделать активной камерой", nullptr, host.ActiveCameraId() == id)) {
                host.SetActiveCameraId(id);
            }
        }
        ImGui::EndPopup();
    }

    // --- Переименование на месте ---
    if (m_renaming == id) {
        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##rename", m_renameBuffer, sizeof(m_renameBuffer),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            host.RenameObject(id, m_renameBuffer);
            m_renaming = -1;
        }
        // Клик мимо поля — отмена: иначе поле висело бы вечно.
        if (ImGui::IsItemDeactivated()) m_renaming = -1;
    }

    // --- Глазок видимости (справа) ---
    StageItemComponent& item = reg.get_or_emplace<StageItemComponent>(e);
    const float eyeX = ImGui::GetWindowContentRegionMax().x - 20.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(eyeX);
    bool visible = item.Visible;
    if (Icons::ToggleIcon("vis", Icon::Eye, Icon::EyeOff, visible,
                          visible ? "Скрыть объект" : "Показать объект")) {
        host.PushUndo();
        item.Visible = visible;
    }

    if (open && hasChildren) {
        if (hasSkeleton) DrawSkeletonTree(host, scene, id);
        if (h) {
            for (entt::entity child : h->Children) DrawEntity(host, scene, child, insideCategory);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void ScenePanel::Draw(DirectorHost& host) {
    ImGui::Begin("Scene");
    Scene& scene = host.CurrentScene();
    auto& reg = scene.Registry();

    // --- Строка поиска ---
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 56.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(24, 4));
        ImGui::InputTextWithHint("##search", "Search...", &m_filter);
        ImGui::PopStyleVar();
        Icons::Draw(ImGui::GetWindowDrawList(), Icon::Search,
                    ImVec2(pos.x + 13.0f, pos.y + ImGui::GetFrameHeight() * 0.5f),
                    13.0f, Theme::Colors::TextFaint);
    }
    ImGui::SameLine();
    if (Icons::IconButton("add", Icon::Add, "Добавить объект")) ImGui::OpenPopup("##addobj");
    ImGui::SameLine();
    if (Icons::IconButton("del", Icon::Trash, "Удалить выбранное", false, host.SelectedId() >= 0))
        host.DeleteSelected();

    if (ImGui::BeginPopup("##addobj")) {
        if (ImGui::MenuItem("Camera")) host.Create(CreateKind::Camera);
        if (ImGui::MenuItem("Point Light")) host.Create(CreateKind::PointLight);
        if (ImGui::MenuItem("Spot Light")) host.Create(CreateKind::SpotLight);
        ImGui::Separator();
        if (ImGui::MenuItem("Cube")) host.Create(CreateKind::Cube);
        if (ImGui::MenuItem("Sphere")) host.Create(CreateKind::Sphere);
        if (ImGui::MenuItem("Plane")) host.Create(CreateKind::Plane);
        if (ImGui::MenuItem("Cylinder")) host.Create(CreateKind::Cylinder);
        if (ImGui::MenuItem("Cone")) host.Create(CreateKind::Cone);
        ImGui::Separator();
        if (ImGui::MenuItem("Character")) host.Create(CreateKind::Character);
        if (ImGui::MenuItem("Particle Effect")) host.Create(CreateKind::ParticleEffect);
        if (ImGui::MenuItem("Group")) host.Create(CreateKind::Group);
        ImGui::EndPopup();
    }

    ImGui::Separator();

    // --- Дерево ---
    ImGui::BeginChild("##tree", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    // Корневой узел «Scene» — как в референсе; на него можно бросить объект,
    // чтобы открепить его от родителя.
    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    const bool rootOpen = ImGui::TreeNodeEx("Scene", ImGuiTreeNodeFlags_SpanAvailWidth |
                                                     ImGuiTreeNodeFlags_DefaultOpen);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("D3D_ENTITY")) {
            host.SetParentOf(*(const int*)payload->Data, -1);
        }
        ImGui::EndDragDropTarget();
    }

    if (rootOpen) {
        // Раскладываем КОРНЕВЫЕ сущности по категориям; потомки рисуются внутри
        // своих родителей, а не вторично в категории.
        std::vector<entt::entity> byCategory[(size_t)Category::Count];
        auto view = reg.view<IdComponent, NameComponent>();
        for (auto e : view) {
            if (scene.ParentOf(e) != entt::null) continue;
            byCategory[(size_t)CategoryOf(scene, e)].push_back(e);
        }

        for (int c = 0; c < (int)Category::Count; ++c) {
            std::vector<entt::entity>& items = byCategory[c];
            if (items.empty()) continue; // пустых категорий не показываем

            const ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
            const bool open = ImGui::TreeNodeEx(CategoryName((Category)c),
                                                ImGuiTreeNodeFlags_SpanAvailWidth |
                                                ImGuiTreeNodeFlags_DefaultOpen,
                                                "     %s", CategoryName((Category)c));
            Icons::Draw(ImGui::GetWindowDrawList(), Icon::Folder,
                        ImVec2(pos.x + ImGui::GetTreeNodeToLabelSpacing() + 6.0f,
                               pos.y + ImGui::GetTextLineHeight() * 0.5f + 2.0f),
                        13.0f, Theme::Colors::TextFaint);
            if (open) {
                for (entt::entity e : items) DrawEntity(host, scene, e, true);
                ImGui::TreePop();
            }
        }
        ImGui::TreePop();
    }

    // Клик по пустому месту снимает выделение — привычное поведение любого
    // дерева объектов.
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsAnyItemHovered()) {
        host.ClearSelection();
    }

    ImGui::EndChild();
    ImGui::End();
}

} // namespace d3d
