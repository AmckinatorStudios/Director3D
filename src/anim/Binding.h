#pragma once
#include <string>
#include <vector>

#include <glm/glm.hpp>

class Scene;

// ---------------------------------------------------------------------------
// Привязка анимации к сцене.
//
// Дорожка анимации не хранит указателей на компоненты — она хранит ЧТО именно
// анимируется: id сущности + идентификатор свойства (Property). Здесь описано,
// как это свойство прочитать из сцены и как записать обратно. Один слой
// косвенности решает сразу три задачи:
//
//   • проект переживает пересоздание сцены (загрузка .d3dproj, undo, откат
//     после предпросмотра) — id стабильны, указатели нет;
//   • «поставить ключ» и «применить кадр» — это Read и Write одного и того же
//     дескриптора, поэтому они не могут разъехаться;
//   • добавить новое анимируемое свойство = одна строка в таблице ниже, без
//     правок таймлайна, редактора кривых и сериализатора.
// ---------------------------------------------------------------------------
namespace d3d {

enum class Property {
    Position,           // Transform.Position           (X/Y/Z)
    Rotation,           // Transform.Rotation, градусы  (X/Y/Z)
    Scale,              // Transform.Scale              (X/Y/Z)
    Color,              // MeshRenderer.Color           (R/G/B)
    LightColor,         // LightComponent.Color         (R/G/B)
    LightIntensity,     // LightComponent.Intensity
    LightRange,         // LightComponent.Range
    CameraFov,          // CameraComponent.Fov
    CameraFocusDistance,// CineCamera.FocusDistance
    CameraAperture,     // CineCamera.Aperture
    PostBloom,          // CineCamera.BloomIntensity
    PostVignette,       // CineCamera.VignetteAmount
    PostMotionBlur,     // CineCamera.MotionBlurAmount
    PostChromatic,      // CineCamera.ChromaticAmount
    Visibility,         // StageItem.Visible (0/1, ступенчато)

    // --- Кости персонажа ---
    // Привязываются к паре «сущность + индекс кости» (Track::Joint), поэтому у
    // одной сущности этих дорожек столько, сколько костей анимирует аниматор.
    // Значения — ЛОКАЛЬНЫЕ, относительно родительской кости: так же, как их
    // хранит клип, и поэтому ручная поза и клип складываются без пересчётов.
    BonePosition,       // локальный перенос кости      (X/Y/Z)
    BoneRotation,       // локальный поворот, градусы   (X/Y/Z)
    BoneScale,          // локальный масштаб кости      (X/Y/Z)

    // Вес блендшейпа. Как и кости, привязывается к паре «сущность + номер», и
    // номер живёт в том же поле Track::Joint: это ПОДЫНДЕКС внутри цели, а не
    // обязательно кость. Заводить второе такое же поле только ради имени
    // означало бы дублировать всю обвязку — поиск дорожек, сериализацию, UI.
    MorphWeight,        // AnimatedModel.MorphWeights[i], 0..1
};

// Свойство относится к морф-цели (блендшейпу)? У таких дорожек Track::Joint —
// номер цели в SkinnedModel::MorphNames().
bool IsMorphProperty(Property prop);

// Свойство относится к кости, а не к самой сущности? У таких дорожек значим
// Track::Joint, и в интерфейсе они живут под скелетом, а не под объектом.
bool IsBoneProperty(Property prop);

// Адресуется ли свойство ПОДЫНДЕКСОМ (номер кости или номер морф-цели). Один
// вопрос вместо двух проверок подряд — и это не косметика: раньше «завести
// дорожку» спрашивало про оба вида, а сохранение — только про кости, и номер
// морф-цели молча терялся при записи в файл. Общий предикат делает такое
// расхождение невозможным.
bool HasSubIndex(Property prop);

// Описание свойства: имя для UI, число каналов, подписи каналов, цвета каналов
// в редакторе кривых, режим интерполяции по умолчанию.
struct PropertyInfo {
    Property Id;
    const char* Key;            // стабильный ключ для файла проекта
    const char* Label;          // подпись в таймлайне/меню
    int Channels;               // 1 или 3
    const char* ChannelLabels[3];
    unsigned int ChannelColors[3]; // ImU32 (0xAABBGGRR — порядок ImGui)
    bool Stepped;               // true — ключи по умолчанию ступенчатые
};

// Таблица всех анимируемых свойств. Единственный источник правды: и меню
// «Добавить дорожку», и загрузка проекта, и подписи каналов идут отсюда.
const std::vector<PropertyInfo>& PropertyTable();
const PropertyInfo& PropertyInfoOf(Property prop);
// Разбор ключа из файла проекта. false — свойства с таким ключом нет (проект
// от более новой версии): дорожка молча пропускается, остальные грузятся.
bool PropertyFromKey(const std::string& key, Property& out);

// Есть ли у сущности то, что нужно этому свойству (свет — у сущности со светом,
// FOV — у камеры). Меню «Добавить дорожку» показывает только применимое, а
// загрузка проекта отсекает дорожки, повисшие после удаления компонента.
bool PropertyApplies(Scene& scene, int entityId, Property prop, int joint = -1);

// Читает текущее значение свойства сущности в values (Channels чисел).
// false — сущности нет или свойство к ней неприменимо (values не трогается).
bool ReadProperty(Scene& scene, int entityId, Property prop, float* values, int joint = -1);

// Пишет значение свойства в сущность. false — записать некуда.
bool WriteProperty(Scene& scene, int entityId, Property prop, const float* values, int joint = -1);

// Список свойств, применимых к сущности прямо сейчас (для меню и авто-ключа).
std::vector<Property> ApplicableProperties(Scene& scene, int entityId);

} // namespace d3d
