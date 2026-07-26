#include "anim/AnimationDocument.h"

#include <algorithm>
#include <cmath>

#include "anim/BonePose.h"
#include "anim/DirectorComponents.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace d3d {

// ============================================================================
//  Дорожки свойств
// ============================================================================

Track* AnimationDocument::FindTrack(int targetId, Property prop, int joint) {
    for (Track& t : Tracks) {
        // Кость различает дорожки только у костных свойств: у обычных Joint
        // всегда -1, и сравнение с ним ничего не меняет.
        if (t.TargetId == targetId && t.Prop == prop && t.Joint == joint) return &t;
    }
    return nullptr;
}

const Track* AnimationDocument::FindTrack(int targetId, Property prop, int joint) const {
    return const_cast<AnimationDocument*>(this)->FindTrack(targetId, prop, joint);
}

Track* AnimationDocument::TrackById(int id) {
    for (Track& t : Tracks) {
        if (t.Id == id) return &t;
    }
    return nullptr;
}

Track& AnimationDocument::EnsureTrack(int targetId, Property prop, int joint) {
    // Подындекс есть только у костей и морфов; у остальных свойств он всегда -1.
    if (!HasSubIndex(prop)) joint = -1;
    if (Track* existing = FindTrack(targetId, prop, joint)) return *existing;
    Track t;
    t.Id = TakeId();
    t.TargetId = targetId;
    t.Prop = prop;
    t.Joint = joint;
    t.Channels.resize((size_t)PropertyInfoOf(prop).Channels);
    Tracks.push_back(std::move(t));
    return Tracks.back();
}

int AnimationDocument::RebindBoneTracks(Scene& scene) {
    int pending = 0;
    for (Track& t : Tracks) {
        if (!IsBoneProperty(t.Prop)) continue;

        const sage::anim::Skeleton* sk = SkeletonOf(scene, t.TargetId);
        if (!sk) {
            // Сущности может уже не быть — тогда ждать нечего, дорожка просто
            // не применится. Ждём только там, где персонаж есть, а модель ещё грузится.
            if (scene.Get(t.TargetId).Valid()) ++pending;
            continue;
        }

        if (!t.JointName.empty()) {
            const int byName = FindJoint(*sk, t.JointName);
            if (byName >= 0) {
                t.Joint = byName;
                continue;
            }
            // Кость переименовали: держимся сохранённого индекса, если он ещё в
            // пределах скелета, и запоминаем новое имя — иначе дорожка каждый
            // раз искала бы призрака.
            if (t.Joint >= 0 && t.Joint < sk->Count()) {
                t.JointName = sk->Joints[(size_t)t.Joint].Name;
                continue;
            }
            t.Joint = -1; // такой кости больше нет — дорожка молча не применяется
            continue;
        }

        // Имени нет (проект от версии без него) — восстанавливаем его по индексу.
        if (t.Joint >= 0 && t.Joint < sk->Count()) t.JointName = sk->Joints[(size_t)t.Joint].Name;
    }
    return pending;
}

std::vector<const Track*> AnimationDocument::BoneTracksOf(int targetId) const {
    std::vector<const Track*> out;
    for (const Track& t : Tracks) {
        if (t.TargetId == targetId && IsBoneProperty(t.Prop)) out.push_back(&t);
    }
    return out;
}

bool AnimationDocument::HasBoneTracks(int targetId) const {
    for (const Track& t : Tracks) {
        if (t.TargetId == targetId && IsBoneProperty(t.Prop)) return true;
    }
    return false;
}

void AnimationDocument::RemoveTrack(int id) {
    Tracks.erase(std::remove_if(Tracks.begin(), Tracks.end(),
                                [id](const Track& t) { return t.Id == id; }),
                 Tracks.end());
}

void AnimationDocument::RemoveTracksOf(int targetId) {
    Tracks.erase(std::remove_if(Tracks.begin(), Tracks.end(),
                                [targetId](const Track& t) { return t.TargetId == targetId; }),
                 Tracks.end());
    ClipTracks.erase(std::remove_if(ClipTracks.begin(), ClipTracks.end(),
                                    [targetId](const ClipTrack& t) { return t.TargetId == targetId; }),
                     ClipTracks.end());
    // Склейки на удалённую камеру убираем здесь же. Оставить их означало бы
    // хранить в монтаже ссылки в пустоту: CameraAt отдал бы запасную камеру,
    // и склейка на таймлайне выглядела бы работающей, ничего не переключая.
    Cameras.Cuts.erase(std::remove_if(Cameras.Cuts.begin(), Cameras.Cuts.end(),
                                      [targetId](const CameraCut& c) { return c.CameraId == targetId; }),
                       Cameras.Cuts.end());
}

// ============================================================================
//  Дорожки клипов
// ============================================================================

ClipTrack* AnimationDocument::FindClipTrack(int targetId) {
    for (ClipTrack& t : ClipTracks) {
        if (t.TargetId == targetId) return &t;
    }
    return nullptr;
}

ClipTrack& AnimationDocument::EnsureClipTrack(int targetId) {
    if (ClipTrack* existing = FindClipTrack(targetId)) return *existing;
    ClipTrack t;
    t.Id = TakeId();
    t.TargetId = targetId;
    ClipTracks.push_back(std::move(t));
    return ClipTracks.back();
}

void AnimationDocument::RemoveClipTrack(int id) {
    ClipTracks.erase(std::remove_if(ClipTracks.begin(), ClipTracks.end(),
                                    [id](const ClipTrack& t) { return t.Id == id; }),
                     ClipTracks.end());
}

// ============================================================================
//  Монтаж камер
// ============================================================================

namespace {
// Две склейки считаются одной, если расходятся меньше чем на это. Величина
// заметно мельче кадра даже при 240 fps: попасть в неё случайной перетаской
// нельзя, а вот кликнуть по уже существующей склейке и получить вторую поверх
// первой — легко, и тогда одна из них навсегда осталась бы недостижимой.
constexpr float kCutEpsilon = 1e-4f;
} // namespace

int AnimationDocument::CutIndexAt(float time) const {
    if (Cameras.Muted) return -1;
    int found = -1;
    for (size_t i = 0; i < Cameras.Cuts.size(); ++i) {
        if (Cameras.Cuts[i].Time > time + kCutEpsilon) break; // список отсортирован
        found = (int)i;
    }
    return found;
}

int AnimationDocument::CameraAt(float time, int fallback) const {
    const int index = CutIndexAt(time);
    if (index < 0) return fallback;
    const int id = Cameras.Cuts[(size_t)index].CameraId;
    // Камеру могли удалить из сцены уже после того, как склейку поставили.
    // Ронять рендер из-за этого нельзя — возвращаем ручной выбор.
    return id >= 0 ? id : fallback;
}

int AnimationDocument::SetCut(float time, int cameraId) {
    for (size_t i = 0; i < Cameras.Cuts.size(); ++i) {
        if (std::abs(Cameras.Cuts[i].Time - time) <= kCutEpsilon) {
            Cameras.Cuts[i].CameraId = cameraId;
            return (int)i;
        }
    }
    CameraCut cut;
    cut.Time = std::max(0.0f, time);
    cut.CameraId = cameraId;
    Cameras.Cuts.push_back(cut);
    std::stable_sort(Cameras.Cuts.begin(), Cameras.Cuts.end(),
                     [](const CameraCut& a, const CameraCut& b) { return a.Time < b.Time; });
    for (size_t i = 0; i < Cameras.Cuts.size(); ++i) {
        if (std::abs(Cameras.Cuts[i].Time - cut.Time) <= kCutEpsilon) return (int)i;
    }
    return (int)Cameras.Cuts.size() - 1;
}

void AnimationDocument::RemoveCut(int index) {
    if (index < 0 || index >= (int)Cameras.Cuts.size()) return;
    Cameras.Cuts.erase(Cameras.Cuts.begin() + index);
}

int AnimationDocument::MoveCut(int index, float time) {
    if (index < 0 || index >= (int)Cameras.Cuts.size()) return index;
    const int camera = Cameras.Cuts[(size_t)index].CameraId;
    Cameras.Cuts.erase(Cameras.Cuts.begin() + index);
    return SetCut(std::max(0.0f, time), camera);
}

// ============================================================================
//  Ключи
// ============================================================================

bool AnimationDocument::KeyFromScene(Scene& scene, int targetId, Property prop, float time,
                                     int joint) {
    float values[3] = {0.0f, 0.0f, 0.0f};
    if (!ReadProperty(scene, targetId, prop, values, joint)) return false;

    const PropertyInfo& info = PropertyInfoOf(prop);
    Track& track = EnsureTrack(targetId, prop, joint);
    if (IsBoneProperty(prop) && track.JointName.empty()) {
        track.JointName = JointName(scene, targetId, joint);
    }
    const Interp mode = info.Stepped ? Interp::Constant : Interp::Smooth;
    for (int c = 0; c < info.Channels && c < track.ChannelCount(); ++c) {
        track.Channels[(size_t)c].SetKey(time, values[c], mode);
    }
    return true;
}

int AnimationDocument::KeyExistingTracks(Scene& scene, int targetId, float time) {
    int keyed = 0;
    for (Track& track : Tracks) {
        if (track.TargetId != targetId || track.Locked) continue;
        float values[3] = {0.0f, 0.0f, 0.0f};
        if (!ReadProperty(scene, targetId, track.Prop, values, track.Joint)) continue;
        const PropertyInfo& info = PropertyInfoOf(track.Prop);
        const Interp mode = info.Stepped ? Interp::Constant : Interp::Smooth;
        for (int c = 0; c < info.Channels && c < track.ChannelCount(); ++c) {
            track.Channels[(size_t)c].SetKey(time, values[c], mode);
        }
        ++keyed;
    }
    return keyed;
}

int AnimationDocument::KeyBone(Scene& scene, int targetId, int joint, float time) {
    // Поза кости — это все три канала сразу. Ключить их порознь можно (дорожки
    // независимы), но кнопка «заключить» ставит позу целиком: так аниматор не
    // получает через десять кадров сюрприз в виде забытого масштаба.
    int keyed = 0;
    const Property props[] = {Property::BonePosition, Property::BoneRotation, Property::BoneScale};
    for (Property prop : props) {
        if (KeyFromScene(scene, targetId, prop, time, joint)) ++keyed;
    }
    return keyed;
}

int AnimationDocument::KeyPose(Scene& scene, int targetId, float time) {
    const sage::anim::Skeleton* sk = SkeletonOf(scene, targetId);
    if (!sk) return 0;

    // Кости, которые уже ведёт документ, — их ключим в любом случае, иначе
    // дорожка «застынет» на прошлом ключе, пока остальные едут.
    std::vector<bool> wanted((size_t)sk->Count(), false);
    for (const Track& t : Tracks) {
        if (t.TargetId != targetId || !IsBoneProperty(t.Prop)) continue;
        if (t.Joint >= 0 && t.Joint < sk->Count()) wanted[(size_t)t.Joint] = true;
    }
    // Плюс кости, которые аниматор трогал руками в этом кадре.
    GameObject obj = scene.Get(targetId);
    if (obj.Valid()) {
        if (const PoseComponent* pose = scene.Registry().try_get<PoseComponent>(obj.Entity())) {
            const int count = std::min((int)pose->Joints.size(), sk->Count());
            for (int i = 0; i < count; ++i) {
                if (pose->Joints[(size_t)i].Any()) wanted[(size_t)i] = true;
            }
        }
    }

    int keyed = 0;
    for (int i = 0; i < sk->Count(); ++i) {
        if (wanted[(size_t)i]) keyed += KeyBone(scene, targetId, i, time);
    }
    return keyed;
}

int AnimationDocument::RemoveKeysAt(Track& track, float time) {
    int removed = 0;
    for (Curve& curve : track.Channels) {
        const int idx = curve.KeyIndexAt(time);
        if (idx >= 0 && curve.RemoveKey(idx)) ++removed;
    }
    return removed;
}

bool AnimationDocument::HasKeyAt(const Track& track, float time) const {
    for (const Curve& curve : track.Channels) {
        if (curve.KeyIndexAt(time) >= 0) return true;
    }
    return false;
}

// ============================================================================
//  Операции над ключами
// ============================================================================

namespace {

// Попадает ли дорожка в запрошенный набор. Пустой набор значит «все».
bool WantedTrack(const std::vector<int>& trackIds, int id) {
    if (trackIds.empty()) return true;
    return std::find(trackIds.begin(), trackIds.end(), id) != trackIds.end();
}

} // namespace

int AnimationDocument::CopyKeys(int targetId, const std::vector<int>& trackIds, float from,
                                float to, KeyClipboard& out) const {
    out.Clear();
    if (to < from) std::swap(from, to);

    // Начало отсчёта — ЛЕВЫЙ КРАЙ ДИАПАЗОНА, а не время первого попавшего
    // ключа. Иначе вставка съезжала бы: выделили отрезок с паузой в начале —
    // пауза при вставке пропала бы, и рисунок сместился.
    for (const Track& track : Tracks) {
        if (track.TargetId != targetId || !WantedTrack(trackIds, track.Id)) continue;
        for (int c = 0; c < track.ChannelCount(); ++c) {
            for (const Keyframe& k : track.Channels[(size_t)c].Keys()) {
                if (k.Time < from - Curve::kTimeEpsilon || k.Time > to + Curve::kTimeEpsilon) continue;
                ClipboardKey copy;
                copy.Prop = track.Prop;
                copy.Joint = track.Joint;
                copy.Channel = c;
                copy.Offset = k.Time - from;
                copy.Key = k;
                out.Keys.push_back(copy);
            }
        }
    }
    return (int)out.Keys.size();
}

int AnimationDocument::PasteKeys(int targetId, const KeyClipboard& clip, float atTime) {
    int pasted = 0;
    for (const ClipboardKey& c : clip.Keys) {
        Track& track = EnsureTrack(targetId, c.Prop, c.Joint);
        if (c.Channel >= track.ChannelCount()) continue; // буфер от свойства с другим числом каналов
        Curve& curve = track.Channels[(size_t)c.Channel];
        const int index = curve.SetKey(atTime + c.Offset, c.Key.Value, c.Key.Mode);
        // Касательные переносим отдельно: SetKey знает только значение и режим,
        // а ручки Безье — это форма, ради которой ключ и копировали.
        Keyframe& placed = curve.AtMutable(index);
        placed.InTangent = c.Key.InTangent;
        placed.OutTangent = c.Key.OutTangent;
        ++pasted;
    }
    return pasted;
}

int AnimationDocument::ScaleKeyTimes(int targetId, const std::vector<int>& trackIds, float from,
                                     float to, float pivot, float factor) {
    if (factor <= 0.0f) return 0;
    if (to < from) std::swap(from, to);

    int moved = 0;
    for (Track& track : Tracks) {
        if (track.TargetId != targetId || track.Locked || !WantedTrack(trackIds, track.Id)) continue;
        for (Curve& curve : track.Channels) {
            // Собираем новые времена ДО правки: menять времена на месте нельзя,
            // кривая держит ключи отсортированными, и перестановка посреди
            // обхода сбила бы индексы.
            std::vector<Keyframe> kept;
            std::vector<Keyframe> scaled;
            for (const Keyframe& k : curve.Keys()) {
                if (k.Time < from - Curve::kTimeEpsilon || k.Time > to + Curve::kTimeEpsilon) {
                    kept.push_back(k);
                    continue;
                }
                Keyframe moved_key = k;
                moved_key.Time = pivot + (k.Time - pivot) * factor;
                // Касательные заданы в «значение за секунду»: растянув время,
                // надо поделить наклон, иначе форма кривой поедет.
                moved_key.InTangent /= factor;
                moved_key.OutTangent /= factor;
                scaled.push_back(moved_key);
                ++moved;
            }
            if (scaled.empty()) continue;

            curve.Clear();
            for (const Keyframe& k : kept) {
                const int i = curve.SetKey(k.Time, k.Value, k.Mode);
                curve.AtMutable(i).InTangent = k.InTangent;
                curve.AtMutable(i).OutTangent = k.OutTangent;
            }
            for (const Keyframe& k : scaled) {
                const int i = curve.SetKey(k.Time, k.Value, k.Mode);
                curve.AtMutable(i).InTangent = k.InTangent;
                curve.AtMutable(i).OutTangent = k.OutTangent;
            }
        }
    }
    return moved;
}

int AnimationDocument::BakeTrack(Track& track, float from, float to, float fps) {
    if (fps <= 0.0f) return 0;
    if (to < from) std::swap(from, to);

    int baked = 0;
    for (Curve& curve : track.Channels) {
        if (curve.Empty()) continue;

        // Сэмплируем ДО очистки: значения читаются со старой кривой, а пишутся
        // в новую. Делать это на месте нельзя — уже поставленные ключи меняли
        // бы результат сэмплирования следующих.
        const int firstFrame = (int)std::lround(from * fps);
        const int lastFrame = (int)std::lround(to * fps);
        std::vector<Keyframe> samples;
        samples.reserve((size_t)std::max(lastFrame - firstFrame + 1, 1));
        for (int f = firstFrame; f <= lastFrame; ++f) {
            Keyframe k;
            k.Time = (float)f / fps;
            k.Value = curve.Evaluate(k.Time);
            // Линейная, а не сглаженная: между соседними кадрами сглаживать
            // нечего, а авто-касательные заново придумали бы форму, которую мы
            // только что зафиксировали покадрово.
            k.Mode = Interp::Linear;
            samples.push_back(k);
        }

        // Ключи ВНЕ диапазона сохраняем: запекают обычно кусок, а не всю
        // дорожку, и стирать остальное — это потеря работы без спроса.
        std::vector<Keyframe> outside;
        for (const Keyframe& k : curve.Keys()) {
            if (k.Time < from - Curve::kTimeEpsilon || k.Time > to + Curve::kTimeEpsilon) {
                outside.push_back(k);
            }
        }

        curve.Clear();
        for (const Keyframe& k : outside) {
            const int i = curve.SetKey(k.Time, k.Value, k.Mode);
            curve.AtMutable(i).InTangent = k.InTangent;
            curve.AtMutable(i).OutTangent = k.OutTangent;
        }
        for (const Keyframe& k : samples) {
            curve.SetKey(k.Time, k.Value, k.Mode);
            ++baked;
        }
    }
    return baked;
}

bool AnimationDocument::PrevKeyTime(float from, float& out) const {
    bool found = false;
    float best = 0.0f;
    auto consider = [&](float t) {
        if (t < from - Curve::kTimeEpsilon && (!found || t > best)) { best = t; found = true; }
    };
    for (const Track& track : Tracks) {
        for (const Curve& curve : track.Channels) {
            for (const Keyframe& k : curve.Keys()) consider(k.Time);
        }
    }
    for (const ClipTrack& track : ClipTracks) {
        for (const ClipBlock& b : track.Blocks) { consider(b.Start); consider(b.Start + b.Duration); }
    }
    for (const Marker& m : Markers) consider(m.Time);
    out = best;
    return found;
}

bool AnimationDocument::NextKeyTime(float from, float& out) const {
    bool found = false;
    float best = 0.0f;
    auto consider = [&](float t) {
        if (t > from + Curve::kTimeEpsilon && (!found || t < best)) { best = t; found = true; }
    };
    for (const Track& track : Tracks) {
        for (const Curve& curve : track.Channels) {
            for (const Keyframe& k : curve.Keys()) consider(k.Time);
        }
    }
    for (const ClipTrack& track : ClipTracks) {
        for (const ClipBlock& b : track.Blocks) { consider(b.Start); consider(b.Start + b.Duration); }
    }
    for (const Marker& m : Markers) consider(m.Time);
    out = best;
    return found;
}

// ============================================================================
//  Применение документа к сцене
// ============================================================================

const ClipBlock* AnimationDocument::ActiveBlockAt(const ClipTrack& track, float time) {
    const ClipBlock* best = nullptr;
    for (const ClipBlock& b : track.Blocks) {
        if (time < b.Start || time > b.Start + b.Duration) continue;
        if (!best || b.Start >= best->Start) best = &b;
    }
    return best;
}

void AnimationDocument::Apply(Scene& scene, float time, bool seeking) const {
    // ПОРЯДОК ВАЖЕН: сначала клипы, потом свойства.
    //
    // Костная дорожка читает текущую позу как базу для каналов без ключей. Если
    // сначала выполнить дорожки свойств, эта база придёт из позы ПРОШЛОГО кадра
    // (клип ещё не перемотан), и кость с ключами только на повороте таскала бы
    // за собой позапрошлый перенос. Клипы ни от чего не зависят, поэтому им
    // ничего не стоит идти первыми.

    // --- Дорожки клипов: ведём движковый Animator сущности ---
    auto& reg = scene.Registry();
    for (const ClipTrack& track : ClipTracks) {
        if (track.Muted) continue;
        GameObject obj = scene.Get(track.TargetId);
        if (!obj.Valid()) continue;
        AnimatedModelComponent* anim = reg.try_get<AnimatedModelComponent>(obj.Entity());
        // Ready выставляет sage::anim::UpdateAnimators после загрузки модели и
        // привязки скелета; до этого управлять проигрывателем нечем.
        if (!anim || !anim->Ready) continue;

        const ClipBlock* block = ActiveBlockAt(track, time);
        if (!block) {
            // Вне блоков персонаж замирает в текущей позе — управляемая пауза
            // лучше, чем «продолжает жить сам по себе» посреди пустого таймлайна.
            anim->Playing = false;
            anim->Anim.Stop();
            continue;
        }

        anim->Clip = block->ClipIndex;
        anim->Speed = block->Speed;
        anim->Loop = block->Loop;
        // При перемотке поза ставится ниже точно на нужное время, и продвигать
        // её нельзя: sage::anim::UpdateAnimators этим же кадром вызовет
        // Animator::Update(dt) и сдвинул бы клип с только что заданной точки.
        // Playing=false заставляет его передать dt = 0 — поза удерживается.
        anim->Playing = !seeking;

        // Локальное время внутри блока с учётом скорости — это и есть позиция
        // в клипе. Перемотка ставит её напрямую, обычное проигрывание идёт
        // через Animator::Update, чтобы работал кросс-фейд движка.
        const float local = (time - block->Start) * block->Speed;
        if (seeking) {
            if (anim->Anim.CurrentClip() != block->ClipIndex) anim->Anim.Play(block->ClipIndex, block->Loop);
            anim->Anim.SetSpeed(block->Speed);
            anim->Anim.Seek(local);
        } else {
            if (anim->Anim.CurrentClip() != block->ClipIndex) {
                anim->Anim.CrossFade(block->ClipIndex, block->BlendIn, block->Loop);
            }
            anim->Anim.SetSpeed(block->Speed);
            // Само продвижение времени делает sage::anim::UpdateAnimators —
            // здесь мы только задаём, ЧТО и с какой скоростью играет.
        }
    }

    // --- Дорожки свойств: сэмплируем кривые и пишем в компоненты ---
    for (const Track& track : Tracks) {
        if (track.Muted || track.Channels.empty()) continue;

        // Полностью пустая дорожка не должна перетирать сцену нулями: пока в ней
        // нет ни одного ключа, объект остаётся там, куда его поставил аниматор.
        bool anyKeys = false;
        for (const Curve& c : track.Channels) {
            if (!c.Empty()) { anyKeys = true; break; }
        }
        if (!anyKeys) continue;

        // Текущее значение — база для каналов без ключей (заключили только X —
        // Y и Z остаются такими, как в сцене, а не схлопываются в ноль).
        float values[3] = {0.0f, 0.0f, 0.0f};
        if (!ReadProperty(scene, track.TargetId, track.Prop, values, track.Joint)) continue;

        const int channels = std::min(track.ChannelCount(), 3);
        for (int c = 0; c < channels; ++c) {
            const Curve& curve = track.Channels[(size_t)c];
            if (!curve.Empty()) values[c] = curve.Evaluate(time, values[c]);
        }
        WriteProperty(scene, track.TargetId, track.Prop, values, track.Joint);
    }

    // Указатель на переопределения позы движок держит «сырым», а хранилище
    // компонентов могло переехать в памяти между кадрами (добавили объект,
    // откатили undo). Переустанавливаем его каждый кадр — это дёшево и снимает
    // целый класс висячих указателей.
    SyncAllPoseOverrides(scene);
}

float AnimationDocument::ContentEnd() const {
    float end = 0.0f;
    for (const Track& track : Tracks) {
        for (const Curve& curve : track.Channels) {
            if (!curve.Empty()) end = std::max(end, curve.LastTime());
        }
    }
    for (const ClipTrack& track : ClipTracks) {
        for (const ClipBlock& b : track.Blocks) end = std::max(end, b.Start + b.Duration);
    }
    for (const CameraCut& c : Cameras.Cuts) end = std::max(end, c.Time);
    for (const Marker& m : Markers) end = std::max(end, m.Time);
    if (Audio.Loaded()) end = std::max(end, Audio.Offset + Audio.Length());
    return end;
}

void AnimationDocument::ClearContent() {
    Tracks.clear();
    ClipTracks.clear();
    Cameras = CameraTrack{};
    Markers.clear();
    Audio.Clear();
    m_nextId = 1;
}

} // namespace d3d
