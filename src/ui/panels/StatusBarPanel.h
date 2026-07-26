#pragma once

namespace d3d {

class DirectorHost;

// Нижняя строка состояния: версия, видеокарта, память, кадры в секунду,
// имя проекта и время последнего сохранения — то же, что в референсе.
class StatusBarPanel {
public:
    void Draw(DirectorHost& host);
    void NoteSaved();

private:
    char m_savedAt[16] = "--:--";
    // Сглаженный FPS: мгновенный прыгает на каждом кадре и его невозможно читать.
    float m_smoothFps = 0.0f;
};

} // namespace d3d
