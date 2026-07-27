#pragma once
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

#include "anim/AnimationDocument.h"

namespace d3d {

class DirectorHost;

// ---------------------------------------------------------------------------
// Нижняя область: транспорт + три представления одной и той же анимации.
//
//   Timeline     — дорожки с ключами и блоками клипов, звуковая волна;
//   Graph Editor — кривые значений с ручками касательных;
//   Dope Sheet   — только ромбы ключей, максимально плотно: перетаскивать
//                  тайминг целыми пачками.
//
// Все три работают НАД ОДНИМ документом и делят общий горизонтальный масштаб
// (m_viewStart/m_viewEnd) — переключение вкладки не сбрасывает то, куда
// смотрел аниматор.
//
// В простом режиме показывается только Timeline: остальные два — инструменты
// «доводки», и новичку они мешают больше, чем помогают.
// ---------------------------------------------------------------------------
class TimelinePanel {
public:
    void Draw(DirectorHost& host);

    // Выбрать представление извне (0 Timeline, 1 Graph Editor, 2 Dope Sheet).
    // Одного присваивания m_tab мало: какая вкладка активна, решает сам
    // ImGui-таббар, и переключить её из кода можно только флагом SetSelected на
    // ближайшем кадре — его и запоминает m_forceTab.
    void SetTab(int tab) { m_tab = tab; m_forceTab = tab; }

    // Транспорт рисуется под вьюпортом (как в референсе), а не внутри окна
    // таймлайна — поэтому отдельный метод.
    void DrawTransport(DirectorHost& host);

private:
    // --- Раскладка -------------------------------------------------------
    // Геометрия текущего кадра отрисовки: считается один раз и используется
    // всеми тремя представлениями, чтобы линейка, дорожки и головка совпадали
    // до пикселя.
    struct Layout {
        ImVec2 Origin;       // левый верхний угол области дорожек
        float TrackX = 0.0f; // где кончается колонка имён и начинается время
        float Width = 0.0f;  // ширина временной области
        float Height = 0.0f;
        float RulerH = 22.0f;
        float RowH = 22.0f;
    };

    float TimeToX(const Layout& l, float time) const;
    float XToTime(const Layout& l, float x) const;

    void DrawRuler(DirectorHost& host, const Layout& l, ImDrawList* dl);
    void DrawPlayhead(DirectorHost& host, const Layout& l, ImDrawList* dl);
    void HandleZoomPan(DirectorHost& host, const Layout& l, bool hovered);

    void DrawTimelineTab(DirectorHost& host, const Layout& l);
    void DrawGraphTab(DirectorHost& host, const Layout& l);
    void DrawDopeTab(DirectorHost& host, const Layout& l);

    // Строка с именем дорожки в левой колонке: иконка, подпись, mute/lock.
    // Возвращает высоту строки.
    float DrawTrackHeader(DirectorHost& host, Track& track, const Layout& l, float y,
                          ImDrawList* dl, bool selected);
    void DrawKeysRow(DirectorHost& host, Track& track, int channel, const Layout& l,
                     float y, ImDrawList* dl);
    void DrawClipTrack(DirectorHost& host, ClipTrack& track, const Layout& l, float y,
                       float rowH, ImDrawList* dl, int colorIndex);
    void DrawAudioRow(DirectorHost& host, const Layout& l, float y, float rowH, ImDrawList* dl);
    void DrawCameraRow(DirectorHost& host, const Layout& l, float y, float rowH, ImDrawList* dl);
    // Полоса-заголовок зоны таймлайна. Возвращает её высоту; open переключается
    // кликом по треугольнику.
    float DrawZoneHeader(const Layout& l, float y, ImDrawList* dl, const char* id,
                         const char* label, const char* hint, bool& open);
    // Список камер сцены для меню выбора; помечает текущую галочкой.
    void DrawCameraPicker(DirectorHost& host, int currentId, int cutIndex);
    void DrawAddTrackMenu(DirectorHost& host);

    // --- Выбор ключей ------------------------------------------------------
    struct KeyRef {
        int TrackId = 0;
        int Channel = 0;
        float Time = 0.0f;
        bool operator==(const KeyRef& o) const {
            return TrackId == o.TrackId && Channel == o.Channel &&
                   std::abs(Time - o.Time) <= Curve::kTimeEpsilon;
        }
    };
    bool IsKeySelected(const KeyRef& ref) const;
    void ToggleKeySelection(const KeyRef& ref, bool additive);
    void DeleteSelectedKeys(DirectorHost& host);
    void DrawKeyOpsMenu(DirectorHost& host);
    // Диапазон времени и набор дорожек, покрытые выделением. Обе операции —
    // копирование и растяжение — работают с диапазоном, а не со списком
    // ссылок: так одна и та же реализация обслуживает и рамку, и «весь отрезок».
    bool SelectionRange(float& from, float& to, std::vector<int>& trackIds) const;
    KeyClipboard m_clipboard;
    float m_scaleFactor = 2.0f;

    // --- Рамка выделения ---------------------------------------------------
    // Тянется по пустому месту дорожек и забирает все ключи внутри. Без неё
    // выделить два десятка ключей можно только двадцатью щелчками с Ctrl.
    //
    // Ключи проверяются на попадание ТАМ ЖЕ, где рисуются: их экранные позиции
    // считаются при отрисовке строки, и хранить их отдельным списком значило бы
    // дублировать расчёт и рисковать расхождением.
    bool m_boxSelecting = false;
    bool m_boxAdditive = false;   // Ctrl — добавлять к выделению, а не заменять
    ImVec2 m_boxStart{0.0f, 0.0f};
    ImVec2 m_boxEnd{0.0f, 0.0f};
    // Попадает ли точка в текущую рамку (в экранных координатах).
    bool InSelectionBox(const ImVec2& p) const;
    void SetSelectedKeysInterp(DirectorHost& host, Interp mode);

    // --- Состояние ---------------------------------------------------------
    float m_viewStart = 0.0f;   // левый край видимого участка, сек
    float m_viewEnd = 20.0f;    // правый край
    int m_tab = 0;              // 0 Timeline, 1 Graph, 2 Dope
    int m_forceTab = -1;        // вкладка, которую надо выбрать из кода (-1 — не надо)
    std::string m_filter;
    std::vector<KeyRef> m_selectedKeys;

    bool m_draggingPlayhead = false;
    bool m_draggingKeys = false;
    float m_dragTimeAnchor = 0.0f;  // время под курсором в начале перетаскивания
    int m_draggingBlockTrack = -1;  // id дорожки клипов, чей блок тащат
    int m_draggingBlockIndex = -1;
    int m_draggingBlockEdge = 0;    // 0 — целиком, -1 — левый край, +1 — правый
    int m_draggingCut = -1;         // индекс перетаскиваемой склейки монтажа
    // Свёрнутость зон. Монтаж и анимация — разные занятия, и человек обычно
    // сидит в одном из них: свернув чужую зону, он освобождает место, не теряя
    // содержимого.
    bool m_montageOpen = true;
    bool m_animationOpen = true;

    // --- Вертикальная прокрутка области дорожек ----------------------------
    // Раньше её не было вообще: строки рисовались от линейки вниз, и всё, что
    // не влезло, просто не рисовалось. С двумя десятками каналов у персонажа
    // это означало, что часть анимации недоступна и об этом ничего не
    // сообщается — дорожка не «спрятана», её как будто нет.
    //
    // Колесо над ВРЕМЕННОЙ областью по-прежнему меняет масштаб времени: это
    // главный жест таймлайна, и переучивать ему незачем. Прокрутка повешена
    // на колесо над КОЛОНКОЙ ИМЁН (там масштабировать нечего) и на Ctrl+колесо
    // в любом месте, плюс есть полоса прокрутки, которую можно тащить.
    float m_scrollY = 0.0f;
    float m_contentH = 0.0f;   // высота содержимого, измеренная на прошлом кадре
    bool m_draggingScroll = false;

    // Открывает область дорожек: ставит отсечение и возвращает верх первой
    // строки с учётом прокрутки.
    float BeginTracks(const Layout& l);
    // Закрывает область: запоминает высоту содержимого, рисует полосу.
    void EndTracks(const Layout& l, float yEnd, ImDrawList* dl);
    void HandleVerticalScroll(const Layout& l);

    // Graph Editor: вертикальный масштаб (значения) и какие каналы показывать.
    float m_valueCenter = 0.0f;
    float m_valueSpan = 10.0f;
    bool m_graphAutoFit = true;
    // НОРМАЛИЗАЦИЯ: каждая кривая растягивается на свой размах, а не на общий.
    //
    // Без неё редактор кривых бесполезен ровно там, где он нужнее всего — когда
    // у объекта одновременно ключуются величины разных ЕДИНИЦ. Поворот в 720
    // градусов и позиция в двух метрах на одной оси значений означают, что
    // позиция превращается в плоскую линию у нуля: править её нечем, потому что
    // её формы не видно.
    bool m_graphNormalize = false;
    // Скрытые каналы: пара «дорожка + номер канала». Скрытие — это про ЧТЕНИЕ
    // графика, поэтому живёт в панели, а не в документе: заглушка дорожки
    // (Track::Muted) меняет анимацию, а тут кривая просто не рисуется.
    std::vector<std::pair<int, int>> m_hiddenChannels;
    void DrawGraphMenu(DirectorHost& host);
    void DrawGraphChannelFilter(DirectorHost& host);
    bool IsChannelHidden(int trackId, int channel) const;
    void ToggleChannelHidden(int trackId, int channel);
    int m_draggingTangent = 0;      // 0 нет, -1 входная ручка, +1 выходная
    int m_tangentTrackId = 0, m_tangentChannel = 0, m_tangentKey = -1;
};

} // namespace d3d
