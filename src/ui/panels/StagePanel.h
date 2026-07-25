#pragma once
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "imgui.h"

namespace d3d {

class DirectorHost;

// Центральная область: вкладки «Viewport» (рабочий вид с гизмо и сеткой) и
// «Render View» (чистовой кадр активной камеры). Здесь же — управление
// свободной камерой, манипулятор ImGuizmo, выбор объекта кликом и
// композиционные направляющие поверх картинки.
class StagePanel {
public:
    void DrawViewport(DirectorHost& host);
    void DrawRenderView(DirectorHost& host);

    // Запросить фокус окна вьюпорта на ближайших кадрах (после старта/загрузки).
    void RequestFocus() { m_focusFrames = 2; }

private:
    void DrawViewToolbar(DirectorHost& host);
    void HandleCamera(DirectorHost& host, bool hovered);
    void DrawGizmo(DirectorHost& host, ImVec2 imagePos, ImVec2 imageSize);
    // Гизмо на выбранной кости. Отдельно от объектного: у кости другая цепочка
    // (мир -> модель -> родительская кость) и другой приёмник результата — не
    // Transform, а переопределение позы.
    void DrawBoneGizmo(DirectorHost& host, ImVec2 imagePos, ImVec2 imageSize);
    // Скелет персонажа поверх картинки: линии костей, точки суставов и выбор
    // кости кликом. Возвращает true, если клик забрала кость, — тогда обычный
    // выбор объекта не срабатывает (иначе клик по кости сбрасывал бы её).
    bool DrawSkeletonOverlay(DirectorHost& host, ImVec2 imagePos, ImVec2 imageSize, bool hovered);
    // Рамка кадра активной камеры, безопасная зона и трети — поверх картинки.
    void DrawFramingGuides(DirectorHost& host, ImVec2 imagePos, ImVec2 imageSize);

    int m_focusFrames = 0;
    bool m_cameraDriving = false;
    bool m_gizmoWasUsing = false;
    bool m_boneGizmoWasUsing = false;
    glm::mat4 m_dragStartPrimary{1.0f};
    std::vector<std::pair<int, glm::mat4>> m_dragStartWorlds;
};

} // namespace d3d
