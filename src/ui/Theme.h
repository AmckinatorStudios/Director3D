#pragma once
#include "imgui.h"

// ---------------------------------------------------------------------------
// Оформление Director 3D.
//
// Тёмная нейтральная база и один голубой акцент: инструмент показывает
// КАРТИНКУ, и любая цветная рамка вокруг вьюпорта врёт оператору о цвете кадра.
// По той же причине панели плоские, без градиентов и толстых теней.
//
// Палитра вынесена в именованные константы (Colors ниже), потому что её знают
// не только виджеты ImGui, но и всё, что рисуется руками через ImDrawList:
// таймлайн, редактор кривых, волна звука, иконки. Один источник цвета — и
// нарисованное вручную не разъезжается с нарисованным ImGui.
// ---------------------------------------------------------------------------
namespace d3d::Theme {

// Общая палитра. ImU32 в порядке ImGui (0xAABBGGRR) — так их принимает ImDrawList.
namespace Colors {
constexpr ImU32 PanelBg      = 0xFF23201D; // фон панелей
constexpr ImU32 PanelDeep    = 0xFF1B1917; // «утопленные» области (таймлайн, граф)
constexpr ImU32 PanelRaised  = 0xFF2E2A26; // приподнятые элементы (шапки, кнопки)
constexpr ImU32 Line         = 0xFF3A3531; // разделители
constexpr ImU32 LineSoft     = 0x553A3531; // сетки, вспомогательные линии
constexpr ImU32 Text         = 0xFFE8E6E3;
constexpr ImU32 TextDim      = 0xFF9A948E;
constexpr ImU32 TextFaint    = 0xFF6E6862;
constexpr ImU32 Accent       = 0xFFE8A03C; // акцент (голубой в ImGui-порядке = 0xFFE8A03C)
constexpr ImU32 AccentSoft   = 0x66E8A03C;
constexpr ImU32 Playhead     = 0xFFF0C24A; // головка воспроизведения
constexpr ImU32 KeySelected  = 0xFF4AC8F0;
constexpr ImU32 Record       = 0xFF4A4AE8; // авто-ключ включён (красный)
constexpr ImU32 Warning      = 0xFF3FC8E8;
constexpr ImU32 Good         = 0xFF5CC85C;
constexpr ImU32 Waveform     = 0xFFD9A03C;
constexpr ImU32 ClipBlockA   = 0xFF8A4A9A; // блоки клипов — фиолетовый
constexpr ImU32 ClipBlockB   = 0xFF4A8A5A; // и зелёный, чередуются по дорожкам
} // namespace Colors

// Ставит стиль и палитру ImGui. Зовётся один раз при старте.
void Apply();

// Грузит шрифт интерфейса (с кириллицей). Зовётся ДО Apply и до создания
// бэкенда ImGui — атлас шрифтов должен быть готов к первой загрузке текстуры.
void LoadFonts();

// Мелкий шрифт для плотных мест (линейка таймлайна, подписи каналов).
// nullptr, если отдельный шрифт не загрузился — вызывающий просто не толкает
// его в стек и получает обычный.
ImFont* SmallFont();
// Крупный шрифт для таймкода на транспорте.
ImFont* MonoFont();

// Высоты фиксированных полос интерфейса — их знают и раскладка дока, и панели.
constexpr float kMenuBarHeight = 26.0f;
constexpr float kToolbarHeight = 56.0f;
constexpr float kStatusBarHeight = 26.0f;

} // namespace d3d::Theme
