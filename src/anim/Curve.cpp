#include "anim/Curve.h"

#include <algorithm>
#include <cmath>

namespace d3d {

const char* InterpName(Interp mode) {
    switch (mode) {
        case Interp::Constant:  return "Ступенька";
        case Interp::Linear:    return "Линейно";
        case Interp::Smooth:    return "Сглаженно";
        case Interp::Bezier:    return "Безье";
        case Interp::EaseIn:    return "Плавный старт";
        case Interp::EaseOut:   return "Плавная остановка";
        case Interp::EaseInOut: return "Плавно с двух сторон";
    }
    return "Сглаженно";
}

const char* InterpKey(Interp mode) {
    switch (mode) {
        case Interp::Constant:  return "constant";
        case Interp::Linear:    return "linear";
        case Interp::Smooth:    return "smooth";
        case Interp::Bezier:    return "bezier";
        case Interp::EaseIn:    return "easeIn";
        case Interp::EaseOut:   return "easeOut";
        case Interp::EaseInOut: return "easeInOut";
    }
    return "smooth";
}

Interp InterpFromKey(const std::string& key) {
    if (key == "constant")  return Interp::Constant;
    if (key == "linear")    return Interp::Linear;
    if (key == "bezier")    return Interp::Bezier;
    if (key == "easeIn")    return Interp::EaseIn;
    if (key == "easeOut")   return Interp::EaseOut;
    if (key == "easeInOut") return Interp::EaseInOut;
    return Interp::Smooth; // неизвестное значение из файла — безопасный дефолт
}

namespace {

// Кубический эрмитов сегмент: значения p0/p1 на концах и касательные m0/m1
// в единицах «значение за секунду». dt — длительность сегмента в секундах:
// касательные умножаются на неё, поэтому форма кривой не зависит от того,
// растянули ли мы сегмент по времени (иначе растянутый сегмент «взрывался» бы).
float Hermite(float p0, float p1, float m0, float m1, float dt, float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 =  2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h10 =         t3 - 2.0f * t2 + t;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 =         t3 -        t2;
    return h00 * p0 + h10 * (m0 * dt) + h01 * p1 + h11 * (m1 * dt);
}

// Сглаживание нормированного параметра для готовых кривых разгона/торможения.
// Совпадает по форме с sage::Easing (Quad) — движок и инструмент «дышат» одинаково.
float EaseParam(Interp mode, float t) {
    switch (mode) {
        case Interp::EaseIn:  return t * t;
        case Interp::EaseOut: return t * (2.0f - t);
        case Interp::EaseInOut:
            return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
        default: return t;
    }
}

} // namespace

int Curve::KeyIndexAt(float time) const {
    // lower_bound по времени, затем проверка соседей: ключ мог оказаться и слева
    // от границы (time чуть больше его времени), и справа (чуть меньше).
    auto it = std::lower_bound(m_keys.begin(), m_keys.end(), time,
                               [](const Keyframe& k, float t) { return k.Time < t; });
    int idx = (int)std::distance(m_keys.begin(), it);
    if (idx < (int)m_keys.size() && std::fabs(m_keys[(size_t)idx].Time - time) <= kTimeEpsilon) return idx;
    if (idx > 0 && std::fabs(m_keys[(size_t)idx - 1].Time - time) <= kTimeEpsilon) return idx - 1;
    return -1;
}

int Curve::SegmentIndexAt(float time) const {
    if (m_keys.empty()) return -1;
    auto it = std::upper_bound(m_keys.begin(), m_keys.end(), time,
                               [](float t, const Keyframe& k) { return t < k.Time; });
    return (int)std::distance(m_keys.begin(), it) - 1;
}

int Curve::SetKey(float time, float value, Interp mode) {
    int existing = KeyIndexAt(time);
    if (existing >= 0) {
        // Ключ на этом кадре уже стоит — обновляем значение, СОХРАНЯЯ настроенную
        // интерполяцию и ручки: перезапись кадра не должна сбрасывать работу
        // аниматора над формой кривой.
        m_keys[(size_t)existing].Value = value;
        return existing;
    }

    Keyframe k;
    k.Time = time;
    k.Value = value;
    k.Mode = mode;
    auto it = std::lower_bound(m_keys.begin(), m_keys.end(), time,
                               [](const Keyframe& a, float t) { return a.Time < t; });
    int idx = (int)std::distance(m_keys.begin(), it);
    m_keys.insert(it, k);
    return idx;
}

bool Curve::RemoveKey(int index) {
    if (index < 0 || index >= (int)m_keys.size()) return false;
    m_keys.erase(m_keys.begin() + index);
    return true;
}

int Curve::MoveKey(int index, float newTime, float newValue) {
    if (index < 0 || index >= (int)m_keys.size()) return index;
    Keyframe moved = m_keys[(size_t)index];
    moved.Time = newTime;
    moved.Value = newValue;
    m_keys.erase(m_keys.begin() + index);

    // Перетащили ровно на чужой кадр — тот ключ вытесняется (аниматор видит
    // один ромб и ждёт один ключ, а не два в одной точке).
    for (size_t i = 0; i < m_keys.size(); ++i) {
        if (std::fabs(m_keys[i].Time - newTime) <= kTimeEpsilon) {
            m_keys.erase(m_keys.begin() + (long)i);
            break;
        }
    }
    auto it = std::lower_bound(m_keys.begin(), m_keys.end(), newTime,
                               [](const Keyframe& a, float t) { return a.Time < t; });
    int idx = (int)std::distance(m_keys.begin(), it);
    m_keys.insert(it, moved);
    return idx;
}

void Curve::EffectiveTangents(int index, float& outIn, float& outOut) const {
    outIn = outOut = 0.0f;
    if (index < 0 || index >= (int)m_keys.size()) return;
    const Keyframe& k = m_keys[(size_t)index];
    if (k.Mode == Interp::Bezier) {
        outIn = k.InTangent;
        outOut = k.OutTangent;
        return;
    }
    // Авто-касательная (Catmull-Rom): наклон секущей через соседей. На краях
    // берём одностороннюю секущую, чтобы кривая не «вылезала» за крайний ключ.
    const int n = (int)m_keys.size();
    const Keyframe& prev = m_keys[(size_t)std::max(index - 1, 0)];
    const Keyframe& next = m_keys[(size_t)std::min(index + 1, n - 1)];
    const float dt = next.Time - prev.Time;
    const float slope = dt > kTimeEpsilon ? (next.Value - prev.Value) / dt : 0.0f;
    outIn = outOut = slope;
}

void Curve::ConvertToBezier(int index) {
    if (index < 0 || index >= (int)m_keys.size()) return;
    float in = 0.0f, out = 0.0f;
    EffectiveTangents(index, in, out);
    Keyframe& k = m_keys[(size_t)index];
    k.InTangent = in;
    k.OutTangent = out;
    k.Mode = Interp::Bezier;
}

float Curve::Evaluate(float time, float fallback) const {
    if (m_keys.empty()) return fallback;
    if (m_keys.size() == 1) return m_keys.front().Value;

    // За пределами диапазона держим крайние значения (см. комментарий в .h).
    if (time <= m_keys.front().Time) return m_keys.front().Value;
    if (time >= m_keys.back().Time) return m_keys.back().Value;

    const int i = SegmentIndexAt(time);
    if (i < 0) return m_keys.front().Value;
    if (i >= (int)m_keys.size() - 1) return m_keys.back().Value;

    const Keyframe& a = m_keys[(size_t)i];
    const Keyframe& b = m_keys[(size_t)i + 1];
    const float span = b.Time - a.Time;
    if (span <= kTimeEpsilon) return b.Value; // вырожденный сегмент — сразу следующий
    const float t = (time - a.Time) / span;

    switch (a.Mode) {
        case Interp::Constant:
            return a.Value;
        case Interp::Linear:
            return a.Value + (b.Value - a.Value) * t;
        case Interp::EaseIn:
        case Interp::EaseOut:
        case Interp::EaseInOut:
            return a.Value + (b.Value - a.Value) * EaseParam(a.Mode, t);
        case Interp::Bezier:
        case Interp::Smooth:
        default: {
            // Касательные берём «эффективные» с обоих концов: выход из левого
            // ключа и вход в правый. Соседние сегменты автоматически стыкуются
            // без излома, потому что для Smooth касательная у ключа одна.
            float aIn = 0.0f, aOut = 0.0f, bIn = 0.0f, bOut = 0.0f;
            EffectiveTangents(i, aIn, aOut);
            EffectiveTangents(i + 1, bIn, bOut);
            return Hermite(a.Value, b.Value, aOut, bIn, span, t);
        }
    }
}

void Curve::ValueRange(float& outMin, float& outMax) const {
    if (m_keys.empty()) { outMin = 0.0f; outMax = 1.0f; return; }
    outMin = outMax = m_keys.front().Value;
    for (const Keyframe& k : m_keys) {
        outMin = std::min(outMin, k.Value);
        outMax = std::max(outMax, k.Value);
    }
}

void Curve::Normalize() {
    std::stable_sort(m_keys.begin(), m_keys.end(),
                     [](const Keyframe& a, const Keyframe& b) { return a.Time < b.Time; });
    // Схлопываем дубликаты по времени: остаётся первый в порядке файла
    // (stable_sort сохранил его), остальные — мусор битого/правленого проекта.
    m_keys.erase(std::unique(m_keys.begin(), m_keys.end(),
                             [](const Keyframe& a, const Keyframe& b) {
                                 return std::fabs(a.Time - b.Time) <= kTimeEpsilon;
                             }),
                 m_keys.end());
}

} // namespace d3d
