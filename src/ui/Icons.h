#pragma once
#include "imgui.h"

// ---------------------------------------------------------------------------
// Иконки Director 3D — векторные, рисуются линиями и фигурами через ImDrawList.
//
// Почему не шрифт иконок: любой (FontAwesome и товарищи) — это внешний файл,
// который надо положить рядом с бинарником, залицензировать и не потерять при
// переносе. Инструмент должен запускаться из каталога сборки и выглядеть
// одинаково везде, поэтому иконки нарисованы кодом: они чёткие на любом
// масштабе, красятся в любой цвет и ничего не весят.
//
// Все Draw* принимают ЦЕНТР и размер описанного квадрата — иконку можно ставить
// в кнопку любого размера, и она останется отцентрованной.
// ---------------------------------------------------------------------------
namespace d3d::Icons {

enum class Icon {
    New, Open, Save, Import, Export,
    Undo, Redo,
    Select, Move, Rotate, Scale,
    Camera, Light, Cube, Character, Effect, Audio, Folder, Model, Material, Texture,
    Play, Pause, Stop, Record, Render,
    SkipStart, SkipEnd, PrevKey, NextKey, StepBack, StepForward,
    Key, Loop, Eye, EyeOff, Lock, Unlock, Mute, Search, Add, Trash, Curve, Grid
};

// Рисует иконку в указанный список отрисовки.
void Draw(ImDrawList* dl, Icon icon, ImVec2 center, float size, ImU32 color);

// --- Готовые виджеты --------------------------------------------------------

// Крупная кнопка тулбара: иконка сверху, подпись снизу (как в референсе).
// active — «нажатое» состояние (текущий инструмент, включённый режим).
// Возвращает true при клике. enabled=false рисует приглушённо и не кликается.
//
// width = 0 означает «по подписи»: ширина считается из текста, но не уже 56 px.
// Фиксированная ширина работала, пока подписи были английскими; «Перемещение»
// вместо «Move» вылезало на соседнюю кнопку. Считать ширину по тексту —
// единственный способ пережить смену языка, не подгоняя числа вручную.
bool ToolbarButton(Icon icon, const char* label, const char* tooltip,
                   bool active = false, bool enabled = true, float width = 0.0f);

// Компактная квадратная кнопка (транспорт таймлайна, кнопки в строке дорожки).
bool IconButton(const char* id, Icon icon, const char* tooltip,
                bool active = false, bool enabled = true, float size = 24.0f);

// Иконка-переключатель без рамки: глазок видимости, замок, mute в дорожках.
// value меняется по клику; возвращает true, если значение изменилось.
bool ToggleIcon(const char* id, Icon onIcon, Icon offIcon, bool& value,
                const char* tooltip, float size = 16.0f);

// Разделитель тулбара — вертикальная линия с воздухом по бокам.
void ToolbarSeparator();

} // namespace d3d::Icons
