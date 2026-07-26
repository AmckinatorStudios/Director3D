#include "anim/Playback.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace d3d {

bool Playback::Advance(float dt, float duration) {
    if (m_state != State::Playing) return false;
    if (duration <= 0.0f) return false;

    m_seeking = false; // непрерывный ход: клипы ведёт движок, не перематываем
    m_time += dt * Speed;

    if (m_time >= duration) {
        if (Loop) {
            // Заворот — это разрыв непрерывности: позы клипов надо поставить
            // заново, иначе персонаж «доигрывает» конец ролика в его начале.
            m_time = std::fmod(m_time, duration);
            m_seeking = true;
        } else {
            m_time = duration;
            m_state = State::Stopped;
        }
    } else if (m_time < 0.0f) { // обратное проигрывание (Speed < 0)
        if (Loop) {
            m_time = duration + std::fmod(m_time, duration);
            m_seeking = true;
        } else {
            m_time = 0.0f;
            m_state = State::Stopped;
        }
    }
    return true;
}

void Playback::SetTime(float t, float duration) {
    m_time = std::clamp(t, 0.0f, duration > 0.0f ? duration : 0.0f);
    m_seeking = true;
}

int Playback::FrameAt(float fps) const {
    if (fps <= 0.0f) return 0;
    return (int)std::lround(m_time * fps);
}

float Playback::SnapToFrame(float time, float fps) {
    if (fps <= 0.0f) return time;
    return std::round(time * fps) / fps;
}

std::string Playback::Timecode(float time, float fps) {
    if (fps <= 0.0f) fps = 24.0f;
    if (time < 0.0f) time = 0.0f;

    // Считаем в целых кадрах: иначе на границе секунды набегает 23.9999 и
    // таймкод показывает предыдущую секунду с кадром 24.
    long long totalFrames = std::llround((double)time * (double)fps);
    const long long framesPerSecond = std::max(1ll, (long long)std::lround(fps));

    const long long frames = totalFrames % framesPerSecond;
    long long seconds = totalFrames / framesPerSecond;
    const long long ss = seconds % 60; seconds /= 60;
    const long long mm = seconds % 60; seconds /= 60;
    const long long hh = seconds;

    // Буфер с запасом: минуты/секунды/кадры ограничены, а часы формально нет —
    // 64 байта покрывают любое значение long long без усечения.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld:%02lld", hh, mm, ss, frames);
    return buf;
}

bool Playback::ParseTimecode(const std::string& text, float fps, float& outTime) {
    if (fps <= 0.0f) fps = 24.0f;
    int hh = 0, mm = 0, ss = 0, ff = 0;

    // Полный таймкод ЧЧ:ММ:СС:КК, укороченный ММ:СС:КК или просто номер кадра —
    // всё три формы удобно набирать руками, и все три однозначны.
    if (std::sscanf(text.c_str(), "%d:%d:%d:%d", &hh, &mm, &ss, &ff) == 4) {
        // разобрано полностью
    } else if (std::sscanf(text.c_str(), "%d:%d:%d", &mm, &ss, &ff) == 3) {
        hh = 0;
    } else if (std::sscanf(text.c_str(), "%d", &ff) == 1) {
        hh = mm = ss = 0;
    } else {
        return false;
    }
    if (hh < 0 || mm < 0 || ss < 0 || ff < 0) return false;

    const double seconds = hh * 3600.0 + mm * 60.0 + ss + (double)ff / (double)fps;
    outTime = (float)seconds;
    return true;
}

} // namespace d3d
