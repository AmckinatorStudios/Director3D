#include "SelfTest.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "anim/AnimationDocument.h"
#include "anim/BonePose.h"
#include "anim/DirectorComponents.h"
#include "anim/Playback.h"
#include "project/Project.h"
#include "project/UndoStack.h"
#include "render/VideoWriter.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Transform.h"

namespace fs = std::filesystem;

namespace d3d {

namespace {

int g_passed = 0;
int g_failed = 0;

void Check(bool condition, const char* what) {
    if (condition) {
        ++g_passed;
    } else {
        ++g_failed;
        std::printf("  ПРОВАЛ: %s\n", what);
    }
}

void CheckNear(float actual, float expected, float tolerance, const char* what) {
    const bool ok = std::fabs(actual - expected) <= tolerance;
    if (!ok) std::printf("  ПРОВАЛ: %s (получено %.6f, ожидалось %.6f)\n", what,
                         (double)actual, (double)expected);
    ok ? ++g_passed : ++g_failed;
}

void Section(const char* name) {
    std::printf("\n[%s]\n", name);
}

// Сцена БЕЗ GPU-мешей: MeshRef::Type::None не трогает ResourceManager, поэтому
// сериализация и загрузка работают без графического контекста.
std::unique_ptr<Scene> MakeHeadlessScene(int& outCubeId, int& outCameraId, int& outLightId) {
    auto scene = std::make_unique<Scene>("SelfTest");

    GameObject cube = scene->CreateObject("Cube");
    cube.GetTransform().Position = {1.0f, 2.0f, 3.0f};
    outCubeId = cube.Id();

    GameObject camera = scene->CreateObject("Camera");
    scene->Registry().emplace<CameraComponent>(camera.Entity(), CameraComponent{});
    scene->Registry().emplace<CineCameraComponent>(camera.Entity(), CineCameraComponent{});
    outCameraId = camera.Id();

    GameObject light = scene->CreateObject("Light");
    scene->Registry().emplace<LightComponent>(light.Entity(), LightComponent{});
    outLightId = light.Id();

    return scene;
}

// --- Кривые ---------------------------------------------------------------

void TestCurves() {
    Section("Кривые");

    Curve curve;
    Check(curve.Empty(), "новая кривая пуста");
    CheckNear(curve.Evaluate(1.0f, 42.0f), 42.0f, 1e-5f, "пустая кривая отдаёт значение по умолчанию");

    curve.SetKey(0.0f, 0.0f, Interp::Linear);
    curve.SetKey(2.0f, 10.0f, Interp::Linear);
    Check(curve.Count() == 2, "поставились два ключа");
    CheckNear(curve.Evaluate(1.0f), 5.0f, 1e-4f, "линейная интерполяция посередине");
    CheckNear(curve.Evaluate(-5.0f), 0.0f, 1e-4f, "до первого ключа держится первое значение");
    CheckNear(curve.Evaluate(99.0f), 10.0f, 1e-4f, "после последнего держится последнее значение");

    // Повторный ключ на том же времени обновляет значение, а не плодит дубль.
    curve.SetKey(2.0f, 20.0f);
    Check(curve.Count() == 2, "ключ на занятом времени не создаёт дубликат");
    CheckNear(curve.Evaluate(2.0f), 20.0f, 1e-4f, "значение ключа перезаписалось");

    // Ступенька держит значение до следующего ключа.
    Curve stepped;
    stepped.SetKey(0.0f, 1.0f, Interp::Constant);
    stepped.SetKey(1.0f, 5.0f, Interp::Constant);
    CheckNear(stepped.Evaluate(0.99f), 1.0f, 1e-4f, "ступенька держит значение до конца сегмента");
    CheckNear(stepped.Evaluate(1.0f), 5.0f, 1e-4f, "ступенька переключается на ключе");

    // Сглаженная кривая проходит ЧЕРЕЗ ключи (это и отличает интерполяцию от
    // аппроксимации) и не выходит за них на монотонном участке.
    Curve smooth;
    smooth.SetKey(0.0f, 0.0f, Interp::Smooth);
    smooth.SetKey(1.0f, 1.0f, Interp::Smooth);
    smooth.SetKey(2.0f, 2.0f, Interp::Smooth);
    CheckNear(smooth.Evaluate(0.0f), 0.0f, 1e-4f, "сглаженная проходит через первый ключ");
    CheckNear(smooth.Evaluate(1.0f), 1.0f, 1e-4f, "сглаженная проходит через средний ключ");
    CheckNear(smooth.Evaluate(2.0f), 2.0f, 1e-4f, "сглаженная проходит через последний ключ");
    CheckNear(smooth.Evaluate(0.5f), 0.5f, 1e-3f, "на равномерных ключах сглаживание = прямая");

    // Разгон отстаёт от прямой в начале, торможение — опережает.
    Curve easeIn, easeOut;
    easeIn.SetKey(0.0f, 0.0f, Interp::EaseIn);
    easeIn.SetKey(1.0f, 1.0f, Interp::EaseIn);
    easeOut.SetKey(0.0f, 0.0f, Interp::EaseOut);
    easeOut.SetKey(1.0f, 1.0f, Interp::EaseOut);
    Check(easeIn.Evaluate(0.5f) < 0.5f, "плавный старт отстаёт от линейного");
    Check(easeOut.Evaluate(0.5f) > 0.5f, "плавная остановка опережает линейный");

    // Поиск ключа по времени и перемещение с сохранением сортировки.
    Check(curve.KeyIndexAt(2.0f) == 1, "ключ находится по времени");
    Check(curve.KeyIndexAt(1.0f) == -1, "на пустом времени ключа нет");
    const int moved = curve.MoveKey(0, 5.0f, 7.0f);
    Check(moved == 1, "перетащенный за соседа ключ переехал в конец");
    Check(curve.At(0).Time < curve.At(1).Time, "порядок ключей сохранился");

    // Безье с ручными касательными считается по эрмиту, а не по прямой.
    Curve bezier;
    bezier.SetKey(0.0f, 0.0f, Interp::Bezier);
    bezier.SetKey(1.0f, 1.0f, Interp::Bezier);
    bezier.AtMutable(0).OutTangent = 3.0f;
    bezier.AtMutable(1).InTangent = 0.0f;
    Check(bezier.Evaluate(0.5f) > 0.5f, "касательная на выходе поднимает кривую");

    // Битый файл: неотсортированные ключи с дубликатами чинятся Normalize.
    Curve messy;
    messy.KeysMutable().push_back(Keyframe{2.0f, 20.0f, Interp::Linear, 0.0f, 0.0f});
    messy.KeysMutable().push_back(Keyframe{0.0f, 0.0f, Interp::Linear, 0.0f, 0.0f});
    messy.KeysMutable().push_back(Keyframe{2.0f, 99.0f, Interp::Linear, 0.0f, 0.0f});
    messy.Normalize();
    Check(messy.Count() == 2, "дубликаты по времени схлопнулись");
    Check(messy.At(0).Time < messy.At(1).Time, "ключи отсортировались");
}

// --- Транспорт -------------------------------------------------------------

void TestPlayback() {
    Section("Транспорт");

    Check(Playback::Timecode(0.0f, 24.0f) == "00:00:00:00", "нулевой таймкод");
    Check(Playback::Timecode(4.5f, 24.0f) == "00:00:04:12", "таймкод 4.5 c при 24 fps");
    Check(Playback::Timecode(3661.0f, 25.0f) == "01:01:01:00", "часы и минуты в таймкоде");
    // Граница секунды не должна показывать 23-й кадр предыдущей.
    Check(Playback::Timecode(1.0f, 24.0f) == "00:00:01:00", "ровная секунда без съезда назад");

    float parsed = 0.0f;
    Check(Playback::ParseTimecode("00:00:04:12", 24.0f, parsed), "полный таймкод разобран");
    CheckNear(parsed, 4.5f, 1e-4f, "разбор полного таймкода");
    Check(Playback::ParseTimecode("48", 24.0f, parsed), "номер кадра разобран");
    CheckNear(parsed, 2.0f, 1e-4f, "разбор номера кадра");
    Check(!Playback::ParseTimecode("не время", 24.0f, parsed), "мусор не разбирается");

    CheckNear(Playback::SnapToFrame(0.51f, 24.0f), 12.0f / 24.0f, 1e-4f, "прилипание к кадру");

    Playback transport;
    transport.Loop = false;
    transport.Play();
    Check(transport.Advance(1.0f, 5.0f), "проигрывание двигает головку");
    CheckNear(transport.Time(), 1.0f, 1e-4f, "головка сдвинулась на dt");
    transport.Advance(10.0f, 5.0f);
    CheckNear(transport.Time(), 5.0f, 1e-4f, "без цикла головка встаёт на конец");
    Check(!transport.Playing(), "без цикла проигрывание останавливается на конце");

    Playback looping;
    looping.Loop = true;
    looping.Play();
    looping.Advance(7.0f, 5.0f);
    CheckNear(looping.Time(), 2.0f, 1e-4f, "цикл заворачивает время");
    Check(looping.Seeking(), "заворот помечается как разрыв непрерывности");

    looping.Stop();
    CheckNear(looping.Time(), 0.0f, 1e-4f, "стоп возвращает в начало");
    Check(!looping.Playing(), "стоп останавливает проигрывание");
}

// --- Документ и применение к сцене ------------------------------------------

void TestDocument() {
    Section("Документ анимации");

    int cubeId = 0, cameraId = 0, lightId = 0;
    std::unique_ptr<Scene> scene = MakeHeadlessScene(cubeId, cameraId, lightId);

    AnimationDocument doc;
    Check(doc.Empty(), "новый документ пуст");

    // Ключ снимается с ТЕКУЩЕГО состояния сцены.
    Check(doc.KeyFromScene(*scene, cubeId, Property::Position, 0.0f), "ключ позиции поставлен");
    Check(doc.Tracks.size() == 1, "дорожка создана");
    Check(doc.Tracks[0].ChannelCount() == 3, "у позиции три канала");
    CheckNear(doc.Tracks[0].Channels[0].At(0).Value, 1.0f, 1e-4f, "в ключ попало текущее X");

    // Второй ключ и применение документа к сцене.
    scene->Get(cubeId).GetTransform().Position = {5.0f, 2.0f, 3.0f};
    doc.KeyFromScene(*scene, cubeId, Property::Position, 2.0f);
    doc.Apply(*scene, 1.0f, /*seeking=*/true);
    CheckNear(scene->Get(cubeId).GetTransform().Position.x, 3.0f, 1e-3f,
              "документ записал промежуточное значение в сцену");

    // Дорожка без ключей НЕ должна перетирать сцену нулями.
    AnimationDocument empty;
    empty.EnsureTrack(cubeId, Property::Scale);
    scene->Get(cubeId).GetTransform().Scale = {2.0f, 2.0f, 2.0f};
    empty.Apply(*scene, 1.0f, true);
    CheckNear(scene->Get(cubeId).GetTransform().Scale.x, 2.0f, 1e-4f,
              "пустая дорожка не трогает сцену");

    // Канал без ключей берёт значение из сцены, а не ноль.
    AnimationDocument partial;
    Track& track = partial.EnsureTrack(cubeId, Property::Position);
    track.Channels[0].SetKey(0.0f, 0.0f, Interp::Linear);
    track.Channels[0].SetKey(1.0f, 10.0f, Interp::Linear);
    scene->Get(cubeId).GetTransform().Position = {0.0f, 7.0f, 9.0f};
    partial.Apply(*scene, 0.5f, true);
    CheckNear(scene->Get(cubeId).GetTransform().Position.x, 5.0f, 1e-3f, "канал с ключами анимируется");
    CheckNear(scene->Get(cubeId).GetTransform().Position.y, 7.0f, 1e-4f, "канал без ключей сохраняет сцену");

    // Заглушенная дорожка не применяется.
    partial.Tracks[0].Muted = true;
    scene->Get(cubeId).GetTransform().Position.x = 123.0f;
    partial.Apply(*scene, 0.5f, true);
    CheckNear(scene->Get(cubeId).GetTransform().Position.x, 123.0f, 1e-4f,
              "заглушенная дорожка не пишет в сцену");

    // Свойства разных типов компонентов.
    Check(PropertyApplies(*scene, lightId, Property::LightIntensity), "свет принимает интенсивность");
    Check(!PropertyApplies(*scene, cubeId, Property::LightIntensity), "куб не принимает интенсивность света");
    Check(PropertyApplies(*scene, cameraId, Property::CameraFov), "камера принимает FOV");
    Check(!PropertyApplies(*scene, cubeId, Property::CameraFov), "куб не принимает FOV");

    // Запись FOV зажимается в рабочий диапазон, а не ломает матрицу проекции.
    AnimationDocument camDoc;
    Track& fov = camDoc.EnsureTrack(cameraId, Property::CameraFov);
    fov.Channels[0].SetKey(0.0f, 500.0f, Interp::Linear);
    camDoc.Apply(*scene, 0.0f, true);
    const CameraComponent& cam = scene->Registry().get<CameraComponent>(scene->Get(cameraId).Entity());
    Check(cam.Fov <= 179.0f && cam.Fov >= 1.0f, "FOV зажат в рабочий диапазон");

    // Киношные параметры камеры анимируются наравне с обычными: именно они
    // управляют глубиной резкости, смазом и хроматической аберрацией в кадре.
    for (Property prop : {Property::CameraFocusDistance, Property::CameraAperture,
                          Property::PostMotionBlur, Property::PostChromatic}) {
        Check(PropertyApplies(*scene, cameraId, prop), "камера принимает киношный параметр");
        Check(!PropertyApplies(*scene, cubeId, prop), "не-камера киношный параметр не принимает");
    }

    // Перевод фокуса: ключи дистанции должны доезжать до компонента камеры,
    // иначе анимация фокуса осталась бы «красивой кривой ни о чём».
    AnimationDocument focusDoc;
    Track& focusTrack = focusDoc.EnsureTrack(cameraId, Property::CameraFocusDistance);
    focusTrack.Channels[0].SetKey(0.0f, 20.0f, Interp::Linear);
    focusTrack.Channels[0].SetKey(2.0f, 5.0f, Interp::Linear);
    focusDoc.Apply(*scene, 1.0f, true);
    const CineCameraComponent& cine =
        scene->Registry().get<CineCameraComponent>(scene->Get(cameraId).Entity());
    CheckNear(cine.FocusDistance, 12.5f, 1e-3f, "дистанция фокуса анимируется");

    // Значения эффектов зажимаются в рабочий диапазон: кривая легко уводит
    // значение за края, а отрицательный смаз или диафрагма — это мусор в шейдер.
    AnimationDocument clampDoc;
    Track& mb = clampDoc.EnsureTrack(cameraId, Property::PostMotionBlur);
    mb.Channels[0].SetKey(0.0f, 5.0f, Interp::Linear);
    Track& ap = clampDoc.EnsureTrack(cameraId, Property::CameraAperture);
    ap.Channels[0].SetKey(0.0f, -3.0f, Interp::Linear);
    clampDoc.Apply(*scene, 0.0f, true);
    Check(cine.MotionBlurAmount <= 1.0f, "сила смаза зажата сверху");
    Check(cine.Aperture >= 0.7f, "диафрагма зажата снизу");

    // Поиск соседних ключей — кнопки «предыдущий/следующий ключ».
    AnimationDocument nav;
    Track& navTrack = nav.EnsureTrack(cubeId, Property::Position);
    navTrack.Channels[0].SetKey(1.0f, 0.0f);
    navTrack.Channels[0].SetKey(3.0f, 1.0f);
    float t = 0.0f;
    Check(nav.NextKeyTime(0.0f, t) && std::fabs(t - 1.0f) < 1e-4f, "следующий ключ найден");
    Check(nav.PrevKeyTime(5.0f, t) && std::fabs(t - 3.0f) < 1e-4f, "предыдущий ключ найден");
    Check(!nav.NextKeyTime(10.0f, t), "за последним ключом следующего нет");

    // Удаление объекта убирает его дорожки.
    nav.RemoveTracksOf(cubeId);
    Check(nav.Tracks.empty(), "дорожки удалённого объекта убраны");

    // Конец содержимого — по самому позднему ключу.
    AnimationDocument endDoc;
    Track& endTrack = endDoc.EnsureTrack(cubeId, Property::Position);
    endTrack.Channels[1].SetKey(7.5f, 1.0f);
    CheckNear(endDoc.ContentEnd(), 7.5f, 1e-4f, "конец содержимого по последнему ключу");
}

// --- Отмена ----------------------------------------------------------------

void TestUndo() {
    Section("Отмена и повтор");

    UndoStack undo;
    Check(!undo.CanUndo() && !undo.CanRedo(), "пустой стек ничего не умеет");

    undo.Push("A");
    undo.Push("B");
    Check(undo.CanUndo(), "после правок отмена доступна");

    std::string out;
    Check(undo.Undo("C", out) && out == "B", "отмена возвращает предыдущее состояние");
    Check(undo.CanRedo(), "после отмены доступен повтор");
    Check(undo.Redo("B", out) && out == "C", "повтор возвращает отменённое");

    // Новая правка после отмены обрывает будущее.
    undo.Undo("C", out);
    undo.Push("D");
    Check(!undo.CanRedo(), "новая правка стирает ветку повтора");

    // Пара Capture/Commit кладёт ОДИН шаг на весь жест.
    UndoStack gesture;
    gesture.Capture("before");
    gesture.Capture("ignored"); // повторный захват внутри жеста игнорируется
    gesture.Commit();
    Check(gesture.UndoDepth() == 1, "жест даёт один шаг отмены");
    Check(gesture.Undo("after", out) && out == "before", "жест откатывается к состоянию до начала");

    // Жест без изменений не должен оставлять шаг.
    UndoStack dropped;
    dropped.Capture("x");
    dropped.DropPending();
    Check(!dropped.CanUndo(), "жест без изменений не создаёт шаг отмены");

    // Глубина ограничена — старое вытесняется, память не растёт бесконечно.
    UndoStack deep;
    for (size_t i = 0; i < UndoStack::kMaxDepth + 20; ++i) deep.Push(std::to_string(i));
    Check(deep.UndoDepth() == UndoStack::kMaxDepth, "глубина стека ограничена");
}

// --- Файл проекта -----------------------------------------------------------

void TestProjectIO() {
    Section("Файл проекта");

    int cubeId = 0, cameraId = 0, lightId = 0;
    std::unique_ptr<Scene> scene = MakeHeadlessScene(cubeId, cameraId, lightId);

    AnimationDocument doc;
    doc.Name = "SelfTestProject";
    doc.Fps = 30.0f;
    doc.Duration = 12.5f;
    Track& track = doc.EnsureTrack(cubeId, Property::Position);
    track.Channels[0].SetKey(0.0f, 0.0f, Interp::EaseInOut);
    track.Channels[0].SetKey(2.0f, 8.0f, Interp::Bezier);
    track.Channels[0].AtMutable(1).InTangent = 2.5f;
    doc.Markers.push_back(Marker{"Удар", 1.25f, 0xFF3FC8E8});

    ClipTrack& clips = doc.EnsureClipTrack(cubeId);
    clips.Blocks.push_back(ClipBlock{"Walk", 1, 0.5f, 2.0f, 1.5f, 0.3f, true});

    // Киношные параметры камеры — свои компоненты, отдельный раздел файла.
    CineCameraComponent& cine =
        scene->Registry().get<CineCameraComponent>(scene->Get(cameraId).Entity());
    cine.Aperture = 1.4f;
    cine.FocusDistance = 7.25f;
    cine.Vignette = false;
    scene->Registry().emplace<StageItemComponent>(scene->Get(lightId).Entity(),
                                                  StageItemComponent{false, true});

    // --- Снимок в память (это же используется undo) ---
    const std::string snapshot = ProjectFile::SnapshotToString(*scene, doc);
    Check(!snapshot.empty(), "снимок состояния собрался");

    std::unique_ptr<Scene> restored;
    AnimationDocument restoredDoc;
    std::string err;
    Check(ProjectFile::RestoreFromString(snapshot, restored, restoredDoc, err),
          "снимок восстановился");
    if (restored) {
        Check(restored->Count() == scene->Count(), "число объектов совпало");
        CheckNear(restored->Get(cubeId).GetTransform().Position.x, 1.0f, 1e-4f, "трансформ пережил снимок");
    }
    Check(restoredDoc.Tracks.size() == 1, "дорожка пережила снимок");
    if (!restoredDoc.Tracks.empty()) {
        const Curve& curve = restoredDoc.Tracks[0].Channels[0];
        Check(curve.Count() == 2, "оба ключа пережили снимок");
        Check(curve.At(0).Mode == Interp::EaseInOut, "режим интерполяции пережил снимок");
        Check(curve.At(1).Mode == Interp::Bezier, "режим Безье пережил снимок");
        CheckNear(curve.At(1).InTangent, 2.5f, 1e-4f, "касательная пережила снимок");
    }
    Check(restoredDoc.ClipTracks.size() == 1, "дорожка клипов пережила снимок");
    if (!restoredDoc.ClipTracks.empty() && !restoredDoc.ClipTracks[0].Blocks.empty()) {
        const ClipBlock& block = restoredDoc.ClipTracks[0].Blocks[0];
        Check(block.Name == "Walk", "имя блока клипа сохранилось");
        CheckNear(block.Speed, 1.5f, 1e-4f, "скорость блока сохранилась");
    }
    Check(restoredDoc.Markers.size() == 1, "метка пережила снимок");

    // --- Файл на диске ---
    const fs::path path = fs::temp_directory_path() / "director3d_selftest.d3dproj";

    // Настройки рабочего вида уходят в файл вместе с проектом: размер площадки
    // — часть постановки, и открывать сцену с чужой сеткой аниматор не должен.
    ViewportOverlays view;
    view.Grid = true;
    view.Thirds = true;
    view.SafeArea = false;
    view.GridConfig.Mode = sage::render::GridSettings::Extent::Radius;
    view.GridConfig.Radius = 17.5f;
    view.GridConfig.CellSize = 0.25f;
    view.GridConfig.MajorEvery = 8;
    view.GridConfig.ShowAxes = false;
    view.GridConfig.Opacity = 0.6f;
    Check(ProjectFile::Save(path.string(), *scene, doc, 3.75f, err, &view),
          "проект сохранился в файл");

    std::unique_ptr<Scene> loaded;
    AnimationDocument loadedDoc;
    float playhead = 0.0f;
    ViewportOverlays loadedView;
    Check(ProjectFile::Load(path.string(), loaded, loadedDoc, playhead, err, &loadedView),
          "проект загрузился из файла");
    Check(loadedView.GridConfig.Mode == sage::render::GridSettings::Extent::Radius,
          "режим сетки сохранился");
    CheckNear(loadedView.GridConfig.Radius, 17.5f, 1e-4f, "радиус сетки сохранился");
    CheckNear(loadedView.GridConfig.CellSize, 0.25f, 1e-4f, "шаг клетки сохранился");
    Check(loadedView.GridConfig.MajorEvery == 8, "период крупных линий сохранился");
    Check(!loadedView.GridConfig.ShowAxes, "выключенные оси сетки сохранились");
    CheckNear(loadedView.GridConfig.Opacity, 0.6f, 1e-4f, "прозрачность сетки сохранилась");
    Check(loadedView.Thirds && !loadedView.SafeArea, "переключатели направляющих сохранились");

    // Проект БЕЗ раздела "viewport" (старый файл или экспорт чужой программой)
    // не должен обнулять текущие настройки вида — их просто нечем заменить.
    {
        std::string bareErr;
        const fs::path bare = fs::temp_directory_path() / "director3d_selftest_bare.d3dproj";
        Check(ProjectFile::Save(bare.string(), *scene, doc, 0.0f, bareErr),
              "проект без настроек вида сохранился");
        std::unique_ptr<Scene> bareScene;
        AnimationDocument bareDoc;
        float barePlayhead = 0.0f;
        ViewportOverlays keep;
        keep.GridConfig.CellSize = 3.0f;
        Check(ProjectFile::Load(bare.string(), bareScene, bareDoc, barePlayhead, bareErr, &keep),
              "проект без настроек вида загрузился");
        CheckNear(keep.GridConfig.CellSize, 3.0f, 1e-4f,
                  "старый проект не сбросил настройки сетки");
        fs::remove(bare);
    }
    CheckNear(playhead, 3.75f, 1e-4f, "положение головки сохранилось");
    CheckNear(loadedDoc.Fps, 30.0f, 1e-4f, "частота кадров сохранилась");
    CheckNear(loadedDoc.Duration, 12.5f, 1e-4f, "длительность сохранилась");
    Check(loadedDoc.Name == "SelfTestProject", "имя проекта сохранилось");

    if (loaded) {
        // Свои компоненты должны пережить круг через файл — иначе настройки
        // камеры и видимость молча терялись бы при каждом сохранении.
        const auto* loadedCine =
            loaded->Registry().try_get<CineCameraComponent>(loaded->Get(cameraId).Entity());
        Check(loadedCine != nullptr, "киношная камера пережила файл");
        if (loadedCine) {
            CheckNear(loadedCine->Aperture, 1.4f, 1e-4f, "диафрагма сохранилась");
            CheckNear(loadedCine->FocusDistance, 7.25f, 1e-4f, "дистанция фокуса сохранилась");
            Check(!loadedCine->Vignette, "выключенная виньетка сохранилась");
        }
        const auto* item =
            loaded->Registry().try_get<StageItemComponent>(loaded->Get(lightId).Entity());
        Check(item != nullptr && !item->Visible && item->Locked,
              "скрытость и блокировка объекта сохранились");
    }

    // Битый файл не должен ронять приложение.
    std::unique_ptr<Scene> broken;
    AnimationDocument brokenDoc;
    Check(!ProjectFile::RestoreFromString("{ это не json", broken, brokenDoc, err),
          "битый JSON отвергается без падения");
    Check(!ProjectFile::RestoreFromString("{\"format\":\"x\"}", broken, brokenDoc, err),
          "JSON без сцены отвергается");

    std::error_code ec;
    fs::remove(path, ec);
}

// --- Разбор звука -----------------------------------------------------------

void TestAudioDecoding() {
    Section("Разбор WAV");

    // Собираем корректный 16-битный моно-WAV на 100 сэмплов: так проверка не
    // зависит ни от каких файлов на диске.
    const int sampleRate = 8000;
    const int samples = 100;
    std::vector<unsigned char> wav;
    auto push32 = [&wav](unsigned int v) {
        wav.push_back((unsigned char)(v & 0xFF));
        wav.push_back((unsigned char)((v >> 8) & 0xFF));
        wav.push_back((unsigned char)((v >> 16) & 0xFF));
        wav.push_back((unsigned char)((v >> 24) & 0xFF));
    };
    auto push16 = [&wav](unsigned short v) {
        wav.push_back((unsigned char)(v & 0xFF));
        wav.push_back((unsigned char)((v >> 8) & 0xFF));
    };
    auto pushTag = [&wav](const char* tag) {
        for (int i = 0; i < 4; ++i) wav.push_back((unsigned char)tag[i]);
    };

    const unsigned int dataBytes = (unsigned int)samples * 2u;
    pushTag("RIFF");
    push32(36u + dataBytes);
    pushTag("WAVE");
    pushTag("fmt ");
    push32(16u);
    push16(1);                      // PCM
    push16(1);                      // моно
    push32((unsigned int)sampleRate);
    push32((unsigned int)sampleRate * 2u); // байт в секунду
    push16(2);                      // выравнивание блока
    push16(16);                     // бит на сэмпл
    pushTag("data");
    push32(dataBytes);
    for (int i = 0; i < samples; ++i) {
        const short value = (short)((i % 2 == 0) ? 16384 : -16384);
        push16((unsigned short)value);
    }

    std::vector<float> mono;
    int rate = 0;
    Check(AudioTrack::DecodeWav(wav, mono, rate), "корректный WAV разобран");
    Check(rate == sampleRate, "частота дискретизации прочитана");
    Check((int)mono.size() == samples, "прочитано верное число сэмплов");
    if (!mono.empty()) {
        CheckNear(mono[0], 0.5f, 1e-3f, "положительный сэмпл нормирован");
        CheckNear(mono[1], -0.5f, 1e-3f, "отрицательный сэмпл нормирован");
    }

    // Мусор и обрезанный файл не должны падать.
    std::vector<unsigned char> garbage(64, 0xAB);
    Check(!AudioTrack::DecodeWav(garbage, mono, rate), "мусор не принимается за WAV");
    std::vector<unsigned char> truncated(wav.begin(), wav.begin() + 30);
    Check(!AudioTrack::DecodeWav(truncated, mono, rate), "обрезанный заголовок отвергается");
}

// ---------------------------------------------------------------------------
// Команда кодировщика. Сам ffmpeg в самотесте не запускается (его может не быть
// на машине сборки), но команда — это то, что ломается тише всего: перепутанный
// порядок опций или неэкранированный путь дают либо мусорный ролик, либо
// выполнение чужой команды. Поэтому строку проверяем целиком.
// ---------------------------------------------------------------------------
void TestVideoCommand() {
    std::printf("\n[Кодировщик видео]\n");

    VideoWriter::Settings s;
    s.OutputPath = "render/shot.mp4";
    s.Width = 1920;
    s.Height = 1080;
    s.Fps = 24.0f;
    s.Quality = 18;

    const std::string cmd = VideoWriter::BuildCommand(s);
    Check(cmd.find("-f rawvideo") != std::string::npos, "вход объявлен сырым видео");
    Check(cmd.find("-pixel_format rgb24") != std::string::npos, "формат пикселей — rgb24");
    Check(cmd.find("-video_size 1920x1080") != std::string::npos, "размер кадра передан");
    Check(cmd.find("-i -") != std::string::npos, "кадры читаются из stdin");
    Check(cmd.find("-c:v libx264") != std::string::npos, "видео кодируется H.264");
    Check(cmd.find("-crf 18") != std::string::npos, "качество передано как CRF");
    Check(cmd.find("-pix_fmt yuv420p") != std::string::npos, "выход в yuv420p — иначе не откроют плееры");
    Check(cmd.find("+faststart") != std::string::npos, "moov в начало файла");
    Check(cmd.find("-c:a") == std::string::npos, "без звуковой дорожки аудио не кодируется");

    // Опции входа обязаны стоять ДО своего -i, иначе ffmpeg отнесёт их к выходу
    // и прочитает поток кадров как попало.
    Check(cmd.find("-video_size") < cmd.find("-i -"), "опции входа идут перед -i");
    Check(cmd.find("-c:v libx264") > cmd.find("-i -"), "опции кодирования идут после входа");

    // Путь с пробелом и кавычкой не должен ни рвать команду, ни дописывать к ней
    // свою: он обязан целиком остаться одним аргументом.
    VideoWriter::Settings tricky = s;
    tricky.OutputPath = "/tmp/my render/a'b; rm -rf x.mp4";
    const std::string quoted = VideoWriter::BuildCommand(tricky);
    Check(quoted.find("; rm -rf x") == std::string::npos ||
              quoted.find("'\\''") != std::string::npos,
          "опасный путь экранирован, а не подставлен как есть");
    Check(quoted.find("rm -rf x.mp4'") != std::string::npos ||
              quoted.find("rm -rf x.mp4\"") != std::string::npos,
          "путь закрыт кавычкой целиком");

    // Звук: смещение дорожки и старт не с нуля должны давать -ss и aac.
    VideoWriter::Settings withAudio = s;
    withAudio.AudioPath = "music.wav";
    withAudio.StartTime = 2.0f;
    withAudio.AudioOffset = 0.5f;
    const std::string audio = VideoWriter::BuildCommand(withAudio);
    Check(audio.find("-c:a aac") != std::string::npos, "звук кодируется в AAC");
    Check(audio.find("-shortest") != std::string::npos, "ролик кончается вместе с картинкой");
    Check(audio.find("-ss 1.5") != std::string::npos, "звук подрезан с учётом смещения дорожки");
    Check(audio.find("-ss") < audio.find("music.wav"), "-ss стоит перед своим входом");

    // Рендер не с нуля без звука не должен добавлять -ss: подрезать нечего.
    VideoWriter::Settings noAudioOffset = s;
    noAudioOffset.StartTime = 3.0f;
    Check(VideoWriter::BuildCommand(noAudioOffset).find("-ss") == std::string::npos,
          "без звука подрезка не добавляется");
}

// ---------------------------------------------------------------------------
// Кости. Сам расчёт позы проверен тестами движка на настоящем скелете — там он
// и живёт. Здесь проверяется то, что принадлежит инструменту и работает БЕЗ
// OpenGL: пересчёт углов, различение дорожек по кости, устойчивость к
// отсутствию скелета (модель грузится лениво — этот случай реален) и файл
// проекта.
// ---------------------------------------------------------------------------
void TestBoneTracks() {
    std::printf("\n[Кости персонажа]\n");

    // --- Углы Эйлера ---
    // Порядок вращений обязан совпадать с движковым Transform, иначе «повернуть
    // кость на 30°» и «повернуть объект на 30°» дали бы разный результат.
    const glm::vec3 angles(30.0f, -45.0f, 15.0f);
    const glm::quat q = QuatFromEulerDegrees(angles);
    const glm::vec3 back = EulerDegreesFromQuat(q);
    CheckNear(back.x, angles.x, 1e-2f, "угол X переживает круг преобразований");
    CheckNear(back.y, angles.y, 1e-2f, "угол Y переживает круг преобразований");
    CheckNear(back.z, angles.z, 1e-2f, "угол Z переживает круг преобразований");

    Transform tr;
    tr.Rotation = angles;
    const glm::mat4 fromTransform = tr.GetMatrix();
    const glm::mat4 fromQuat = glm::mat4_cast(q);
    float worst = 0.0f;
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) {
            worst = std::max(worst, std::fabs(fromTransform[c][r] - fromQuat[c][r]));
        }
    }
    Check(worst < 1e-4f, "поворот кости считается той же конвенцией, что и Transform движка");

    const glm::vec3 zero = EulerDegreesFromQuat(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    Check(std::fabs(zero.x) + std::fabs(zero.y) + std::fabs(zero.z) < 1e-4f,
          "единичный кватернион даёт нулевые углы");

    // --- Дорожки различаются по кости ---
    AnimationDocument doc;
    const int hipId = doc.EnsureTrack(7, Property::BoneRotation, 3).Id;
    const int kneeId = doc.EnsureTrack(7, Property::BoneRotation, 9).Id;
    Check(hipId != kneeId, "две кости одной сущности — две разные дорожки");
    Check(doc.Tracks.size() == 2, "повторных дорожек не создалось");
    Check(doc.EnsureTrack(7, Property::BoneRotation, 3).Id == hipId,
          "та же кость возвращает ту же дорожку");
    Check(doc.FindTrack(7, Property::BoneRotation, 9) != nullptr, "дорожка ищется по кости");
    Check(doc.FindTrack(7, Property::BoneRotation, 5) == nullptr, "у нетронутой кости дорожки нет");
    Check(doc.BoneTracksOf(7).size() == 2, "костные дорожки сущности перечисляются");
    Check(doc.HasBoneTracks(7), "сущность помечена как анимируемая покостно");
    Check(!doc.HasBoneTracks(8), "у чужой сущности костных дорожек нет");

    // Обычное свойство не должно смешиваться с костным: у него кости не бывает.
    const int posId = doc.EnsureTrack(7, Property::Position, 3).Id;
    Check(doc.TrackById(posId)->Joint == -1, "у свойства объекта кость сбрасывается в -1");
    Check(doc.EnsureTrack(7, Property::Position).Id == posId,
          "свойство объекта не двоится из-за кости");

    Check(IsBoneProperty(Property::BoneRotation), "костное свойство распознаётся");
    Check(!IsBoneProperty(Property::Rotation), "поворот объекта — не костное свойство");

    // --- Без скелета ничего не падает и не выдумывается ---
    int cubeId = 0, cameraId = 0, lightId = 0;
    std::unique_ptr<Scene> scene = MakeHeadlessScene(cubeId, cameraId, lightId);
    Check(SkeletonOf(*scene, cubeId) == nullptr, "у обычного объекта скелета нет");
    float values[3] = {1.0f, 2.0f, 3.0f};
    Check(!ReadProperty(*scene, cubeId, Property::BoneRotation, values, 0),
          "кость несуществующего скелета не читается");
    Check(!WriteProperty(*scene, cubeId, Property::BoneRotation, values, 0),
          "кость несуществующего скелета не пишется");
    Check(doc.KeyBone(*scene, cubeId, 0, 0.0f) == 0, "ключ на кость без скелета не ставится");
    Check(doc.KeyPose(*scene, cubeId, 0.0f) == 0, "поза без скелета не ключится");
    Check(!PropertyApplies(*scene, cubeId, Property::BoneRotation, 0),
          "костное свойство неприменимо к объекту без скелета");

    // Костные свойства не показываются в меню свойств объекта: у них своё меню,
    // где заодно выбирается кость.
    bool anyBone = false;
    for (Property p : ApplicableProperties(*scene, cubeId)) {
        if (IsBoneProperty(p)) anyBone = true;
    }
    Check(!anyBone, "костных свойств нет в списке свойств объекта");

    // Дорожка ждёт скелета, пока модель грузится, и не ждёт удалённой сущности.
    AnimationDocument pending;
    pending.EnsureTrack(cubeId, Property::BoneRotation, 2).JointName = "spine";
    Check(pending.RebindBoneTracks(*scene) == 1, "дорожка живой сущности ждёт скелета");
    AnimationDocument orphan;
    orphan.EnsureTrack(9999, Property::BoneRotation, 2).JointName = "spine";
    Check(orphan.RebindBoneTracks(*scene) == 0, "дорожка удалённой сущности ничего не ждёт");

    // --- Файл проекта ---
    AnimationDocument saved;
    Track& t = saved.EnsureTrack(cubeId, Property::BoneRotation, 4);
    t.JointName = "Bone_Spine";
    t.Channels[0].SetKey(0.0f, 0.0f);
    t.Channels[0].SetKey(1.0f, 45.0f);

    // Ручная поза кости — тоже часть проекта.
    PoseComponent pose;
    pose.Joints.resize(5);
    pose.Joints[4].HasRotation = true;
    pose.Joints[4].Rotation = QuatFromEulerDegrees({10.0f, 20.0f, 30.0f});
    pose.Joints[4].HasTranslation = true;
    pose.Joints[4].Translation = {0.5f, 1.5f, -2.5f};
    scene->Registry().emplace<PoseComponent>(scene->Get(cubeId).Entity(), pose);

    const std::string path = "/tmp/director3d_bones.d3dproj";
    std::string err;
    Check(ProjectFile::Save(path, *scene, saved, 0.0f, err), "проект с костями сохранён");

    std::unique_ptr<Scene> loadedScene;
    AnimationDocument loaded;
    float playhead = 0.0f;
    Check(ProjectFile::Load(path, loadedScene, loaded, playhead, err), "проект с костями загружен");

    const Track* rt = loaded.FindTrack(cubeId, Property::BoneRotation, 4);
    Check(rt != nullptr, "костная дорожка нашлась после загрузки");
    if (rt) {
        Check(rt->JointName == "Bone_Spine", "имя кости сохранилось");
        Check(rt->Joint == 4, "индекс кости сохранился");
        CheckNear(rt->Channels[0].Evaluate(1.0f), 45.0f, 1e-3f, "ключи костной дорожки на месте");
    }

    if (loadedScene) {
        const PoseComponent* rp =
            loadedScene->Registry().try_get<PoseComponent>(loadedScene->Get(cubeId).Entity());
        Check(rp != nullptr, "ручная поза сохранилась в проекте");
        if (rp && rp->Joints.size() > 4) {
            const sage::anim::JointPose& jp = rp->Joints[4];
            Check(jp.HasRotation && jp.HasTranslation, "переопределённые каналы кости восстановлены");
            Check(!jp.HasScale, "нетронутый канал не выдумывается при загрузке");
            CheckNear(jp.Translation.y, 1.5f, 1e-4f, "перенос кости восстановлен");
            const glm::vec3 deg = EulerDegreesFromQuat(jp.Rotation);
            CheckNear(deg.y, 20.0f, 1e-2f, "поворот кости восстановлен");
            // Кости 0..3 не трогали — они не должны стать «переопределёнными».
            bool untouchedClean = true;
            for (size_t i = 0; i < 4 && i < rp->Joints.size(); ++i) {
                if (rp->Joints[i].Any()) untouchedClean = false;
            }
            Check(untouchedClean, "нетронутые кости не попали в файл как переопределённые");
        }
    }
    std::error_code ec;
    fs::remove(path, ec);
}

} // namespace

int RunSelfTest() {
    std::printf("Director 3D — самотест ядра анимации\n");
    std::printf("=====================================\n");

    TestCurves();
    TestPlayback();
    TestDocument();
    TestUndo();
    TestProjectIO();
    TestAudioDecoding();
    TestVideoCommand();
    TestBoneTracks();

    std::printf("\n=====================================\n");
    std::printf("Пройдено: %d, провалено: %d\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace d3d
