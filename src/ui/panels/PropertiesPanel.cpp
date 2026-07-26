#include "ui/panels/PropertiesPanel.h"

#include <cstdio>

#include "imgui.h"

#include <algorithm>
#include <string>

#include "anim/BonePose.h"
#include "anim/DirectorComponents.h"
#include "sage/render/SkinnedModel.h" // список клипов персонажа в инспекторе
#include "sage/render/SkinnedModel.h"
#include "sage/scene/Components.h"
#include "ui/DirectorHost.h"
#include "ui/Icons.h"
#include "ui/Localization.h"
#include "ui/Theme.h"

namespace d3d {

namespace {

// Ширина колонки подписей — общая для всех строк инспектора, чтобы поля ввода
// стояли ровно в столбик, а не «лесенкой» под подписями разной длины.
constexpr float kLabelWidth = 108.0f;
// Место справа, зарезервированное под ромб-ключ.
constexpr float kKeyColumn = 24.0f;

// Заголовок раздела в стиле референса: раскрывающийся, с заметной подложкой.
bool SectionHeader(const char* label, bool defaultOpen = true) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 5));
    const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth |
                                     ImGuiTreeNodeFlags_FramePadding |
                                     (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    const bool open = ImGui::CollapsingHeader(label, flags);
    ImGui::PopStyleVar();
    return open;
}

// Укорачивает путь слева: имя файла важнее каталога, а панель узкая.
std::string ShortPath(const std::string& path) {
    const float maxWidth = ImGui::GetContentRegionAvail().x - 60.0f;
    if (maxWidth <= 0.0f || ImGui::CalcTextSize(path.c_str()).x <= maxWidth) return path;
    size_t cut = 0;
    while (cut < path.size() &&
           ImGui::CalcTextSize(("…" + path.substr(cut)).c_str()).x > maxWidth) {
        ++cut;
    }
    return "…" + path.substr(cut);
}

void RowLabel(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(kLabelWidth);
}

// Три поля вектора с цветными подписями осей — раскладка одна на весь
// инспектор: строки трансформа, кости и цели IK должны стоять в один столбик,
// иначе панель читается как набор разных диалогов. rightMargin — сколько места
// оставить справа: строке с ромбом-ключом нужен столбик под него, цели IK нет.
//
// host нужен только строкам, которые правят СЦЕНУ: каждое поле должно само
// открыть и закрыть шаг отмены, иначе перетаскивание мышью запишется в стек
// сотней шагов по одному кадру. Цель IK — состояние панели, а не сцены, ей
// отмена не нужна, поэтому host там nullptr.
bool AxisVec3(DirectorHost* host, float* value, float speed, const char* format,
              float rightMargin) {
    const float avail = ImGui::GetContentRegionAvail().x - rightMargin;
    const float fieldWidth = (avail - 2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
    static const char* kAxis[3] = {"X", "Y", "Z"};
    static const ImU32 kAxisColor[3] = {0xFF4A4AE8, 0xFF5CC85C, 0xFFE8964A};

    bool changed = false;
    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine();
        ImGui::PushID(i);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemWidth(fieldWidth);
        // Отступ слева освобождает место под букву оси: она рисуется поверх
        // поля, а не отдельной подписью, — иначе три подписи съели бы строку.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18, 4));
        if (ImGui::DragFloat("##v", &value[i], speed, 0.0f, 0.0f, format)) changed = true;
        ImGui::PopStyleVar();
        if (host) host->TrackLastItem();
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(pos.x + 7.0f, pos.y + ImGui::GetStyle().FramePadding.y), kAxisColor[i], kAxis[i]);
        ImGui::PopID();
    }
    return changed;
}

} // namespace

// ============================================================================
//  Ромб-ключ
// ============================================================================

void PropertiesPanel::DrawKeyDiamond(DirectorHost& host, Property prop, bool hasProp, int joint) {
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - kKeyColumn + 4.0f);

    const int id = host.SelectedId();
    const AnimationDocument& doc = host.Document();
    const Track* track = doc.FindTrack(id, prop, joint);
    const bool hasTrack = track != nullptr;
    const bool hasKey = hasTrack && doc.HasKeyAt(*track, host.CurrentTime());

    ImGui::PushID((int)prop);
    ImGui::PushID(joint);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float size = ImGui::GetFrameHeight();
    const bool clicked = ImGui::InvisibleButton("##key", ImVec2(size, size)) && hasProp;
    const bool hovered = ImGui::IsItemHovered() && hasProp;

    // Три состояния ромба читаются с одного взгляда:
    //   закрашен ярко — ключ стоит ровно на этом кадре;
    //   закрашен тускло — дорожка есть, но на этом кадре ключа нет;
    //   контур — дорожки ещё нет.
    const ImVec2 c(pos.x + size * 0.5f, pos.y + size * 0.5f);
    const float r = 5.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 top(c.x, c.y - r), right(c.x + r, c.y), bottom(c.x, c.y + r), left(c.x - r, c.y);

    if (!hasProp) {
        dl->AddQuad(top, right, bottom, left, Theme::Colors::TextFaint, 1.0f);
    } else if (hasKey) {
        dl->AddQuadFilled(top, right, bottom, left, Theme::Colors::KeySelected);
    } else if (hasTrack) {
        dl->AddQuadFilled(top, right, bottom, left, Theme::Colors::TextFaint);
    } else {
        dl->AddQuad(top, right, bottom, left,
                    hovered ? Theme::Colors::Text : Theme::Colors::TextDim, 1.4f);
    }

    if (hovered) {
        ImGui::SetTooltip("%s", hasKey ? T("Ключ на этом кадре — клик уберёт его")
                                       : T("Поставить ключ на текущем кадре"));
    }
    if (clicked) {
        host.PushUndo();
        if (hasKey) {
            // Убираем ключ, а вместе с последним — и опустевшую дорожку: пустая
            // строка в таймлайне только мешает.
            Track* mutableTrack = host.Document().TrackById(track->Id);
            host.Document().RemoveKeysAt(*mutableTrack, host.CurrentTime());
            bool anyLeft = false;
            for (const Curve& curve : mutableTrack->Channels) {
                if (!curve.Empty()) { anyLeft = true; break; }
            }
            if (!anyLeft) host.Document().RemoveTrack(mutableTrack->Id);
            host.SetStatus(T("Ключ убран"));
        } else if (joint >= 0) {
            // Костное свойство ключится напрямую: у KeyProperty кости нет, а
            // заводить ради этого ещё один метод хоста — лишний слой.
            host.Document().KeyFromScene(host.CurrentScene(), id, prop, host.CurrentTime(), joint);
            host.SetStatus(T("Ключ на кости поставлен"));
        } else {
            host.KeyProperty(id, prop);
            host.SetStatus(T("Ключ поставлен"));
        }
    }
    ImGui::PopID();
    ImGui::PopID();
}

// ============================================================================
//  Строки свойств
// ============================================================================

bool PropertiesPanel::DrawVec3Row(DirectorHost& host, const char* label, float* value,
                                  Property prop, float speed, const char* format, int joint) {
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);

    // Подписи осей X/Y/Z цветные — те же цвета, что у осей гизмо и у каналов
    // в редакторе кривых.
    const bool changed = AxisVec3(&host, value, speed, format, kKeyColumn);

    DrawKeyDiamond(host, prop, true, joint);
    ImGui::PopID();
    return changed;
}

bool PropertiesPanel::DrawFloatRow(DirectorHost& host, const char* label, float* value,
                                   Property prop, float speed, float lo, float hi,
                                   const char* format) {
    ImGui::PushID(label);
    RowLabel(label);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
    const bool changed = ImGui::DragFloat("##f", value, speed, lo, hi, format);
    host.TrackLastItem();
    DrawKeyDiamond(host, prop, true);
    ImGui::PopID();
    return changed;
}

bool PropertiesPanel::DrawEffectRow(DirectorHost& host, const char* label, bool* enabled,
                                    float* amount, Property prop, float lo, float hi) {
    ImGui::PushID(label);
    bool changed = ImGui::Checkbox(label, enabled);
    if (changed) host.PushUndo();

    ImGui::SameLine(kLabelWidth + 24.0f);
    ImGui::BeginDisabled(!*enabled);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn - 52.0f);
    if (ImGui::SliderFloat("##amt", amount, lo, hi, "")) changed = true;
    host.TrackLastItem();
    ImGui::SameLine();
    ImGui::Text("%.2f", (double)*amount);
    ImGui::EndDisabled();

    // Ромб есть только у эффектов, которые реально анимируются документом;
    // остальным рисуем пустое место, чтобы столбик не «прыгал».
    const bool animatable = (prop == Property::PostBloom || prop == Property::PostVignette ||
                             prop == Property::PostMotionBlur || prop == Property::PostChromatic);
    DrawKeyDiamond(host, prop, animatable);
    ImGui::PopID();
    return changed;
}

// ============================================================================
//  Панель
// ============================================================================

void PropertiesPanel::DrawMorphSection(DirectorHost& host) {
    Scene& scene = host.CurrentScene();
    GameObject obj = host.SelectedObject();
    if (!obj.Valid()) return;

    AnimatedModelComponent* am =
        scene.Registry().try_get<AnimatedModelComponent>(obj.Entity());
    if (!am || !am->Model || am->Model->MorphCount() == 0) return;

    char title[64];
    std::snprintf(title, sizeof(title), T("Формы смешивания (%d)"), am->Model->MorphCount());
    if (!SectionHeader(title)) return;

    ImGui::Spacing();
    const std::vector<std::string>& names = am->Model->MorphNames();
    am->MorphWeights.resize(names.size(), 0.0f);

    bool edited = false;
    for (size_t i = 0; i < names.size(); ++i) {
        ImGui::PushID((int)i);
        RowLabel(names[i].c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
        // Диапазон 0..1: за его пределами блендшейп выворачивает геометрию, и
        // ползунок не должен предлагать этого как норму.
        if (ImGui::SliderFloat("##w", &am->MorphWeights[i], 0.0f, 1.0f, "%.3f")) edited = true;
        host.TrackLastItem();
        DrawKeyDiamond(host, Property::MorphWeight, true, (int)i);
        ImGui::PopID();
    }

    if (edited) host.NotifyObjectEdited(obj.Id());

    ImGui::Spacing();
    if (ImGui::Button(T("Сбросить все"))) {
        host.PushUndo();
        std::fill(am->MorphWeights.begin(), am->MorphWeights.end(), 0.0f);
        host.SetStatus(T("Блендшейпы сброшены"));
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Заключить все"))) {
        host.PushUndo();
        int keyed = 0;
        for (size_t i = 0; i < names.size(); ++i) {
            if (host.Document().KeyFromScene(scene, obj.Id(), Property::MorphWeight,
                                             host.CurrentTime(), (int)i)) ++keyed;
        }
        host.SetStatus(T("Ключей на блендшейпах: ") + std::to_string(keyed));
    }
    ImGui::Spacing();
}

void PropertiesPanel::DrawIKSection(DirectorHost& host, int entityId, int joint) {
    Scene& scene = host.CurrentScene();
    const sage::anim::Skeleton* skeleton = SkeletonOf(scene, entityId);
    if (!skeleton) return;

    // Цепочка должна набраться от выбранной кости вверх — у кости под самым
    // корнем тянуть нечего, и предлагать IK там незачем.
    const int maxChain = [&] {
        int length = 0;
        for (int j = joint; j >= 0; j = skeleton->Joints[(size_t)j].Parent) ++length;
        return length;
    }();
    if (maxChain < 3) return;

    ImGui::Spacing();
    if (!ImGui::TreeNodeEx("##ik", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", T("Обратная кинематика"))) return;

    // Смена кости обнуляет цель: ручка от прошлой конечности на новой означала
    // бы рывок в чужую точку при первом же нажатии.
    if (m_ikBone != joint) {
        m_ikBone = joint;
        glm::vec3 here(0.0f);
        if (BoneWorldPosition(scene, entityId, joint, here)) m_ikTarget = here;
        m_ikPole = m_ikTarget + glm::vec3(0.0f, 0.0f, 1.0f);
        m_ikChainLength = std::min(3, maxChain);
        m_ikReached = true;
    }

    ImGui::Spacing();
    RowLabel(T("Костей"));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
    ImGui::SliderInt("##chain", &m_ikChainLength, 3, std::min(maxChain, 12), "%d");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Длина цепочки от выбранной кости вверх по скелету.\n"
                          "Три кости — аналитическое решение (рука, нога).\n"
                          "Больше — FABRIK: хвост, позвоночник, щупальце."));
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(T("Цель (мир)"));
    AxisVec3(nullptr, &m_ikTarget.x, 0.01f, "%.3f", kKeyColumn);
    if (ImGui::Button(T("Взять от кости"))) {
        glm::vec3 here(0.0f);
        if (BoneWorldPosition(scene, entityId, joint, here)) m_ikTarget = here;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Поставить цель туда, где кость сейчас"));

    ImGui::Spacing();
    ImGui::Checkbox(T("Задать полюс"), &m_ikUsePole);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Куда смотрит локоть или колено.\n"
                          "Без полюса плоскость сгиба берётся от текущей позы."));
    }
    if (m_ikUsePole) AxisVec3(nullptr, &m_ikPole.x, 0.01f, "%.3f", kKeyColumn);

    ImGui::Spacing();
    RowLabel(T("Сила"));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
    ImGui::SliderFloat("##ikweight", &m_ikWeight, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Смешивание с текущей позой: 0 — IK не влияет,\n"
                          "1 — поза целиком от солвера."));
    }

    ImGui::Spacing();
    if (ImGui::Button(T("Дотянуться"))) {
        host.PushUndo();
        bool reached = false;
        const glm::vec3* pole = m_ikUsePole ? &m_ikPole : nullptr;
        if (SolveBoneIK(scene, entityId, joint, m_ikChainLength, m_ikTarget, pole, m_ikWeight,
                        reached)) {
            m_ikReached = reached;
            host.NotifyObjectEdited(entityId);
            host.SetStatus(reached ? T("IK: цель достигнута")
                                   : T("IK: цель дальше вытянутой конечности"));
        } else {
            host.SetStatus(T("IK не сработал — скелет не готов или цепочка короткая"));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Заключить позу##ik"))) host.KeyWholePose(entityId);

    if (!m_ikReached) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Warning);
        ImGui::TextWrapped("%s", T("Цель дальше, чем достаёт конечность — она вытянута в её сторону."));
        ImGui::PopStyleColor();
    }

    ImGui::TreePop();
    ImGui::Spacing();
}

void PropertiesPanel::DrawBoneSection(DirectorHost& host) {
    const BoneSelection& bone = host.SelectedBone();
    if (!bone.Valid() || bone.EntityId != host.SelectedId()) return;

    Scene& scene = host.CurrentScene();
    const sage::anim::Skeleton* skeleton = SkeletonOf(scene, bone.EntityId);
    if (!skeleton || bone.Joint >= skeleton->Count()) return;

    const sage::anim::Joint& joint = skeleton->Joints[(size_t)bone.Joint];
    char title[160];
    std::snprintf(title, sizeof(title), T("Кость — %s"),
                  joint.Name.empty() ? T("(без имени)") : joint.Name.c_str());
    if (!SectionHeader(title)) return;

    ImGui::Spacing();
    if (joint.Parent >= 0 && joint.Parent < skeleton->Count()) {
        ImGui::TextDisabled(T("Родитель: %s"), skeleton->Joints[(size_t)joint.Parent].Name.c_str());
    } else {
        ImGui::TextDisabled("%s", T("Корневая кость"));
    }
    ImGui::Spacing();

    // Значения ЛОКАЛЬНЫЕ — относительно родительской кости, как их хранит клип.
    // Мировые координаты кости аниматору не нужны и только путали бы: «согнуть
    // локоть на 30°» — это локальный поворот, а не позиция в мире.
    glm::vec3 t, s;
    glm::quat r;
    if (!ReadBoneLocal(scene, bone.EntityId, bone.Joint, t, r, s)) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextDim);
        ImGui::TextWrapped("%s", T("Поза ещё не посчитана — модель загружается."));
        ImGui::PopStyleColor();
        return;
    }
    glm::vec3 euler = EulerDegreesFromQuat(r);

    bool edited = false;
    if (DrawVec3Row(host, T("Положение"), &t.x, Property::BonePosition, 0.005f, "%.4f", bone.Joint)) {
        const float v[3] = {t.x, t.y, t.z};
        WriteBoneChannel(scene, bone.EntityId, bone.Joint, BoneChannel::Translation, v);
        edited = true;
    }
    ImGui::Spacing();
    if (DrawVec3Row(host, T("Поворот"), &euler.x, Property::BoneRotation, 0.25f, "%.2f", bone.Joint)) {
        const float v[3] = {euler.x, euler.y, euler.z};
        WriteBoneChannel(scene, bone.EntityId, bone.Joint, BoneChannel::Rotation, v);
        edited = true;
    }
    ImGui::Spacing();
    if (DrawVec3Row(host, T("Масштаб"), &s.x, Property::BoneScale, 0.005f, "%.4f", bone.Joint)) {
        const float v[3] = {s.x, s.y, s.z};
        WriteBoneChannel(scene, bone.EntityId, bone.Joint, BoneChannel::Scale, v);
        edited = true;
    }
    if (edited) host.NotifyObjectEdited(bone.EntityId);

    DrawIKSection(host, bone.EntityId, bone.Joint);

    ImGui::Spacing();
    if (ImGui::Button(T("Ключ на кость"))) host.KeyBone();
    ImGui::SameLine();
    if (ImGui::Button(T("Заключить позу"))) host.KeyWholePose(bone.EntityId);
    ImGui::SameLine();
    if (ImGui::Button(T("Сброс"))) {
        // Сбрасываем только ЭТУ кость: «сброс всей позы» есть в дереве сцены, и
        // потерять всю работу по кнопке рядом с одной костью было бы обидно.
        host.PushUndo();
        for (BoneChannel channel : {BoneChannel::Translation, BoneChannel::Rotation,
                                    BoneChannel::Scale}) {
            ClearBoneChannel(scene, bone.EntityId, bone.Joint, channel);
        }
        host.SetStatus(T("Кость вернулась под управление клипа"));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Снять ручную позу с ЭТОЙ кости — она снова пойдёт за клипом"));
    }

    ImGui::Spacing();
    // TextWrapped, а не TextDisabled: панель узкая, и однострочная подсказка
    // обрывалась на полуслове («…относительно родител»).
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextDim);
    ImGui::TextWrapped("%s", T("Значения локальные — относительно родительской кости."));
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

void PropertiesPanel::Draw(DirectorHost& host) {
    ImGui::Begin((std::string(T("Свойства")) + "###Properties").c_str());

    GameObject obj = host.SelectedObject();
    if (!obj.Valid()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", T("Ничего не выбрано."));
        ImGui::Spacing();
        ImGui::TextWrapped("%s", T("Выберите объект в дереве сцены или кликните по нему во вьюпорте — "
                           "здесь появятся его свойства и ромбы для постановки ключей."));
        ImGui::End();
        return;
    }

    Scene& scene = host.CurrentScene();
    auto& reg = scene.Registry();
    const entt::entity e = obj.Entity();
    const int id = obj.Id();
    const bool simple = host.SimpleMode();

    // --- Шапка: иконка типа, имя, активность ---
    {
        std::string name = obj.Name();
        // Место под галочку Active справа: ширину считаем от её реального
        // размера, иначе на узкой панели подпись обрезается.
        const float activeWidth = ImGui::CalcTextSize(T("Активен")).x + ImGui::GetFrameHeight() +
                                  ImGui::GetStyle().ItemSpacing.x * 2.0f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - activeWidth);
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%s", name.c_str());
        if (ImGui::InputText("##name", buffer, sizeof(buffer),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            host.RenameObject(id, buffer);
        }
        ImGui::SameLine();
        StageItemComponent& item = reg.get_or_emplace<StageItemComponent>(e);
        if (ImGui::Checkbox(T("Активен"), &item.Visible)) host.PushUndo();
    }
    ImGui::Separator();

    // --- Transform ---
    if (SectionHeader(T("Трансформация"))) {
        Transform& tr = obj.GetTransform();
        bool edited = false;
        ImGui::Spacing();
        edited |= DrawVec3Row(host, T("Положение"), &tr.Position.x, Property::Position, 0.01f, "%.3f");
        ImGui::Spacing();
        edited |= DrawVec3Row(host, T("Поворот"), &tr.Rotation.x, Property::Rotation, 0.25f, "%.3f");
        ImGui::Spacing();
        edited |= DrawVec3Row(host, T("Масштаб"), &tr.Scale.x, Property::Scale, 0.01f, "%.3f");
        ImGui::Spacing();
        if (edited) host.NotifyObjectEdited(id);
    }

    // --- Blend Shapes ---
    DrawMorphSection(host);

    // --- Bone ---
    // Сразу после Transform: когда правишь кость, это и есть главное, ради чего
    // открыт инспектор, и искать его под настройками пост-обработки незачем.
    DrawBoneSection(host);

    // --- Camera ---
    if (CameraComponent* cam = reg.try_get<CameraComponent>(e)) {
        if (SectionHeader(T("Камера"))) {
            bool edited = false;
            const bool isActive = host.ActiveCameraId() == id;
            if (ImGui::RadioButton(T("Активная камера (с неё идёт рендер)"), isActive)) {
                host.SetActiveCameraId(id);
            }
            ImGui::Spacing();

            RowLabel(T("Проекция"));
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
            // Ортографической камеры у движка нет — показываем честно один
            // рабочий вариант, а не выпадающий список с неработающим пунктом.
            ImGui::BeginDisabled(true);
            int projection = 0;
            ImGui::Combo("##proj", &projection, T("Перспектива\0"));
            ImGui::EndDisabled();

            edited |= DrawFloatRow(host, T("Угол обзора"), &cam->Fov, Property::CameraFov, 0.2f, 1.0f, 179.0f, "%.2f");

            // Near/Far — строки без ромба: их анимация даёт артефакты глубины,
            // а не выразительный приём, поэтому ключей для них мы не заводим.
            RowLabel(T("Ближняя"));
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
            if (ImGui::DragFloat("##nearclip", &cam->NearClip, 0.01f, 0.001f, 10.0f, "%.2f")) edited = true;
            host.TrackLastItem();
            RowLabel(T("Дальняя"));
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
            if (ImGui::DragFloat("##farclip", &cam->FarClip, 1.0f, 1.0f, 10000.0f, "%.2f")) edited = true;
            host.TrackLastItem();

            CineCameraComponent& cine = reg.get_or_emplace<CineCameraComponent>(e);
            edited |= DrawFloatRow(host, T("Дистанция фокуса"), &cine.FocusDistance,
                                   Property::CameraFocusDistance, 0.05f, 0.01f, 500.0f, "%.2f");
            if (ImGui::Checkbox(T("Автофокус"), &cine.AutoFocus)) { host.PushUndo(); edited = true; }
            edited |= DrawFloatRow(host, T("Диафрагма"), &cine.Aperture,
                                   Property::CameraAperture, 0.01f, 0.7f, 32.0f, "%.2f");
            if (ImGui::Checkbox(T("Глубина резкости"), &cine.DepthOfField)) { host.PushUndo(); edited = true; }
            if (ImGui::Checkbox(T("Кадровое окно"), &cine.FilmBack)) { host.PushUndo(); edited = true; }
            if (!simple) {
                ImGui::SameLine(kLabelWidth + 24.0f);
                ImGui::SetNextItemWidth(90.0f);
                if (ImGui::DragFloat("##aspect", &cine.AspectRatio, 0.01f, 1.0f, 3.0f, "%.2f:1")) edited = true;
                host.TrackLastItem();
            }
            if (ImGui::Checkbox(T("Тряска камеры"), &cine.CameraShake)) { host.PushUndo(); edited = true; }
            if (cine.CameraShake && !simple) {
                RowLabel(T("Амплитуда"));
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
                if (ImGui::DragFloat("##shakeAmp", &cine.ShakeAmplitude, 0.005f, 0.0f, 1.0f, "%.3f")) edited = true;
                host.TrackLastItem();
                RowLabel(T("Частота"));
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
                if (ImGui::DragFloat("##shakeFreq", &cine.ShakeFrequency, 0.1f, 0.1f, 30.0f, "%.2f")) edited = true;
                host.TrackLastItem();
            }
            if (edited) host.NotifyObjectEdited(id);
        }

        // --- Post Process (свойство камеры, как в референсе) ---
        if (SectionHeader(T("Постобработка"))) {
            CineCameraComponent& cine = reg.get_or_emplace<CineCameraComponent>(e);
            bool edited = false;
            edited |= DrawEffectRow(host, T("Свечение"), &cine.Bloom, &cine.BloomIntensity,
                                    Property::PostBloom, 0.0f, 2.0f);
            edited |= DrawEffectRow(host, T("Смаз движения"), &cine.MotionBlur, &cine.MotionBlurAmount,
                                    Property::PostMotionBlur, 0.0f, 1.0f);
            edited |= DrawEffectRow(host, T("Цветокоррекция"), &cine.ColorGrading, &cine.ColorGradingAmount,
                                    Property::PostBloom, 0.0f, 2.0f);
            edited |= DrawEffectRow(host, T("Виньетка"), &cine.Vignette, &cine.VignetteAmount,
                                    Property::PostVignette, 0.0f, 1.0f);
            edited |= DrawEffectRow(host, T("Хроматическая аберрация"), &cine.ChromaticAberration,
                                    &cine.ChromaticAmount, Property::PostChromatic, 0.0f, 1.0f);
            if (edited) host.NotifyObjectEdited(id);
            if (!simple) {
                ImGui::Spacing();
                // Раньше подсказка была разбита на две строки вручную и всё
                // равно не помещалась: перенос по ширине панели надёжнее.
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextDim);
                ImGui::TextWrapped("%s", T("Motion Blur — камерный: смазывает движение и поворот камеры. "
                                   "Смаз от движения самих объектов не считается."));
                ImGui::PopStyleColor();
            }
        }
    }

    // --- Light ---
    if (LightComponent* light = reg.try_get<LightComponent>(e)) {
        if (SectionHeader(T("Свет"))) {
            bool edited = false;
            RowLabel(T("Тип"));
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
            int kind = light->Kind == LightComponent::Type::Spot ? 1 : 0;
            if (ImGui::Combo("##ltype", &kind, T("Точечный\0Прожектор\0"))) {
                host.PushUndo();
                light->Kind = kind == 1 ? LightComponent::Type::Spot : LightComponent::Type::Point;
                edited = true;
            }

            ImGui::TextUnformatted(T("Цвет"));
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
            if (ImGui::ColorEdit3("##lcolor", &light->Color.x, ImGuiColorEditFlags_Float)) edited = true;
            host.TrackLastItem();
            DrawKeyDiamond(host, Property::LightColor, true);

            edited |= DrawFloatRow(host, T("Яркость"), &light->Intensity,
                                   Property::LightIntensity, 0.02f, 0.0f, 100.0f, "%.2f");
            edited |= DrawFloatRow(host, T("Дальность"), &light->Range,
                                   Property::LightRange, 0.05f, 0.01f, 200.0f, "%.2f");

            if (light->Kind == LightComponent::Type::Spot) {
                RowLabel(T("Внутренний конус"));
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
                if (ImGui::SliderFloat("##inner", &light->InnerConeDeg, 0.0f, 89.0f, "%.1f°")) edited = true;
                host.TrackLastItem();
                RowLabel(T("Внешний конус"));
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
                if (ImGui::SliderFloat("##outer", &light->OuterConeDeg, 0.0f, 90.0f, "%.1f°")) edited = true;
                host.TrackLastItem();
                // Внутренний угол больше внешнего даёт вывернутый конус — чиним
                // молча, вместо того чтобы показывать сломанный свет.
                if (light->InnerConeDeg > light->OuterConeDeg) light->InnerConeDeg = light->OuterConeDeg;
            }
            if (edited) host.NotifyObjectEdited(id);
        }
    }

    // --- Mesh / материал ---
    if (MeshRendererComponent* mr = reg.try_get<MeshRendererComponent>(e)) {
        if (mr->Ref.type != MeshRef::Type::None && SectionHeader(T("Внешний вид"))) {
            ImGui::TextUnformatted(T("Цвет"));
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
            bool edited = ImGui::ColorEdit3("##mcolor", &mr->Color.x, ImGuiColorEditFlags_Float);
            host.TrackLastItem();
            DrawKeyDiamond(host, Property::Color, true);
            if (!mr->MaterialPath.empty()) {
                ImGui::TextDisabled(T("Материал: %s"), ShortPath(mr->MaterialPath).c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", mr->MaterialPath.c_str());
                ImGui::TextDisabled("%s", T("(материал перекрывает цвет)"));
            }
            if (edited) host.NotifyObjectEdited(id);
        }
    }

    // --- Персонаж (скелетная модель) ---
    if (AnimatedModelComponent* am = reg.try_get<AnimatedModelComponent>(e)) {
        if (SectionHeader(T("Персонаж"))) {
            ImGui::TextDisabled(T("Модель: %s"), am->Path.empty() ? T("<встроенная демо>")
                                                               : ShortPath(am->Path).c_str());
            if (!am->Path.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", am->Path.c_str());
            const int clipCount = am->Model ? (int)am->Model->Clips().size() : 0;
            if (clipCount == 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextDim);
            ImGui::TextWrapped("%s", T("Клипов в модели нет — анимировать можно только трансформом."));
            ImGui::PopStyleColor();
            } else {
                ImGui::Text(T("Клипов: %d"), clipCount);
                RowLabel(T("Клип"));
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
                if (ImGui::BeginCombo("##clip", am->Model->Clips()[(size_t)am->Clip % (size_t)clipCount].Name.c_str())) {
                    for (int i = 0; i < clipCount; ++i) {
                        const bool selected = am->Clip == i;
                        if (ImGui::Selectable(am->Model->Clips()[(size_t)i].Name.c_str(), selected)) {
                            host.PushUndo();
                            am->Clip = i;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::Spacing();
                // Кнопка кладёт блок клипа на таймлайн — это и есть способ
                // «собрать» анимацию персонажа из готовых клипов.
                if (ImGui::Button(T("Положить клип на таймлайн"), ImVec2(-1, 0))) {
                    host.PushUndo();
                    ClipTrack& track = host.Document().EnsureClipTrack(id);
                    ClipBlock block;
                    block.ClipIndex = am->Clip;
                    block.Name = am->Model->Clips()[(size_t)am->Clip].Name;
                    block.Start = host.CurrentTime();
                    const float clipLen = am->Model->Clips()[(size_t)am->Clip].Duration;
                    block.Duration = clipLen > 0.01f ? clipLen : 1.0f;
                    track.Blocks.push_back(block);
                    host.SetStatus(T("Клип добавлен на таймлайн"));
                }
            }
        }
    }

    // --- Дорожки этого объекта ---
    if (!simple) {
        AnimationDocument& doc = host.Document();
        int ownTracks = 0;
        for (const Track& t : doc.Tracks) {
            if (t.TargetId == id) ++ownTracks;
        }
        if (ownTracks > 0 && SectionHeader(T("Дорожки анимации"), false)) {
            for (Track& t : doc.Tracks) {
                if (t.TargetId != id) continue;
                int keys = 0;
                for (const Curve& c : t.Channels) keys += c.Count();
                ImGui::PushID(t.Id);
                ImGui::Text(T("%s — ключей: %d"), T(PropertyInfoOf(t.Prop).Label), keys);
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 60.0f);
                ImGui::Checkbox(T("Без звука"), &t.Muted);
                ImGui::PopID();
            }
        }
    }

    ImGui::End();
}

} // namespace d3d
