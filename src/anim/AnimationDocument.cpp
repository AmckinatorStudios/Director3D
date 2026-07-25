#include "anim/AnimationDocument.h"

#include <algorithm>
#include <cmath>

#include "anim/DirectorComponents.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace d3d {

// ============================================================================
//  Дорожки свойств
// ============================================================================

Track* AnimationDocument::FindTrack(int targetId, Property prop) {
    for (Track& t : Tracks) {
        if (t.TargetId == targetId && t.Prop == prop) return &t;
    }
    return nullptr;
}

const Track* AnimationDocument::FindTrack(int targetId, Property prop) const {
    return const_cast<AnimationDocument*>(this)->FindTrack(targetId, prop);
}

Track* AnimationDocument::TrackById(int id) {
    for (Track& t : Tracks) {
        if (t.Id == id) return &t;
    }
    return nullptr;
}

Track& AnimationDocument::EnsureTrack(int targetId, Property prop) {
    if (Track* existing = FindTrack(targetId, prop)) return *existing;
    Track t;
    t.Id = TakeId();
    t.TargetId = targetId;
    t.Prop = prop;
    t.Channels.resize((size_t)PropertyInfoOf(prop).Channels);
    Tracks.push_back(std::move(t));
    return Tracks.back();
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
//  Ключи
// ============================================================================

bool AnimationDocument::KeyFromScene(Scene& scene, int targetId, Property prop, float time) {
    float values[3] = {0.0f, 0.0f, 0.0f};
    if (!ReadProperty(scene, targetId, prop, values)) return false;

    const PropertyInfo& info = PropertyInfoOf(prop);
    Track& track = EnsureTrack(targetId, prop);
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
        if (!ReadProperty(scene, targetId, track.Prop, values)) continue;
        const PropertyInfo& info = PropertyInfoOf(track.Prop);
        const Interp mode = info.Stepped ? Interp::Constant : Interp::Smooth;
        for (int c = 0; c < info.Channels && c < track.ChannelCount(); ++c) {
            track.Channels[(size_t)c].SetKey(time, values[c], mode);
        }
        ++keyed;
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

namespace {

// Блок клипа, активный в момент time (последний начавшийся из перекрывающихся —
// так «положить блок поверх» работает как в любом NLE). nullptr — тишина.
const ClipBlock* ActiveBlock(const ClipTrack& track, float time) {
    const ClipBlock* best = nullptr;
    for (const ClipBlock& b : track.Blocks) {
        if (time < b.Start || time > b.Start + b.Duration) continue;
        if (!best || b.Start >= best->Start) best = &b;
    }
    return best;
}

} // namespace

void AnimationDocument::Apply(Scene& scene, float time, bool seeking) const {
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
        if (!ReadProperty(scene, track.TargetId, track.Prop, values)) continue;

        const int channels = std::min(track.ChannelCount(), 3);
        for (int c = 0; c < channels; ++c) {
            const Curve& curve = track.Channels[(size_t)c];
            if (!curve.Empty()) values[c] = curve.Evaluate(time, values[c]);
        }
        WriteProperty(scene, track.TargetId, track.Prop, values);
    }

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

        const ClipBlock* block = ActiveBlock(track, time);
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
    for (const Marker& m : Markers) end = std::max(end, m.Time);
    if (Audio.Loaded()) end = std::max(end, Audio.Offset + Audio.Length());
    return end;
}

void AnimationDocument::ClearContent() {
    Tracks.clear();
    ClipTracks.clear();
    Markers.clear();
    Audio.Clear();
    m_nextId = 1;
}

} // namespace d3d
