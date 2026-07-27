#include "anim/Constraints.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include <functional>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
// decompose лежит в экспериментальных расширениях glm и без этого макроса
// отказывается собираться. Разложение матрицы на TRS нужно только ограничению
// «привязка»: цель могла быть повёрнута и отмасштабирована, а Transform
// хранит T/R/S по отдельности, и вернуть их из матрицы иначе нечем.
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace d3d {

const char* ConstraintTypeName(ConstraintType type) {
    switch (type) {
        case ConstraintType::LookAt: return "LookAt";
        case ConstraintType::Parent: return "Parent";
        case ConstraintType::Path:   return "Path";
        default:                     return "None";
    }
}

namespace {

// Углы Эйлера из матрицы поворота, в том же порядке (X→Y→Z), в котором их
// собирает Transform::GetMatrix. Обратная операция обязана быть ТОЧНО обратной:
// иначе объект с ограничением дрожал бы каждый кадр, пересчитывая себя из
// собственного результата.
glm::vec3 EulerDegreesFromMatrix(const glm::mat3& m) {
    // Формула выведена из произведения, а не подобрана. M = Rx * Ry * Rz даёт
    // (в математической записи M[строка][столбец]):
    //     M[0][2] = sin(y)
    //     M[1][2] = -sin(x)cos(y)      M[2][2] = cos(x)cos(y)
    //     M[0][1] = -cos(y)sin(z)      M[0][0] = cos(y)cos(z)
    // В glm хранение по столбцам, то есть M[строка][столбец] = m[столбец][строка].
    //
    // Первая версия перепутала здесь все три знака, и объект со слежением
    // разворачивался ОТ цели вместо к ней. Прямо перед собой цель при этом
    // давала нули, и ошибка не была видна ровно в том случае, который
    // проверяешь первым. Нашлось проверкой «цель справа даёт -90°».
    const float sy = m[2][0];
    glm::vec3 e;
    if (std::abs(sy) < 0.99999f) {
        e.y = std::asin(sy);
        e.x = std::atan2(-m[2][1], m[2][2]);
        e.z = std::atan2(-m[1][0], m[0][0]);
    } else {
        // Взгляд строго вверх или вниз: cos(y) = 0, и по матрице различимы лишь
        // сумма или разность крена с рысканием. Крен берём нулевым — иначе он
        // скачет от кадра к кадру на величину, которой в кадре не видно.
        e.y = sy > 0.0f ? glm::half_pi<float>() : -glm::half_pi<float>();
        const float mixed = std::atan2(m[0][1], m[1][1]);
        e.x = sy > 0.0f ? mixed : -mixed;
        e.z = 0.0f;
    }
    return glm::degrees(e);
}

// Матрица поворота «смотреть из eye на target». Строится теми же осями, что и
// камера движка: вперёд -Z, вверх +Y.
glm::mat3 LookRotation(const glm::vec3& forward, const glm::vec3& up) {
    const glm::vec3 f = glm::normalize(forward);
    glm::vec3 u = up;
    // Вырожденный случай: смотрим ровно вдоль оси «вверх». Подменяем её, иначе
    // векторное произведение даёт ноль и матрица разваливается.
    if (std::abs(glm::dot(f, glm::normalize(u))) > 0.9995f) {
        u = std::abs(f.y) > 0.9f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    }
    const glm::vec3 right = glm::normalize(glm::cross(f, glm::normalize(u)));
    const glm::vec3 realUp = glm::cross(right, f);
    // Столбцы: X = right, Y = up, Z = -forward (взгляд по -Z).
    return glm::mat3(right, realUp, -f);
}

// Кратчайшая интерполяция углов: 350° и 10° — это 20° разницы, а не 340.
// Без этого частичная сила ограничения крутила бы объект «через всё поле».
float LerpAngle(float a, float b, float t) {
    float d = std::fmod(b - a + 540.0f, 360.0f) - 180.0f;
    return a + d * t;
}

const ConstraintComponent* Get(Scene& scene, int id) {
    GameObject obj = scene.Get(id);
    if (!obj.Valid()) return nullptr;
    return scene.Registry().try_get<ConstraintComponent>(obj.Entity());
}

} // namespace

std::vector<glm::vec3> PathPoints(Scene& scene, int targetId) {
    std::vector<glm::vec3> points;
    GameObject obj = scene.Get(targetId);
    if (!obj.Valid()) return points;

    auto& reg = scene.Registry();
    const HierarchyComponent* h = reg.try_get<HierarchyComponent>(obj.Entity());
    if (!h) return points;
    for (entt::entity child : h->Children) {
        if (!reg.valid(child) || !reg.all_of<Transform>(child)) continue;
        points.push_back(glm::vec3(scene.WorldMatrix(child)[3]));
    }
    return points;
}

bool SamplePath(const std::vector<glm::vec3>& points, float t, glm::vec3& position,
                glm::vec3& tangent) {
    if (points.size() < 2) return false;
    t = std::clamp(t, 0.0f, 1.0f);

    const int segments = (int)points.size() - 1;
    const float scaled = t * (float)segments;
    int i = std::min((int)scaled, segments - 1);
    const float local = scaled - (float)i;

    // Catmull-Rom по четырём точкам: кривая ПРОХОДИТ через опорные точки, в
    // отличие от Безье. Для траектории это обязательно — человек ставит точку
    // там, где объект должен оказаться, а не рядом.
    auto at = [&](int index) {
        return points[(size_t)std::clamp(index, 0, (int)points.size() - 1)];
    };
    const glm::vec3 p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
    const float t2 = local * local, t3 = t2 * local;

    position = 0.5f * ((2.0f * p1) + (-p0 + p2) * local +
                       (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                       (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
    tangent = 0.5f * ((-p0 + p2) + 2.0f * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * local +
                      3.0f * (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t2);
    if (glm::length(tangent) < 1e-5f) tangent = glm::vec3(0.0f, 0.0f, -1.0f);
    return true;
}

std::vector<int> ConstraintOrder(Scene& scene) {
    auto& reg = scene.Registry();
    std::vector<int> ordered;
    std::unordered_set<int> done, onStack;

    // Обход в глубину: перед тем как посчитать объект, считаем его цель. Стек
    // помнит путь, по которому мы сюда пришли, — по нему и распознаётся кольцо.
    std::function<void(int)> visit = [&](int id) {
        if (done.count(id) || onStack.count(id)) return; // кольцо: молча выходим
        onStack.insert(id);
        if (const ConstraintComponent* c = Get(scene, id)) {
            if (c->Type != ConstraintType::None && c->TargetId >= 0) visit(c->TargetId);
        }
        onStack.erase(id);
        done.insert(id);
        ordered.push_back(id);
    };

    auto view = reg.view<ConstraintComponent, IdComponent>();
    for (auto e : view) visit(view.get<IdComponent>(e).Id);
    return ordered;
}

bool ConstraintCycle(Scene& scene, int objectId) {
    std::unordered_set<int> seen;
    int id = objectId;
    for (int guard = 0; guard < 256; ++guard) {
        const ConstraintComponent* c = Get(scene, id);
        if (!c || c->Type == ConstraintType::None || c->TargetId < 0) return false;
        id = c->TargetId;
        if (id == objectId) return true;
        if (!seen.insert(id).second) return false; // кольцо есть, но не через нас
    }
    return true; // цепочка неправдоподобно длинная — считаем кольцом
}

void ApplyConstraints(Scene& scene) {
    auto& reg = scene.Registry();
    if (reg.view<ConstraintComponent>().empty()) return;

    for (int id : ConstraintOrder(scene)) {
        GameObject obj = scene.Get(id);
        if (!obj.Valid()) continue;
        ConstraintComponent* c = reg.try_get<ConstraintComponent>(obj.Entity());
        if (!c || c->Type == ConstraintType::None) continue;

        const float w = std::clamp(c->Influence, 0.0f, 1.0f);
        if (w <= 0.0f) continue;
        if (c->TargetId == id || ConstraintCycle(scene, id)) continue;

        GameObject target = scene.Get(c->TargetId);
        if (!target.Valid()) continue;

        Transform& tf = obj.GetTransform();
        const glm::mat4 targetWorld = scene.WorldMatrix(target.Entity());

        switch (c->Type) {
            case ConstraintType::LookAt: {
                const glm::vec3 self = glm::vec3(scene.WorldMatrix(obj.Entity())[3]);
                const glm::vec3 to = glm::vec3(targetWorld[3]) - self;
                if (glm::length(to) < 1e-5f) break;
                const glm::vec3 wanted = EulerDegreesFromMatrix(LookRotation(to, c->UpAxis));
                // Смешиваем по КРАТЧАЙШЕМУ пути на каждой оси: линейное
                // смешение углов провело бы камеру через полный оборот там,
                // где надо было довернуть на пару градусов.
                tf.Rotation.x = LerpAngle(tf.Rotation.x, wanted.x, w);
                tf.Rotation.y = LerpAngle(tf.Rotation.y, wanted.y, w);
                tf.Rotation.z = LerpAngle(tf.Rotation.z, wanted.z, w);
                break;
            }
            case ConstraintType::Parent: {
                if (c->KeepOffset && !c->OffsetValid) {
                    // Смещение считается ОДИН раз, в момент включения: это
                    // взаимное расположение «как было». Пересчитывать его
                    // каждый кадр значило бы гоняться за собственным хвостом —
                    // объект никогда бы не сдвинулся.
                    c->Offset = glm::inverse(targetWorld) * scene.WorldMatrix(obj.Entity());
                    c->OffsetValid = true;
                }
                const glm::mat4 wanted = c->KeepOffset ? targetWorld * c->Offset : targetWorld;

                glm::vec3 scale, translation, skew;
                glm::quat rotation;
                glm::vec4 perspective;
                if (!glm::decompose(wanted, scale, rotation, translation, skew, perspective)) break;
                const glm::vec3 euler = EulerDegreesFromMatrix(glm::mat3_cast(rotation));

                tf.Position = glm::mix(tf.Position, translation, w);
                tf.Scale = glm::mix(tf.Scale, scale, w);
                tf.Rotation.x = LerpAngle(tf.Rotation.x, euler.x, w);
                tf.Rotation.y = LerpAngle(tf.Rotation.y, euler.y, w);
                tf.Rotation.z = LerpAngle(tf.Rotation.z, euler.z, w);
                break;
            }
            case ConstraintType::Path: {
                const std::vector<glm::vec3> points = PathPoints(scene, c->TargetId);
                glm::vec3 position, tangent;
                if (!SamplePath(points, c->Progress, position, tangent)) break;
                tf.Position = glm::mix(tf.Position, position, w);
                if (c->FollowTangent) {
                    const glm::vec3 wanted = EulerDegreesFromMatrix(LookRotation(tangent, c->UpAxis));
                    tf.Rotation.x = LerpAngle(tf.Rotation.x, wanted.x, w);
                    tf.Rotation.y = LerpAngle(tf.Rotation.y, wanted.y, w);
                    tf.Rotation.z = LerpAngle(tf.Rotation.z, wanted.z, w);
                }
                break;
            }
            default: break;
        }
    }
}

} // namespace d3d
