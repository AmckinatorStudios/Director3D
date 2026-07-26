#pragma once
#include <glm/glm.hpp>

#include "anim/Binding.h"

namespace d3d {

class DirectorHost;

// Инспектор выбранного объекта: трансформ, параметры камеры/света/материала,
// пост-обработка. У каждого анимируемого свойства — ромб-ключ справа: он
// показывает, есть ли ключ на текущем кадре, и ставит/убирает его по клику.
// Это главный способ анимировать что угодно, не открывая таймлайн.
class PropertiesPanel {
public:
    void Draw(DirectorHost& host);

private:
    // Строка «подпись + поля XYZ + ромб-ключ». Возвращает true, если значение
    // изменили — вызывающий пишет его в компонент.
    // joint >= 0 — строка относится к КОСТИ: ромб смотрит на костную дорожку
    // этой кости, а не на дорожку объекта.
    bool DrawVec3Row(DirectorHost& host, const char* label, float* value,
                     Property prop, float speed, const char* format, int joint = -1);
    // То же для одного числа (FOV, интенсивность, диафрагма).
    bool DrawFloatRow(DirectorHost& host, const char* label, float* value,
                      Property prop, float speed, float lo, float hi, const char* format);
    // Строка «галочка + ползунок» для пост-обработки, как в референсе.
    bool DrawEffectRow(DirectorHost& host, const char* label, bool* enabled, float* amount,
                       Property prop, float lo, float hi);
    // Ромб-ключ справа от строки: закрашен, если ключ на текущем кадре есть.
    // Клик ставит/убирает ключ. prop == Property::Position и т.п.; hasProp ==
    // false рисует место под ромб, но не даёт его нажать (свойство не анимируемо).
    void DrawKeyDiamond(DirectorHost& host, Property prop, bool hasProp, int joint = -1);
    // Раздел «Bone» — появляется, когда выбрана кость. Здесь локальные TRS
    // кости с ромбами ключей и кнопки работы с позой.
    void DrawBoneSection(DirectorHost& host);
    // Раздел «Blend Shapes» — ползунки весов морф-целей с ромбами ключей.
    void DrawMorphSection(DirectorHost& host);
    // Раздел IK внутри секции кости: цель, полюс и длина цепочки.
    void DrawIKSection(DirectorHost& host, int entityId, int joint);

    // Состояние ручки IK. Живёт в панели, а не в сцене: это ИНСТРУМЕНТ, а не
    // свойство персонажа — цель задаётся на время правки позы и снимается
    // ключом вместе с ней.
    glm::vec3 m_ikTarget{0.0f};
    glm::vec3 m_ikPole{0.0f};
    bool m_ikUsePole = false;
    int m_ikChainLength = 3;
    float m_ikWeight = 1.0f;
    int m_ikBone = -1;     // для какой кости взята цель (смена кости обнуляет)
    bool m_ikReached = true;
};

} // namespace d3d
