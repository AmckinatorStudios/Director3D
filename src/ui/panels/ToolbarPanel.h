#pragma once

namespace d3d {

class DirectorHost;

// Верхняя панель инструментов: файловые операции, отмена/повтор, инструменты
// манипуляции, быстрое создание объектов, транспорт и рендер, а справа —
// переключатель «Простой / Продвинутый».
class ToolbarPanel {
public:
    // Рисуется в полосе фиксированной высоты под меню (см. Theme::kToolbarHeight).
    void Draw(DirectorHost& host);
};

} // namespace d3d
