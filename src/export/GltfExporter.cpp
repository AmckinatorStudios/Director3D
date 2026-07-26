#include "export/GltfExporter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include "anim/BonePose.h"
#include "anim/DirectorComponents.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/core/Log.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

using json = nlohmann::json;

namespace d3d::gltf {
namespace {

// --- Буфер бинарных данных --------------------------------------------------
//
// glTF требует, чтобы смещение каждого accessor'а было кратно размеру его
// компонента. У нас всё float, поэтому достаточно выравнивать на 4 — но
// выравнивать НАДО: несоблюдение этого правила даёт файл, который часть
// загрузчиков читает, а часть молча отвергает, и разбираться в таком потом
// крайне неприятно.
struct Blob {
    std::vector<unsigned char> Bytes;

    size_t Append(const float* data, size_t count) {
        Align(4);
        const size_t offset = Bytes.size();
        const unsigned char* raw = reinterpret_cast<const unsigned char*>(data);
        Bytes.insert(Bytes.end(), raw, raw + count * sizeof(float));
        return offset;
    }
    void Align(size_t to, unsigned char fill = 0) {
        while (Bytes.size() % to != 0) Bytes.push_back(fill);
    }
};

// Один узел будущего файла.
struct Node {
    std::string Name;
    int Parent = -1;                 // индекс в списке узлов
    std::vector<int> Children;
    int Camera = -1;                 // индекс в массиве cameras
    // Снимки по кадрам.
    std::vector<glm::vec3> Translation;
    std::vector<glm::quat> Rotation;
    std::vector<glm::vec3> Scale;
};

// Поворот объекта хранится углами Эйлера в градусах, а glTF нужен кватернион.
// Порядок обязан совпадать с Transform::GetMatrix, где матрица собирается как
// Rx * Ry * Rz, — иначе экспортированный поворот разойдётся с тем, что видно
// в кадре, причём только у объектов, повёрнутых больше чем по одной оси.
glm::quat QuatFromEulerDegrees(const glm::vec3& degrees) {
    const glm::vec3 r = glm::radians(degrees);
    return glm::angleAxis(r.x, glm::vec3(1, 0, 0)) *
           glm::angleAxis(r.y, glm::vec3(0, 1, 0)) *
           glm::angleAxis(r.z, glm::vec3(0, 0, 1));
}

// Меняется ли величина за ролик. Дорожки постоянных значений в файл не
// попадают: сотня каналов, где всё время одно и то же, раздувает файл и
// засоряет редактор на принимающей стороне — там это выглядит как анимация,
// которой на самом деле нет.
template <typename T>
bool Varies(const std::vector<T>& values, float epsilon) {
    for (size_t i = 1; i < values.size(); ++i) {
        for (int c = 0; c < T::length(); ++c) {
            if (std::abs(values[i][c] - values[0][c]) > epsilon) return true;
        }
    }
    return false;
}

// Кватернионы сравниваем отдельно: q и -q — один поворот, и покомпонентная
// разница показала бы движение там, где его нет.
bool VariesRotation(const std::vector<glm::quat>& values, float epsilon) {
    for (size_t i = 1; i < values.size(); ++i) {
        const float dot = std::abs(glm::dot(values[0], values[i]));
        if (dot < 1.0f - epsilon) return true;
    }
    return false;
}

} // namespace

bool Export(const std::string& path, Scene& scene, const AnimationDocument& doc,
            const Options& options, Result& result, std::string& err) {
    const float fps = options.Fps > 0.0f ? options.Fps : doc.Fps;
    if (fps <= 0.0f) { err = "частота кадров не задана"; return false; }
    if (doc.Duration <= 0.0f) { err = "нулевая длительность ролика"; return false; }

    const int sampleCount = std::max(2, (int)std::lround(doc.Duration * fps) + 1);

    // --- 1. Узлы -----------------------------------------------------------
    std::vector<Node> nodes;
    std::unordered_map<entt::entity, int> nodeOf;
    std::vector<entt::entity> nodeEntity;   // параллельно nodes, entt::null у костей
    // Кости: индекс узла -> (сущность персонажа, номер кости).
    std::vector<std::pair<int, int>> boneOf(0);
    std::unordered_map<int, std::pair<int, int>> boneNode; // узел -> (entityId, joint)

    auto& reg = scene.Registry();
    auto view = reg.view<IdComponent, Transform>();
    for (auto e : view) {
        Node n;
        if (const NameComponent* nc = reg.try_get<NameComponent>(e)) n.Name = nc->Name;
        if (n.Name.empty()) n.Name = "Node_" + std::to_string(view.get<IdComponent>(e).Id);
        nodeOf[e] = (int)nodes.size();
        nodes.push_back(std::move(n));
        nodeEntity.push_back(e);
    }
    if (nodes.empty()) { err = "в сцене нет объектов"; return false; }

    // Иерархия сцены. Ссылки ставим только на узлы, которые действительно
    // попали в файл: родитель без IdComponent сюда не дошёл, и ссылка на него
    // сделала бы файл битым.
    for (size_t i = 0; i < nodeEntity.size(); ++i) {
        if (const HierarchyComponent* h = reg.try_get<HierarchyComponent>(nodeEntity[i])) {
            auto it = nodeOf.find(h->Parent);
            if (it != nodeOf.end()) {
                nodes[i].Parent = it->second;
                nodes[it->second].Children.push_back((int)i);
            }
        }
    }

    // Камеры.
    json cameras = json::array();
    if (options.IncludeCameras) {
        for (size_t i = 0; i < nodeEntity.size(); ++i) {
            const CameraComponent* cc = reg.try_get<CameraComponent>(nodeEntity[i]);
            if (!cc) continue;
            nodes[i].Camera = (int)cameras.size();
            // yfov в glTF — ВЕРТИКАЛЬНЫЙ угол в радианах. У нас Fov уходит в
            // glm::perspective тоже как вертикальный, поэтому пересчёта нет.
            cameras.push_back({{"type", "perspective"},
                               {"perspective",
                                {{"yfov", glm::radians(cc->Fov)},
                                 {"znear", cc->NearClip},
                                 {"zfar", cc->FarClip}}},
                               {"name", nodes[i].Name}});
        }
    }

    // Кости персонажей — узлами внутри узла персонажа.
    if (options.IncludeSkeletons) {
        const size_t objectNodes = nodes.size();
        for (size_t i = 0; i < objectNodes; ++i) {
            const int entityId = reg.get<IdComponent>(nodeEntity[i]).Id;
            const sage::anim::Skeleton* sk = SkeletonOf(scene, entityId);
            if (!sk || sk->Count() == 0) continue;

            const int base = (int)nodes.size();
            for (int j = 0; j < sk->Count(); ++j) {
                Node bone;
                bone.Name = sk->Joints[(size_t)j].Name.empty()
                                ? "Joint_" + std::to_string(j)
                                : sk->Joints[(size_t)j].Name;
                nodes.push_back(std::move(bone));
                nodeEntity.push_back(entt::null);
                boneNode[base + j] = {entityId, j};
            }
            // Родство костей — из скелета; корни цепляются к самому персонажу,
            // иначе они всплыли бы отдельными объектами в начале координат.
            for (int j = 0; j < sk->Count(); ++j) {
                const int parent = sk->Joints[(size_t)j].Parent;
                const int self = base + j;
                nodes[(size_t)self].Parent = parent >= 0 ? base + parent : (int)i;
                nodes[(size_t)nodes[(size_t)self].Parent].Children.push_back(self);
            }
        }
    }

    for (Node& n : nodes) {
        n.Translation.reserve((size_t)sampleCount);
        n.Rotation.reserve((size_t)sampleCount);
        n.Scale.reserve((size_t)sampleCount);
    }

    // --- 2. Снятие кадров ---------------------------------------------------
    // Через тот же Apply, которым рисуется кадр: второй путь расчёта кривых
    // неизбежно разошёлся бы с первым, и файл перестал бы совпадать с тем,
    // что человек видит в программе.
    std::vector<float> times((size_t)sampleCount);
    for (int s = 0; s < sampleCount; ++s) {
        const float t = (sampleCount == 1) ? 0.0f
                                           : doc.Duration * (float)s / (float)(sampleCount - 1);
        times[(size_t)s] = t;

        const_cast<AnimationDocument&>(doc).Apply(scene, t, /*seeking=*/true);
        sage::anim::UpdateAnimators(scene, 0.0f);

        for (size_t i = 0; i < nodes.size(); ++i) {
            auto bone = boneNode.find((int)i);
            if (bone != boneNode.end()) {
                glm::vec3 tr(0.0f), sc(1.0f);
                glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
                ReadBoneLocal(scene, bone->second.first, bone->second.second, tr, rot, sc);
                nodes[i].Translation.push_back(tr);
                nodes[i].Rotation.push_back(rot);
                nodes[i].Scale.push_back(sc);
                continue;
            }
            const Transform& tf = reg.get<Transform>(nodeEntity[i]);
            nodes[i].Translation.push_back(tf.Position);
            nodes[i].Rotation.push_back(QuatFromEulerDegrees(tf.Rotation));
            nodes[i].Scale.push_back(tf.Scale);
        }
    }

    // --- 3. Сборка файла ----------------------------------------------------
    Blob blob;
    json accessors = json::array();
    json bufferViews = json::array();

    auto addAccessor = [&](const float* data, int count, int components,
                           const char* type, bool withBounds) {
        const size_t offset = blob.Append(data, (size_t)count * components);
        json bv = {{"buffer", 0}, {"byteOffset", offset},
                   {"byteLength", (size_t)count * components * sizeof(float)}};
        bufferViews.push_back(std::move(bv));

        json acc = {{"bufferView", (int)bufferViews.size() - 1},
                    {"componentType", 5126 /*FLOAT*/},
                    {"count", count},
                    {"type", type}};
        // Спецификация ТРЕБУЕТ min/max у входа сэмплера анимации: по ним
        // загрузчик узнаёт длительность, не читая сами данные. Без них файл
        // формально невалиден, и строгие загрузчики его отклоняют.
        if (withBounds) {
            std::vector<float> mn(components, data[0]), mx(components, data[0]);
            for (int i = 0; i < count; ++i) {
                for (int c = 0; c < components; ++c) {
                    mn[(size_t)c] = std::min(mn[(size_t)c], data[i * components + c]);
                    mx[(size_t)c] = std::max(mx[(size_t)c], data[i * components + c]);
                }
            }
            acc["min"] = mn;
            acc["max"] = mx;
        }
        accessors.push_back(std::move(acc));
        return (int)accessors.size() - 1;
    };

    // Время у всех дорожек одно — один accessor на весь файл.
    const int timeAccessor = addAccessor(times.data(), sampleCount, 1, "SCALAR", true);

    json samplers = json::array();
    json channels = json::array();
    // Порог «величина изменилась». 1e-5 — заметно ниже точности, с которой
    // аниматор что-либо ставит руками, но выше шума пересчёта поз.
    constexpr float kEpsilon = 1e-5f;

    auto addChannel = [&](int node, const char* pathName, const float* data, int components,
                          const char* type) {
        const int out = addAccessor(data, sampleCount, components, type, false);
        samplers.push_back({{"input", timeAccessor}, {"output", out}, {"interpolation", "LINEAR"}});
        channels.push_back({{"sampler", (int)samplers.size() - 1},
                            {"target", {{"node", node}, {"path", pathName}}}});
    };

    for (size_t i = 0; i < nodes.size(); ++i) {
        Node& n = nodes[i];
        if (Varies(n.Translation, kEpsilon)) {
            std::vector<float> flat;
            flat.reserve(n.Translation.size() * 3);
            for (const glm::vec3& v : n.Translation) { flat.push_back(v.x); flat.push_back(v.y); flat.push_back(v.z); }
            addChannel((int)i, "translation", flat.data(), 3, "VEC3");
        }
        if (VariesRotation(n.Rotation, kEpsilon)) {
            std::vector<float> flat;
            flat.reserve(n.Rotation.size() * 4);
            glm::quat prev = n.Rotation[0];
            for (glm::quat q : n.Rotation) {
                // Разворачиваем знак вслед за предыдущим кватернионом: q и -q
                // означают один поворот, но ЛИНЕЙНАЯ интерполяция между ними
                // проходит длинным путём. Без этого объект, доворачивающий за
                // границу, дёргается на пол-оборота — в файле, но не в
                // программе, потому что там интерполируются углы.
                if (glm::dot(prev, q) < 0.0f) q = -q;
                prev = q;
                flat.push_back(q.x); flat.push_back(q.y); flat.push_back(q.z); flat.push_back(q.w);
            }
            addChannel((int)i, "rotation", flat.data(), 4, "VEC4");
        }
        if (Varies(n.Scale, kEpsilon)) {
            std::vector<float> flat;
            flat.reserve(n.Scale.size() * 3);
            for (const glm::vec3& v : n.Scale) { flat.push_back(v.x); flat.push_back(v.y); flat.push_back(v.z); }
            addChannel((int)i, "scale", flat.data(), 3, "VEC3");
        }
    }

    // --- 4. JSON ------------------------------------------------------------
    json jnodes = json::array();
    std::vector<int> roots;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const Node& n = nodes[i];
        json jn;
        jn["name"] = n.Name;
        // Поза узла — ПЕРВЫЙ кадр, а не единица: файл, открытый без
        // проигрывания анимации, должен показывать начало ролика, а не свалку
        // объектов в начале координат.
        jn["translation"] = {n.Translation[0].x, n.Translation[0].y, n.Translation[0].z};
        jn["rotation"] = {n.Rotation[0].x, n.Rotation[0].y, n.Rotation[0].z, n.Rotation[0].w};
        jn["scale"] = {n.Scale[0].x, n.Scale[0].y, n.Scale[0].z};
        if (!n.Children.empty()) jn["children"] = n.Children;
        if (n.Camera >= 0) jn["camera"] = n.Camera;
        jnodes.push_back(std::move(jn));
        if (n.Parent < 0) roots.push_back((int)i);
    }

    json root;
    root["asset"] = {{"version", "2.0"}, {"generator", "Director 3D"}};
    root["scene"] = 0;
    root["scenes"] = json::array({{{"name", doc.Name}, {"nodes", roots}}});
    root["nodes"] = std::move(jnodes);
    if (!cameras.empty()) root["cameras"] = std::move(cameras);
    if (!channels.empty()) {
        root["animations"] = json::array({{{"name", doc.Name.empty() ? "Animation" : doc.Name},
                                           {"samplers", samplers},
                                           {"channels", channels}}});
    }
    blob.Align(4);
    root["buffers"] = json::array({{{"byteLength", blob.Bytes.size()}}});
    root["bufferViews"] = std::move(bufferViews);
    root["accessors"] = std::move(accessors);

    // --- 5. Контейнер GLB ---------------------------------------------------
    // Один файл вместо пары .gltf + .bin: анимацию отдают и пересылают целиком,
    // и потерять половину пары проще, чем кажется.
    std::string jsonText = root.dump();
    while (jsonText.size() % 4 != 0) jsonText.push_back(' '); // добивка пробелами по спецификации

    std::ofstream file(path, std::ios::binary);
    if (!file) { err = "не удалось создать файл " + path; return false; }

    auto u32 = [&file](unsigned int v) { file.write(reinterpret_cast<const char*>(&v), 4); };
    const unsigned int total = 12 + 8 + (unsigned int)jsonText.size() + 8 + (unsigned int)blob.Bytes.size();
    u32(0x46546C67); // "glTF"
    u32(2);
    u32(total);
    u32((unsigned int)jsonText.size());
    u32(0x4E4F534A); // "JSON"
    file.write(jsonText.data(), (std::streamsize)jsonText.size());
    u32((unsigned int)blob.Bytes.size());
    u32(0x004E4942); // "BIN"
    file.write(reinterpret_cast<const char*>(blob.Bytes.data()), (std::streamsize)blob.Bytes.size());
    if (!file) { err = "запись файла оборвалась"; return false; }
    file.close();

    result.Nodes = (int)nodes.size();
    result.Channels = (int)channels.size();
    result.Samples = sampleCount;
    result.Bytes = (int)total;
    LOG_INFO("glTF") << "Экспорт: " << path << " — узлов " << result.Nodes << ", дорожек "
                     << result.Channels << ", кадров " << result.Samples;
    return true;
}

} // namespace d3d::gltf
