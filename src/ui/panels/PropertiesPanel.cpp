#include "ui/panels/PropertiesPanel.h"

#include <cstdio>

#include "imgui.h"

#include "anim/BonePose.h"
#include "anim/DirectorComponents.h"
#include "sage/render/SkinnedModel.h" // список клипов персонажа в инспекторе
#include "sage/scene/Components.h"
#include "ui/DirectorHost.h"
#include "ui/Icons.h"
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

void RowLabel(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(kLabelWidth);
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
        ImGui::SetTooltip(hasKey ? "Ключ на этом кадре — клик уберёт его"
                                 : "Поставить ключ на текущем кадре");
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
            host.SetStatus("Ключ убран");
        } else if (joint >= 0) {
            // Костное свойство ключится напрямую: у KeyProperty кости нет, а
            // заводить ради этого ещё один метод хоста — лишний слой.
            host.Document().KeyFromScene(host.CurrentScene(), id, prop, host.CurrentTime(), joint);
            host.SetStatus("Ключ на кости поставлен");
        } else {
            host.KeyProperty(id, prop);
            host.SetStatus("Ключ поставлен");
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
    const float avail = ImGui::GetContentRegionAvail().x - kKeyColumn;
    const float fieldWidth = (avail - 2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
    static const char* kAxis[3] = {"X", "Y", "Z"};
    static const ImU32 kAxisColor[3] = {0xFF4A4AE8, 0xFF5CC85C, 0xFFE8964A};

    bool changed = false;
    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine();
        ImGui::PushID(i);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18, 4));
        if (ImGui::DragFloat("##v", &value[i], speed, 0.0f, 0.0f, format)) changed = true;
        ImGui::PopStyleVar();
        host.TrackLastItem();
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(pos.x + 7.0f, pos.y + ImGui::GetStyle().FramePadding.y), kAxisColor[i], kAxis[i]);
        ImGui::PopID();
    }

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

void PropertiesPanel::DrawBoneSection(DirectorHost& host) {
    const BoneSelection& bone = host.SelectedBone();
    if (!bone.Valid() || bone.EntityId != host.SelectedId()) return;

    Scene& scene = host.CurrentScene();
    const sage::anim::Skeleton* skeleton = SkeletonOf(scene, bone.EntityId);
    if (!skeleton || bone.Joint >= skeleton->Count()) return;

    const sage::anim::Joint& joint = skeleton->Joints[(size_t)bone.Joint];
    char title[160];
    std::snprintf(title, sizeof(title), "Bone — %s",
                  joint.Name.empty() ? "(без имени)" : joint.Name.c_str());
    if (!SectionHeader(title)) return;

    ImGui::Spacing();
    if (joint.Parent >= 0 && joint.Parent < skeleton->Count()) {
        ImGui::TextDisabled("Родитель: %s", skeleton->Joints[(size_t)joint.Parent].Name.c_str());
    } else {
        ImGui::TextDisabled("Корневая кость");
    }
    ImGui::Spacing();

    // Значения ЛОКАЛЬНЫЕ — относительно родительской кости, как их хранит клип.
    // Мировые координаты кости аниматору не нужны и только путали бы: «согнуть
    // локоть на 30°» — это локальный поворот, а не позиция в мире.
    glm::vec3 t, s;
    glm::quat r;
    if (!ReadBoneLocal(scene, bone.EntityId, bone.Joint, t, r, s)) {
        ImGui::TextDisabled("Поза ещё не посчитана — модель загружается.");
        return;
    }
    glm::vec3 euler = EulerDegreesFromQuat(r);

    bool edited = false;
    if (DrawVec3Row(host, "Location", &t.x, Property::BonePosition, 0.005f, "%.4f", bone.Joint)) {
        const float v[3] = {t.x, t.y, t.z};
        WriteBoneChannel(scene, bone.EntityId, bone.Joint, BoneChannel::Translation, v);
        edited = true;
    }
    ImGui::Spacing();
    if (DrawVec3Row(host, "Rotation", &euler.x, Property::BoneRotation, 0.25f, "%.2f", bone.Joint)) {
        const float v[3] = {euler.x, euler.y, euler.z};
        WriteBoneChannel(scene, bone.EntityId, bone.Joint, BoneChannel::Rotation, v);
        edited = true;
    }
    ImGui::Spacing();
    if (DrawVec3Row(host, "Scale", &s.x, Property::BoneScale, 0.005f, "%.4f", bone.Joint)) {
        const float v[3] = {s.x, s.y, s.z};
        WriteBoneChannel(scene, bone.EntityId, bone.Joint, BoneChannel::Scale, v);
        edited = true;
    }
    if (edited) host.NotifyObjectEdited(bone.EntityId);

    ImGui::Spacing();
    if (ImGui::Button("Ключ на кость")) host.KeyBone();
    ImGui::SameLine();
    if (ImGui::Button("Заключить позу")) host.KeyWholePose(bone.EntityId);
    ImGui::SameLine();
    if (ImGui::Button("Сброс")) {
        // Сбрасываем только ЭТУ кость: «сброс всей позы» есть в дереве сцены, и
        // потерять всю работу по кнопке рядом с одной костью было бы обидно.
        host.PushUndo();
        for (BoneChannel channel : {BoneChannel::Translation, BoneChannel::Rotation,
                                    BoneChannel::Scale}) {
            ClearBoneChannel(scene, bone.EntityId, bone.Joint, channel);
        }
        host.SetStatus("Кость вернулась под управление клипа");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Снять ручную позу с ЭТОЙ кости — она снова пойдёт за клипом");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Значения локальные — относительно родительской кости.");
    ImGui::Spacing();
}

void PropertiesPanel::Draw(DirectorHost& host) {
    ImGui::Begin("Properties");

    GameObject obj = host.SelectedObject();
    if (!obj.Valid()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Ничего не выбрано.");
        ImGui::Spacing();
        ImGui::TextWrapped("Выберите объект в дереве сцены или кликните по нему во вьюпорте — "
                           "здесь появятся его свойства и ромбы для постановки ключей.");
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
        const float activeWidth = ImGui::CalcTextSize("Active").x + ImGui::GetFrameHeight() +
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
        if (ImGui::Checkbox("Active", &item.Visible)) host.PushUndo();
    }
    ImGui::Separator();

    // --- Transform ---
    if (SectionHeader("Transform")) {
        Transform& tr = obj.GetTransform();
        bool edited = false;
        ImGui::Spacing();
        edited |= DrawVec3Row(host, "Location", &tr.Position.x, Property::Position, 0.01f, "%.3f");
        ImGui::Spacing();
        edited |= DrawVec3Row(host, "Rotation", &tr.Rotation.x, Property::Rotation, 0.25f, "%.3f");
        ImGui::Spacing();
        edited |= DrawVec3Row(host, "Scale", &tr.Scale.x, Property::Scale, 0.01f, "%.3f");
        ImGui::Spacing();
        if (edited) host.NotifyObjectEdited(id);
    }

    // --- Bone ---
    // Сразу после Transform: когда правишь кость, это и есть главное, ради чего
    // открыт инспектор, и искать его под настройками пост-обработки незачем.
    DrawBoneSection(host);

    // --- Camera ---
    if (CameraComponent* cam = reg.try_get<CameraComponent>(e)) {
        if (SectionHeader("Camera")) {
            bool edited = false;
            const bool isActive = host.ActiveCameraId() == id;
            if (ImGui::RadioButton("Активная камера (с неё идёт рендер)", isActive)) {
                host.SetActiveCameraId(id);
            }
            ImGui::Spacing();

            RowLabel("Projection");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
            // Ортографической камеры у движка нет — показываем честно один
            // рабочий вариант, а не выпадающий список с неработающим пунктом.
            ImGui::BeginDisabled(true);
            int projection = 0;
            ImGui::Combo("##proj", &projection, "Perspective\0");
            ImGui::EndDisabled();

            edited |= DrawFloatRow(host, "FOV", &cam->Fov, Property::CameraFov, 0.2f, 1.0f, 179.0f, "%.2f");

            // Near/Far — строки без ромба: их анимация даёт артефакты глубины,
            // а не выразительный приём, поэтому ключей для них мы не заводим.
            RowLabel("Near");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
            if (ImGui::DragFloat("##nearclip", &cam->NearClip, 0.01f, 0.001f, 10.0f, "%.2f")) edited = true;
            host.TrackLastItem();
            RowLabel("Far");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
            if (ImGui::DragFloat("##farclip", &cam->FarClip, 1.0f, 1.0f, 10000.0f, "%.2f")) edited = true;
            host.TrackLastItem();

            CineCameraComponent& cine = reg.get_or_emplace<CineCameraComponent>(e);
            edited |= DrawFloatRow(host, "Focus Distance", &cine.FocusDistance,
                                   Property::CameraFocusDistance, 0.05f, 0.01f, 500.0f, "%.2f");
            if (ImGui::Checkbox("Auto Focus", &cine.AutoFocus)) { host.PushUndo(); edited = true; }
            edited |= DrawFloatRow(host, "Aperture", &cine.Aperture,
                                   Property::CameraAperture, 0.01f, 0.7f, 32.0f, "%.2f");
            if (ImGui::Checkbox("Depth of Field", &cine.DepthOfField)) { host.PushUndo(); edited = true; }
            if (ImGui::Checkbox("Film Back", &cine.FilmBack)) { host.PushUndo(); edited = true; }
            if (!simple) {
                ImGui::SameLine(kLabelWidth + 24.0f);
                ImGui::SetNextItemWidth(90.0f);
                if (ImGui::DragFloat("##aspect", &cine.AspectRatio, 0.01f, 1.0f, 3.0f, "%.2f:1")) edited = true;
                host.TrackLastItem();
            }
            if (ImGui::Checkbox("Camera Shake", &cine.CameraShake)) { host.PushUndo(); edited = true; }
            if (cine.CameraShake && !simple) {
                RowLabel("Amplitude");
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
                if (ImGui::DragFloat("##shakeAmp", &cine.ShakeAmplitude, 0.005f, 0.0f, 1.0f, "%.3f")) edited = true;
                host.TrackLastItem();
                RowLabel("Frequency");
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
                if (ImGui::DragFloat("##shakeFreq", &cine.ShakeFrequency, 0.1f, 0.1f, 30.0f, "%.2f")) edited = true;
                host.TrackLastItem();
            }
            if (edited) host.NotifyObjectEdited(id);
        }

        // --- Post Process (свойство камеры, как в референсе) ---
        if (SectionHeader("Post Process")) {
            CineCameraComponent& cine = reg.get_or_emplace<CineCameraComponent>(e);
            bool edited = false;
            edited |= DrawEffectRow(host, "Bloom", &cine.Bloom, &cine.BloomIntensity,
                                    Property::PostBloom, 0.0f, 2.0f);
            edited |= DrawEffectRow(host, "Motion Blur", &cine.MotionBlur, &cine.MotionBlurAmount,
                                    Property::PostMotionBlur, 0.0f, 1.0f);
            edited |= DrawEffectRow(host, "Color Grading", &cine.ColorGrading, &cine.ColorGradingAmount,
                                    Property::PostBloom, 0.0f, 2.0f);
            edited |= DrawEffectRow(host, "Vignette", &cine.Vignette, &cine.VignetteAmount,
                                    Property::PostVignette, 0.0f, 1.0f);
            edited |= DrawEffectRow(host, "Chromatic Aberration", &cine.ChromaticAberration,
                                    &cine.ChromaticAmount, Property::PostChromatic, 0.0f, 1.0f);
            if (edited) host.NotifyObjectEdited(id);
            if (!simple) {
                ImGui::Spacing();
                ImGui::TextDisabled("Motion Blur — камерный: смазывает движение и поворот");
                ImGui::TextDisabled("камеры. Смаз от движения самих объектов не считается.");
            }
        }
    }

    // --- Light ---
    if (LightComponent* light = reg.try_get<LightComponent>(e)) {
        if (SectionHeader("Light")) {
            bool edited = false;
            RowLabel("Type");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
            int kind = light->Kind == LightComponent::Type::Spot ? 1 : 0;
            if (ImGui::Combo("##ltype", &kind, "Point\0Spot\0")) {
                host.PushUndo();
                light->Kind = kind == 1 ? LightComponent::Type::Spot : LightComponent::Type::Point;
                edited = true;
            }

            ImGui::TextUnformatted("Color");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
            if (ImGui::ColorEdit3("##lcolor", &light->Color.x, ImGuiColorEditFlags_Float)) edited = true;
            host.TrackLastItem();
            DrawKeyDiamond(host, Property::LightColor, true);

            edited |= DrawFloatRow(host, "Intensity", &light->Intensity,
                                   Property::LightIntensity, 0.02f, 0.0f, 100.0f, "%.2f");
            edited |= DrawFloatRow(host, "Range", &light->Range,
                                   Property::LightRange, 0.05f, 0.01f, 200.0f, "%.2f");

            if (light->Kind == LightComponent::Type::Spot) {
                RowLabel("Inner Cone");
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
                if (ImGui::SliderFloat("##inner", &light->InnerConeDeg, 0.0f, 89.0f, "%.1f°")) edited = true;
                host.TrackLastItem();
                RowLabel("Outer Cone");
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
        if (mr->Ref.type != MeshRef::Type::None && SectionHeader("Appearance")) {
            ImGui::TextUnformatted("Color");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kKeyColumn);
            bool edited = ImGui::ColorEdit3("##mcolor", &mr->Color.x, ImGuiColorEditFlags_Float);
            host.TrackLastItem();
            DrawKeyDiamond(host, Property::Color, true);
            if (!mr->MaterialPath.empty()) {
                ImGui::TextDisabled("Материал: %s", mr->MaterialPath.c_str());
                ImGui::TextDisabled("(материал перекрывает цвет)");
            }
            if (edited) host.NotifyObjectEdited(id);
        }
    }

    // --- Персонаж (скелетная модель) ---
    if (AnimatedModelComponent* am = reg.try_get<AnimatedModelComponent>(e)) {
        if (SectionHeader("Character")) {
            ImGui::TextDisabled("Модель: %s", am->Path.empty() ? "<встроенная демо>" : am->Path.c_str());
            const int clipCount = am->Model ? (int)am->Model->Clips().size() : 0;
            if (clipCount == 0) {
                ImGui::TextDisabled("Клипов в модели нет — анимировать можно только трансформом.");
            } else {
                ImGui::Text("Клипов: %d", clipCount);
                RowLabel("Clip");
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
                if (ImGui::Button("Положить клип на таймлайн", ImVec2(-1, 0))) {
                    host.PushUndo();
                    ClipTrack& track = host.Document().EnsureClipTrack(id);
                    ClipBlock block;
                    block.ClipIndex = am->Clip;
                    block.Name = am->Model->Clips()[(size_t)am->Clip].Name;
                    block.Start = host.CurrentTime();
                    const float clipLen = am->Model->Clips()[(size_t)am->Clip].Duration;
                    block.Duration = clipLen > 0.01f ? clipLen : 1.0f;
                    track.Blocks.push_back(block);
                    host.SetStatus("Клип добавлен на таймлайн");
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
        if (ownTracks > 0 && SectionHeader("Animation Tracks", false)) {
            for (Track& t : doc.Tracks) {
                if (t.TargetId != id) continue;
                int keys = 0;
                for (const Curve& c : t.Channels) keys += c.Count();
                ImGui::PushID(t.Id);
                ImGui::Text("%s — ключей: %d", PropertyInfoOf(t.Prop).Label, keys);
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 60.0f);
                ImGui::Checkbox("Mute", &t.Muted);
                ImGui::PopID();
            }
        }
    }

    ImGui::End();
}

} // namespace d3d
