#include "anim/BonePose.h"

// Разложение матрицы и углы Эйлера живут в экспериментальных расширениях GLM;
// движок включает их тем же способом (см. sage/scene/Transform.h).
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include "anim/DirectorComponents.h"
#include "sage/render/SkinnedModel.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace d3d {

namespace {

// Компонент анимированной модели сущности, если он есть и модель загрузилась.
AnimatedModelComponent* AnimOf(Scene& scene, int entityId) {
    GameObject obj = scene.Get(entityId);
    if (!obj.Valid()) return nullptr;
    AnimatedModelComponent* am = scene.Registry().try_get<AnimatedModelComponent>(obj.Entity());
    if (!am || !am->Model) return nullptr;
    return am;
}

// Разбор матрицы на TRS. Своя реализация вместо glm::decompose: нам не нужны
// ни перекос, ни перспектива (их не бывает у костей), а нужен предсказуемый
// результат — glm::decompose на вырожденном масштабе отдаёт мусор молча.
void DecomposeTRS(const glm::mat4& m, glm::vec3& t, glm::quat& r, glm::vec3& s) {
    t = glm::vec3(m[3]);

    glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    s.x = glm::length(c0);
    s.y = glm::length(c1);
    s.z = glm::length(c2);

    // Отрицательный определитель = зеркальный масштаб. Кватернион зеркало не
    // выражает, поэтому знак уводим в масштаб по X — так хотя бы одна ось
    // остаётся честной, а поворот не «выворачивается».
    if (glm::determinant(glm::mat3(m)) < 0.0f) {
        s.x = -s.x;
        c0 = -c0;
    }

    // Нулевой масштаб не нормируется — оставляем поворот единичным, иначе
    // получили бы NaN во всей позе.
    const float kEps = 1e-8f;
    if (s.x > kEps && s.y > kEps && s.z > kEps) {
        glm::mat3 rot(c0 / glm::abs(s.x), c1 / s.y, c2 / s.z);
        r = glm::normalize(glm::quat_cast(rot));
    } else {
        r = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
}

} // namespace

glm::quat QuatFromEulerDegrees(const glm::vec3& degrees) {
    // Порядок ровно как в Transform::LocalMatrix движка: Rx * Ry * Rz.
    return glm::quat_cast(glm::eulerAngleXYZ(glm::radians(degrees.x), glm::radians(degrees.y),
                                             glm::radians(degrees.z)));
}

glm::vec3 EulerDegreesFromQuat(const glm::quat& q) {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    glm::extractEulerAngleXYZ(glm::mat4_cast(q), x, y, z);
    return glm::degrees(glm::vec3(x, y, z));
}

const sage::anim::Skeleton* SkeletonOf(Scene& scene, int entityId) {
    AnimatedModelComponent* am = AnimOf(scene, entityId);
    if (!am) return nullptr;
    const sage::anim::Skeleton& sk = am->Model->GetSkeleton();
    return sk.Count() > 0 ? &sk : nullptr;
}

int FindJoint(const sage::anim::Skeleton& skeleton, const std::string& name) {
    for (int i = 0; i < skeleton.Count(); ++i) {
        if (skeleton.Joints[(size_t)i].Name == name) return i;
    }
    return -1;
}

std::string JointName(Scene& scene, int entityId, int joint) {
    const sage::anim::Skeleton* sk = SkeletonOf(scene, entityId);
    if (!sk || joint < 0 || joint >= sk->Count()) return {};
    return sk->Joints[(size_t)joint].Name;
}

bool ReadBoneLocal(Scene& scene, int entityId, int joint, glm::vec3& translation,
                   glm::quat& rotation, glm::vec3& scale) {
    AnimatedModelComponent* am = AnimOf(scene, entityId);
    if (!am) return false;
    const sage::anim::Skeleton& sk = am->Model->GetSkeleton();
    if (joint < 0 || joint >= sk.Count()) return false;

    // База — дефолт скелета. Он же ответ, если позы ещё нет (модель только что
    // загрузилась и Animator не считал ни одного кадра).
    const sage::anim::Joint& j = sk.Joints[(size_t)joint];
    translation = j.Translation;
    rotation = j.Rotation;
    scale = j.Scale;

    // Что сейчас видно: локаль восстанавливается из глобалей движка, потому что
    // наружу он отдаёт только их. G_local = G_parent⁻¹ * G_joint.
    const std::vector<glm::mat4>& globals = am->Anim.GlobalMatrices();
    if ((int)globals.size() == sk.Count()) {
        const int parent = j.Parent;
        const glm::mat4 local = parent >= 0
                                    ? glm::inverse(globals[(size_t)parent]) * globals[(size_t)joint]
                                    : globals[(size_t)joint];
        DecomposeTRS(local, translation, rotation, scale);
    }

    // Переопределение поверх: оно и есть «что видно», когда стоит.
    GameObject obj = scene.Get(entityId);
    if (const PoseComponent* pose = scene.Registry().try_get<PoseComponent>(obj.Entity())) {
        if (const sage::anim::JointPose* p = pose->Find(joint)) {
            if (p->HasTranslation) translation = p->Translation;
            if (p->HasRotation) rotation = p->Rotation;
            if (p->HasScale) scale = p->Scale;
        }
    }
    return true;
}

bool ReadBoneChannel(Scene& scene, int entityId, int joint, BoneChannel channel, float* values) {
    if (!values) return false;
    glm::vec3 t, s;
    glm::quat r;
    if (!ReadBoneLocal(scene, entityId, joint, t, r, s)) return false;

    const glm::vec3 out = channel == BoneChannel::Translation ? t
                          : channel == BoneChannel::Scale     ? s
                                                              : EulerDegreesFromQuat(r);
    values[0] = out.x;
    values[1] = out.y;
    values[2] = out.z;
    return true;
}

bool WriteBoneChannel(Scene& scene, int entityId, int joint, BoneChannel channel,
                      const float* values) {
    if (!values) return false;
    AnimatedModelComponent* am = AnimOf(scene, entityId);
    if (!am) return false;
    const int count = am->Model->GetSkeleton().Count();
    if (joint < 0 || joint >= count) return false;

    GameObject obj = scene.Get(entityId);
    auto& reg = scene.Registry();
    PoseComponent& pose = reg.all_of<PoseComponent>(obj.Entity())
                              ? reg.get<PoseComponent>(obj.Entity())
                              : reg.emplace<PoseComponent>(obj.Entity());

    sage::anim::JointPose& jp = pose.Ensure(joint, count);
    const glm::vec3 v(values[0], values[1], values[2]);
    switch (channel) {
        case BoneChannel::Translation: jp.Translation = v; jp.HasTranslation = true; break;
        case BoneChannel::Rotation:    jp.Rotation = QuatFromEulerDegrees(v); jp.HasRotation = true; break;
        case BoneChannel::Scale:       jp.Scale = v; jp.HasScale = true; break;
    }

    // Переустановка указателя обязательна: emplace выше мог перевыделить
    // хранилище компонентов, и старый указатель в Animator указывал бы в никуда.
    am->Anim.SetPoseOverride(&pose.Joints);
    am->Anim.RefreshPose();
    return true;
}

bool ClearBoneChannel(Scene& scene, int entityId, int joint, BoneChannel channel) {
    AnimatedModelComponent* am = AnimOf(scene, entityId);
    if (!am) return false;
    GameObject obj = scene.Get(entityId);
    PoseComponent* pose = scene.Registry().try_get<PoseComponent>(obj.Entity());
    if (!pose || joint < 0 || joint >= (int)pose->Joints.size()) return false;

    sage::anim::JointPose& jp = pose->Joints[(size_t)joint];
    switch (channel) {
        case BoneChannel::Translation: jp.HasTranslation = false; break;
        case BoneChannel::Rotation:    jp.HasRotation = false; break;
        case BoneChannel::Scale:       jp.HasScale = false; break;
    }
    am->Anim.SetPoseOverride(&pose->Joints);
    am->Anim.RefreshPose();
    return true;
}

bool BoneWorldMatrix(Scene& scene, int entityId, int joint, glm::mat4& out) {
    AnimatedModelComponent* am = AnimOf(scene, entityId);
    if (!am) return false;
    const std::vector<glm::mat4>& globals = am->Anim.GlobalMatrices();
    if (joint < 0 || joint >= (int)globals.size()) return false;

    GameObject obj = scene.Get(entityId);
    out = scene.WorldMatrix(obj.Entity()) * globals[(size_t)joint];
    return true;
}

bool BoneLocalFromWorld(Scene& scene, int entityId, int joint, const glm::mat4& world,
                        glm::vec3& translation, glm::quat& rotation, glm::vec3& scale) {
    AnimatedModelComponent* am = AnimOf(scene, entityId);
    if (!am) return false;
    const sage::anim::Skeleton& sk = am->Model->GetSkeleton();
    const std::vector<glm::mat4>& globals = am->Anim.GlobalMatrices();
    if (joint < 0 || joint >= sk.Count() || (int)globals.size() != sk.Count()) return false;

    GameObject obj = scene.Get(entityId);
    // Разворачиваем цепочку: мир -> пространство модели -> пространство родителя.
    const glm::mat4 model = glm::inverse(scene.WorldMatrix(obj.Entity())) * world;
    const int parent = sk.Joints[(size_t)joint].Parent;
    const glm::mat4 local = parent >= 0 ? glm::inverse(globals[(size_t)parent]) * model : model;

    DecomposeTRS(local, translation, rotation, scale);
    return true;
}

void SyncPoseOverride(Scene& scene, int entityId) {
    AnimatedModelComponent* am = AnimOf(scene, entityId);
    if (!am) return;
    GameObject obj = scene.Get(entityId);
    PoseComponent* pose = scene.Registry().try_get<PoseComponent>(obj.Entity());
    // Пустая поза — это отсутствие переопределений, а не «переопределить нулями».
    am->Anim.SetPoseOverride(pose && !pose->Joints.empty() ? &pose->Joints : nullptr);
}

void SyncAllPoseOverrides(Scene& scene) {
    auto view = scene.Registry().view<AnimatedModelComponent>();
    for (auto e : view) {
        AnimatedModelComponent& am = view.get<AnimatedModelComponent>(e);
        if (!am.Model) continue;
        PoseComponent* pose = scene.Registry().try_get<PoseComponent>(e);
        am.Anim.SetPoseOverride(pose && !pose->Joints.empty() ? &pose->Joints : nullptr);
    }
}

void ClearPose(Scene& scene, int entityId) {
    GameObject obj = scene.Get(entityId);
    if (!obj.Valid()) return;
    auto& reg = scene.Registry();
    if (PoseComponent* pose = reg.try_get<PoseComponent>(obj.Entity())) {
        pose->Joints.clear();
    }
    if (AnimatedModelComponent* am = reg.try_get<AnimatedModelComponent>(obj.Entity())) {
        am->Anim.SetPoseOverride(nullptr);
        am->Anim.RefreshPose();
    }
}

} // namespace d3d
