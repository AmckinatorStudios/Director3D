#pragma once
#include <string>

// ---------------------------------------------------------------------------
// Playback — транспорт ролика: где стоит головка, играем мы или нет, как
// показать время человеку.
//
// Единица хранения — секунды; кадры и таймкод ЧЧ:ММ:СС:КК — это перевод, а не
// хранение. Поэтому смена частоты кадров (24 → 30) не сдвигает ни одного ключа
// и не меняет длительность ролика: меняется только сетка, по которой время
// показывается и прилипает.
// ---------------------------------------------------------------------------
namespace d3d {

class Playback {
public:
    enum class State { Stopped, Playing };

    // Продвигает головку на dt секунд, если играем. Возвращает true, если
    // головка сдвинулась (вызывающему надо переприменить документ к сцене).
    // duration — длина ролика: по её достижении либо стоп, либо заворот (Loop).
    bool Advance(float dt, float duration);

    void Play() { m_state = State::Playing; }
    void Pause() { m_state = State::Stopped; }
    void TogglePlay() { m_state = m_state == State::Playing ? State::Stopped : State::Playing; }
    // Стоп — это «остановиться и вернуться в начало» (кнопка Stop в референсе),
    // в отличие от паузы, которая оставляет головку на месте.
    void Stop() { m_state = State::Stopped; m_time = 0.0f; }

    bool Playing() const { return m_state == State::Playing; }

    float Time() const { return m_time; }
    // Ставит головку (с зажимом в [0, duration]). Помечает кадр как «перемотка»,
    // чтобы скелетные клипы встали позой точно на это время (см. Seeking).
    void SetTime(float t, float duration);

    // Был ли последний сдвиг головки перемоткой (а не непрерывным ходом).
    // Сбрасывается в Advance при обычном проигрывании.
    bool Seeking() const { return m_seeking; }
    void ClearSeeking() { m_seeking = false; }

    bool Loop = true;
    float Speed = 1.0f; // множитель скорости проигрывания (замедленная съёмка)

    // --- Кадры и таймкод ---------------------------------------------------
    // Номер кадра, на котором стоит головка при данной частоте.
    int FrameAt(float fps) const;
    // Прилипание времени к сетке кадров: аниматор всегда стоит на кадре, а не
    // между ними — иначе ключи попадают на дробное время и перестают совпадать
    // с тем, что уйдёт в рендер.
    static float SnapToFrame(float time, float fps);
    // Таймкод ЧЧ:ММ:СС:КК — формат из референса (00:00:04:12).
    static std::string Timecode(float time, float fps);
    // Разбор таймкода обратно во время; false — строка не разобрана.
    static bool ParseTimecode(const std::string& text, float fps, float& outTime);

private:
    State m_state = State::Stopped;
    float m_time = 0.0f;
    bool m_seeking = true; // первый кадр после старта — всегда постановка позы
};

} // namespace d3d
