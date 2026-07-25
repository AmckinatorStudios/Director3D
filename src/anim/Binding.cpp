#include "anim/Binding.h"

#include "anim/DirectorComponents.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace d3d {

namespace {

// Цвета каналов совпадают с осями гизмо: X — красный, Y — зелёный, Z — синий.
// Аниматор видит в графе ту же ось, за которую тянул во вьюпорте.
constexpr unsigned int kAxisX = 0xFF4A4AE8; // ImU32: 0xAABBGGRR
constexpr unsigned int kAxisY = 0xFF5CC85C;
constexpr unsigned int kAxisZ = 0xFFE8964A;
constexpr unsigned int kScalar = 0xFF6FC4E8;
constexpr unsigned int kChanR = 0xFF4A4AE8;
constexpr unsigned int kChanG = 0xFF5CC85C;
constexpr unsigned int kChanB = 0xFFE86F4A;

const std::vector<PropertyInfo> kTable = {
    {Property::Position,            "position",       "Location",        3, {"X", "Y", "Z"}, {kAxisX, kAxisY, kAxisZ}, false},
    {Property::Rotation,            "rotation",       "Rotation",        3, {"X", "Y", "Z"}, {kAxisX, kAxisY, kAxisZ}, false},
    {Property::Scale,               "scale",          "Scale",           3, {"X", "Y", "Z"}, {kAxisX, kAxisY, kAxisZ}, false},
    {Property::Color,               "color",          "Color",           3, {"R", "G", "B"}, {kChanR, kChanG, kChanB}, false},
    {Property::LightColor,          "lightColor",     "Light Color",     3, {"R", "G", "B"}, {kChanR, kChanG, kChanB}, false},
    {Property::LightIntensity,      "lightIntensity", "Light Intensity", 1, {"", "", ""},    {kScalar, 0, 0},          false},
    {Property::LightRange,          "lightRange",     "Light Range",     1, {"", "", ""},    {kScalar, 0, 0},          false},
    {Property::CameraFov,           "cameraFov",      "FOV",             1, {"", "", ""},    {kScalar, 0, 0},          false},
    {Property::CameraFocusDistance, "focusDistance",  "Focus Distance",  1, {"", "", ""},    {kScalar, 0, 0},          false},
    {Property::CameraAperture,      "aperture",       "Aperture",        1, {"", "", ""},    {kScalar, 0, 0},          false},
    {Property::PostBloom,           "postBloom",      "Bloom",           1, {"", "", ""},    {kScalar, 0, 0},          false},
    {Property::PostVignette,        "postVignette",   "Vignette",        1, {"", "", ""},    {kScalar, 0, 0},          false},
    {Property::PostMotionBlur,      "postMotionBlur", "Motion Blur",     1, {"", "", ""},    {kScalar, 0, 0},          false},
    {Property::PostChromatic,       "postChromatic",  "Chromatic Ab.",   1, {"", "", ""},    {kScalar, 0, 0},          false},
    {Property::Visibility,          "visibility",     "Visibility",      1, {"", "", ""},    {kScalar, 0, 0},          true},
};

// Сущность по id + её entity. Возвращает entt::null, если сущности нет —
// дорожка могла пережить удаление объекта (проект грузится целиком, а не падает).
entt::entity Resolve(Scene& scene, int entityId) {
    GameObject obj = scene.Get(entityId);
    return obj.Valid() ? obj.Entity() : entt::null;
}

} // namespace

const std::vector<PropertyInfo>& PropertyTable() { return kTable; }

const PropertyInfo& PropertyInfoOf(Property prop) {
    for (const PropertyInfo& info : kTable) {
        if (info.Id == prop) return info;
    }
    return kTable.front(); // недостижимо: все значения enum есть в таблице
}

bool PropertyFromKey(const std::string& key, Property& out) {
    for (const PropertyInfo& info : kTable) {
        if (key == info.Key) { out = info.Id; return true; }
    }
    return false;
}

bool PropertyApplies(Scene& scene, int entityId, Property prop) {
    entt::entity e = Resolve(scene, entityId);
    if (e == entt::null) return false;
    auto& reg = scene.Registry();
    switch (prop) {
        case Property::Position:
        case Property::Rotation:
        case Property::Scale:
            return reg.all_of<Transform>(e);
        case Property::Color:
            return reg.all_of<MeshRendererComponent>(e);
        case Property::LightColor:
        case Property::LightIntensity:
        case Property::LightRange:
            return reg.all_of<LightComponent>(e);
        case Property::CameraFov:
            return reg.all_of<CameraComponent>(e);
        case Property::CameraFocusDistance:
        case Property::CameraAperture:
        case Property::PostBloom:
        case Property::PostVignette:
        case Property::PostMotionBlur:
        case Property::PostChromatic:
            // Киношные параметры живут на камере: компонент навешивается
            // Director 3D при создании камеры, но проект мог прийти и без него.
            return reg.all_of<CameraComponent>(e);
        case Property::Visibility:
            return true; // спрятать можно что угодно
    }
    return false;
}

bool ReadProperty(Scene& scene, int entityId, Property prop, float* values) {
    entt::entity e = Resolve(scene, entityId);
    if (e == entt::null || !values) return false;
    auto& reg = scene.Registry();

    switch (prop) {
        case Property::Position:
        case Property::Rotation:
        case Property::Scale: {
            const Transform* tr = reg.try_get<Transform>(e);
            if (!tr) return false;
            const glm::vec3& v = prop == Property::Position ? tr->Position
                               : prop == Property::Rotation ? tr->Rotation
                                                            : tr->Scale;
            values[0] = v.x; values[1] = v.y; values[2] = v.z;
            return true;
        }
        case Property::Color: {
            const MeshRendererComponent* mr = reg.try_get<MeshRendererComponent>(e);
            if (!mr) return false;
            values[0] = mr->Color.r; values[1] = mr->Color.g; values[2] = mr->Color.b;
            return true;
        }
        case Property::LightColor: {
            const LightComponent* lc = reg.try_get<LightComponent>(e);
            if (!lc) return false;
            values[0] = lc->Color.r; values[1] = lc->Color.g; values[2] = lc->Color.b;
            return true;
        }
        case Property::LightIntensity: {
            const LightComponent* lc = reg.try_get<LightComponent>(e);
            if (!lc) return false;
            values[0] = lc->Intensity;
            return true;
        }
        case Property::LightRange: {
            const LightComponent* lc = reg.try_get<LightComponent>(e);
            if (!lc) return false;
            values[0] = lc->Range;
            return true;
        }
        case Property::CameraFov: {
            const CameraComponent* cc = reg.try_get<CameraComponent>(e);
            if (!cc) return false;
            values[0] = cc->Fov;
            return true;
        }
        case Property::CameraFocusDistance:
        case Property::CameraAperture:
        case Property::PostBloom:
        case Property::PostVignette:
        case Property::PostMotionBlur:
        case Property::PostChromatic: {
            // Компонента может не быть (сцена из .sage без Director-данных) —
            // тогда отдаём дефолты, а не «нет свойства»: ключ поставится, и
            // при записи компонент создастся.
            const CineCameraComponent* cine = reg.try_get<CineCameraComponent>(e);
            CineCameraComponent fallback;
            const CineCameraComponent& c = cine ? *cine : fallback;
            switch (prop) {
                case Property::CameraFocusDistance: values[0] = c.FocusDistance; break;
                case Property::CameraAperture:      values[0] = c.Aperture; break;
                case Property::PostBloom:           values[0] = c.BloomIntensity; break;
                case Property::PostVignette:        values[0] = c.VignetteAmount; break;
                case Property::PostMotionBlur:      values[0] = c.MotionBlurAmount; break;
                default:                            values[0] = c.ChromaticAmount; break;
            }
            return reg.all_of<CameraComponent>(e);
        }
        case Property::Visibility: {
            const StageItemComponent* item = reg.try_get<StageItemComponent>(e);
            values[0] = (!item || item->Visible) ? 1.0f : 0.0f;
            return true;
        }
    }
    return false;
}

bool WriteProperty(Scene& scene, int entityId, Property prop, const float* values) {
    entt::entity e = Resolve(scene, entityId);
    if (e == entt::null || !values) return false;
    auto& reg = scene.Registry();

    switch (prop) {
        case Property::Position:
        case Property::Rotation:
        case Property::Scale: {
            Transform* tr = reg.try_get<Transform>(e);
            if (!tr) return false;
            glm::vec3 v(values[0], values[1], values[2]);
            if (prop == Property::Position) tr->Position = v;
            else if (prop == Property::Rotation) tr->Rotation = v;
            else tr->Scale = v;
            return true;
        }
        case Property::Color: {
            MeshRendererComponent* mr = reg.try_get<MeshRendererComponent>(e);
            if (!mr) return false;
            mr->Color = glm::vec3(values[0], values[1], values[2]);
            return true;
        }
        case Property::LightColor: {
            LightComponent* lc = reg.try_get<LightComponent>(e);
            if (!lc) return false;
            lc->Color = glm::vec3(values[0], values[1], values[2]);
            return true;
        }
        case Property::LightIntensity: {
            LightComponent* lc = reg.try_get<LightComponent>(e);
            if (!lc) return false;
            lc->Intensity = values[0];
            return true;
        }
        case Property::LightRange: {
            LightComponent* lc = reg.try_get<LightComponent>(e);
            if (!lc) return false;
            // Ноль и отрицательный радиус ломают затухание (деление на Range
            // в PointLight::Linear) — держим минимум, а не пускаем NaN в шейдер.
            lc->Range = values[0] < 0.01f ? 0.01f : values[0];
            return true;
        }
        case Property::CameraFov: {
            CameraComponent* cc = reg.try_get<CameraComponent>(e);
            if (!cc) return false;
            // Вырожденный/развёрнутый FOV даёт нерабочую матрицу проекции.
            cc->Fov = glm::clamp(values[0], 1.0f, 179.0f);
            return true;
        }
        case Property::CameraFocusDistance:
        case Property::CameraAperture:
        case Property::PostBloom:
        case Property::PostVignette:
        case Property::PostMotionBlur:
        case Property::PostChromatic: {
            if (!reg.all_of<CameraComponent>(e)) return false;
            CineCameraComponent& c = reg.get_or_emplace<CineCameraComponent>(e);
            switch (prop) {
                case Property::CameraFocusDistance: c.FocusDistance = glm::max(values[0], 0.01f); break;
                case Property::CameraAperture:      c.Aperture = glm::clamp(values[0], 0.7f, 32.0f); break;
                case Property::PostBloom:           c.BloomIntensity = glm::clamp(values[0], 0.0f, 4.0f); break;
                case Property::PostVignette:        c.VignetteAmount = glm::clamp(values[0], 0.0f, 1.0f); break;
                case Property::PostMotionBlur:      c.MotionBlurAmount = glm::clamp(values[0], 0.0f, 1.0f); break;
                default:                            c.ChromaticAmount = glm::clamp(values[0], 0.0f, 1.0f); break;
            }
            return true;
        }
        case Property::Visibility: {
            reg.get_or_emplace<StageItemComponent>(e).Visible = values[0] >= 0.5f;
            return true;
        }
    }
    return false;
}

std::vector<Property> ApplicableProperties(Scene& scene, int entityId) {
    std::vector<Property> out;
    out.reserve(kTable.size());
    for (const PropertyInfo& info : kTable) {
        if (PropertyApplies(scene, entityId, info.Id)) out.push_back(info.Id);
    }
    return out;
}

} // namespace d3d
