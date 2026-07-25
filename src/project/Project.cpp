#include "project/Project.h"

#include <fstream>

#include <nlohmann/json.hpp>

#include "anim/DirectorComponents.h"
#include "sage/core/Log.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"

using json = nlohmann::json;

namespace d3d {

namespace {

// --- Компоненты Director 3D --------------------------------------------------
// Движковый SceneSerializer знает только про движковые компоненты, поэтому свои
// мы пишем отдельным разделом, привязывая их к сущностям по тому же id, что и
// сцена. Раздел необязателен: проект без него грузится с дефолтами, а .sage без
// него — просто сцена.

json SaveDirectorComponents(Scene& scene) {
    json out = json::array();
    auto& reg = scene.Registry();
    auto view = reg.view<IdComponent>();
    for (auto e : view) {
        const int id = view.get<IdComponent>(e).Id;
        json entry;
        bool any = false;

        if (const CineCameraComponent* c = reg.try_get<CineCameraComponent>(e)) {
            entry["cineCamera"] = {
                {"focusDistance", c->FocusDistance}, {"autoFocus", c->AutoFocus},
                {"aperture", c->Aperture}, {"depthOfField", c->DepthOfField},
                {"filmBack", c->FilmBack}, {"aspectRatio", c->AspectRatio},
                {"cameraShake", c->CameraShake}, {"shakeAmplitude", c->ShakeAmplitude},
                {"shakeFrequency", c->ShakeFrequency},
                {"bloom", c->Bloom}, {"bloomIntensity", c->BloomIntensity},
                {"motionBlur", c->MotionBlur}, {"motionBlurAmount", c->MotionBlurAmount},
                {"colorGrading", c->ColorGrading}, {"colorGradingAmount", c->ColorGradingAmount},
                {"vignette", c->Vignette}, {"vignetteAmount", c->VignetteAmount},
                {"chromaticAberration", c->ChromaticAberration}, {"chromaticAmount", c->ChromaticAmount},
            };
            any = true;
        }
        if (const StageItemComponent* s = reg.try_get<StageItemComponent>(e)) {
            // Дефолтное состояние (виден, не заблокирован) не пишем — файл
            // проекта не должен раздуваться строчками «всё как обычно».
            if (!s->Visible || s->Locked) {
                entry["stageItem"] = {{"visible", s->Visible}, {"locked", s->Locked}};
                any = true;
            }
        }
        if (const SourceAssetComponent* a = reg.try_get<SourceAssetComponent>(e)) {
            if (!a->Path.empty()) { entry["sourceAsset"] = a->Path; any = true; }
        }

        if (any) {
            entry["id"] = id;
            out.push_back(std::move(entry));
        }
    }
    return out;
}

void LoadDirectorComponents(Scene& scene, const json& arr) {
    if (!arr.is_array()) return;
    auto& reg = scene.Registry();
    for (const json& entry : arr) {
        if (!entry.contains("id")) continue;
        GameObject obj = scene.Get(entry.value("id", -1));
        if (!obj.Valid()) continue; // сущность из раздела удалили — просто пропускаем
        const entt::entity e = obj.Entity();

        if (entry.contains("cineCamera")) {
            const json& c = entry["cineCamera"];
            CineCameraComponent cine; // дефолты для полей, которых в файле нет
            cine.FocusDistance = c.value("focusDistance", cine.FocusDistance);
            cine.AutoFocus = c.value("autoFocus", cine.AutoFocus);
            cine.Aperture = c.value("aperture", cine.Aperture);
            cine.DepthOfField = c.value("depthOfField", cine.DepthOfField);
            cine.FilmBack = c.value("filmBack", cine.FilmBack);
            cine.AspectRatio = c.value("aspectRatio", cine.AspectRatio);
            cine.CameraShake = c.value("cameraShake", cine.CameraShake);
            cine.ShakeAmplitude = c.value("shakeAmplitude", cine.ShakeAmplitude);
            cine.ShakeFrequency = c.value("shakeFrequency", cine.ShakeFrequency);
            cine.Bloom = c.value("bloom", cine.Bloom);
            cine.BloomIntensity = c.value("bloomIntensity", cine.BloomIntensity);
            cine.MotionBlur = c.value("motionBlur", cine.MotionBlur);
            cine.MotionBlurAmount = c.value("motionBlurAmount", cine.MotionBlurAmount);
            cine.ColorGrading = c.value("colorGrading", cine.ColorGrading);
            cine.ColorGradingAmount = c.value("colorGradingAmount", cine.ColorGradingAmount);
            cine.Vignette = c.value("vignette", cine.Vignette);
            cine.VignetteAmount = c.value("vignetteAmount", cine.VignetteAmount);
            cine.ChromaticAberration = c.value("chromaticAberration", cine.ChromaticAberration);
            cine.ChromaticAmount = c.value("chromaticAmount", cine.ChromaticAmount);
            reg.emplace_or_replace<CineCameraComponent>(e, cine);
        }
        if (entry.contains("stageItem")) {
            const json& s = entry["stageItem"];
            StageItemComponent item;
            item.Visible = s.value("visible", true);
            item.Locked = s.value("locked", false);
            reg.emplace_or_replace<StageItemComponent>(e, item);
        }
        if (entry.contains("sourceAsset")) {
            reg.emplace_or_replace<SourceAssetComponent>(
                e, SourceAssetComponent{entry.value("sourceAsset", std::string{})});
        }
    }
}

// --- Анимация ---------------------------------------------------------------

json SaveCurve(const Curve& curve) {
    json keys = json::array();
    for (const Keyframe& k : curve.Keys()) {
        json jk = {{"t", k.Time}, {"v", k.Value}, {"i", InterpKey(k.Mode)}};
        // Касательные значимы только у Безье — иначе это шум в файле.
        if (k.Mode == Interp::Bezier) {
            jk["ti"] = k.InTangent;
            jk["to"] = k.OutTangent;
        }
        keys.push_back(std::move(jk));
    }
    return keys;
}

void LoadCurve(Curve& curve, const json& keys) {
    curve.Clear();
    if (!keys.is_array()) return;
    std::vector<Keyframe>& raw = curve.KeysMutable();
    raw.reserve(keys.size());
    for (const json& jk : keys) {
        Keyframe k;
        k.Time = jk.value("t", 0.0f);
        k.Value = jk.value("v", 0.0f);
        k.Mode = InterpFromKey(jk.value("i", std::string("smooth")));
        k.InTangent = jk.value("ti", 0.0f);
        k.OutTangent = jk.value("to", 0.0f);
        raw.push_back(k);
    }
    curve.Normalize(); // файл мог быть правлен руками — восстанавливаем инвариант
}

json SaveDocument(const AnimationDocument& doc) {
    json out;
    out["nextId"] = doc.NextId();

    json tracks = json::array();
    for (const Track& t : doc.Tracks) {
        json jt;
        jt["id"] = t.Id;
        jt["target"] = t.TargetId;
        jt["property"] = PropertyInfoOf(t.Prop).Key;
        jt["muted"] = t.Muted;
        jt["locked"] = t.Locked;
        jt["expanded"] = t.Expanded;
        json channels = json::array();
        for (const Curve& c : t.Channels) channels.push_back(SaveCurve(c));
        jt["channels"] = std::move(channels);
        tracks.push_back(std::move(jt));
    }
    out["tracks"] = std::move(tracks);

    json clipTracks = json::array();
    for (const ClipTrack& t : doc.ClipTracks) {
        json jt;
        jt["id"] = t.Id;
        jt["target"] = t.TargetId;
        jt["muted"] = t.Muted;
        json blocks = json::array();
        for (const ClipBlock& b : t.Blocks) {
            blocks.push_back({{"name", b.Name}, {"clip", b.ClipIndex}, {"start", b.Start},
                              {"duration", b.Duration}, {"speed", b.Speed},
                              {"blendIn", b.BlendIn}, {"loop", b.Loop}});
        }
        jt["blocks"] = std::move(blocks);
        clipTracks.push_back(std::move(jt));
    }
    out["clipTracks"] = std::move(clipTracks);

    json markers = json::array();
    for (const Marker& m : doc.Markers) {
        markers.push_back({{"name", m.Name}, {"time", m.Time}, {"color", m.Color}});
    }
    out["markers"] = std::move(markers);

    if (doc.Audio.Loaded()) {
        out["audio"] = {{"path", doc.Audio.Path()}, {"offset", doc.Audio.Offset},
                        {"volume", doc.Audio.Volume}, {"muted", doc.Audio.Muted}};
    }
    return out;
}

void LoadDocument(AnimationDocument& doc, const json& in) {
    doc.ClearContent();
    if (!in.is_object()) return;

    for (const json& jt : in.value("tracks", json::array())) {
        Property prop;
        // Свойство из более новой версии инструмента — дорожку пропускаем,
        // остальной проект грузится (лучше частично, чем никак).
        if (!PropertyFromKey(jt.value("property", std::string{}), prop)) {
            LOG_INFO("Project") << "Пропущена дорожка неизвестного свойства: "
                                << jt.value("property", std::string{});
            continue;
        }
        Track t;
        t.Id = jt.value("id", 0);
        t.TargetId = jt.value("target", 0);
        t.Prop = prop;
        t.Muted = jt.value("muted", false);
        t.Locked = jt.value("locked", false);
        t.Expanded = jt.value("expanded", true);

        const int channelCount = PropertyInfoOf(prop).Channels;
        t.Channels.resize((size_t)channelCount);
        const json& channels = jt.value("channels", json::array());
        for (int c = 0; c < channelCount && c < (int)channels.size(); ++c) {
            LoadCurve(t.Channels[(size_t)c], channels[(size_t)c]);
        }
        doc.Tracks.push_back(std::move(t));
    }

    for (const json& jt : in.value("clipTracks", json::array())) {
        ClipTrack t;
        t.Id = jt.value("id", 0);
        t.TargetId = jt.value("target", 0);
        t.Muted = jt.value("muted", false);
        for (const json& jb : jt.value("blocks", json::array())) {
            ClipBlock b;
            b.Name = jb.value("name", std::string("Clip"));
            b.ClipIndex = jb.value("clip", 0);
            b.Start = jb.value("start", 0.0f);
            b.Duration = jb.value("duration", 1.0f);
            b.Speed = jb.value("speed", 1.0f);
            b.BlendIn = jb.value("blendIn", 0.25f);
            b.Loop = jb.value("loop", true);
            t.Blocks.push_back(std::move(b));
        }
        doc.ClipTracks.push_back(std::move(t));
    }

    for (const json& jm : in.value("markers", json::array())) {
        Marker m;
        m.Name = jm.value("name", std::string("Marker"));
        m.Time = jm.value("time", 0.0f);
        m.Color = jm.value("color", 0xFF3FC8E8u);
        doc.Markers.push_back(std::move(m));
    }

    if (in.contains("audio")) {
        const json& ja = in["audio"];
        const std::string path = ja.value("path", std::string{});
        if (!path.empty()) doc.Audio.Load(path); // файла может уже не быть — не ошибка проекта
        doc.Audio.Offset = ja.value("offset", 0.0f);
        doc.Audio.Volume = ja.value("volume", 1.0f);
        doc.Audio.Muted = ja.value("muted", false);
    }

    // Счётчик id должен быть строго больше всех занятых, иначе следующая
    // созданная дорожка получит чужой id и «склеится» с ней в UI.
    int maxId = in.value("nextId", 1) - 1;
    for (const Track& t : doc.Tracks) maxId = std::max(maxId, t.Id);
    for (const ClipTrack& t : doc.ClipTracks) maxId = std::max(maxId, t.Id);
    doc.SetNextId(maxId + 1);
}

// Собирает целиком дерево проекта (используется и файлом, и снимком в памяти).
json BuildProjectJson(Scene& scene, const AnimationDocument& doc, float playheadTime) {
    json root;
    root["format"] = "director3d.project";
    root["version"] = ProjectFile::kFormatVersion;
    root["name"] = doc.Name;
    // Сцена — движковым сериализатором, как есть: формат общий с .sage.
    root["scene"] = json::parse(SceneSerializer::SaveToString(scene));
    root["director"] = SaveDirectorComponents(scene);
    root["animation"] = SaveDocument(doc);
    root["timeline"] = {{"fps", doc.Fps}, {"duration", doc.Duration}, {"playhead", playheadTime}};
    return root;
}

bool ParseProjectJson(const json& root, std::unique_ptr<Scene>& outScene,
                      AnimationDocument& doc, float& outPlayheadTime, std::string& err) {
    if (!root.contains("scene")) {
        err = "в файле нет раздела \"scene\" — это не проект Director 3D";
        return false;
    }
    try {
        outScene = SceneSerializer::LoadFromString(root["scene"].dump());
    } catch (const std::exception& e) {
        err = std::string("не удалось прочитать сцену: ") + e.what();
        return false;
    }
    if (!outScene) {
        err = "сцена не загрузилась";
        return false;
    }

    LoadDirectorComponents(*outScene, root.value("director", json::array()));
    LoadDocument(doc, root.value("animation", json::object()));

    doc.Name = root.value("name", std::string("My_Animation_Project"));
    const json& tl = root.value("timeline", json::object());
    doc.Fps = tl.value("fps", 24.0f);
    doc.Duration = tl.value("duration", 20.0f);
    outPlayheadTime = tl.value("playhead", 0.0f);
    if (doc.Fps <= 0.0f) doc.Fps = 24.0f;       // защита от битого файла: деление
    if (doc.Duration <= 0.0f) doc.Duration = 20.0f; // на ноль в таймкоде/таймлайне
    return true;
}

} // namespace

bool ProjectFile::Save(const std::string& path, Scene& scene, const AnimationDocument& doc,
                       float playheadTime, std::string& err) {
    try {
        std::ofstream file(path);
        if (!file) { err = "не удалось открыть файл на запись: " + path; return false; }
        file << BuildProjectJson(scene, doc, playheadTime).dump(2);
        if (!file) { err = "ошибка записи файла: " + path; return false; }
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    LOG_INFO("Project") << "Проект сохранён: " << path;
    return true;
}

bool ProjectFile::Load(const std::string& path, std::unique_ptr<Scene>& outScene,
                       AnimationDocument& doc, float& outPlayheadTime, std::string& err) {
    try {
        std::ifstream file(path);
        if (!file) { err = "файл не найден: " + path; return false; }
        json root = json::parse(file);
        if (!ParseProjectJson(root, outScene, doc, outPlayheadTime, err)) return false;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    LOG_INFO("Project") << "Проект загружен: " << path;
    return true;
}

bool ProjectFile::SaveSceneOnly(const std::string& path, Scene& scene, std::string& err) {
    try {
        SceneSerializer::Save(scene, path);
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    LOG_INFO("Project") << "Сцена экспортирована в формат движка: " << path;
    return true;
}

std::string ProjectFile::SnapshotToString(Scene& scene, const AnimationDocument& doc) {
    // Снимок для undo — тот же JSON, но компактный (он живёт в памяти пачками
    // по десяткам штук, и отступы стоили бы заметно больше памяти).
    return BuildProjectJson(scene, doc, 0.0f).dump();
}

bool ProjectFile::RestoreFromString(const std::string& jsonText, std::unique_ptr<Scene>& outScene,
                                    AnimationDocument& doc, std::string& err) {
    try {
        json root = json::parse(jsonText);
        float playhead = 0.0f;
        return ParseProjectJson(root, outScene, doc, playhead, err);
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

} // namespace d3d
