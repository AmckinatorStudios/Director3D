#include "ui/panels/TimelinePanel.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "imgui.h"
#include "imgui_internal.h" // ImRect для попадания мышью в произвольные области
#include "imgui_stdlib.h"

#include "anim/BonePose.h"
#include "anim/DirectorComponents.h"
#include "sage/render/SkinnedModel.h"
#include "sage/render/SkinnedModel.h"
#include "sage/scene/Components.h"
#include "ui/DirectorHost.h"
#include "ui/Icons.h"
#include "ui/Localization.h"
#include "ui/Theme.h"

namespace d3d {

using Icons::Icon;

namespace {

// Ширина колонки с именами дорожек. Совпадает у линейки, дорожек и графа —
// именно она делает так, что имя, ключи и кривая одной дорожки лежат на одной
// строке.
// Колонка имён. 210 не хватало: под подпись оставалось ~70 пикселей (46 —
// отступ под треугольник и значок, 78 — кнопки ключа/звука/замка справа), и
// «bone2 · Bone Rotation» обрезалось до «bone2 · Bor». Костные и морф-дорожки
// несут в подписи ещё и имя кости или цели, поэтому места нужно больше.
constexpr float kNameColumn = 300.0f;
// Ширина блока кнопок в конце колонки имён — она же граница обрезки подписи.
constexpr float kTrackButtonsWidth = 78.0f;
constexpr float kRulerHeight = 22.0f;
// Высота строки КЛЮЧЕЙ: здесь важна плотность. У персонажа таких строк
// десятки, и лишние пиксели на каждой означают, что половина не влезет.
constexpr float kRowHeight = 22.0f;
// Высота строки МОНТАЖА (склейки камер, блоки клипов). Она заметно больше не
// ради красоты: в этой строке лежит ТЕКСТ — имя камеры, имя клипа, — и в 22
// пикселя он помещается только обрезанным. Плюс блоки монтажа таскают мышью,
// а 22-пиксельную мишень с двумя краями по 4 пикселя поймать тяжело.
constexpr float kMontageRow = 38.0f;
// Звуковая дорожка выше остальных: в ней рисуется фонограмма, и на низкой
// строке волна вырождается в полоску, по которой не найти ни удара, ни паузы.
constexpr float kAudioRow = 56.0f;
// Полоса-заголовок зоны.
constexpr float kZoneHeader = 20.0f;

// Шаг сетки линейки: подбираем «круглый» интервал так, чтобы подписи не
// налезали друг на друга при любом зуме.
float NiceStep(float visibleSeconds, float pixelWidth) {
    const float targetPx = 90.0f; // желаемое расстояние между подписями
    const float raw = visibleSeconds * targetPx / std::max(pixelWidth, 1.0f);
    const float candidates[] = {0.04f, 0.1f, 0.2f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f,
                                15.0f, 30.0f, 60.0f, 120.0f, 300.0f, 600.0f};
    for (float c : candidates) {
        if (c >= raw) return c;
    }
    return candidates[sizeof(candidates) / sizeof(candidates[0]) - 1];
}

// Короткая подпись времени для линейки: минуты:секунды или секунды с долями.
void FormatTick(char* out, size_t size, float seconds, float step) {
    if (step >= 1.0f) {
        // Знак выносим отдельно: у отрицательного времени деление на минуты и
        // секунды дало бы «00:-9» вместо «-00:09».
        const int total = (int)std::lround(std::fabs(seconds));
        std::snprintf(out, size, "%s%02d:%02d", seconds < -0.5f ? "-" : "", total / 60, total % 60);
    } else {
        std::snprintf(out, size, "%.2f", (double)seconds);
    }
}

// Ромб ключа. Заполненный — обычный, с каймой — выбранный.
void DrawDiamond(ImDrawList* dl, ImVec2 center, float radius, ImU32 fill, bool selected) {
    const ImVec2 top(center.x, center.y - radius), right(center.x + radius, center.y);
    const ImVec2 bottom(center.x, center.y + radius), left(center.x - radius, center.y);
    dl->AddQuadFilled(top, right, bottom, left, fill);
    if (selected) dl->AddQuad(top, right, bottom, left, IM_COL32(255, 255, 255, 230), 1.6f);
}

// Имя объекта для колонки дорожек (объект мог быть удалён — тогда так и пишем).
// Имя морф-цели сущности по номеру (пусто, если модель ещё не загрузилась).
std::string MorphName(Scene& scene, int entityId, int index) {
    GameObject obj = scene.Get(entityId);
    if (!obj.Valid()) return {};
    const AnimatedModelComponent* am =
        scene.Registry().try_get<AnimatedModelComponent>(obj.Entity());
    if (!am || !am->Model || index < 0 || index >= am->Model->MorphCount()) return {};
    return am->Model->MorphNames()[(size_t)index];
}

std::string TargetName(Scene& scene, int id) {
    GameObject obj = scene.Get(id);
    return obj.Valid() ? obj.Name() : std::string(T("<удалён>"));
}

} // namespace

// ============================================================================
//  Преобразования времени
// ============================================================================

float TimelinePanel::TimeToX(const Layout& l, float time) const {
    const float span = std::max(m_viewEnd - m_viewStart, 1e-4f);
    return l.Origin.x + l.TrackX + (time - m_viewStart) / span * l.Width;
}

float TimelinePanel::XToTime(const Layout& l, float x) const {
    const float span = std::max(m_viewEnd - m_viewStart, 1e-4f);
    return m_viewStart + (x - l.Origin.x - l.TrackX) / std::max(l.Width, 1.0f) * span;
}

// ============================================================================
//  Транспорт (под вьюпортом, как в референсе)
// ============================================================================

void TimelinePanel::DrawTransport(DirectorHost& host) {
    Playback& transport = host.Transport();
    AnimationDocument& doc = host.Document();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));

    // Ряд делится на три блока с АБСОЛЮТНЫМИ позициями. Раньше центр и правый
    // край выставлялись через SameLine(0, отступ), то есть отсчитывались от
    // конца предыдущего элемента: стоило панели стать уже — и таймкод с частотой
    // кадров уезжали за край, оставляя от транспорта одну лупу. Абсолютная
    // раскладка от ширины строки этого не допускает.
    const float rowX = ImGui::GetCursorPosX();
    const float rowWidth = ImGui::GetContentRegionAvail().x;

    constexpr float kLeftWidth = 210.0f;   // таймкод + частота кадров
    constexpr float kCenterWidth = 226.0f; // семь кнопок + цикл
    constexpr float kRightWidth = 30.0f;   // «весь ролик»

    // --- Слева: таймкод и частота кадров ---
    // Таймкод — редактируемый: набрать «00:00:04:12» точнее, чем целиться мышью.
    char timecode[32];
    std::snprintf(timecode, sizeof(timecode), "%s",
                  Playback::Timecode(host.CurrentTime(), doc.Fps).c_str());
    ImGui::SetNextItemWidth(112.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Playhead);
    if (ImGui::InputText("##timecode", timecode, sizeof(timecode),
                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
        float parsed = 0.0f;
        if (Playback::ParseTimecode(timecode, doc.Fps, parsed)) host.SetCurrentTime(parsed);
        else host.SetStatus(T("Не разобрал таймкод — ожидается ЧЧ:ММ:СС:КК или номер кадра"));
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(T("Таймкод ЧЧ:ММ:СС:КК — можно ввести вручную\nКадр: %d"),
                          transport.FrameAt(doc.Fps));
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(86.0f);
    static const float kFpsChoices[] = {12.0f, 15.0f, 24.0f, 25.0f, 30.0f, 48.0f, 50.0f, 60.0f};
    // %g у произвольного float может дать длинную запись (частоту правят и
    // вручную в настройках таймлайна) — буфер с запасом.
    char fpsLabel[32];
    std::snprintf(fpsLabel, sizeof(fpsLabel), "%g fps", (double)doc.Fps);
    if (ImGui::BeginCombo("##fps", fpsLabel)) {
        for (float fps : kFpsChoices) {
            char item[32];
            std::snprintf(item, sizeof(item), "%g fps", (double)fps);
            if (ImGui::Selectable(item, doc.Fps == fps)) {
                host.PushUndo();
                // Время хранится в секундах, поэтому смена частоты НЕ двигает
                // ключи — меняется только сетка, к которой прилипает головка.
                doc.Fps = fps;
                host.SetCurrentTime(host.CurrentTime());
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Частота кадров ролика.\nКлючи не сдвигаются: время хранится в секундах."));
    }

    // --- По центру: перемотка и проигрывание ---
    // Центр не заезжает на левый блок даже на узкой панели: лучше сдвинутые
    // вправо кнопки, чем кнопки поверх таймкода.
    const float centerX = ImMax(rowX + kLeftWidth + 8.0f,
                                rowX + (rowWidth - kCenterWidth) * 0.5f);
    ImGui::SameLine(centerX);

    if (Icons::IconButton("first", Icon::SkipStart, T("В начало (Home)"), false, true, 26.0f)) {
        host.SetCurrentTime(0.0f);
    }
    ImGui::SameLine();
    if (Icons::IconButton("prevKey", Icon::PrevKey, T("Предыдущий ключ (,)"), false, true, 26.0f)) {
        float t = 0.0f;
        if (doc.PrevKeyTime(host.CurrentTime(), t)) host.SetCurrentTime(t);
        else host.SetCurrentTime(0.0f);
    }
    ImGui::SameLine();
    if (Icons::IconButton("stepBack", Icon::StepBack, T("Кадр назад (←)"), false, true, 26.0f)) {
        host.StepFrames(-1);
    }
    ImGui::SameLine();
    if (Icons::IconButton("play", transport.Playing() ? Icon::Pause : Icon::Play,
                          T("Проигрывание (Пробел)"), transport.Playing(), true, 30.0f)) {
        transport.TogglePlay();
    }
    ImGui::SameLine();
    if (Icons::IconButton("stepFwd", Icon::StepForward, T("Кадр вперёд (→)"), false, true, 26.0f)) {
        host.StepFrames(1);
    }
    ImGui::SameLine();
    if (Icons::IconButton("nextKey", Icon::NextKey, T("Следующий ключ (.)"), false, true, 26.0f)) {
        float t = 0.0f;
        if (doc.NextKeyTime(host.CurrentTime(), t)) host.SetCurrentTime(t);
        else host.SetCurrentTime(doc.Duration);
    }
    ImGui::SameLine();
    if (Icons::IconButton("last", Icon::SkipEnd, T("В конец (End)"), false, true, 26.0f)) {
        host.SetCurrentTime(doc.Duration);
    }
    ImGui::SameLine(0.0f, 10.0f);
    if (Icons::IconButton("loop", Icon::Loop, T("Зациклить проигрывание (L)"), transport.Loop, true,
                          26.0f)) {
        transport.Loop = !transport.Loop;
    }

    // --- Справа: показать весь ролик ---
    if (rowWidth > kLeftWidth + kCenterWidth + kRightWidth + 24.0f) {
        ImGui::SameLine(rowX + rowWidth - kRightWidth);
        if (Icons::IconButton("zoomAll", Icon::Search, T("Показать весь ролик"), false, true, 26.0f)) {
            m_viewStart = 0.0f;
            m_viewEnd = doc.Duration;
        }
    }

    ImGui::PopStyleVar();
}

// ============================================================================
//  Линейка и головка
// ============================================================================

void TimelinePanel::DrawRuler(DirectorHost& host, const Layout& l, ImDrawList* dl) {
    const AnimationDocument& doc = host.Document();
    const ImVec2 a(l.Origin.x + l.TrackX, l.Origin.y);
    const ImVec2 b(a.x + l.Width, a.y + l.RulerH);
    dl->AddRectFilled(ImVec2(l.Origin.x, a.y), b, Theme::Colors::PanelRaised);

    // Область за пределами ролика затемнена: сразу видно, где он кончается.
    const float endX = TimeToX(l, doc.Duration);
    if (endX < b.x) dl->AddRectFilled(ImVec2(endX, a.y), ImVec2(b.x, l.Origin.y + l.Height),
                                      IM_COL32(0, 0, 0, 70));
    const float startX = TimeToX(l, 0.0f);
    if (startX > a.x) dl->AddRectFilled(a, ImVec2(startX, l.Origin.y + l.Height), IM_COL32(0, 0, 0, 70));

    const float step = NiceStep(m_viewEnd - m_viewStart, l.Width);
    const float first = std::floor(m_viewStart / step) * step;

    if (ImFont* small = Theme::SmallFont()) ImGui::PushFont(small);
    for (float t = first; t <= m_viewEnd + step; t += step) {
        const float x = TimeToX(l, t);
        if (x < a.x - 1.0f || x > b.x + 1.0f) continue;
        dl->AddLine(ImVec2(x, a.y + 4.0f), ImVec2(x, b.y), Theme::Colors::TextFaint, 1.0f);
        // Вертикальная линия сетки через все дорожки — по ней глаз выравнивает
        // ключи разных объектов друг относительно друга.
        dl->AddLine(ImVec2(x, b.y), ImVec2(x, l.Origin.y + l.Height), Theme::Colors::LineSoft, 1.0f);
        char label[24];
        FormatTick(label, sizeof(label), t, step);
        dl->AddText(ImVec2(x + 3.0f, a.y + 3.0f), Theme::Colors::TextDim, label);
    }
    if (Theme::SmallFont()) ImGui::PopFont();

    // Метки поверх линейки.
    for (const Marker& marker : doc.Markers) {
        const float x = TimeToX(l, marker.Time);
        if (x < a.x || x > b.x) continue;
        dl->AddTriangleFilled(ImVec2(x - 5.0f, a.y + 2.0f), ImVec2(x + 5.0f, a.y + 2.0f),
                              ImVec2(x, a.y + 11.0f), marker.Color);
        dl->AddLine(ImVec2(x, a.y + 11.0f), ImVec2(x, l.Origin.y + l.Height),
                    (marker.Color & 0x00FFFFFF) | 0x50000000, 1.0f);
    }

    dl->AddLine(ImVec2(l.Origin.x, b.y), ImVec2(b.x, b.y), Theme::Colors::Line, 1.0f);
}

void TimelinePanel::DrawPlayhead(DirectorHost& host, const Layout& l, ImDrawList* dl) {
    const float x = TimeToX(l, host.CurrentTime());
    if (x < l.Origin.x + l.TrackX - 1.0f || x > l.Origin.x + l.TrackX + l.Width + 1.0f) return;

    dl->AddLine(ImVec2(x, l.Origin.y + l.RulerH), ImVec2(x, l.Origin.y + l.Height),
                Theme::Colors::Playhead, 1.6f);

    // Флажок с номером кадра: при перемотке взгляд держится на нём, а не бегает
    // к таймкоду в другом углу экрана.
    char label[16];
    std::snprintf(label, sizeof(label), "%d", host.Transport().FrameAt(host.Document().Fps));
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const float w = std::max(textSize.x + 10.0f, 26.0f);
    const ImVec2 a(x - w * 0.5f, l.Origin.y + 2.0f);
    const ImVec2 b(a.x + w, l.Origin.y + l.RulerH - 1.0f);
    dl->AddRectFilled(a, b, Theme::Colors::Playhead, 3.0f);
    dl->AddText(ImVec2(a.x + (w - textSize.x) * 0.5f, a.y + 1.0f), IM_COL32(25, 25, 25, 255), label);
}

void TimelinePanel::HandleZoomPan(DirectorHost& host, const Layout& l, bool hovered) {
    ImGuiIO& io = ImGui::GetIO();
    const AnimationDocument& doc = host.Document();

    if (hovered && io.MouseWheel != 0.0f) {
        if (io.KeyShift) {
            // Shift+колесо — прокрутка по времени без изменения масштаба.
            const float shift = (m_viewEnd - m_viewStart) * 0.12f * -io.MouseWheel;
            m_viewStart += shift;
            m_viewEnd += shift;
        } else {
            // Зум вокруг курсора: точка под мышью остаётся на месте — иначе
            // после каждого приближения приходится искать, куда всё уехало.
            const float anchor = XToTime(l, io.MousePos.x);
            const float factor = io.MouseWheel > 0.0f ? 0.85f : 1.0f / 0.85f;
            m_viewStart = anchor + (m_viewStart - anchor) * factor;
            m_viewEnd = anchor + (m_viewEnd - anchor) * factor;
        }
    }

    // Панорама средней кнопкой.
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        const float span = m_viewEnd - m_viewStart;
        const float shift = -io.MouseDelta.x / std::max(l.Width, 1.0f) * span;
        m_viewStart += shift;
        m_viewEnd += shift;
        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
    }

    // Ограничители видимого участка. Порядок здесь принципиален: подвинуть
    // один край, не проверив второй, — и окно уезжает целиком (так, например,
    // сокращение ролика до 6 секунд при видимом участке 0..20 утаскивало левый
    // край в минус, и линейка показывала отрицательное время).
    //
    // Допустимый диапазон — [minStart, hardEnd]: чуть левее нуля, чтобы первый
    // ключ не лип к самому краю, и в полтора раза правее конца ролика, чтобы
    // было куда двигать ключи за его пределы.
    const float minSpan = 4.0f / std::max(doc.Fps, 1.0f); // не мельче четырёх кадров
    const float hardEnd = std::max(doc.Duration, 1.0f) * 1.5f;
    float span = std::max(m_viewEnd - m_viewStart, minSpan);
    const float minStart = -span * 0.25f;
    const float maxSpan = hardEnd - minStart;

    if (span >= maxSpan) {
        // Просят больше, чем вообще есть — показываем весь допустимый диапазон.
        m_viewStart = minStart;
        m_viewEnd = hardEnd;
    } else {
        m_viewEnd = m_viewStart + span;
        if (m_viewEnd > hardEnd) { m_viewEnd = hardEnd; m_viewStart = m_viewEnd - span; }
        if (m_viewStart < minStart) { m_viewStart = minStart; m_viewEnd = m_viewStart + span; }
    }
}

// ============================================================================
//  Выбор ключей
// ============================================================================

bool TimelinePanel::IsKeySelected(const KeyRef& ref) const {
    return std::find(m_selectedKeys.begin(), m_selectedKeys.end(), ref) != m_selectedKeys.end();
}

void TimelinePanel::ToggleKeySelection(const KeyRef& ref, bool additive) {
    if (!additive) {
        const bool wasOnly = m_selectedKeys.size() == 1 && m_selectedKeys[0] == ref;
        m_selectedKeys.clear();
        if (wasOnly) return; // повторный клик по единственному выбранному снимает выбор
        m_selectedKeys.push_back(ref);
        return;
    }
    auto it = std::find(m_selectedKeys.begin(), m_selectedKeys.end(), ref);
    if (it != m_selectedKeys.end()) m_selectedKeys.erase(it);
    else m_selectedKeys.push_back(ref);
}

void TimelinePanel::DeleteSelectedKeys(DirectorHost& host) {
    if (m_selectedKeys.empty()) return;
    host.PushUndo();
    AnimationDocument& doc = host.Document();
    for (const KeyRef& ref : m_selectedKeys) {
        Track* track = doc.TrackById(ref.TrackId);
        if (!track || ref.Channel >= track->ChannelCount()) continue;
        Curve& curve = track->Channels[(size_t)ref.Channel];
        const int index = curve.KeyIndexAt(ref.Time);
        if (index >= 0) curve.RemoveKey(index);
    }
    host.SetStatus(T("Удалено ключей: ") + std::to_string(m_selectedKeys.size()));
    m_selectedKeys.clear();
}

void TimelinePanel::SetSelectedKeysInterp(DirectorHost& host, Interp mode) {
    if (m_selectedKeys.empty()) return;
    host.PushUndo();
    AnimationDocument& doc = host.Document();
    for (const KeyRef& ref : m_selectedKeys) {
        Track* track = doc.TrackById(ref.TrackId);
        if (!track || ref.Channel >= track->ChannelCount()) continue;
        Curve& curve = track->Channels[(size_t)ref.Channel];
        const int index = curve.KeyIndexAt(ref.Time);
        if (index < 0) continue;
        // В Безье переводим через ConvertToBezier: он подставляет уже
        // посчитанные касательные, и кривая не дёргается в момент смены режима.
        if (mode == Interp::Bezier) curve.ConvertToBezier(index);
        else curve.AtMutable(index).Mode = mode;
    }
    host.SetStatus(std::string(T("Интерполяция: ")) + InterpName(mode));
}

// ============================================================================
//  Строки дорожек
// ============================================================================

float TimelinePanel::DrawTrackHeader(DirectorHost& host, Track& track, const Layout& l, float y,
                                     ImDrawList* dl, bool selected) {
    Scene& scene = host.CurrentScene();
    const PropertyInfo& info = PropertyInfoOf(track.Prop);

    const ImVec2 rowA(l.Origin.x, y);
    const ImVec2 rowB(l.Origin.x + l.TrackX, y + l.RowH);
    if (selected) dl->AddRectFilled(rowA, ImVec2(l.Origin.x + l.TrackX + l.Width, rowB.y),
                                    IM_COL32(255, 255, 255, 10));

    // Раскрывающий треугольник — по нему дорожка показывает свои каналы.
    ImGui::SetCursorScreenPos(ImVec2(rowA.x + 22.0f, y + 2.0f));
    ImGui::PushID(track.Id);
    if (ImGui::InvisibleButton("##expand", ImVec2(l.TrackX - 90.0f, l.RowH - 3.0f))) {
        // Клик по имени выделяет объект дорожки — привычная связь между
        // таймлайном и остальным интерфейсом.
        host.SetSelectedId(track.TargetId);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        track.Expanded = !track.Expanded;
    }

    const float arrowX = rowA.x + 12.0f;
    const float arrowY = y + l.RowH * 0.5f;
    if (info.Channels > 1) {
        if (track.Expanded) {
            dl->AddTriangleFilled(ImVec2(arrowX - 4.0f, arrowY - 2.5f), ImVec2(arrowX + 4.0f, arrowY - 2.5f),
                                  ImVec2(arrowX, arrowY + 3.5f), Theme::Colors::TextDim);
        } else {
            dl->AddTriangleFilled(ImVec2(arrowX - 2.5f, arrowY - 4.0f), ImVec2(arrowX + 3.5f, arrowY),
                                  ImVec2(arrowX - 2.5f, arrowY + 4.0f), Theme::Colors::TextDim);
        }
        // Клик по самому треугольнику тоже раскрывает — двойной клик по имени
        // не единственный путь.
        ImGui::SetCursorScreenPos(ImVec2(rowA.x + 4.0f, y + 3.0f));
        if (ImGui::InvisibleButton("##arrow", ImVec2(16.0f, l.RowH - 5.0f))) track.Expanded = !track.Expanded;
    }

    Icons::Draw(dl, Icon::Curve, ImVec2(rowA.x + 34.0f, arrowY), 12.0f,
                track.Muted ? Theme::Colors::TextFaint : info.ChannelColors[0]);

    char label[192];
    if (IsBoneProperty(track.Prop)) {
        // У персонажа таких дорожек десятки, и «Bone Rotation» без имени кости
        // не отличить одну от другой.
        const std::string bone = !track.JointName.empty()
                                     ? track.JointName
                                     : JointName(scene, track.TargetId, track.Joint);
        std::snprintf(label, sizeof(label), "%s · %s",
                      bone.empty() ? "?" : bone.c_str(), T(info.Label));
    } else if (IsMorphProperty(track.Prop)) {
        // «Blend Shape» без имени цели неотличимо от соседних: у лица их десятки.
        const std::string name = MorphName(scene, track.TargetId, track.Joint);
        std::snprintf(label, sizeof(label), "%s · %s", name.empty() ? "?" : name.c_str(),
                      T(info.Label));
    } else {
        std::snprintf(label, sizeof(label), "%s", T(info.Label));
    }
    // Подпись ОБРЕЗАЕТСЯ по свободному месту в колонке имён. Без обрезки
    // «bone2 · Bone Rotation» уезжал под кнопки справа и дальше на сами дорожки:
    // читать нельзя ни подпись, ни то, что она перекрыла. AddText с clipRect
    // режет по границе, а не по символам, поэтому обрезанное слово видно как
    // обрезанное — и понятно, что имя длиннее.
    {
        const ImVec2 textPos(rowA.x + 46.0f, y + 3.0f);
        // 78 — ширина блока кнопок (ключ/звук/замок), 8 — зазор до него.
        const ImVec4 clip(textPos.x, y, rowA.x + l.TrackX - kTrackButtonsWidth - 8.0f, y + l.RowH);
        dl->AddText(nullptr, 0.0f, textPos,
                    track.Muted ? Theme::Colors::TextFaint : Theme::Colors::Text, label, nullptr,
                    0.0f, &clip);
    }

    // Кнопки справа в колонке имён: ключ на текущем кадре, mute, lock.
    ImGui::SetCursorScreenPos(ImVec2(rowA.x + l.TrackX - kTrackButtonsWidth, y + 2.0f));
    const bool hasKey = host.Document().HasKeyAt(track, host.CurrentTime());
    if (Icons::IconButton("key", Icon::Key,
                          hasKey ? T("Убрать ключ на этом кадре") : T("Поставить ключ на этом кадре"),
                          hasKey, !track.Locked, 18.0f)) {
        host.PushUndo();
        if (hasKey) host.Document().RemoveKeysAt(track, host.CurrentTime());
        else if (IsBoneProperty(track.Prop) || IsMorphProperty(track.Prop)) {
            host.Document().KeyFromScene(scene, track.TargetId, track.Prop, host.CurrentTime(),
                                         track.Joint);
        } else {
            host.KeyProperty(track.TargetId, track.Prop);
        }
    }
    ImGui::SameLine(0.0f, 2.0f);
    bool notMuted = !track.Muted;
    if (Icons::ToggleIcon("mute", Icon::Audio, Icon::Mute, notMuted,
                          notMuted ? T("Заглушить дорожку") : T("Включить дорожку"), 16.0f)) {
        track.Muted = !notMuted;
    }
    ImGui::SameLine(0.0f, 2.0f);
    bool unlocked = !track.Locked;
    if (Icons::ToggleIcon("lock", Icon::Unlock, Icon::Lock, unlocked,
                          unlocked ? T("Заблокировать дорожку") : T("Разблокировать дорожку"), 16.0f)) {
        track.Locked = !unlocked;
    }

    // Контекстное меню дорожки.
    ImGui::SetCursorScreenPos(rowA);
    ImGui::InvisibleButton("##rowctx", ImVec2(l.TrackX, l.RowH));
    if (ImGui::BeginPopupContextItem("##trackctx")) {
        ImGui::TextDisabled("%s — %s", TargetName(scene, track.TargetId).c_str(), T(info.Label));
        ImGui::Separator();
        if (ImGui::MenuItem(T("Поставить ключ"))) {
            host.PushUndo();
            if (IsBoneProperty(track.Prop) || IsMorphProperty(track.Prop)) {
                host.Document().KeyFromScene(scene, track.TargetId, track.Prop, host.CurrentTime(),
                                             track.Joint);
            } else {
                host.KeyProperty(track.TargetId, track.Prop);
            }
        }
        if (IsBoneProperty(track.Prop) && ImGui::MenuItem(T("Выбрать эту кость"))) {
            host.SelectBone(track.TargetId, track.Joint);
        }
        if (ImGui::MenuItem(T("Очистить все ключи"))) {
            host.PushUndo();
            for (Curve& c : track.Channels) c.Clear();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T("Удалить дорожку"))) {
            host.PushUndo();
            const int id = track.Id;
            host.Document().RemoveTrack(id);
            ImGui::EndPopup();
            ImGui::PopID();
            return l.RowH; // дорожки больше нет — дальше её трогать нельзя
        }
        ImGui::EndPopup();
    }

    ImGui::PopID();
    dl->AddLine(ImVec2(rowA.x, rowB.y), ImVec2(l.Origin.x + l.TrackX + l.Width, rowB.y),
                Theme::Colors::LineSoft, 1.0f);
    return l.RowH;
}

void TimelinePanel::DrawKeysRow(DirectorHost& host, Track& track, int channel, const Layout& l,
                                float y, ImDrawList* dl) {
    if (channel >= track.ChannelCount()) return;
    Curve& curve = track.Channels[(size_t)channel];
    const PropertyInfo& info = PropertyInfoOf(track.Prop);
    const float cy = y + l.RowH * 0.5f;
    const float left = l.Origin.x + l.TrackX;
    const float right = left + l.Width;

    // Подпись канала в колонке имён (X/Y/Z или R/G/B).
    if (info.Channels > 1) {
        if (ImFont* small = Theme::SmallFont()) ImGui::PushFont(small);
        dl->AddText(ImVec2(l.Origin.x + 62.0f, y + 3.0f), info.ChannelColors[channel],
                    info.ChannelLabels[channel]);
        if (Theme::SmallFont()) ImGui::PopFont();
    }

    // Линия связи между ключами — видно, что дорожка непрерывна.
    if (curve.Count() >= 2) {
        const float x0 = std::max(TimeToX(l, curve.FirstTime()), left);
        const float x1 = std::min(TimeToX(l, curve.LastTime()), right);
        if (x1 > x0) {
            dl->AddLine(ImVec2(x0, cy), ImVec2(x1, cy),
                        (info.ChannelColors[channel] & 0x00FFFFFF) | 0x40000000, 2.0f);
        }
    }

    for (int i = 0; i < curve.Count(); ++i) {
        const Keyframe& key = curve.At(i);
        const float x = TimeToX(l, key.Time);
        if (x < left - 8.0f || x > right + 8.0f) continue;

        const KeyRef ref{track.Id, channel, key.Time};
        const bool selected = IsKeySelected(ref);

        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(ImVec2(x - 6.0f, cy - 6.0f));
        ImGui::InvisibleButton("##k", ImVec2(12.0f, 12.0f));
        const bool hovered = ImGui::IsItemHovered();

        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !track.Locked) {
            ToggleKeySelection(ref, ImGui::GetIO().KeyCtrl);
            m_draggingKeys = true;
            m_dragTimeAnchor = XToTime(l, ImGui::GetIO().MousePos.x);
            host.CaptureUndo();
        }
        if (hovered) {
            ImGui::SetTooltip(T("%s %s\nвремя %.3f c (кадр %d)\nзначение %.3f\n%s"),
                              T(info.Label), info.Channels > 1 ? info.ChannelLabels[channel] : "",
                              (double)key.Time, (int)(key.Time * host.Document().Fps + 0.5f),
                              (double)key.Value, InterpName(key.Mode));
        }

        // Рамка выделения проверяется здесь же, где ключ рисуется: его экранная
        // позиция уже посчитана, и второй расчёт мог бы разойтись с первым.
        if (m_boxSelecting && !track.Locked && InSelectionBox(ImVec2(x, cy)) &&
            !IsKeySelected(ref)) {
            m_selectedKeys.push_back(ref);
        }

        // Ступенчатые ключи рисуем квадратом: форма сразу говорит, что значение
        // не интерполируется, без наведения мышью.
        const ImU32 color = track.Muted ? Theme::Colors::TextFaint : info.ChannelColors[channel];
        if (key.Mode == Interp::Constant) {
            dl->AddRectFilled(ImVec2(x - 4.5f, cy - 4.5f), ImVec2(x + 4.5f, cy + 4.5f), color, 1.0f);
            if (selected) dl->AddRect(ImVec2(x - 5.5f, cy - 5.5f), ImVec2(x + 5.5f, cy + 5.5f),
                                      IM_COL32(255, 255, 255, 230), 1.0f, 0, 1.6f);
        } else {
            DrawDiamond(dl, ImVec2(x, cy), hovered ? 6.5f : 5.5f, color, selected);
        }
        ImGui::PopID();
    }

    dl->AddLine(ImVec2(l.Origin.x, y + l.RowH), ImVec2(right, y + l.RowH), Theme::Colors::LineSoft, 1.0f);
}

void TimelinePanel::DrawClipTrack(DirectorHost& host, ClipTrack& track, const Layout& l, float y,
                                  float rowH, ImDrawList* dl, int colorIndex) {
    Scene& scene = host.CurrentScene();
    const float left = l.Origin.x + l.TrackX;
    const float right = left + l.Width;
    const ImU32 base = (colorIndex % 2 == 0) ? Theme::Colors::ClipBlockA : Theme::Colors::ClipBlockB;

    // Имя персонажа в колонке.
    Icons::Draw(dl, Icon::Character, ImVec2(l.Origin.x + 20.0f, y + rowH * 0.5f), 14.0f,
                track.Muted ? Theme::Colors::TextFaint : Theme::Colors::TextDim);
    dl->AddText(ImVec2(l.Origin.x + 32.0f, y + 3.0f),
                track.Muted ? Theme::Colors::TextFaint : Theme::Colors::Text,
                TargetName(scene, track.TargetId).c_str());

    ImGui::PushID(track.Id * 1000);
    ImGui::SetCursorScreenPos(ImVec2(l.Origin.x + l.TrackX - 40.0f, y + 2.0f));
    bool notMuted = !track.Muted;
    if (Icons::ToggleIcon("cmute", Icon::Audio, Icon::Mute, notMuted, T("Заглушить дорожку клипов"), 16.0f)) {
        track.Muted = !notMuted;
    }

    for (size_t i = 0; i < track.Blocks.size(); ++i) {
        ClipBlock& block = track.Blocks[i];
        const float x0 = TimeToX(l, block.Start);
        const float x1 = TimeToX(l, block.Start + block.Duration);
        if (x1 < left || x0 > right) continue;

        const ImVec2 a(std::max(x0, left), y + 2.0f);
        const ImVec2 b(std::min(x1, right), y + rowH - 2.0f);
        if (b.x - a.x < 2.0f) continue;

        dl->AddRectFilled(a, b, base, 3.0f);
        dl->AddRect(a, b, IM_COL32(255, 255, 255, 45), 3.0f, 0, 1.0f);

        // Кросс-фейд показываем градиентным клином слева: видно, где блок
        // «въезжает» в предыдущий.
        if (block.BlendIn > 0.0f) {
            const float blendX = std::min(TimeToX(l, block.Start + block.BlendIn), b.x);
            if (blendX > a.x) {
                dl->AddRectFilledMultiColor(a, ImVec2(blendX, b.y), IM_COL32(255, 255, 255, 70),
                                            IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0),
                                            IM_COL32(255, 255, 255, 70));
            }
        }

        // Подпись, если влезает.
        const ImVec2 textSize = ImGui::CalcTextSize(block.Name.c_str());
        if (textSize.x < b.x - a.x - 10.0f) {
            dl->AddText(ImVec2(a.x + 6.0f, a.y + 1.0f), IM_COL32(255, 255, 255, 235), block.Name.c_str());
        }

        // --- Перетаскивание: тело двигает блок, края меняют длительность ---
        ImGui::PushID((int)i);
        const float edge = 5.0f;
        ImGui::SetCursorScreenPos(ImVec2(a.x, a.y));
        ImGui::InvisibleButton("##body", ImVec2(std::max(b.x - a.x, 1.0f), b.y - a.y));
        const bool bodyHovered = ImGui::IsItemHovered();
        if (bodyHovered) {
            const float mx = ImGui::GetIO().MousePos.x;
            const int zone = (mx - a.x < edge) ? -1 : (b.x - mx < edge ? 1 : 0);
            ImGui::SetMouseCursor(zone == 0 ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                host.CaptureUndo();
                m_draggingBlockTrack = track.Id;
                m_draggingBlockIndex = (int)i;
                m_draggingBlockEdge = zone;
                m_dragTimeAnchor = XToTime(l, mx);
            }
            ImGui::SetTooltip(T("%s\n%.2f — %.2f c (%.2f c)\nскорость %.2fx, переход %.2f c"),
                              block.Name.c_str(), (double)block.Start,
                              (double)(block.Start + block.Duration), (double)block.Duration,
                              (double)block.Speed, (double)block.BlendIn);
        }

        if (ImGui::BeginPopupContextItem("##blockctx")) {
            ImGui::TextDisabled("%s", block.Name.c_str());
            ImGui::Separator();
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::DragFloat(T("Скорость"), &block.Speed, 0.01f, 0.05f, 8.0f, "%.2fx")) host.PushUndo();
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::DragFloat(T("Переход"), &block.BlendIn, 0.01f, 0.0f, 3.0f, T("%.2f c"))) host.PushUndo();
            ImGui::Checkbox(T("Зациклить"), &block.Loop);
            ImGui::Separator();
            if (ImGui::MenuItem(T("Удалить блок"))) {
                host.PushUndo();
                track.Blocks.erase(track.Blocks.begin() + (long)i);
                ImGui::EndPopup();
                ImGui::PopID();
                break;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    ImGui::PopID();
    dl->AddLine(ImVec2(l.Origin.x, y + rowH), ImVec2(right, y + rowH), Theme::Colors::LineSoft, 1.0f);
}

void TimelinePanel::DrawAudioRow(DirectorHost& host, const Layout& l, float y, float rowH,
                                ImDrawList* dl) {
    AnimationDocument& doc = host.Document();
    AudioTrack& audio = doc.Audio;
    const float left = l.Origin.x + l.TrackX;
    const float right = left + l.Width;

    Icons::Draw(dl, audio.Muted ? Icon::Mute : Icon::Audio,
                ImVec2(l.Origin.x + 20.0f, y + rowH * 0.5f), 14.0f, Theme::Colors::Waveform);
    const std::string name = std::filesystem::path(audio.Path()).filename().string();
    dl->AddText(ImVec2(l.Origin.x + 32.0f, y + 4.0f), Theme::Colors::Text, name.c_str());

    ImGui::PushID("audioRow");
    ImGui::SetCursorScreenPos(ImVec2(l.Origin.x + l.TrackX - 40.0f, y + 4.0f));
    bool notMuted = !audio.Muted;
    if (Icons::ToggleIcon("amute", Icon::Audio, Icon::Mute, notMuted, T("Заглушить звук"), 16.0f)) {
        audio.Muted = !notMuted;
    }

    const ImVec2 a(left, y + 2.0f);
    const ImVec2 b(right, y + rowH - 2.0f);
    dl->AddRectFilled(a, b, IM_COL32(20, 26, 34, 200), 2.0f);

    if (audio.HasWaveform()) {
        const int width = (int)(b.x - a.x);
        const std::vector<AudioTrack::Peak> peaks = audio.Sample(m_viewStart, m_viewEnd, width);
        const float mid = (a.y + b.y) * 0.5f;
        const float halfH = (b.y - a.y) * 0.45f;
        const ImU32 color = audio.Muted ? Theme::Colors::TextFaint : Theme::Colors::Waveform;
        for (int x = 0; x < (int)peaks.size(); ++x) {
            const AudioTrack::Peak& p = peaks[(size_t)x];
            if (p.Min == 0.0f && p.Max == 0.0f) continue;
            dl->AddLine(ImVec2(a.x + (float)x, mid - p.Max * halfH),
                        ImVec2(a.x + (float)x, mid - p.Min * halfH), color, 1.0f);
        }
        dl->AddLine(ImVec2(a.x, mid), ImVec2(b.x, mid), (color & 0x00FFFFFF) | 0x40000000, 1.0f);
    } else {
        dl->AddText(ImVec2(a.x + 8.0f, a.y + 6.0f), Theme::Colors::TextFaint,
                    T("Волна недоступна для этого формата — звук всё равно проигрывается"));
    }

    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##audioBody", ImVec2(std::max(b.x - a.x, 1.0f), b.y - a.y));
    if (ImGui::BeginPopupContextItem("##audioctx")) {
        ImGui::TextDisabled("%s", audio.Path().c_str());
        ImGui::Separator();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::DragFloat(T("Смещение"), &audio.Offset, 0.01f, -60.0f, 600.0f, T("%.2f c"));
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderFloat(T("Громкость"), &audio.Volume, 0.0f, 2.0f, "%.2f");
        ImGui::Separator();
        if (ImGui::MenuItem(T("Убрать звук"))) {
            host.PushUndo();
            audio.Clear();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();

    dl->AddLine(ImVec2(l.Origin.x, y + rowH), ImVec2(right, y + rowH), Theme::Colors::LineSoft, 1.0f);
}

// ============================================================================
//  Монтаж камер
// ============================================================================

void TimelinePanel::DrawCameraPicker(DirectorHost& host, int currentId, int cutIndex) {
    Scene& scene = host.CurrentScene();
    auto view = scene.Registry().view<CameraComponent, IdComponent>();
    bool any = false;
    for (auto e : view) {
        const int id = view.get<IdComponent>(e).Id;
        any = true;
        const std::string name = TargetName(scene, id);
        if (ImGui::MenuItem(name.c_str(), nullptr, id == currentId)) {
            host.PushUndo();
            AnimationDocument& doc = host.Document();
            if (cutIndex >= 0) doc.Cameras.Cuts[(size_t)cutIndex].CameraId = id;
            else doc.SetCut(host.CurrentTime(), id);
        }
    }
    if (!any) ImGui::TextDisabled("%s", T("В сцене нет камер"));
}

void TimelinePanel::DrawCameraRow(DirectorHost& host, const Layout& l, float y, float rowH,
                                 ImDrawList* dl) {
    AnimationDocument& doc = host.Document();
    Scene& scene = host.CurrentScene();
    CameraTrack& track = doc.Cameras;
    const float left = l.Origin.x + l.TrackX;
    const float right = left + l.Width;
    const ImU32 dim = track.Muted ? Theme::Colors::TextFaint : Theme::Colors::Text;

    Icons::Draw(dl, Icon::Camera, ImVec2(l.Origin.x + 20.0f, y + rowH * 0.5f), 14.0f,
                track.Muted ? Theme::Colors::TextFaint : Theme::Colors::TextDim);
    dl->AddText(ImVec2(l.Origin.x + 32.0f, y + 3.0f), dim, T("Монтаж"));

    ImGui::PushID("cameraRow");
    ImGui::SetCursorScreenPos(ImVec2(l.Origin.x + l.TrackX - 40.0f, y + 2.0f));
    bool notMuted = !track.Muted;
    if (Icons::ToggleIcon("camMute", Icon::Camera, Icon::Mute, notMuted,
                          T("Отключить монтаж: кадр снимается ручной камерой"), 16.0f)) {
        track.Muted = !notMuted;
    }

    const float top = y + 2.0f;
    const float bottom = y + rowH - 2.0f;
    const int liveCut = doc.CutIndexAt(host.CurrentTime());

    // Участок ДО первой склейки рисуется отдельно и приглушённо: там работает
    // ручная камера, и это правило должно быть видно, а не подразумеваться.
    const float firstX = track.Cuts.empty() ? right : TimeToX(l, track.Cuts.front().Time);
    if (firstX > left + 2.0f) {
        const ImVec2 a(left, top), b(std::min(firstX, right), bottom);
        dl->AddRectFilled(a, b, IM_COL32(38, 44, 54, 160), 3.0f);
        const std::string label = std::string(T("ручная: ")) + TargetName(scene, host.ActiveCameraId());
        dl->PushClipRect(a, b, true);
        dl->AddText(ImVec2(a.x + 6.0f, a.y + 3.0f), Theme::Colors::TextFaint, label.c_str());
        dl->PopClipRect();
    }

    for (size_t i = 0; i < track.Cuts.size(); ++i) {
        CameraCut& cut = track.Cuts[i];
        const float x0 = TimeToX(l, cut.Time);
        const float x1 = (i + 1 < track.Cuts.size()) ? TimeToX(l, track.Cuts[i + 1].Time)
                                                     : TimeToX(l, doc.Duration);
        if (x1 < left || x0 > right) continue;

        const ImVec2 a(std::max(x0, left), top);
        const ImVec2 b(std::min(x1, right), bottom);
        if (b.x - a.x < 2.0f) continue;

        const bool live = (int)i == liveCut;
        // Цвет плана — по камере, а не по порядку: два плана одной камерой
        // должны читаться как одна камера, иначе монтаж не видно глазом.
        const ImU32 base = Theme::BlockColor(cut.CameraId);
        dl->AddRectFilled(a, b, track.Muted ? IM_COL32(60, 60, 60, 120) : base, 3.0f);
        if (live) dl->AddRect(a, b, IM_COL32(255, 255, 255, 200), 3.0f, 0, 2.0f);

        dl->PushClipRect(a, b, true);
        dl->AddText(ImVec2(a.x + 6.0f, a.y + 3.0f), IM_COL32(255, 255, 255, 230),
                    TargetName(scene, cut.CameraId).c_str());
        dl->PopClipRect();

        // Ручка склейки — сам её левый край: тащить план целиком бессмысленно,
        // у плана нет длины, она задаётся следующей склейкой.
        ImGui::SetCursorScreenPos(ImVec2(a.x - 3.0f, top));
        ImGui::PushID((int)i);
        ImGui::InvisibleButton("##cut", ImVec2(10.0f, bottom - top));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActivated()) {
            host.CaptureUndo();
            m_draggingCut = (int)i;
        }
        if (hovered && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ImGui::SetTooltip(T("%s\nсклейка на %.2f c\nтащить — сдвинуть, ПКМ — сменить камеру"),
                              TargetName(scene, cut.CameraId).c_str(), (double)cut.Time);
        }
        ImGui::PopID();

        // Тело плана: контекстное меню и выбор камеры.
        ImGui::SetCursorScreenPos(ImVec2(a.x + 7.0f, top));
        ImGui::PushID(1000 + (int)i);
        ImGui::InvisibleButton("##shot", ImVec2(std::max(b.x - a.x - 7.0f, 1.0f), bottom - top));
        if (ImGui::BeginPopupContextItem("##shotctx")) {
            ImGui::TextDisabled("%s", T("Камера плана"));
            DrawCameraPicker(host, cut.CameraId, (int)i);
            ImGui::Separator();
            if (ImGui::MenuItem(T("Удалить склейку"))) {
                host.PushUndo();
                doc.RemoveCut((int)i);
                ImGui::EndPopup();
                ImGui::PopID();
                break;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    // Перетаскивание склейки. Индекс МЕНЯЕТСЯ при пересечении соседней —
    // MoveCut возвращает новый, и его надо запомнить, иначе следующий кадр
    // потащит чужую склейку.
    if (m_draggingCut >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float t = std::clamp(XToTime(l, ImGui::GetIO().MousePos.x), 0.0f, doc.Duration);
            m_draggingCut = doc.MoveCut(m_draggingCut, t);
        } else {
            m_draggingCut = -1;
        }
    }

    // Пустое место дорожки: поставить склейку прямо здесь.
    ImGui::SetCursorScreenPos(ImVec2(left, top));
    ImGui::InvisibleButton("##camBody", ImVec2(std::max(right - left, 1.0f), bottom - top));
    if (ImGui::BeginPopupContextItem("##camctx")) {
        ImGui::TextDisabled("%s", T("Поставить склейку на головке"));
        DrawCameraPicker(host, -1, -1);
        ImGui::Separator();
        ImGui::MenuItem(T("Отключить монтаж"), nullptr, &track.Muted);
        if (ImGui::MenuItem(T("Убрать монтаж целиком"), nullptr, false, !track.Cuts.empty())) {
            host.PushUndo();
            track.Cuts.clear();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();

    dl->AddLine(ImVec2(l.Origin.x, y + rowH), ImVec2(right, y + rowH), Theme::Colors::LineSoft, 1.0f);
}

// ============================================================================
//  Меню добавления дорожки
// ============================================================================

void TimelinePanel::DrawAddTrackMenu(DirectorHost& host) {
    Scene& scene = host.CurrentScene();
    AnimationDocument& doc = host.Document();

    // Монтаж камер заводится ПЕРВЫМ пунктом и не зависит от выбранного
    // объекта: он относится ко всему ролику, а не к чему-то одному в сцене.
    // Первая склейка ставится сразу — дорожка без склеек не показывается и
    // выглядела бы как «пункт ничего не сделал».
    {
        const bool hasCamera = !scene.Registry().view<CameraComponent>().empty();
        const bool exists = !doc.Cameras.Cuts.empty();
        if (ImGui::MenuItem(T("Монтаж камер"), nullptr, false, hasCamera && !exists)) {
            host.PushUndo();
            doc.SetCut(host.CurrentTime(), host.ActiveCameraId());
            host.SetStatus(T("Монтаж камер заведён — ПКМ по плану меняет камеру"));
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", exists ? T("Монтаж уже заведён")
                              : hasCamera ? T("Переключение активной камеры по времени.\n"
                                              "Каждая склейка задаёт, чем снимается план.")
                                          : T("Сначала создайте камеру (Создать > Камера)"));
        }
        ImGui::Separator();
    }

    const int id = host.SelectedId();
    if (id < 0) {
        ImGui::TextDisabled("%s", T("Сначала выберите объект в сцене"));
        return;
    }

    ImGui::TextDisabled("%s", TargetName(scene, id).c_str());
    ImGui::Separator();

    const std::vector<Property> applicable = ApplicableProperties(scene, id);
    for (Property prop : applicable) {
        const PropertyInfo& info = PropertyInfoOf(prop);
        const bool exists = host.Document().FindTrack(id, prop) != nullptr;
        if (ImGui::MenuItem(T(info.Label), nullptr, false, !exists)) {
            host.PushUndo();
            // Новая дорожка сразу получает ключ на текущем кадре: пустая
            // дорожка ничего не делает и выглядит как «не сработало».
            host.KeyProperty(id, prop);
            host.SetStatus(std::string(T("Дорожка добавлена: ")) + T(info.Label));
        }
        if (exists && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Дорожка уже есть"));
    }

    // Персонаж — отдельный пункт: у него не свойство, а блоки клипов.
    GameObject obj = scene.Get(id);
    if (obj.Valid()) {
        if (const AnimatedModelComponent* am = scene.Registry().try_get<AnimatedModelComponent>(obj.Entity())) {
            ImGui::Separator();
            const bool hasClips = am->Model && !am->Model->Clips().empty();
            if (ImGui::MenuItem(T("Дорожка клипов"), nullptr, false, hasClips)) {
                host.PushUndo();
                ClipTrack& track = host.Document().EnsureClipTrack(id);
                ClipBlock block;
                block.ClipIndex = 0;
                block.Name = am->Model->Clips()[0].Name;
                block.Start = host.CurrentTime();
                const float length = am->Model->Clips()[0].Duration;
                block.Duration = length > 0.01f ? length : 1.0f;
                track.Blocks.push_back(block);
                host.SetStatus(T("Дорожка клипов добавлена"));
            }
            if (!hasClips && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", T("В модели нет анимационных клипов"));
            }
        }
    }
}

// ============================================================================
//  Представления
// ============================================================================

// Число дорожек с правильным окончанием. «3 дорожк(и)» в заголовке зоны
// выглядит как недоделка, а склонение в русском зависит не от числа, а от его
// ДВУХ последних цифр: 11-14 всегда «дорожек», хотя оканчиваются на 1-4.
std::string TrackCountLabel(int n) {
    const int tens = n % 100, ones = n % 10;
    const char* form = (tens >= 11 && tens <= 14) ? T("%d дорожек")
                     : (ones == 1)                ? T("%d дорожка")
                     : (ones >= 2 && ones <= 4)   ? T("%d дорожки")
                                                  : T("%d дорожек");
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), form, n);
    return buffer;
}

// --- Вертикальная прокрутка -------------------------------------------------

float TimelinePanel::BeginTracks(const Layout& l) {
    const float top = l.Origin.y + l.RulerH;
    // Отсечение ставим через ImGui, а не через список отрисовки: оно должно
    // резать не только пиксели, но и НАЖАТИЯ. Иначе уехавшая под линейку
    // строка остаётся кликабельной, и мышь ловит невидимый ключ.
    ImGui::PushClipRect(ImVec2(l.Origin.x, top),
                        ImVec2(l.Origin.x + l.TrackX + l.Width, l.Origin.y + l.Height), true);
    return top - m_scrollY;
}

void TimelinePanel::EndTracks(const Layout& l, float yEnd, ImDrawList* dl) {
    const float top = l.Origin.y + l.RulerH;
    const float viewH = std::max(l.Height - l.RulerH, 1.0f);
    m_contentH = (yEnd + m_scrollY) - top;
    ImGui::PopClipRect();

    const float maxScroll = std::max(0.0f, m_contentH - viewH);
    m_scrollY = std::clamp(m_scrollY, 0.0f, maxScroll);
    if (maxScroll <= 0.0f) { m_draggingScroll = false; return; }

    // Полоса прокрутки. Она нужна не только чтобы тащить: это единственный
    // признак того, что ниже ЕЩЁ ЕСТЬ дорожки. Без неё пропажа выглядит как
    // «дорожек больше нет».
    const float barW = 9.0f;
    const float x0 = l.Origin.x + l.TrackX + l.Width - barW;
    const float thumbH = std::max(24.0f, viewH * viewH / m_contentH);
    const float thumbY = top + (viewH - thumbH) * (m_scrollY / maxScroll);
    dl->AddRectFilled(ImVec2(x0, top), ImVec2(x0 + barW, top + viewH), IM_COL32(0, 0, 0, 60));
    dl->AddRectFilled(ImVec2(x0 + 2.0f, thumbY), ImVec2(x0 + barW - 2.0f, thumbY + thumbH),
                      m_draggingScroll ? Theme::Colors::Accent : Theme::Colors::Line, 3.0f);

    ImGui::SetCursorScreenPos(ImVec2(x0, top));
    ImGui::InvisibleButton("##vscroll", ImVec2(barW, viewH));
    if (ImGui::IsItemActivated()) m_draggingScroll = true;
    if (m_draggingScroll) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float track = std::max(viewH - thumbH, 1.0f);
            const float rel = (ImGui::GetIO().MousePos.y - top - thumbH * 0.5f) / track;
            m_scrollY = std::clamp(rel, 0.0f, 1.0f) * maxScroll;
        } else {
            m_draggingScroll = false;
        }
    }
}

void TimelinePanel::HandleVerticalScroll(const Layout& l) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseWheel == 0.0f) return;
    const ImVec2 m = io.MousePos;
    const bool inArea = m.x >= l.Origin.x && m.x <= l.Origin.x + l.TrackX + l.Width &&
                        m.y >= l.Origin.y && m.y <= l.Origin.y + l.Height;
    if (!inArea) return;
    // Над колонкой имён масштабировать нечего, поэтому колесо там прокручивает.
    const bool overNames = m.x < l.Origin.x + l.TrackX;
    if (!overNames && !io.KeyCtrl) return;
    m_scrollY -= io.MouseWheel * 48.0f;
    const float maxScroll = std::max(0.0f, m_contentH - std::max(l.Height - l.RulerH, 1.0f));
    m_scrollY = std::clamp(m_scrollY, 0.0f, maxScroll);
}

// Полоса, разделяющая таймлайн на зоны.
//
// Зачем вообще зоны. Монтаж и покадровая анимация — два разных занятия с
// разным масштабом мысли: в монтаже двигают куски по несколько секунд, в
// анимации — отдельные ключи. Вперемешку они мешают друг другу: строка с
// именем камеры выглядит как ещё одна дорожка ключей, а десяток каналов
// поворота отодвигает монтаж за нижний край. Разделив их полосой и дав разную
// высоту строк, мы делаем видимым то, что и так верно по смыслу.
float TimelinePanel::DrawZoneHeader(const Layout& l, float y, ImDrawList* dl, const char* id,
                                    const char* label, const char* hint, bool& open) {
    const ImVec2 a(l.Origin.x, y);
    const ImVec2 b(l.Origin.x + l.TrackX + l.Width, y + kZoneHeader);
    dl->AddRectFilled(a, b, Theme::Colors::PanelRaised);
    dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), Theme::Colors::Line, 1.0f);

    // Треугольник сворачивания.
    const ImVec2 c(a.x + 12.0f, (a.y + b.y) * 0.5f);
    const ImU32 arrow = Theme::Colors::TextDim;
    if (open) dl->AddTriangleFilled(ImVec2(c.x - 4.0f, c.y - 2.5f), ImVec2(c.x + 4.0f, c.y - 2.5f),
                                    ImVec2(c.x, c.y + 3.5f), arrow);
    else      dl->AddTriangleFilled(ImVec2(c.x - 2.5f, c.y - 4.0f), ImVec2(c.x - 2.5f, c.y + 4.0f),
                                    ImVec2(c.x + 3.5f, c.y), arrow);

    dl->AddText(ImVec2(a.x + 24.0f, a.y + 2.0f), Theme::Colors::Text, label);
    if (hint && *hint) {
        const float labelW = ImGui::CalcTextSize(label).x;
        dl->AddText(ImVec2(a.x + 32.0f + labelW, a.y + 2.0f), Theme::Colors::TextFaint, hint);
    }

    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(a);
    if (ImGui::InvisibleButton("##zone", ImVec2(std::max(b.x - a.x, 1.0f), kZoneHeader))) {
        open = !open;
    }
    ImGui::PopID();
    return kZoneHeader;
}

void TimelinePanel::DrawTimelineTab(DirectorHost& host, const Layout& l) {
    AnimationDocument& doc = host.Document();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = l.Origin.y + l.RulerH;

    const int selectedId = host.SelectedId();
    int clipColorIndex = 0;
    const float top = l.Origin.y + l.RulerH;
    const float bottom = l.Origin.y + l.Height;
    y = BeginTracks(l);

    // Уехавшие за край строки НЕ рисуются, но их высота всё равно
    // накапливается: иначе прокрутка не знала бы, сколько ещё содержимого
    // ниже, и полоса врала бы. Раньше здесь стоял break, и в этом была вся
    // беда: цикл обрывался, остаток дорожек просто переставал существовать.
    auto visible = [&](float rowTop, float rowH) {
        return rowTop + rowH >= top && rowTop <= bottom;
    };

    // --- ЗОНА МОНТАЖА ------------------------------------------------------
    // Склейки камер, блоки клипов и фонограмма: всё, из чего собирается ролик
    // крупными кусками. Строки здесь высокие — в них лежит текст, и их таскают
    // мышью.
    const int montageRows = (doc.Cameras.Cuts.empty() ? 0 : 1) + (int)doc.ClipTracks.size() +
                            (doc.Audio.Loaded() ? 1 : 0);
    if (montageRows > 0) {
        const std::string hint = "— " + TrackCountLabel(montageRows);
        y += DrawZoneHeader(l, y, dl, "montage", T("МОНТАЖ"), hint.c_str(), m_montageOpen);

        if (m_montageOpen) {
            if (!doc.Cameras.Cuts.empty()) {
                if (visible(y, kMontageRow)) DrawCameraRow(host, l, y, kMontageRow, dl);
                y += kMontageRow;
            }
            for (ClipTrack& track : doc.ClipTracks) {
                if (visible(y, kMontageRow)) {
                    DrawClipTrack(host, track, l, y, kMontageRow, dl, clipColorIndex++);
                }
                y += kMontageRow;
            }
            // Фонограмма — в зоне монтажа, а не в конце списка: звук задаёт ритм
            // ролика, и смотреть на него надо рядом со склейками, под которые
            // его и подгоняют.
            if (doc.Audio.Loaded()) {
                if (visible(y, kAudioRow)) DrawAudioRow(host, l, y, kAudioRow, dl);
                y += kAudioRow;
            }
        }
        y += 4.0f; // воздух между зонами
    }

    // --- ЗОНА АНИМАЦИИ -----------------------------------------------------
    if (!doc.Tracks.empty()) {
        const std::string hint = "— " + TrackCountLabel((int)doc.Tracks.size());
        y += DrawZoneHeader(l, y, dl, "anim", T("АНИМАЦИЯ"), hint.c_str(), m_animationOpen);
    }
    if (!m_animationOpen) { EndTracks(l, y, dl); return; }

    for (Track& track : doc.Tracks) {
        if (!m_filter.empty()) {
            const std::string name = TargetName(host.CurrentScene(), track.TargetId);
            const std::string label = T(PropertyInfoOf(track.Prop).Label);
            if (name.find(m_filter) == std::string::npos &&
                label.find(m_filter) == std::string::npos &&
                track.JointName.find(m_filter) == std::string::npos) continue;
        }

        if (visible(y, l.RowH)) {
            y += DrawTrackHeader(host, track, l, y, dl, track.TargetId == selectedId);
        } else {
            y += l.RowH; // та же высота, что вернул бы заголовок
        }
        if (track.Expanded) {
            for (int c = 0; c < track.ChannelCount(); ++c) {
                if (visible(y, l.RowH)) DrawKeysRow(host, track, c, l, y, dl);
                y += l.RowH;
            }
        } else if (track.ChannelCount() == 1) {
            // Однокональная дорожка (FOV, интенсивность) не имеет смысла в
            // свёрнутом виде — ключи показываем прямо в её строке.
            DrawKeysRow(host, track, 0, l, y - l.RowH, dl);
        }
    }

    if (doc.Tracks.empty() && doc.ClipTracks.empty()) {
        dl->AddText(ImVec2(l.Origin.x + 16.0f, l.Origin.y + l.RulerH + 14.0f), Theme::Colors::TextDim,
                    T("Дорожек пока нет."));
        dl->AddText(ImVec2(l.Origin.x + 16.0f, l.Origin.y + l.RulerH + 34.0f), Theme::Colors::TextFaint,
                    T("Выберите объект и нажмите «+ Дорожка», либо включите авто-ключ и просто двигайте объект."));
    }

    EndTracks(l, y, dl);
}

void TimelinePanel::DrawDopeTab(DirectorHost& host, const Layout& l) {
    AnimationDocument& doc = host.Document();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Scene& scene = host.CurrentScene();
    const float top = l.Origin.y + l.RulerH;
    const float bottom = l.Origin.y + l.Height;
    float y = BeginTracks(l);

    // Dope Sheet: одна строка на дорожку, все каналы сведены в один ряд ромбов.
    // Так виден ТАЙМИНГ целиком, без разбиения на оси — им и правят ритм сцены.
    for (Track& track : doc.Tracks) {
        if (y + l.RowH < top || y > bottom) { y += l.RowH; continue; }

        dl->AddText(ImVec2(l.Origin.x + 10.0f, y + 3.0f),
                    track.Muted ? Theme::Colors::TextFaint : Theme::Colors::Text,
                    (TargetName(scene, track.TargetId) + " · " + T(PropertyInfoOf(track.Prop).Label)).c_str());

        const float cy = y + l.RowH * 0.5f;
        const float left = l.Origin.x + l.TrackX;
        const float right = left + l.Width;

        // Собираем времена всех каналов: ключ на кадре есть, если он есть хотя
        // бы в одном канале.
        std::vector<float> times;
        for (const Curve& curve : track.Channels) {
            for (const Keyframe& key : curve.Keys()) {
                bool known = false;
                for (float t : times) {
                    if (std::fabs(t - key.Time) <= Curve::kTimeEpsilon) { known = true; break; }
                }
                if (!known) times.push_back(key.Time);
            }
        }
        std::sort(times.begin(), times.end());

        ImGui::PushID(track.Id);
        for (size_t i = 0; i < times.size(); ++i) {
            const float x = TimeToX(l, times[i]);
            if (x < left - 8.0f || x > right + 8.0f) continue;

            const KeyRef ref{track.Id, 0, times[i]};
            const bool selected = IsKeySelected(ref);
            ImGui::PushID((int)i);
            ImGui::SetCursorScreenPos(ImVec2(x - 6.0f, cy - 6.0f));
            ImGui::InvisibleButton("##dk", ImVec2(12.0f, 12.0f));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !track.Locked) {
                // В Dope Sheet выбираем ключ во ВСЕХ каналах на этом времени —
                // иначе перетаскивание разъехалось бы по осям.
                if (!ImGui::GetIO().KeyCtrl) m_selectedKeys.clear();
                for (int c = 0; c < track.ChannelCount(); ++c) {
                    if (track.Channels[(size_t)c].KeyIndexAt(times[i]) >= 0) {
                        m_selectedKeys.push_back(KeyRef{track.Id, c, times[i]});
                    }
                }
                m_draggingKeys = true;
                m_dragTimeAnchor = XToTime(l, ImGui::GetIO().MousePos.x);
                host.CaptureUndo();
            }
            DrawDiamond(dl, ImVec2(x, cy), ImGui::IsItemHovered() ? 6.5f : 5.5f,
                        track.Muted ? Theme::Colors::TextFaint : PropertyInfoOf(track.Prop).ChannelColors[0],
                        selected);
            ImGui::PopID();
        }
        ImGui::PopID();

        dl->AddLine(ImVec2(l.Origin.x, y + l.RowH), ImVec2(right, y + l.RowH),
                    Theme::Colors::LineSoft, 1.0f);
        y += l.RowH;
    }

    if (doc.Tracks.empty()) {
        dl->AddText(ImVec2(l.Origin.x + 16.0f, l.Origin.y + l.RulerH + 14.0f), Theme::Colors::TextDim,
                    T("Ключей пока нет — Dope Sheet показывает тайминг уже поставленных ключей."));
    }

    EndTracks(l, y, dl);
}

// Всё, что относится к чтению графика, — в одном меню. Тремя отдельными
// элементами панель переполнялась, и подписи обрезались на середине слова.
bool TimelinePanel::InSelectionBox(const ImVec2& p) const {
    if (!m_boxSelecting) return false;
    const float x0 = std::min(m_boxStart.x, m_boxEnd.x), x1 = std::max(m_boxStart.x, m_boxEnd.x);
    const float y0 = std::min(m_boxStart.y, m_boxEnd.y), y1 = std::max(m_boxStart.y, m_boxEnd.y);
    return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1;
}

bool TimelinePanel::SelectionRange(float& from, float& to, std::vector<int>& trackIds) const {
    if (m_selectedKeys.empty()) return false;
    from = 1e9f;
    to = -1e9f;
    trackIds.clear();
    for (const KeyRef& ref : m_selectedKeys) {
        from = std::min(from, ref.Time);
        to = std::max(to, ref.Time);
        if (std::find(trackIds.begin(), trackIds.end(), ref.TrackId) == trackIds.end()) {
            trackIds.push_back(ref.TrackId);
        }
    }
    return true;
}

// Операции над ключами: копирование между объектами, растяжение во времени,
// запекание кривой в кадры.
void TimelinePanel::DrawKeyOpsMenu(DirectorHost& host) {
    AnimationDocument& doc = host.Document();
    char label[48];
    if (m_clipboard.Empty()) std::snprintf(label, sizeof(label), "%s", T("Ключи"));
    else std::snprintf(label, sizeof(label), T("Ключи (в буфере %d)"), m_clipboard.Count());
    if (!ImGui::BeginMenu(label)) return;

    float from = 0.0f, to = 0.0f;
    std::vector<int> tracks;
    const bool haveSelection = SelectionRange(from, to, tracks);

    if (!haveSelection) ImGui::TextDisabled("%s", T("Выберите ключи в таймлайне"));

    ImGui::BeginDisabled(!haveSelection);
    if (ImGui::MenuItem(T("Копировать"))) {
        const int n = doc.CopyKeys(host.SelectedId(), tracks, from, to, m_clipboard);
        host.SetStatus(T("Скопировано ключей: ") + std::to_string(n));
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(m_clipboard.Empty() || host.SelectedId() < 0);
    if (ImGui::MenuItem(T("Вставить на головку"))) {
        host.PushUndo();
        const int n = doc.PasteKeys(host.SelectedId(), m_clipboard, host.CurrentTime());
        host.SetStatus(T("Вставлено ключей: ") + std::to_string(n));
        host.SetCurrentTime(host.CurrentTime()); // переприменить документ к сцене
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Ключи лягут на ВЫБРАННЫЙ сейчас объект.\n"
                          "Так анимация переносится между объектами:\n"
                          "скопировать у одного, выбрать другой, вставить."));
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    ImGui::BeginDisabled(!haveSelection);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat(T("Множитель"), &m_scaleFactor, 0.01f, 0.05f, 20.0f, "%.2fx");
    if (ImGui::MenuItem(T("Растянуть во времени"))) {
        host.PushUndo();
        // Точка опоры — ЛЕВЫЙ край выделения: растягивая кусок, аниматор ждёт,
        // что его начало останется на месте, а сдвинется хвост.
        const int n = doc.ScaleKeyTimes(host.SelectedId(), tracks, from, to, from, m_scaleFactor);
        host.SetStatus(T("Растянуто ключей: ") + std::to_string(n));
        host.SetCurrentTime(host.CurrentTime());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Движение получилось правильным, но слишком быстрым\n"
                          "или медленным: растянуть весь кусок, сохранив рисунок."));
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    ImGui::BeginDisabled(!haveSelection);
    if (ImGui::MenuItem(T("Запечь в ключи по кадрам"))) {
        host.PushUndo();
        int n = 0;
        for (int id : tracks) {
            if (Track* track = doc.TrackById(id)) n += doc.BakeTrack(*track, from, to, doc.Fps);
        }
        host.SetStatus(T("Запечено ключей: ") + std::to_string(n));
        host.SetCurrentTime(host.CurrentTime());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Кривая превращается в ключ на каждом кадре.\n"
                          "Нужно, чтобы править отдельные кадры руками\n"
                          "и чтобы отдать анимацию туда, где нет Безье.\n"
                          "Форма сохранится, но сглаживание уже не вернуть."));
    }
    ImGui::EndDisabled();

    ImGui::EndMenu();
}

void TimelinePanel::DrawGraphMenu(DirectorHost& host) {
    int hidden = 0;
    for (const Track& t : host.Document().Tracks) {
        if (t.TargetId != host.SelectedId()) continue;
        for (int c = 0; c < t.ChannelCount(); ++c) {
            if (IsChannelHidden(t.Id, c)) ++hidden;
        }
    }
    char label[64];
    if (hidden > 0) std::snprintf(label, sizeof(label), T("Кривые (скрыто %d)"), hidden);
    else std::snprintf(label, sizeof(label), "%s", T("Кривые"));

    if (!ImGui::BeginMenu(label)) return;

    ImGui::MenuItem(T("Автомасштаб"), nullptr, &m_graphAutoFit);
    if (ImGui::MenuItem(T("Нормализовать"), nullptr, &m_graphNormalize) && m_graphNormalize) {
        m_graphAutoFit = false; // общий масштаб в этом режиме ни при чём
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "%s", T("Растянуть каждую кривую на её собственный размах.\n"
            "Нужно, когда у объекта ключуются величины разных единиц:\n"
            "поворот в 720° и позиция в 2 м на общей шкале означают,\n"
            "что позиция становится плоской линией у нуля."));
    }
    ImGui::Separator();
    DrawGraphChannelFilter(host);
    ImGui::EndMenu();
}

// Список кривых выбранного объекта с галками видимости.
//
// Скрытие — про ЧТЕНИЕ графика, а не про анимацию: скрытая кривая продолжает
// применяться к сцене. Для «не применять» есть заглушка дорожки, и путать эти
// две вещи нельзя — иначе спрятанный на время канал молча перестал бы работать.
void TimelinePanel::DrawGraphChannelFilter(DirectorHost& host) {
    AnimationDocument& doc = host.Document();
    const int selectedId = host.SelectedId();

    int hidden = 0;
    for (const Track& t : doc.Tracks) {
        if (t.TargetId != selectedId) continue;
        for (int c = 0; c < t.ChannelCount(); ++c) {
            if (IsChannelHidden(t.Id, c)) ++hidden;
        }
    }

    (void)hidden;
    bool any = false;
    for (Track& track : doc.Tracks) {
        if (track.TargetId != selectedId) continue;
        const PropertyInfo& info = PropertyInfoOf(track.Prop);
        for (int c = 0; c < track.ChannelCount(); ++c) {
            if (track.Channels[(size_t)c].Empty()) continue; // пустой канал и так не рисуется
            any = true;
            ImGui::PushID(track.Id * 100 + c);
            bool visible = !IsChannelHidden(track.Id, c);
            char name[96];
            const char* channelName = info.ChannelLabels[c];
            if (channelName && channelName[0]) {
                std::snprintf(name, sizeof(name), "%s · %s", T(info.Label), channelName);
            } else {
                std::snprintf(name, sizeof(name), "%s", T(info.Label));
            }
            // Цветная метка перед именем — те же цвета, что у самих кривых:
            // иначе в списке из девяти строк непонятно, какая из них какая.
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(pos.x, pos.y + 4.0f), ImVec2(pos.x + 8.0f, pos.y + 14.0f),
                info.ChannelColors[c], 2.0f);
            ImGui::Dummy(ImVec2(12.0f, 1.0f));
            ImGui::SameLine();
            if (ImGui::MenuItem(name, nullptr, visible)) ToggleChannelHidden(track.Id, c);
            ImGui::PopID();
        }
    }
    if (!any) ImGui::TextDisabled("%s", T("У объекта нет кривых"));

    if (!m_hiddenChannels.empty()) {
        ImGui::Separator();
        if (ImGui::MenuItem(T("Показать все"))) m_hiddenChannels.clear();
    }
}

bool TimelinePanel::IsChannelHidden(int trackId, int channel) const {
    for (const auto& kv : m_hiddenChannels) {
        if (kv.first == trackId && kv.second == channel) return true;
    }
    return false;
}

void TimelinePanel::ToggleChannelHidden(int trackId, int channel) {
    for (size_t i = 0; i < m_hiddenChannels.size(); ++i) {
        if (m_hiddenChannels[i].first == trackId && m_hiddenChannels[i].second == channel) {
            m_hiddenChannels.erase(m_hiddenChannels.begin() + (long)i);
            return;
        }
    }
    m_hiddenChannels.push_back({trackId, channel});
}

void TimelinePanel::DrawGraphTab(DirectorHost& host, const Layout& l) {
    AnimationDocument& doc = host.Document();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int selectedId = host.SelectedId();
    const float left = l.Origin.x + l.TrackX;
    const float right = left + l.Width;
    const float top = l.Origin.y + l.RulerH;
    const float bottom = l.Origin.y + l.Height;

    // Показываем кривые ТОЛЬКО выбранного объекта: график всех дорожек сцены
    // сразу — это нечитаемый клубок.
    std::vector<Track*> shown;
    for (Track& track : doc.Tracks) {
        if (track.TargetId == selectedId && !track.Muted) shown.push_back(&track);
    }

    if (shown.empty()) {
        dl->AddText(ImVec2(l.Origin.x + 16.0f, top + 14.0f), Theme::Colors::TextDim,
                    selectedId < 0 ? T("Выберите объект — здесь появятся его кривые.")
                                   : T("У выбранного объекта нет дорожек анимации."));
        return;
    }

    // Автоподбор вертикального масштаба по видимым кривым.
    if (m_graphAutoFit) {
        float lo = 1e9f, hi = -1e9f;
        for (const Track* track : shown) {
            for (const Curve& curve : track->Channels) {
                if (curve.Empty()) continue;
                float cMin, cMax;
                curve.ValueRange(cMin, cMax);
                lo = std::min(lo, cMin);
                hi = std::max(hi, cMax);
            }
        }
        if (lo <= hi) {
            const float pad = std::max((hi - lo) * 0.2f, 0.5f);
            m_valueCenter = (lo + hi) * 0.5f;
            m_valueSpan = (hi - lo) + pad * 2.0f;
        }
    }
    const float valueLo = m_valueCenter - m_valueSpan * 0.5f;
    const float valueHi = m_valueCenter + m_valueSpan * 0.5f;

    // Диапазон, на который растягивается КОНКРЕТНАЯ кривая. В обычном режиме он
    // общий для всех — так видно, что одна величина больше другой. В режиме
    // нормализации у каждой кривой свой: сравнивать величины разных единиц
    // бессмысленно, а видеть форму каждой — необходимо.
    auto rangeOf = [&](const Curve& curve, float& lo, float& hi) {
        if (!m_graphNormalize || curve.Empty()) { lo = valueLo; hi = valueHi; return; }
        curve.ValueRange(lo, hi);
        // Постоянная кривая (все ключи с одним значением) дала бы нулевой
        // размах и деление на ноль — разводим её в полосу вокруг значения.
        const float span = hi - lo;
        if (span < 1e-4f) { lo -= 1.0f; hi += 1.0f; return; }
        const float pad = span * 0.15f;
        lo -= pad;
        hi += pad;
    };
    auto valueToYOf = [&](const Curve& curve, float v) {
        float lo, hi;
        rangeOf(curve, lo, hi);
        const float t = (v - lo) / std::max(hi - lo, 1e-4f);
        return bottom - t * (bottom - top);
    };
    auto yToValueOf = [&](const Curve& curve, float y) {
        float lo, hi;
        rangeOf(curve, lo, hi);
        const float t = (bottom - y) / std::max(bottom - top, 1.0f);
        return lo + t * (hi - lo);
    };
    auto valueToY = [&](float v) {
        const float t = (v - valueLo) / std::max(valueHi - valueLo, 1e-4f);
        return bottom - t * (bottom - top);
    };

    // Горизонтальная сетка значений с подписями. В режиме нормализации её нет:
    // у каждой кривой своя шкала, и одна общая подпись «2.00» врала бы про все
    // кривые, кроме одной. Точные значения там читаются наведением на ключ —
    // подсказка показывает время и значение как есть, без пересчёта.
    if (ImFont* small = Theme::SmallFont()) ImGui::PushFont(small);
    const float valueStep = NiceStep(m_valueSpan, bottom - top);
    for (float v = m_graphNormalize ? valueHi + valueStep // цикл не выполнится
                                    : std::floor(valueLo / valueStep) * valueStep;
         v <= valueHi; v += valueStep) {
        const float y = valueToY(v);
        if (y < top || y > bottom) continue;
        const bool zero = std::fabs(v) < valueStep * 0.01f;
        dl->AddLine(ImVec2(left, y), ImVec2(right, y),
                    zero ? Theme::Colors::Line : Theme::Colors::LineSoft, zero ? 1.4f : 1.0f);
        char label[24];
        std::snprintf(label, sizeof(label), "%.2f", (double)v);
        dl->AddText(ImVec2(l.Origin.x + 8.0f, y - 7.0f), Theme::Colors::TextFaint, label);
    }
    if (Theme::SmallFont()) ImGui::PopFont();

    dl->PushClipRect(ImVec2(left, top), ImVec2(right, bottom), true);

    for (Track* track : shown) {
        const PropertyInfo& info = PropertyInfoOf(track->Prop);
        for (int c = 0; c < track->ChannelCount(); ++c) {
            Curve& curve = track->Channels[(size_t)c];
            if (curve.Empty()) continue;
            if (IsChannelHidden(track->Id, c)) continue;
            const ImU32 color = info.ChannelColors[c];

            // Саму кривую рисуем сэмплированием по пикселям: так на экране
            // видна ИМЕННО та форма, по которой считается анимация, включая
            // ступеньки и разгоны, — а не ломаная между ключами.
            const int steps = (int)std::min(l.Width, 1200.0f);
            ImVec2 prev(0.0f, 0.0f);
            for (int s = 0; s <= steps; ++s) {
                const float t = m_viewStart + (m_viewEnd - m_viewStart) * (float)s / (float)steps;
                const ImVec2 p(TimeToX(l, t), valueToYOf(curve, curve.Evaluate(t)));
                if (s > 0) dl->AddLine(prev, p, color, 1.8f);
                prev = p;
            }

            // Ключи и ручки касательных.
            for (int i = 0; i < curve.Count(); ++i) {
                const Keyframe& key = curve.At(i);
                const ImVec2 p(TimeToX(l, key.Time), valueToYOf(curve, key.Value));
                if (p.x < left - 10.0f || p.x > right + 10.0f) continue;

                const KeyRef ref{track->Id, c, key.Time};
                const bool selected = IsKeySelected(ref);

                if (key.Mode == Interp::Bezier && selected) {
                    // Ручки: длина по времени фиксирована, наклон — касательная.
                    float inTan = 0.0f, outTan = 0.0f;
                    curve.EffectiveTangents(i, inTan, outTan);
                    const float handleSeconds = (m_viewEnd - m_viewStart) * 0.05f;
                    const ImVec2 hIn(TimeToX(l, key.Time - handleSeconds),
                                     valueToYOf(curve, key.Value - inTan * handleSeconds));
                    const ImVec2 hOut(TimeToX(l, key.Time + handleSeconds),
                                      valueToYOf(curve, key.Value + outTan * handleSeconds));
                    dl->AddLine(hIn, p, IM_COL32(255, 255, 255, 120), 1.0f);
                    dl->AddLine(p, hOut, IM_COL32(255, 255, 255, 120), 1.0f);

                    ImGui::PushID(track->Id * 10000 + c * 100 + i);
                    ImGui::SetCursorScreenPos(ImVec2(hIn.x - 5.0f, hIn.y - 5.0f));
                    ImGui::InvisibleButton("##hin", ImVec2(10.0f, 10.0f));
                    if (ImGui::IsItemClicked()) {
                        host.CaptureUndo();
                        m_draggingTangent = -1;
                        m_tangentTrackId = track->Id;
                        m_tangentChannel = c;
                        m_tangentKey = i;
                    }
                    ImGui::SetCursorScreenPos(ImVec2(hOut.x - 5.0f, hOut.y - 5.0f));
                    ImGui::InvisibleButton("##hout", ImVec2(10.0f, 10.0f));
                    if (ImGui::IsItemClicked()) {
                        host.CaptureUndo();
                        m_draggingTangent = 1;
                        m_tangentTrackId = track->Id;
                        m_tangentChannel = c;
                        m_tangentKey = i;
                    }
                    ImGui::PopID();

                    dl->AddCircleFilled(hIn, 3.5f, IM_COL32(255, 255, 255, 200), 10);
                    dl->AddCircleFilled(hOut, 3.5f, IM_COL32(255, 255, 255, 200), 10);
                }

                ImGui::PushID(track->Id * 1000 + c * 100 + i);
                ImGui::SetCursorScreenPos(ImVec2(p.x - 6.0f, p.y - 6.0f));
                ImGui::InvisibleButton("##gk", ImVec2(12.0f, 12.0f));
                const bool hovered = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !track->Locked) {
                    ToggleKeySelection(ref, ImGui::GetIO().KeyCtrl);
                    m_draggingKeys = true;
                    m_graphAutoFit = false; // тащим значение — масштаб не должен убегать
                    m_dragTimeAnchor = XToTime(l, ImGui::GetIO().MousePos.x);
                    host.CaptureUndo();
                }
                if (hovered) {
                    ImGui::SetTooltip("%.3f c → %.3f\n%s", (double)key.Time, (double)key.Value,
                                      InterpName(key.Mode));
                }
                DrawDiamond(dl, p, hovered ? 6.0f : 5.0f, color, selected);
                ImGui::PopID();
            }
        }
    }

    dl->PopClipRect();

    // --- Вертикальное перетаскивание значений выбранных ключей ---
    if (m_draggingKeys && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !m_selectedKeys.empty()) {
        const float dy = ImGui::GetIO().MouseDelta.y;
        if (dy != 0.0f) {
            host.CommitUndo();
            for (const KeyRef& ref : m_selectedKeys) {
                Track* track = doc.TrackById(ref.TrackId);
                if (!track || ref.Channel >= track->ChannelCount()) continue;
                Curve& curve = track->Channels[(size_t)ref.Channel];
                const int index = curve.KeyIndexAt(ref.Time);
                if (index < 0) continue;
                // Перевод пикселей в значения делается ПО СВОЕЙ кривой: в режиме
                // нормализации у каждой свой масштаб, и общий коэффициент увёл
                // бы поворот в градусах на столько же, на сколько позицию в
                // метрах, — то есть в никуда.
                const float dv = yToValueOf(curve, 0.0f) - yToValueOf(curve, dy);
                curve.AtMutable(index).Value += dv;
            }
            host.SetCurrentTime(host.CurrentTime()); // переприменить документ к сцене
        }
    }

    // --- Перетаскивание ручки касательной ---
    if (m_draggingTangent != 0) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_draggingTangent = 0;
        } else if (Track* track = doc.TrackById(m_tangentTrackId)) {
            if (m_tangentChannel < track->ChannelCount()) {
                Curve& curve = track->Channels[(size_t)m_tangentChannel];
                if (m_tangentKey >= 0 && m_tangentKey < curve.Count()) {
                    host.CommitUndo();
                    Keyframe& key = curve.AtMutable(m_tangentKey);
                    const ImVec2 mouse = ImGui::GetIO().MousePos;
                    const float dt = XToTime(l, mouse.x) - key.Time;
                    const float dv = yToValueOf(curve, mouse.y) - key.Value;
                    // Наклон = приращение значения на приращение времени.
                    // Слишком близко к ключу знаменатель вырождается — держим
                    // минимальное плечо, иначе касательная улетает в бесконечность.
                    const float safeDt = std::max(std::fabs(dt), (m_viewEnd - m_viewStart) * 0.005f);
                    const float slope = dv / (m_draggingTangent < 0 ? -safeDt : safeDt);
                    if (m_draggingTangent < 0) key.InTangent = slope;
                    else key.OutTangent = slope;
                    host.SetCurrentTime(host.CurrentTime());
                }
            }
        }
    }
}

// ============================================================================
//  Панель целиком
// ============================================================================

void TimelinePanel::Draw(DirectorHost& host) {
    ImGui::Begin((std::string(T("Таймлайн")) + "###Timeline").c_str());

    AnimationDocument& doc = host.Document();
    const bool simple = host.SimpleMode();

    // --- Транспорт: первая строка панели ---
    // Он относится ко всем трём представлениям сразу (Timeline/Graph/Dope),
    // поэтому стоит НАД вкладками, а не внутри одной из них.
    DrawTransport(host);
    ImGui::Separator();

    // --- Вкладки представлений ---
    if (simple) {
        // В простом режиме вкладок нет: единственный доступный вид — Timeline.
        m_tab = 0;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(T("Таймлайн"));
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::TextDisabled("%s", T("(редактор кривых и Dope Sheet — в продвинутом режиме)"));
    } else if (ImGui::BeginTabBar("##timelineTabs")) {
        const char* kTabs[] = {T("Таймлайн"), T("Редактор кривых"), "Dope Sheet"};
        for (int i = 0; i < 3; ++i) {
            const ImGuiTabItemFlags flags =
                (m_forceTab == i) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(kTabs[i], nullptr, flags)) {
                m_tab = i;
                ImGui::EndTabItem();
            }
        }
        m_forceTab = -1; // флаг действует ровно один кадр
        ImGui::EndTabBar();
    }

    // --- Строка управления над дорожками ---
    if (ImGui::Button(T("+ Дорожка"))) ImGui::OpenPopup("##addTrack");
    if (ImGui::BeginPopup("##addTrack")) {
        DrawAddTrackMenu(host);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputTextWithHint("##trackSearch", T("Поиск дорожек…"), &m_filter);

    if (!simple) {
        ImGui::SameLine();
        if (ImGui::Button(T("Интерполяция")) ) ImGui::OpenPopup("##interp");
        if (ImGui::BeginPopup("##interp")) {
            if (m_selectedKeys.empty()) {
                ImGui::TextDisabled("%s", T("Сначала выберите ключи"));
            } else {
                const Interp modes[] = {Interp::Smooth, Interp::Linear, Interp::Constant,
                                        Interp::EaseIn, Interp::EaseOut, Interp::EaseInOut,
                                        Interp::Bezier};
                for (Interp mode : modes) {
                    if (ImGui::MenuItem(InterpName(mode))) SetSelectedKeysInterp(host, mode);
                }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        DrawKeyOpsMenu(host);
        if (m_tab == 1) {
            ImGui::SameLine();
            DrawGraphMenu(host);
        }
    }

    ImGui::SameLine();
    if (ImGui::Button(T("Вписать"))) { m_viewStart = 0.0f; m_viewEnd = doc.Duration; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Показать весь ролик"));

    // Правый блок «длительность + подгонка»: позиционируем от правого края с
    // запасом под обе кнопки, иначе на узкой панели последняя обрезается.
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 268.0f);
    ImGui::SetNextItemWidth(110.0f);
    float duration = doc.Duration;
    if (ImGui::DragFloat("##duration", &duration, 0.1f, 0.1f, 36000.0f, T("%.2f c"))) {
        host.PushUndo();
        doc.Duration = duration;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Длительность ролика"));
    ImGui::SameLine();
    if (ImGui::SmallButton(T("По содерж."))) {
        const float end = doc.ContentEnd();
        if (end > 0.0f) {
            host.PushUndo();
            doc.Duration = end;
        }
    }

    // --- Область дорожек ---
    ImGui::BeginChild("##tracks", ImVec2(0, 0), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    Layout l;
    l.Origin = ImGui::GetCursorScreenPos();
    l.TrackX = kNameColumn;
    l.Width = std::max(ImGui::GetContentRegionAvail().x - kNameColumn, 40.0f);
    l.Height = std::max(ImGui::GetContentRegionAvail().y, 60.0f);
    l.RulerH = kRulerHeight;
    l.RowH = kRowHeight;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(l.Origin, ImVec2(l.Origin.x + l.TrackX + l.Width, l.Origin.y + l.Height),
                      Theme::Colors::PanelDeep);
    dl->AddLine(ImVec2(l.Origin.x + l.TrackX, l.Origin.y),
                ImVec2(l.Origin.x + l.TrackX, l.Origin.y + l.Height), Theme::Colors::Line, 1.0f);

    DrawRuler(host, l, dl);

    const bool areaHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    HandleZoomPan(host, l, areaHovered);
    HandleVerticalScroll(l);

    switch (m_tab) {
        case 1: DrawGraphTab(host, l); break;
        case 2: DrawDopeTab(host, l); break;
        default: DrawTimelineTab(host, l); break;
    }

    DrawPlayhead(host, l, dl);

    // --- Перетаскивание головки ---
    // Кладём невидимую кнопку на всю область времени ПОСЛЕДНЕЙ, чтобы клики по
    // ключам и блокам достались им, а не ей.
    ImGui::SetCursorScreenPos(ImVec2(l.Origin.x + l.TrackX, l.Origin.y));
    ImGui::InvisibleButton("##scrub", ImVec2(l.Width, l.Height));
    const bool scrubHovered = ImGui::IsItemHovered();
    if (scrubHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // Shift переключает жест на РАМКУ выделения. Именно модификатор, а не
        // отдельная область: перетаскивание головки — главное действие в
        // таймлайне, и отбирать у него пустое место нельзя.
        if (ImGui::GetIO().KeyShift) {
            m_boxSelecting = true;
            m_boxAdditive = ImGui::GetIO().KeyCtrl;
            m_boxStart = m_boxEnd = ImGui::GetIO().MousePos;
            if (!m_boxAdditive) m_selectedKeys.clear();
        } else {
            m_draggingPlayhead = true;
            m_selectedKeys.clear(); // клик по пустому месту снимает выбор ключей
        }
    }

    if (m_boxSelecting) {
        m_boxEnd = ImGui::GetIO().MousePos;
        // Рамку рисуем поверх всего: она — обратная связь жеста, и прятать её
        // за дорожками незачем.
        const ImVec2 a(std::min(m_boxStart.x, m_boxEnd.x), std::min(m_boxStart.y, m_boxEnd.y));
        const ImVec2 b(std::max(m_boxStart.x, m_boxEnd.x), std::max(m_boxStart.y, m_boxEnd.y));
        dl->AddRectFilled(a, b, IM_COL32(80, 160, 235, 40));
        dl->AddRect(a, b, IM_COL32(120, 190, 250, 200), 0.0f, 0, 1.2f);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_boxSelecting = false;
            host.SetStatus(T("Выбрано ключей: ") + std::to_string(m_selectedKeys.size()));
        }
    }
    if (m_draggingPlayhead) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_draggingPlayhead = false;
        } else {
            host.SetCurrentTime(XToTime(l, ImGui::GetIO().MousePos.x));
        }
    }

    // --- Перетаскивание ключей по времени ---
    if (m_draggingKeys) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_draggingKeys = false;
            // Времена ключей после перетаскивания изменились — ссылки в наборе
            // выбора уже не найдут их. Проще сбросить выбор, чем показывать
            // «выбранные» ключи, которых нет.
            m_selectedKeys.clear();
            host.SetCurrentTime(host.CurrentTime());
        } else if (!m_selectedKeys.empty()) {
            const float now = XToTime(l, ImGui::GetIO().MousePos.x);
            float delta = now - m_dragTimeAnchor;
            if (!ImGui::GetIO().KeyAlt) {
                // По умолчанию прилипаем к кадрам: попасть ключом между кадрами
                // почти всегда ошибка. Alt отключает прилипание.
                delta = Playback::SnapToFrame(delta, doc.Fps);
            }
            if (std::fabs(delta) > 1e-5f) {
                host.CommitUndo();
                // Двигаем по одному, обновляя ссылки: MoveKey может поменять
                // порядок ключей внутри кривой.
                for (KeyRef& ref : m_selectedKeys) {
                    Track* track = doc.TrackById(ref.TrackId);
                    if (!track || ref.Channel >= track->ChannelCount()) continue;
                    Curve& curve = track->Channels[(size_t)ref.Channel];
                    const int index = curve.KeyIndexAt(ref.Time);
                    if (index < 0) continue;
                    const float newTime = std::max(curve.At(index).Time + delta, 0.0f);
                    const int moved = curve.MoveKey(index, newTime, curve.At(index).Value);
                    ref.Time = curve.At(moved).Time;
                }
                m_dragTimeAnchor += delta;
                host.SetCurrentTime(host.CurrentTime());
            }
        }
    }

    // --- Перетаскивание блока клипа ---
    if (m_draggingBlockTrack >= 0) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_draggingBlockTrack = -1;
            m_draggingBlockIndex = -1;
        } else {
            ClipTrack* track = nullptr;
            for (ClipTrack& t : doc.ClipTracks) {
                if (t.Id == m_draggingBlockTrack) { track = &t; break; }
            }
            if (track && m_draggingBlockIndex >= 0 && m_draggingBlockIndex < (int)track->Blocks.size()) {
                ClipBlock& block = track->Blocks[(size_t)m_draggingBlockIndex];
                const float now = XToTime(l, ImGui::GetIO().MousePos.x);
                float delta = now - m_dragTimeAnchor;
                if (!ImGui::GetIO().KeyAlt) delta = Playback::SnapToFrame(delta, doc.Fps);
                if (std::fabs(delta) > 1e-5f) {
                    host.CommitUndo();
                    if (m_draggingBlockEdge == 0) {
                        block.Start = std::max(block.Start + delta, 0.0f);
                    } else if (m_draggingBlockEdge < 0) {
                        // Левый край: двигаем начало, сохраняя конец на месте.
                        const float end = block.Start + block.Duration;
                        block.Start = std::min(std::max(block.Start + delta, 0.0f), end - 0.05f);
                        block.Duration = end - block.Start;
                    } else {
                        block.Duration = std::max(block.Duration + delta, 0.05f);
                    }
                    m_dragTimeAnchor += delta;
                    host.SetCurrentTime(host.CurrentTime());
                }
            }
        }
    }

    // --- Клавиши в области таймлайна ---
    if (areaHovered && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) DeleteSelectedKeys(host);
    }

    ImGui::EndChild();
    ImGui::End();
}

} // namespace d3d
