#include "ui/Icons.h"

#include <algorithm>

#include <cmath>
#include <unordered_map>

#include "imgui_internal.h" // ImGui::ItemAdd/ItemSize — своя геометрия у кнопок
#include "ui/Theme.h"

namespace d3d::Icons {

namespace {

constexpr float kPi = 3.14159265f;

// Все примитивы работают в нормированных координатах [-0.5, 0.5] вокруг центра:
// иконка описывается один раз и масштабируется под любую кнопку.
// Pen работает в двух режимах. С Dl != nullptr он рисует; с Dl == nullptr
// только ЗАМЕРЯЕТ рамку «чернил» иконки, ничего не выводя.
//
// Замер нужен, чтобы центрировать иконки автоматически. Иконки описаны от руки,
// и «чернила» у них расположены в квадрате по-разному: у лупы и стрелок отмены
// рисунок жмётся кверху, у папки — книзу. На панели инструментов это читается
// как «иконки скачут», и подгонять координаты каждой из трёх десятков вручную —
// работа, которую придётся повторять после любой правки и при каждой новой
// иконке. Проще посчитать рамку один раз и сдвинуть.
struct Pen {
    ImDrawList* Dl = nullptr;
    ImVec2 C{0.0f, 0.0f};
    float S = 1.0f;
    ImU32 Color = 0;
    float Thick = 1.0f;
    // Сдвиг, выравнивающий чернила по центру рамки (считается замером).
    float OffX = 0.0f, OffY = 0.0f;
    // Рамка чернил в нормированных координатах — заполняется в режиме замера.
    mutable float MinX = 1e9f, MinY = 1e9f, MaxX = -1e9f, MaxY = -1e9f;

    void Track(float x, float y, float pad = 0.0f) const {
        MinX = ImMin(MinX, x - pad); MaxX = ImMax(MaxX, x + pad);
        MinY = ImMin(MinY, y - pad); MaxY = ImMax(MaxY, y + pad);
    }
    ImVec2 P(float x, float y) const {
        Track(x, y);
        return ImVec2(C.x + (x + OffX) * S, C.y + (y + OffY) * S);
    }
    void Line(float x0, float y0, float x1, float y1) const {
        const ImVec2 a = P(x0, y0), b = P(x1, y1);
        if (Dl) Dl->AddLine(a, b, Color, Thick);
    }
    void Rect(float x0, float y0, float x1, float y1, float rounding = 0.0f) const {
        const ImVec2 a = P(x0, y0), b = P(x1, y1);
        if (Dl) Dl->AddRect(a, b, Color, rounding * S, 0, Thick);
    }
    void RectFilled(float x0, float y0, float x1, float y1, float rounding = 0.0f) const {
        const ImVec2 a = P(x0, y0), b = P(x1, y1);
        if (Dl) Dl->AddRectFilled(a, b, Color, rounding * S);
    }
    void Circle(float x, float y, float r, int seg = 16) const {
        Track(x, y, r);
        const ImVec2 c = P(x, y);
        if (Dl) Dl->AddCircle(c, r * S, Color, seg, Thick);
    }
    void CircleFilled(float x, float y, float r, int seg = 16) const {
        Track(x, y, r);
        const ImVec2 c = P(x, y);
        if (Dl) Dl->AddCircleFilled(c, r * S, Color, seg);
    }
    void Tri(float x0, float y0, float x1, float y1, float x2, float y2) const {
        const ImVec2 a = P(x0, y0), b = P(x1, y1), c = P(x2, y2);
        if (Dl) Dl->AddTriangleFilled(a, b, c, Color);
    }
    void Quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3,
              float y3) const {
        const ImVec2 a = P(x0, y0), b = P(x1, y1), c = P(x2, y2), d = P(x3, y3);
        if (Dl) Dl->AddQuadFilled(a, b, c, d, Color);
    }
    // Кубическая кривая по четырём точкам (волна, дуги переходов).
    void Bezier(float x0, float y0, float x1, float y1, float x2, float y2, float x3,
                float y3) const {
        const ImVec2 a = P(x0, y0), b = P(x1, y1), c = P(x2, y2), d = P(x3, y3);
        if (!Dl) return;
        Dl->PathLineTo(a);
        Dl->PathBezierCubicCurveTo(b, c, d, 20);
        Dl->PathStroke(Color, 0, Thick);
    }
    // Дуга по углам в градусах (0° — вправо, отсчёт по часовой в экранных координатах).
    void Arc(float x, float y, float r, float a0Deg, float a1Deg) const {
        // Замеряем дугу ПО ТОЧКАМ, а не по описанной окружности. Разница
        // принципиальная: у стрелок отмены рисуется верхняя треть круга, и
        // рамка целой окружности вдвое выше настоящих чернил — иконка после
        // такого «центрирования» уезжала вверх ровно на эту ошибку.
        const int steps = 16;
        for (int i = 0; i <= steps; ++i) {
            const float a = (a0Deg + (a1Deg - a0Deg) * (float)i / (float)steps) * kPi / 180.0f;
            Track(x + std::cos(a) * r, y + std::sin(a) * r);
        }
        const ImVec2 c = ImVec2(C.x + (x + OffX) * S, C.y + (y + OffY) * S);
        if (!Dl) return;
        Dl->PathArcTo(c, r * S, a0Deg * kPi / 180.0f, a1Deg * kPi / 180.0f, 24);
        Dl->PathStroke(Color, 0, Thick);
    }
};

// --- Отдельные иконки -------------------------------------------------------

void DrawDocument(const Pen& p, bool withPlus, bool withArrowDown, bool withArrowUp) {
    // Лист бумаги с загнутым уголком — общая основа для New/Open/Import/Export.
    p.Line(-0.28f, -0.40f, 0.12f, -0.40f);
    p.Line(0.12f, -0.40f, 0.28f, -0.24f);
    p.Line(0.28f, -0.24f, 0.28f, 0.40f);
    p.Line(0.28f, 0.40f, -0.28f, 0.40f);
    p.Line(-0.28f, 0.40f, -0.28f, -0.40f);
    p.Line(0.12f, -0.40f, 0.12f, -0.24f);
    p.Line(0.12f, -0.24f, 0.28f, -0.24f);
    if (withPlus) { p.Line(0.0f, -0.06f, 0.0f, 0.22f); p.Line(-0.14f, 0.08f, 0.14f, 0.08f); }
    if (withArrowDown) { p.Line(0.0f, -0.10f, 0.0f, 0.20f); p.Tri(-0.11f, 0.12f, 0.11f, 0.12f, 0.0f, 0.28f); }
    if (withArrowUp) { p.Line(0.0f, 0.28f, 0.0f, -0.02f); p.Tri(-0.11f, 0.06f, 0.11f, 0.06f, 0.0f, -0.10f); }
}

void DrawIcon(const Pen& p, Icon icon) {
    switch (icon) {
        case Icon::New:  DrawDocument(p, true, false, false); break;
        case Icon::Import: DrawDocument(p, false, true, false); break;
        case Icon::Export: DrawDocument(p, false, false, true); break;

        case Icon::Open: // раскрытая папка
            p.Line(-0.38f, -0.24f, -0.06f, -0.24f);
            p.Line(-0.06f, -0.24f, 0.02f, -0.12f);
            p.Line(0.02f, -0.12f, 0.34f, -0.12f);
            p.Line(-0.38f, -0.24f, -0.38f, 0.30f);
            p.Line(-0.38f, 0.30f, 0.38f, 0.30f);
            p.Line(0.38f, 0.30f, 0.44f, -0.02f);
            p.Line(0.44f, -0.02f, -0.30f, -0.02f);
            p.Line(-0.30f, -0.02f, -0.38f, 0.30f);
            break;

        case Icon::Save: // дискета
            p.Rect(-0.34f, -0.34f, 0.34f, 0.34f, 0.06f);
            p.Rect(-0.18f, -0.34f, 0.18f, -0.08f);
            p.Rect(-0.24f, 0.06f, 0.24f, 0.34f);
            break;

        case Icon::Undo:
            p.Arc(0.0f, 0.04f, 0.30f, 200.0f, 340.0f);
            p.Line(-0.30f, 0.02f, -0.30f, -0.20f);
            p.Line(-0.30f, 0.02f, -0.10f, 0.02f);
            break;
        case Icon::Redo:
            p.Arc(0.0f, 0.04f, 0.30f, 200.0f, 340.0f);
            p.Line(0.30f, 0.02f, 0.30f, -0.20f);
            p.Line(0.30f, 0.02f, 0.10f, 0.02f);
            break;

        case Icon::Select: // курсор-стрелка
            p.Line(-0.20f, -0.34f, -0.20f, 0.28f);
            p.Line(-0.20f, -0.34f, 0.24f, 0.10f);
            p.Line(0.24f, 0.10f, 0.04f, 0.12f);
            p.Line(0.04f, 0.12f, -0.20f, 0.28f);
            p.Line(0.04f, 0.12f, 0.16f, 0.36f);
            break;

        case Icon::Move: // четыре стрелки
            p.Line(-0.34f, 0.0f, 0.34f, 0.0f);
            p.Line(0.0f, -0.34f, 0.0f, 0.34f);
            p.Tri(-0.34f, 0.0f, -0.20f, -0.09f, -0.20f, 0.09f);
            p.Tri(0.34f, 0.0f, 0.20f, -0.09f, 0.20f, 0.09f);
            p.Tri(0.0f, -0.34f, -0.09f, -0.20f, 0.09f, -0.20f);
            p.Tri(0.0f, 0.34f, -0.09f, 0.20f, 0.09f, 0.20f);
            break;

        case Icon::Rotate:
            p.Arc(0.0f, 0.0f, 0.30f, 40.0f, 320.0f);
            p.Tri(0.22f, -0.20f, 0.36f, -0.10f, 0.20f, -0.02f);
            break;

        case Icon::Scale:
            p.Rect(-0.34f, 0.04f, -0.04f, 0.34f);
            p.Rect(0.10f, -0.34f, 0.34f, -0.10f);
            p.Line(-0.04f, 0.04f, 0.10f, -0.10f);
            break;

        case Icon::Camera:
            p.Rect(-0.36f, -0.16f, 0.14f, 0.26f, 0.05f);
            p.Tri(0.14f, 0.02f, 0.36f, -0.16f, 0.36f, 0.22f);
            p.CircleFilled(-0.11f, 0.05f, 0.09f, 12);
            p.Line(-0.24f, -0.16f, -0.16f, -0.28f);
            p.Line(-0.16f, -0.28f, 0.00f, -0.28f);
            break;

        case Icon::Light: // лампочка
            p.Circle(0.0f, -0.10f, 0.24f, 20);
            p.Line(-0.10f, 0.16f, 0.10f, 0.16f);
            p.Line(-0.08f, 0.28f, 0.08f, 0.28f);
            p.Line(-0.10f, 0.14f, -0.10f, 0.22f);
            p.Line(0.10f, 0.14f, 0.10f, 0.22f);
            break;

        case Icon::Cube: // изометрический куб
            p.Line(0.0f, -0.38f, 0.34f, -0.18f);
            p.Line(0.34f, -0.18f, 0.34f, 0.20f);
            p.Line(0.34f, 0.20f, 0.0f, 0.40f);
            p.Line(0.0f, 0.40f, -0.34f, 0.20f);
            p.Line(-0.34f, 0.20f, -0.34f, -0.18f);
            p.Line(-0.34f, -0.18f, 0.0f, -0.38f);
            p.Line(0.0f, -0.38f, 0.0f, 0.02f);
            p.Line(0.0f, 0.02f, 0.34f, -0.18f);
            p.Line(0.0f, 0.02f, -0.34f, -0.18f);
            break;

        case Icon::Character: // фигурка человека
            p.Circle(0.0f, -0.26f, 0.13f, 14);
            p.Line(0.0f, -0.12f, 0.0f, 0.12f);
            p.Line(-0.22f, -0.02f, 0.22f, -0.02f);
            p.Line(0.0f, 0.12f, -0.16f, 0.38f);
            p.Line(0.0f, 0.12f, 0.16f, 0.38f);
            break;

        case Icon::Effect: // искры
            p.Line(0.0f, -0.36f, 0.0f, -0.12f);
            p.Line(0.0f, 0.12f, 0.0f, 0.36f);
            p.Line(-0.36f, 0.0f, -0.12f, 0.0f);
            p.Line(0.12f, 0.0f, 0.36f, 0.0f);
            p.Line(-0.26f, -0.26f, -0.10f, -0.10f);
            p.Line(0.10f, 0.10f, 0.26f, 0.26f);
            p.Line(0.26f, -0.26f, 0.10f, -0.10f);
            p.Line(-0.10f, 0.10f, -0.26f, 0.26f);
            p.CircleFilled(0.0f, 0.0f, 0.07f, 10);
            break;

        case Icon::Audio: // динамик со звуковыми волнами
            p.RectFilled(-0.34f, -0.12f, -0.16f, 0.12f);
            p.Tri(-0.16f, -0.12f, -0.16f, 0.12f, 0.04f, 0.30f);
            p.Tri(-0.16f, -0.12f, 0.04f, 0.30f, 0.04f, -0.30f);
            p.Arc(0.02f, 0.0f, 0.18f, -60.0f, 60.0f);
            p.Arc(0.02f, 0.0f, 0.30f, -60.0f, 60.0f);
            break;

        case Icon::Folder:
            p.Line(-0.36f, -0.22f, -0.06f, -0.22f);
            p.Line(-0.06f, -0.22f, 0.02f, -0.10f);
            p.Line(0.02f, -0.10f, 0.36f, -0.10f);
            p.Rect(-0.36f, -0.10f, 0.36f, 0.28f, 0.04f);
            break;

        case Icon::Model: // сетка на кубе
            p.Rect(-0.32f, -0.32f, 0.32f, 0.32f, 0.04f);
            p.Line(-0.32f, 0.0f, 0.32f, 0.0f);
            p.Line(0.0f, -0.32f, 0.0f, 0.32f);
            p.Line(-0.32f, -0.32f, 0.32f, 0.32f);
            break;

        case Icon::Material: // шар с бликом
            p.Circle(0.0f, 0.0f, 0.32f, 24);
            p.CircleFilled(-0.11f, -0.12f, 0.07f, 10);
            break;

        case Icon::Texture: // картинка с горами
            p.Rect(-0.34f, -0.28f, 0.34f, 0.28f, 0.04f);
            p.Tri(-0.24f, 0.20f, 0.02f, -0.10f, 0.28f, 0.20f);
            p.CircleFilled(0.16f, -0.14f, 0.07f, 10);
            break;

        case Icon::Play:
            p.Tri(-0.20f, -0.32f, -0.20f, 0.32f, 0.32f, 0.0f);
            break;
        case Icon::Pause:
            p.RectFilled(-0.24f, -0.30f, -0.06f, 0.30f);
            p.RectFilled(0.06f, -0.30f, 0.24f, 0.30f);
            break;
        case Icon::Stop:
            p.RectFilled(-0.26f, -0.26f, 0.26f, 0.26f, 0.04f);
            break;
        case Icon::Record:
            p.CircleFilled(0.0f, 0.0f, 0.28f, 20);
            break;

        case Icon::Render: // кадр с «пикселями» рендера
            p.Rect(-0.36f, -0.28f, 0.36f, 0.28f, 0.04f);
            p.RectFilled(-0.28f, -0.18f, -0.06f, 0.02f);
            p.RectFilled(-0.02f, -0.06f, 0.18f, 0.18f);
            p.Line(0.22f, -0.18f, 0.28f, -0.18f);
            break;

        case Icon::SkipStart:
            p.RectFilled(-0.28f, -0.28f, -0.20f, 0.28f);
            p.Tri(0.26f, -0.28f, 0.26f, 0.28f, -0.16f, 0.0f);
            break;
        case Icon::SkipEnd:
            p.RectFilled(0.20f, -0.28f, 0.28f, 0.28f);
            p.Tri(-0.26f, -0.28f, -0.26f, 0.28f, 0.16f, 0.0f);
            break;
        case Icon::StepBack:
            p.Tri(0.26f, -0.26f, 0.26f, 0.26f, -0.06f, 0.0f);
            p.Tri(-0.02f, -0.26f, -0.02f, 0.26f, -0.30f, 0.0f);
            break;
        case Icon::StepForward:
            p.Tri(-0.26f, -0.26f, -0.26f, 0.26f, 0.06f, 0.0f);
            p.Tri(0.02f, -0.26f, 0.02f, 0.26f, 0.30f, 0.0f);
            break;
        case Icon::PrevKey:
            p.RectFilled(-0.30f, -0.26f, -0.22f, 0.26f);
            p.Quad(0.02f, -0.24f, 0.26f, 0.0f, 0.02f, 0.24f, -0.22f, 0.0f);
            break;
        case Icon::NextKey:
            p.RectFilled(0.22f, -0.26f, 0.30f, 0.26f);
            p.Quad(-0.02f, -0.24f, 0.22f, 0.0f, -0.02f, 0.24f, -0.26f, 0.0f);
            break;

        case Icon::Key: // ромб ключа
            p.Quad(0.0f, -0.30f, 0.30f, 0.0f, 0.0f, 0.30f, -0.30f, 0.0f);
            break;

        case Icon::Loop:
            p.Arc(0.0f, 0.0f, 0.28f, 150.0f, 390.0f);
            p.Tri(0.10f, -0.34f, 0.30f, -0.22f, 0.10f, -0.10f);
            break;

        case Icon::Eye:
            p.Arc(0.0f, 0.16f, 0.36f, 210.0f, 330.0f);
            p.Arc(0.0f, -0.16f, 0.36f, 30.0f, 150.0f);
            p.Circle(0.0f, 0.0f, 0.13f, 12);
            break;
        case Icon::EyeOff:
            p.Arc(0.0f, 0.16f, 0.36f, 210.0f, 330.0f);
            p.Arc(0.0f, -0.16f, 0.36f, 30.0f, 150.0f);
            p.Line(-0.32f, -0.32f, 0.32f, 0.32f);
            break;

        case Icon::Lock:
            p.Rect(-0.24f, -0.02f, 0.24f, 0.32f, 0.05f);
            p.Arc(0.0f, -0.02f, 0.16f, 180.0f, 360.0f);
            break;
        case Icon::Unlock:
            p.Rect(-0.24f, -0.02f, 0.24f, 0.32f, 0.05f);
            p.Arc(-0.12f, -0.02f, 0.16f, 180.0f, 300.0f);
            break;

        case Icon::Mute:
            p.RectFilled(-0.32f, -0.12f, -0.14f, 0.12f);
            p.Tri(-0.14f, -0.12f, -0.14f, 0.12f, 0.06f, 0.30f);
            p.Tri(-0.14f, -0.12f, 0.06f, 0.30f, 0.06f, -0.30f);
            p.Line(0.16f, -0.16f, 0.36f, 0.16f);
            p.Line(0.36f, -0.16f, 0.16f, 0.16f);
            break;

        case Icon::Search:
            p.Circle(-0.06f, -0.06f, 0.24f, 20);
            p.Line(0.12f, 0.12f, 0.32f, 0.32f);
            break;

        case Icon::Add:
            p.Line(0.0f, -0.28f, 0.0f, 0.28f);
            p.Line(-0.28f, 0.0f, 0.28f, 0.0f);
            break;

        case Icon::Trash:
            p.Line(-0.28f, -0.20f, 0.28f, -0.20f);
            p.Line(-0.12f, -0.20f, -0.12f, -0.30f);
            p.Line(-0.12f, -0.30f, 0.12f, -0.30f);
            p.Line(0.12f, -0.30f, 0.12f, -0.20f);
            p.Line(-0.20f, -0.20f, -0.16f, 0.32f);
            p.Line(0.20f, -0.20f, 0.16f, 0.32f);
            p.Line(-0.16f, 0.32f, 0.16f, 0.32f);
            break;

        case Icon::Curve: // S-образная кривая с ключами
            p.Bezier(-0.34f, 0.26f, -0.10f, 0.26f, 0.10f, -0.26f, 0.34f, -0.26f);
            p.Quad(-0.34f, 0.16f, -0.24f, 0.26f, -0.34f, 0.36f, -0.44f, 0.26f);
            p.Quad(0.34f, -0.36f, 0.44f, -0.26f, 0.34f, -0.16f, 0.24f, -0.26f);
            break;

        case Icon::Grid:
            p.Rect(-0.32f, -0.32f, 0.32f, 0.32f);
            p.Line(-0.32f, -0.11f, 0.32f, -0.11f);
            p.Line(-0.32f, 0.11f, 0.32f, 0.11f);
            p.Line(-0.11f, -0.32f, -0.11f, 0.32f);
            p.Line(0.11f, -0.32f, 0.11f, 0.32f);
            break;
    }
}

} // namespace

namespace {

// Как посадить чернила иконки в её рамку: сдвиг к центру и поправка размера.
struct InkFit {
    ImVec2 Offset{0.0f, 0.0f};
    float Scale = 1.0f;
};

// Доля рамки, которую должны занимать чернила. Одинаковая для всех иконок —
// именно постоянство оптического размера и читается как «ряд ровный». Иконки
// нарисованы от руки в разное время, и без нормировки стрелки отмены выходили
// заметно мельче листа документа рядом.
constexpr float kTargetExtent = 0.80f;

// Считается один раз на иконку: геометрия статична, а гонять замер каждый кадр
// на каждую кнопку — лишняя работа на ровном месте.
InkFit FitOf(Icon icon) {
    static std::unordered_map<int, InkFit> cache;
    const int key = (int)icon;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    Pen probe;             // Dl == nullptr — режим замера
    DrawIcon(probe, icon);

    InkFit fit;
    if (probe.MaxX >= probe.MinX) {
        fit.Offset = ImVec2(-(probe.MinX + probe.MaxX) * 0.5f, -(probe.MinY + probe.MaxY) * 0.5f);
        const float extent = ImMax(probe.MaxX - probe.MinX, probe.MaxY - probe.MinY);
        // Масштаб общий по обеим осям: раздельный исказил бы пропорции, и
        // круглая иконка стала бы овальной.
        if (extent > 0.01f) fit.Scale = ImClamp(kTargetExtent / extent, 0.75f, 1.6f);
    }
    cache[key] = fit;
    return fit;
}

} // namespace

void Draw(ImDrawList* dl, Icon icon, ImVec2 center, float size, ImU32 color) {
    const InkFit fit = FitOf(icon);
    Pen pen;
    pen.Dl = dl;
    pen.C = center;
    // Масштаб иконки входит в S, а сдвиг задан в её собственных координатах —
    // поэтому сдвигаем ДО масштабирования, и порядок здесь важен.
    pen.S = size * fit.Scale;
    pen.Color = color;
    pen.Thick = ImMax(1.0f, size * 0.075f); // толщина линии от размера КНОПКИ, а не от масштаба
    pen.OffX = fit.Offset.x;
    pen.OffY = fit.Offset.y;
    DrawIcon(pen, icon);
}

// ============================================================================
//  Виджеты
// ============================================================================

bool ToolbarButton(Icon icon, const char* label, const char* tooltip,
                   bool active, bool enabled, float width) {
    ImGui::PushID(label);

    // Подпись рисуется мелким шрифтом, поэтому и мерить её надо им же: с
    // основным шрифтом ширина вышла бы завышенной, и кнопки разъехались.
    if (ImFont* small = Theme::SmallFont()) ImGui::PushFont(small);
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    if (Theme::SmallFont()) ImGui::PopFont();
    if (width <= 0.0f) width = std::max(56.0f, textSize.x + 10.0f);

    const ImVec2 size(width, Theme::kToolbarHeight - 8.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    // Своя отрисовка вместо ImGui::Button: нужен вертикальный блок «иконка +
    // подпись», а штатная кнопка кладёт только текст в одну строку.
    const bool clicked = ImGui::InvisibleButton("##btn", size) && enabled;
    const bool hovered = ImGui::IsItemHovered() && enabled;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active || hovered) {
        const ImU32 bg = active ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
                                : ImGui::GetColorU32(ImGuiCol_ButtonHovered);
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg, 4.0f);
    }

    const ImU32 fg = !enabled ? Theme::Colors::TextFaint
                   : active   ? IM_COL32(255, 255, 255, 255)
                              : Theme::Colors::Text;
    Draw(dl, icon, ImVec2(pos.x + size.x * 0.5f, pos.y + 15.0f), 22.0f, fg);

    if (ImFont* small = Theme::SmallFont()) ImGui::PushFont(small);
    dl->AddText(ImVec2(pos.x + (size.x - textSize.x) * 0.5f, pos.y + size.y - textSize.y - 3.0f),
                fg, label);
    if (Theme::SmallFont()) ImGui::PopFont();

    if (hovered && tooltip) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return clicked;
}

bool IconButton(const char* id, Icon icon, const char* tooltip, bool active, bool enabled, float size) {
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##ib", ImVec2(size, size)) && enabled;
    const bool hovered = ImGui::IsItemHovered() && enabled;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active || hovered) {
        const ImU32 bg = active ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
                                : ImGui::GetColorU32(ImGuiCol_ButtonHovered);
        dl->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), bg, 3.0f);
    }
    const ImU32 fg = !enabled ? Theme::Colors::TextFaint
                   : active   ? IM_COL32(255, 255, 255, 255)
                              : Theme::Colors::Text;
    Draw(dl, icon, ImVec2(pos.x + size * 0.5f, pos.y + size * 0.5f), size * 0.72f, fg);

    if (hovered && tooltip) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return clicked;
}

bool ToggleIcon(const char* id, Icon onIcon, Icon offIcon, bool& value,
                const char* tooltip, float size) {
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##tg", ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    if (clicked) value = !value;

    // Выключенное состояние приглушено, но НЕ невидимо: скрытый объект должен
    // оставаться заметен в списке, иначе его невозможно вернуть.
    const ImU32 fg = value ? (hovered ? Theme::Colors::Text : Theme::Colors::TextDim)
                           : (hovered ? Theme::Colors::TextDim : Theme::Colors::TextFaint);
    Draw(ImGui::GetWindowDrawList(), value ? onIcon : offIcon,
         ImVec2(pos.x + size * 0.5f, pos.y + size * 0.5f), size, fg);

    if (hovered && tooltip) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return clicked;
}

void ToolbarSeparator() {
    ImGui::SameLine(0.0f, 10.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float h = Theme::kToolbarHeight - 18.0f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x, pos.y + 5.0f), ImVec2(pos.x, pos.y + 5.0f + h),
                                        Theme::Colors::Line, 1.0f);
    ImGui::Dummy(ImVec2(1.0f, h));
    ImGui::SameLine(0.0f, 10.0f);
}

} // namespace d3d::Icons
