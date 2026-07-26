#include "SelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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
#include "ui/FileDialog.h"
#include "ui/Localization.h"
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

    // КУРСОР сегмента — это кэш, и главное его свойство: он не должен менять
    // ответ. Проверяем самым прямым способом — считаем одну и ту же кривую в
    // трёх порядках обхода. Вперёд курсор попадает почти всегда, назад
    // промахивается на каждом шаге, вразнобой — как придётся; все три обязаны
    // совпасть до последнего бита, иначе кэш где-то «залипает».
    {
        Curve dense;
        for (int i = 0; i < 40; ++i) {
            const float t = (float)i * 0.25f;
            dense.SetKey(t, std::sin(t) * 3.0f + (float)(i % 5),
                         i % 3 == 0 ? Interp::Linear : Interp::Smooth);
        }

        std::vector<float> times;
        for (int i = 0; i <= 400; ++i) times.push_back((float)i * 0.025f);

        std::vector<float> forward, backward, shuffled;
        for (float t : times) forward.push_back(dense.Evaluate(t));
        for (size_t i = times.size(); i-- > 0;) backward.push_back(dense.Evaluate(times[i]));
        std::reverse(backward.begin(), backward.end());
        // Псевдослучайный порядок без <random>: шаг, взаимно простой с длиной.
        shuffled.resize(times.size());
        for (size_t i = 0; i < times.size(); ++i) {
            const size_t j = (i * 173u + 61u) % times.size();
            shuffled[j] = dense.Evaluate(times[j]);
        }

        bool sameBackward = true, sameShuffled = true;
        for (size_t i = 0; i < times.size(); ++i) {
            if (forward[i] != backward[i]) sameBackward = false;
            if (forward[i] != shuffled[i]) sameShuffled = false;
        }
        Check(sameBackward, "обход назад даёт те же значения, что и вперёд");
        Check(sameShuffled, "обход вразнобой даёт те же значения, что и вперёд");

        // Правка ключей обязана сбросить курсор: иначе он указывал бы на
        // сегмент, которого уже нет, и следующий запрос вернул бы старое.
        const float before = dense.Evaluate(5.0f);
        dense.SetKey(5.0f, before + 100.0f, Interp::Linear);
        CheckNear(dense.Evaluate(5.0f), before + 100.0f, 1e-3f,
                  "после правки ключа кривая отдаёт новое значение");
        dense.RemoveKey(0);
        Check(dense.Evaluate(0.0f) == dense.At(0).Value,
              "после удаления первого ключа начало кривой пересчитано");
    }

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

// --- Монтажные дорожки -----------------------------------------------------

// Дорожка клипов — это МОНТАЖ: из кусков готовых анимаций собирается
// последовательность, и правила «какой кусок играет сейчас» решают всё.
// Раньше здесь было пять проверок на «создалась и удалилась» — то есть само
// монтажное поведение не проверялось вовсе.
void TestClipTracks() {
    Section("Монтажные дорожки");

    int cubeId = 0, cameraId = 0, lightId = 0;
    std::unique_ptr<Scene> scene = MakeHeadlessScene(cubeId, cameraId, lightId);

    // --- Выбор активного блока ---
    {
        ClipTrack track;
        track.Blocks.push_back(ClipBlock{"Idle", 0, 0.0f, 2.0f, 1.0f, 0.0f, true});
        track.Blocks.push_back(ClipBlock{"Walk", 1, 3.0f, 2.0f, 1.0f, 0.0f, true});

        const ClipBlock* b = AnimationDocument::ActiveBlockAt(track, 1.0f);
        Check(b && b->Name == "Idle", "внутри первого блока играет он");
        b = AnimationDocument::ActiveBlockAt(track, 4.0f);
        Check(b && b->Name == "Walk", "внутри второго блока играет он");

        // ПРОБЕЛ между блоками — это осознанная пауза, а не «доиграй
        // предыдущий». Если бы здесь возвращался Idle, персонаж продолжал бы
        // жить своей жизнью посреди пустого таймлайна.
        Check(AnimationDocument::ActiveBlockAt(track, 2.5f) == nullptr,
              "в пробеле между блоками не играет ничего");
        Check(AnimationDocument::ActiveBlockAt(track, -1.0f) == nullptr,
              "до первого блока не играет ничего");
        Check(AnimationDocument::ActiveBlockAt(track, 10.0f) == nullptr,
              "после последнего блока не играет ничего");

        // Границы включительны с обеих сторон: на стыке двух соприкасающихся
        // блоков кадр не должен проваливаться в тишину.
        b = AnimationDocument::ActiveBlockAt(track, 0.0f);
        Check(b && b->Name == "Idle", "начало блока принадлежит блоку");
        b = AnimationDocument::ActiveBlockAt(track, 2.0f);
        Check(b && b->Name == "Idle", "конец блока принадлежит блоку");

        // ПЕРЕКРЫТИЕ: побеждает позже начавшийся — «положить поверх».
        track.Blocks.push_back(ClipBlock{"Overlay", 2, 1.0f, 3.0f, 1.0f, 0.0f, true});
        b = AnimationDocument::ActiveBlockAt(track, 1.5f);
        Check(b && b->Name == "Overlay", "при перекрытии играет позже начавшийся блок");
        b = AnimationDocument::ActiveBlockAt(track, 0.5f);
        Check(b && b->Name == "Idle", "до перекрытия играет прежний блок");

        // Порядок в векторе не должен влиять на результат: блоки приходят из
        // файла и после перетаскивания мышью в произвольном порядке.
        ClipTrack shuffled;
        shuffled.Blocks.push_back(ClipBlock{"Overlay", 2, 1.0f, 3.0f, 1.0f, 0.0f, true});
        shuffled.Blocks.push_back(ClipBlock{"Idle", 0, 0.0f, 2.0f, 1.0f, 0.0f, true});
        const ClipBlock* s1 = AnimationDocument::ActiveBlockAt(shuffled, 1.5f);
        Check(s1 && s1->Name == "Overlay", "порядок блоков в списке не меняет выбор");
    }

    // --- Несколько персонажей: дорожки независимы ---
    {
        AnimationDocument doc;
        ClipTrack& a = doc.EnsureClipTrack(cubeId);
        ClipTrack& b = doc.EnsureClipTrack(cameraId);
        a.Blocks.push_back(ClipBlock{"A", 0, 0.0f, 5.0f, 1.0f, 0.0f, true});
        b.Blocks.push_back(ClipBlock{"B", 1, 6.0f, 2.0f, 1.0f, 0.0f, true});
        Check(doc.ClipTracks.size() == 2, "у двух объектов две дорожки клипов");
        Check(doc.FindClipTrack(cubeId)->Blocks[0].Name == "A", "первая дорожка своя");
        Check(doc.FindClipTrack(cameraId)->Blocks[0].Name == "B", "вторая дорожка своя");
        Check(doc.FindClipTrack(lightId) == nullptr, "у третьего объекта дорожки нет");
        // Конец содержимого — по самому позднему блоку ЛЮБОЙ дорожки.
        CheckNear(doc.ContentEnd(), 8.0f, 1e-4f, "конец содержимого по позднейшему блоку");
    }

    // --- Заглушка дорожки клипов ---
    {
        AnimationDocument doc;
        ClipTrack& t = doc.EnsureClipTrack(cubeId);
        t.Blocks.push_back(ClipBlock{"A", 0, 0.0f, 5.0f, 1.0f, 0.0f, true});
        t.Muted = true;
        // Заглушенная дорожка не применяется, но остаётся в документе и в
        // подсчёте длительности: она никуда не делась, её просто не слышно.
        CheckNear(doc.ContentEnd(), 5.0f, 1e-4f, "заглушенная дорожка учитывается в длительности");
        doc.Apply(*scene, 1.0f, true);
        Check(true, "применение с заглушенной дорожкой не падает");
    }

    // --- Навигация по границам блоков ---
    // Кнопки «предыдущий/следующий ключ» на транспорте обязаны видеть монтаж:
    // границы блоков — это те точки, куда аниматор прыгает чаще всего.
    {
        AnimationDocument doc;
        ClipTrack& t = doc.EnsureClipTrack(cubeId);
        t.Blocks.push_back(ClipBlock{"A", 0, 1.0f, 2.0f, 1.0f, 0.0f, true});
        float out = 0.0f;
        Check(doc.NextKeyTime(0.0f, out) && std::fabs(out - 1.0f) < 1e-4f,
              "следующая точка — начало блока");
        Check(doc.NextKeyTime(1.5f, out) && std::fabs(out - 3.0f) < 1e-4f,
              "следующая точка — конец блока");
        Check(doc.PrevKeyTime(5.0f, out) && std::fabs(out - 3.0f) < 1e-4f,
              "предыдущая точка — конец блока");
    }

    // --- Круг через файл со всеми полями блока ---
    {
        AnimationDocument doc;
        doc.Fps = 25.0f;
        ClipTrack& t = doc.EnsureClipTrack(cubeId);
        t.Muted = true;
        t.Blocks.push_back(ClipBlock{"Walk", 3, 1.25f, 4.5f, 0.75f, 0.35f, false});
        t.Blocks.push_back(ClipBlock{"Run", 4, 6.0f, 2.0f, 1.5f, 0.1f, true});

        const fs::path path = fs::temp_directory_path() / "director3d_clips.d3dproj";
        std::string err;
        Check(ProjectFile::Save(path.string(), *scene, doc, 0.0f, err), "монтаж сохранён");

        std::unique_ptr<Scene> loadedScene;
        AnimationDocument loaded;
        float playhead = 0.0f;
        Check(ProjectFile::Load(path.string(), loadedScene, loaded, playhead, err), "монтаж загружен");
        Check(loaded.ClipTracks.size() == 1, "дорожка клипов одна");
        if (!loaded.ClipTracks.empty()) {
            const ClipTrack& lt = loaded.ClipTracks[0];
            Check(lt.Muted, "заглушка дорожки сохранилась");
            Check(lt.Blocks.size() == 2, "оба блока сохранились");
            if (lt.Blocks.size() == 2) {
                Check(lt.Blocks[0].Name == "Walk", "имя первого блока");
                Check(lt.Blocks[0].ClipIndex == 3, "индекс клипа первого блока");
                CheckNear(lt.Blocks[0].Start, 1.25f, 1e-4f, "начало первого блока");
                CheckNear(lt.Blocks[0].Duration, 4.5f, 1e-4f, "длительность первого блока");
                CheckNear(lt.Blocks[0].Speed, 0.75f, 1e-4f, "скорость первого блока");
                CheckNear(lt.Blocks[0].BlendIn, 0.35f, 1e-4f, "кросс-фейд первого блока");
                Check(!lt.Blocks[0].Loop, "выключенный цикл первого блока");
                Check(lt.Blocks[1].Loop, "включённый цикл второго блока");
                CheckNear(lt.Blocks[1].Speed, 1.5f, 1e-4f, "скорость второго блока");
            }
            // Выбор активного блока после загрузки обязан работать так же.
            const ClipBlock* b = AnimationDocument::ActiveBlockAt(lt, 2.0f);
            Check(b && b->Name == "Walk", "после загрузки активный блок определяется");
        }
        std::error_code ec;
        fs::remove(path, ec);
    }
}

// --- Звуковая дорожка ------------------------------------------------------

void TestAudioTrackTimeline() {
    Section("Звуковая дорожка");

    int cubeId = 0, cameraId = 0, lightId = 0;
    std::unique_ptr<Scene> scene = MakeHeadlessScene(cubeId, cameraId, lightId);

    // Пишем настоящий WAV на диск: дорожка обязана уметь читать файл, а не
    // только буфер, и длительность считается именно из него.
    const fs::path wav = fs::temp_directory_path() / "director3d_track.wav";
    {
        const int rate = 8000, seconds = 2;
        const int samples = rate * seconds;
        std::vector<unsigned char> bytes;
        auto put32 = [&](unsigned int v) {
            for (int i = 0; i < 4; ++i) bytes.push_back((unsigned char)((v >> (8 * i)) & 0xFF));
        };
        auto put16 = [&](unsigned short v) {
            bytes.push_back((unsigned char)(v & 0xFF));
            bytes.push_back((unsigned char)((v >> 8) & 0xFF));
        };
        const unsigned int dataBytes = (unsigned int)samples * 2u;
        for (char c : std::string("RIFF")) bytes.push_back((unsigned char)c);
        put32(36u + dataBytes);
        for (char c : std::string("WAVEfmt ")) bytes.push_back((unsigned char)c);
        put32(16u); put16(1); put16(1); put32((unsigned int)rate);
        put32((unsigned int)rate * 2u); put16(2); put16(16);
        for (char c : std::string("data")) bytes.push_back((unsigned char)c);
        put32(dataBytes);
        for (int i = 0; i < samples; ++i) {
            // Тишина в первой половине, синус во второй — так видно, что
            // огибающая привязана ко ВРЕМЕНИ, а не размазана по всей дорожке.
            const float t = (float)i / (float)rate;
            const float v = t < 1.0f ? 0.0f : std::sin(t * 440.0f * 6.28318f) * 0.8f;
            put16((unsigned short)(short)(v * 32767.0f));
        }
        std::ofstream out(wav, std::ios::binary);
        out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    }

    AnimationDocument doc;
    Check(doc.Audio.Load(wav.string()), "звуковой файл загрузился");
    Check(doc.Audio.Loaded(), "дорожка считается заполненной");
    Check(doc.Audio.HasWaveform(), "огибающая построена");
    CheckNear(doc.Audio.Length(), 2.0f, 0.05f, "длительность звука определена");

    // Огибающая привязана ко времени: в тишине пики около нуля, в сигнале — нет.
    const std::vector<AudioTrack::Peak> quiet = doc.Audio.Sample(0.0f, 0.9f, 32);
    const std::vector<AudioTrack::Peak> loud = doc.Audio.Sample(1.1f, 1.9f, 32);
    Check(!quiet.empty() && !loud.empty(), "огибающая берётся на обоих участках");
    float quietMax = 0.0f, loudMax = 0.0f;
    for (const AudioTrack::Peak& p : quiet) quietMax = std::max(quietMax, std::fabs(p.Max));
    for (const AudioTrack::Peak& p : loud) loudMax = std::max(loudMax, std::fabs(p.Max));
    Check(quietMax < 0.05f, "в тишине огибающая около нуля");
    Check(loudMax > 0.3f, "в сигнале огибающая заметна");

    // Смещение двигает звук по таймлайну и обязано попадать в длительность.
    doc.Audio.Offset = 3.0f;
    CheckNear(doc.ContentEnd(), 5.0f, 0.05f, "смещение звука учтено в длительности ролика");

    // Круг через файл: путь, смещение, громкость и заглушка.
    doc.Audio.Volume = 0.42f;
    doc.Audio.Muted = true;
    const fs::path proj = fs::temp_directory_path() / "director3d_audio.d3dproj";
    std::string err;
    Check(ProjectFile::Save(proj.string(), *scene, doc, 0.0f, err), "проект со звуком сохранён");

    std::unique_ptr<Scene> loadedScene;
    AnimationDocument loaded;
    float playhead = 0.0f;
    Check(ProjectFile::Load(proj.string(), loadedScene, loaded, playhead, err),
          "проект со звуком загружен");
    Check(loaded.Audio.Loaded(), "звук нашёлся после загрузки");
    CheckNear(loaded.Audio.Offset, 3.0f, 1e-4f, "смещение звука сохранилось");
    CheckNear(loaded.Audio.Volume, 0.42f, 1e-4f, "громкость сохранилась");
    Check(loaded.Audio.Muted, "заглушка звука сохранилась");

    // Пропавший файл не должен ронять открытие проекта: звук мог переехать, а
    // терять из-за этого всю анимацию нельзя.
    std::error_code ec;
    fs::remove(wav, ec);
    std::unique_ptr<Scene> againScene;
    AnimationDocument again;
    Check(ProjectFile::Load(proj.string(), againScene, again, playhead, err),
          "проект открывается и без звукового файла");
    Check(!again.Audio.HasWaveform(), "у пропавшего файла нет огибающей");
    fs::remove(proj, ec);

    // Очистка дорожки.
    doc.Audio.Clear();
    Check(!doc.Audio.Loaded(), "дорожка очищается");
    Check(!doc.Audio.HasWaveform(), "огибающая очищается вместе с ней");
}

// --- Системные диалоги -----------------------------------------------------

void TestFileDialog() {
    Section("Системные диалоги");

    // Экранирование — единственное место здесь, где ошибка опасна, а не
    // неудобна: путь и имя проекта попадают в командную строку оболочки, и
    // пробел без кавычек ломает команду, а кавычка позволяет дописать к ней
    // произвольный вызов.
    //
    // Проверяем не ВИД строки, а её ПОВЕДЕНИЕ: подставляем в настоящую команду
    // и смотрим, что оболочка вернула ровно то, что мы ей дали, и ничего не
    // выполнила. Сравнение с эталонной строкой проверяло бы наши же
    // представления о правилах цитирования, а не сами правила.
    auto roundTrip = [](const std::string& value) {
        const std::string cmd = "printf '%s' " + filedialog::QuoteForShell(value);
        std::FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return std::string("<не запустилось>");
        std::string out;
        char buf[512];
        while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
        pclose(pipe);
        return out;
    };

    const char* cases[] = {
        "/home/user/simple.d3dproj",
        "/home/user/с пробелами/мой проект.d3dproj",
        "/tmp/it's mine.d3dproj",                 // одинарная кавычка
        "/tmp/say \"hi\".d3dproj",                // двойная кавычка
        "/tmp/$HOME.d3dproj",                     // подстановка переменной
        "/tmp/`id`.d3dproj",                      // обратные кавычки
        "/tmp/$(id).d3dproj",                     // подстановка команды
        "/tmp/a;rm -rf x.d3dproj",                // разделитель команд
        "/tmp/a|b&c.d3dproj",                     // конвейер и фон
        "/tmp/звёзды*и?знаки[].d3dproj",          // шаблоны имён
    };
    for (const char* value : cases) {
        const std::string got = roundTrip(value);
        if (got != value) {
            std::printf("  ПРОВАЛ: экранирование исказило путь\n    дано:     %s\n    получено: %s\n",
                        value, got.c_str());
            ++g_failed;
        } else {
            ++g_passed;
        }
    }

    // Отдельно и явно: подстановка команды НЕ выполняется. Признак — в ответе
    // остался САМ СИНТАКСИС подстановки. Искать в ответе слово-маркер нельзя:
    // оно есть и во входной строке, поэтому его наличие не отличает
    // «не выполнилось» от «выполнилось» — на этом первая версия проверки и
    // провалилась.
    const std::string dangerous = roundTrip("/tmp/$(echo ВЫПОЛНЕНО).d3dproj");
    Check(dangerous.find("$(echo") != std::string::npos,
          "подстановка команды в пути осталась текстом, а не выполнилась");
    const std::string backticks = roundTrip("/tmp/`echo ВЫПОЛНЕНО`.d3dproj");
    Check(backticks.find('`') != std::string::npos,
          "обратные кавычки в пути остались текстом, а не выполнились");

    // Доступность и имя утилиты обязаны быть согласованы: кнопка «Обзор»
    // показывается по Available(), а подсказка печатает Backend().
    Check(filedialog::Available() == !filedialog::Backend().empty(),
          "доступность диалога и имя утилиты согласованы");
    // Без утилиты вызов обязан честно вернуть false, а не подвиснуть и не
    // испортить выходную строку.
    if (!filedialog::Available()) {
        std::string out = "не трогать";
        Check(!filedialog::OpenFile("Тест", {}, "", out), "без утилиты диалог не открывается");
        Check(out == "не трогать", "неудачный диалог не портит результат");
    }
}

// --- Монтаж камер ----------------------------------------------------------

// Дорожка монтажа — единственное место, где инструмент отвечает на вопрос
// «чем снимаем этот кадр», и ошибка здесь не выглядит как ошибка: ролик просто
// снят не той камерой, и понять это можно только пересмотрев его целиком.
// Поэтому правила проверяются поимённо, а не «в целом работает».
void TestCameraTrack() {
    Section("Монтаж камер");

    AnimationDocument doc;
    doc.Duration = 10.0f;
    const int kManual = 100, kCamA = 1, kCamB = 2, kCamC = 3;

    // Пустая дорожка не должна ничего решать за человека.
    Check(doc.CameraAt(0.0f, kManual) == kManual, "без склеек снимает ручная камера");
    Check(doc.CameraAt(5.0f, kManual) == kManual, "без склеек это верно на всём ролике");
    Check(doc.CutIndexAt(3.0f) == -1, "без склеек активного плана нет");

    doc.SetCut(2.0f, kCamA);
    doc.SetCut(6.0f, kCamB);

    // Главное правило: ДО первой склейки работает ручной выбор. Обратное
    // («берём камеру первой склейки») выглядело бы как склейка, которую никто
    // не ставил, и именно на начале ролика это дороже всего.
    Check(doc.CameraAt(0.0f, kManual) == kManual, "до первой склейки — ручная камера");
    Check(doc.CameraAt(1.99f, kManual) == kManual, "и вплотную к первой склейке тоже");
    Check(doc.CameraAt(2.0f, kManual) == kCamA, "на склейке план уже новый");
    Check(doc.CameraAt(4.0f, kManual) == kCamA, "между склейками план держится");
    Check(doc.CameraAt(6.0f, kManual) == kCamB, "вторая склейка переключает");
    Check(doc.CameraAt(100.0f, kManual) == kCamB, "последний план тянется до конца");

    // Склейки вносятся не по порядку — список обязан остаться отсортированным,
    // иначе поиск плана по времени молча вернёт не тот.
    doc.SetCut(4.0f, kCamC);
    Check(doc.Cameras.Cuts.size() == 3, "склейка вставлена");
    bool sorted = true;
    for (size_t i = 1; i < doc.Cameras.Cuts.size(); ++i) {
        if (doc.Cameras.Cuts[i - 1].Time > doc.Cameras.Cuts[i].Time) sorted = false;
    }
    Check(sorted, "список склеек отсортирован по времени");
    Check(doc.CameraAt(5.0f, kManual) == kCamC, "вставленная в середину склейка действует");

    // Повторная склейка на том же времени меняет камеру, а не заводит вторую:
    // иначе одна из двух навсегда осталась бы недостижимой мышью.
    const size_t before = doc.Cameras.Cuts.size();
    doc.SetCut(4.0f, kCamB);
    Check(doc.Cameras.Cuts.size() == before, "склейка на занятом времени не дублируется");
    Check(doc.CameraAt(5.0f, kManual) == kCamB, "ей сменилась камера");

    // Перетаскивание через соседнюю склейку меняет ИНДЕКС — MoveCut обязан
    // вернуть новый, иначе следующее движение мыши потащит чужую склейку.
    doc.Cameras.Cuts.clear();
    doc.SetCut(1.0f, kCamA);
    doc.SetCut(3.0f, kCamB);
    const int moved = doc.MoveCut(0, 5.0f); // тащим первую за вторую
    Check(moved == 1, "после перестановки индекс склейки обновился");
    Check(doc.Cameras.Cuts[(size_t)moved].CameraId == kCamA, "и это та же склейка");
    // Склейки теперь [3 -> B, 5 -> A]. Момент 2 c оказался РАНЬШЕ первой
    // склейки — и по общему правилу снимает ручная камера. Первая версия этой
    // проверки ждала здесь kCamB и провалилась: правило легко забыть даже
    // тому, кто его написал, поэтому оно и закреплено отдельной строкой.
    Check(doc.CameraAt(2.0f, kManual) == kManual, "освободившееся начало вернулось ручной камере");
    Check(doc.CameraAt(3.5f, kManual) == kCamB, "порядок планов пересчитался");
    Check(doc.CameraAt(6.0f, kManual) == kCamA, "и хвост тоже");

    // Отрицательное время физически невозможно: ролик начинается с нуля.
    doc.MoveCut(0, -3.0f);
    Check(doc.Cameras.Cuts.front().Time >= 0.0f, "склейку нельзя утащить за начало ролика");

    // Заглушенная дорожка обязана вести себя как отсутствующая — это способ
    // сравнить монтаж с ручной камерой, не потеряв склейки.
    doc.Cameras.Muted = true;
    Check(doc.CameraAt(6.0f, kManual) == kManual, "заглушенный монтаж не переключает");
    Check(!doc.Cameras.Cuts.empty(), "но склейки при этом целы");
    doc.Cameras.Muted = false;

    // Удаление камеры из сцены не должно оставлять склейку, которая выглядит
    // работающей, ничего не переключая.
    doc.Cameras.Cuts.clear();
    doc.SetCut(1.0f, kCamA);
    doc.SetCut(4.0f, kCamB);
    doc.RemoveTracksOf(kCamA);
    Check(doc.Cameras.Cuts.size() == 1, "склейка на удалённую камеру убрана");
    Check(doc.CameraAt(2.0f, kManual) == kManual, "её время вернулось ручной камере");

    // Монтаж задаёт длину ролика наравне с ключами: план, начинающийся на 30-й
    // секунде, означает, что ролик минимум до неё.
    doc.Cameras.Cuts.clear();
    doc.SetCut(30.0f, kCamB);
    Check(doc.ContentEnd() >= 30.0f, "склейка учитывается в длине содержимого");

    // Круг через файл: монтаж обязан пережить сохранение и открытие.
    {
        AnimationDocument saved;
        saved.Duration = 12.0f;
        saved.SetCut(0.0f, kCamA);
        saved.SetCut(3.5f, kCamB);
        saved.SetCut(8.25f, kCamC);
        saved.Cameras.Muted = true;

        auto scene = std::make_unique<Scene>();
        const std::string path = "/tmp/director3d_cuts.d3dproj";
        std::string err;
        Check(ProjectFile::Save(path, *scene, saved, 0.0f, err), "проект со склейками сохранён");

        std::unique_ptr<Scene> loadedScene;
        AnimationDocument loaded;
        float playhead = 0.0f;
        Check(ProjectFile::Load(path, loadedScene, loaded, playhead, err),
              "проект со склейками открыт");
        Check(loaded.Cameras.Cuts.size() == 3, "склейки вернулись все");
        Check(loaded.Cameras.Muted, "заглушка дорожки вернулась");
        bool same = loaded.Cameras.Cuts.size() == saved.Cameras.Cuts.size();
        for (size_t i = 0; same && i < loaded.Cameras.Cuts.size(); ++i) {
            same = std::fabs(loaded.Cameras.Cuts[i].Time - saved.Cameras.Cuts[i].Time) < 1e-4f &&
                   loaded.Cameras.Cuts[i].CameraId == saved.Cameras.Cuts[i].CameraId;
        }
        Check(same, "время и камеры склеек не изменились");
    }

    // Проект БЕЗ монтажа не должен обзаводиться разделом сам собой: иначе
    // любое открытие-сохранение раздувало бы файл тем, чего человек не заводил.
    {
        AnimationDocument plain;
        auto scene = std::make_unique<Scene>();
        const std::string path = "/tmp/director3d_nocuts.d3dproj";
        std::string err;
        ProjectFile::Save(path, *scene, plain, 0.0f, err);
        std::ifstream file(path);
        const std::string text((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
        Check(text.find("cameraTrack") == std::string::npos,
              "пустой монтаж в файл не пишется");
    }
}

// --- Локализация -----------------------------------------------------------

// Полноту перевода считает scripts/check_i18n.py по исходникам — во время
// работы полного списка ключей просто не существует (ключ равен русской
// строке, и «все ключи» есть только в коде панелей).
//
// Здесь проверяется другое и не менее важное: САМ МЕХАНИЗМ. Словарь читается,
// переключение языка работает, отсутствие перевода не превращает надпись в
// пустоту, а указатель живёт дольше вызова. Последнее — не педантизм: ImGui
// хранит переданную ему строку до конца кадра, и возврат c_str() от временного
// объекта дал бы мусор на экране в самом безобидном случае и падение в худшем.
void TestLocalization() {
    Section("Локализация");

    // Словарь ищется там же, где его ищет программа, — рядом с рабочим
    // каталогом. Не найти его здесь означало бы, что собранная программа
    // тоже его не найдёт, поэтому это провал, а не пропуск проверки.
    const char* candidates[] = {"assets/i18n", "../assets/i18n", "../../assets/i18n"};
    bool loaded = false;
    for (const char* dir : candidates) {
        if (i18n::LoadDictionary(dir)) { loaded = true; break; }
    }
    Check(loaded, "словарь en.json найден и прочитан");
    if (!loaded) return;
    Check(i18n::DictionarySize() > 300, "в словаре есть содержимое");

    const i18n::Language before = i18n::CurrentLanguage();

    // По-русски перевод обязан вернуть ТУ ЖЕ строку — не копию: русский путь
    // проходят каждый кадр сотни раз, и заглядывать в словарь там незачем.
    i18n::SetLanguage(i18n::Language::Russian);
    const char* russian = "Открыть проект";
    Check(T(russian) == russian, "русский язык возвращает исходный указатель");

    i18n::SetLanguage(i18n::Language::English);
    // Команды меню по-английски пишутся с заглавных («Open Project»), подсказки
    // — обычным предложением. Разнобоя здесь нет, так принято в английском
    // интерфейсе, и проверка закрепляет именно это.
    Check(std::string(T("Открыть проект")) == "Open Project", "надпись переведена");
    Check(std::string(T("Открыть проект (Ctrl+O)")) == "Open project (Ctrl+O)",
          "подсказка переведена обычным предложением");
    Check(std::string(T("Файл")) == "File", "меню переведено");
    // Формат с суффиксом секунд — тот случай, ради которого проверка и нужна:
    // в строке нет ни одной кириллической буквы, и её легко не заметить.
    Check(std::string(T("%.2f c")) == "%.2f s", "суффикс секунд переведён");

    // Указатель обязан пережить вызов: два запроса подряд дают один и тот же
    // адрес, потому что строка лежит в словаре, а не в буфере на стеке.
    const char* first = T("Открыть проект");
    const char* second = T("Открыть проект");
    Check(first == second, "перевод возвращает стабильный указатель");

    // Незнакомая строка — не дыра в интерфейсе, а сама строка, плюс запись в
    // список пропусков.
    const char* unknown = "Строка, которой заведомо нет в словаре 42";
    const size_t missingBefore = i18n::MissingKeys().size();
    Check(std::string(T(unknown)) == unknown, "без перевода показывается русский текст");
    Check(i18n::MissingKeys().size() == missingBefore + 1, "пропуск записан");
    T(unknown);
    Check(i18n::MissingKeys().size() == missingBefore + 1, "повтор не дублирует запись");

    Check(std::string(i18n::LanguageCode()) == "en", "код языка совпадает с выбранным");
    Check(i18n::SetLanguageByCode("ru") && i18n::CurrentLanguage() == i18n::Language::Russian,
          "язык переключается по коду");
    Check(!i18n::SetLanguageByCode("de"), "неизвестный код отвергается");
    Check(i18n::CurrentLanguage() == i18n::Language::Russian,
          "отвергнутый код не меняет язык");

    // Все остальные проверки печатают по-русски — возвращаем как было.
    i18n::SetLanguage(before);
}

// --- Дорожки ---------------------------------------------------------------

// Дорожка — центральная сущность инструмента: всё, что аниматор делает, в итоге
// оказывается в ней, и всё, что видно в кадре, из неё читается. Проверки ниже
// идут по её жизненному циклу: завести — наполнить ключами — применить к сцене
// — сохранить и открыть заново.
void TestTracks() {
    Section("Дорожки");

    int cubeId = 0, cameraId = 0, lightId = 0;
    std::unique_ptr<Scene> scene = MakeHeadlessScene(cubeId, cameraId, lightId);

    // --- Заведение и поиск ---
    {
        AnimationDocument doc;
        Track& a = doc.EnsureTrack(cubeId, Property::Position);
        const int firstId = a.Id;
        Track& b = doc.EnsureTrack(cubeId, Property::Position);
        Check(doc.Tracks.size() == 1, "повторный EnsureTrack не плодит дубликат");
        Check(b.Id == firstId, "повторный EnsureTrack отдаёт ту же дорожку");

        doc.EnsureTrack(cubeId, Property::Rotation);
        doc.EnsureTrack(cameraId, Property::Position);
        Check(doc.Tracks.size() == 3, "разные свойства и разные объекты — разные дорожки");
        Check(doc.FindTrack(cameraId, Property::Position) != nullptr, "дорожка находится по объекту и свойству");
        Check(doc.FindTrack(lightId, Property::Position) == nullptr, "чужая дорожка не находится");

        // Идентификаторы обязаны быть уникальны: по ним таймлайн адресует
        // дорожки при перетаскивании ключей, и совпадение означало бы правку
        // не той строки.
        bool unique = true;
        for (size_t i = 0; i < doc.Tracks.size(); ++i) {
            for (size_t j = i + 1; j < doc.Tracks.size(); ++j) {
                if (doc.Tracks[i].Id == doc.Tracks[j].Id) unique = false;
            }
        }
        Check(unique, "идентификаторы дорожек уникальны");
        Check(doc.TrackById(firstId) != nullptr, "дорожка находится по идентификатору");
        Check(doc.TrackById(99999) == nullptr, "несуществующий идентификатор не находится");

        // Число каналов диктует свойство, а не вызывающий.
        Check(doc.EnsureTrack(cubeId, Property::Position).ChannelCount() == 3, "у вектора три канала");
        Check(doc.EnsureTrack(lightId, Property::LightIntensity).ChannelCount() == 1, "у скаляра один канал");
    }

    // --- Ссылки на дорожки переживают добавление новых ---
    // Это НЕ формальность. EnsureTrack отдаёт ссылку, и весь код инструмента
    // построен на «завёл дорожку — пишу в неё». Если хранилище двигает элементы
    // при добавлении, такая ссылка повисает, а код выглядит совершенно
    // нормально и компилируется без замечаний. Ровно этот случай уронил
    // самотест сегфолтом, когда проверка монтажных дорожек завела две подряд.
    {
        AnimationDocument doc;
        Track& first = doc.EnsureTrack(cubeId, Property::Position);
        first.Channels[0].SetKey(0.0f, 1.0f, Interp::Linear);
        const int firstId = first.Id;

        // Заводим ещё десяток — с вектором это гарантированно перевыделение.
        for (int i = 0; i < 10; ++i) doc.EnsureTrack(1000 + i, Property::Position);

        // Пишем через СТАРУЮ ссылку: она обязана указывать на ту же дорожку.
        first.Channels[0].SetKey(1.0f, 2.0f, Interp::Linear);
        Check(first.Id == firstId, "ссылка на дорожку осталась той же после добавления других");
        const Track* found = doc.TrackById(firstId);
        Check(found == &first, "ссылка и поиск по идентификатору дают один объект");
        Check(found && found->Channels[0].Count() == 2,
              "запись через старую ссылку попала в нужную дорожку");

        // То же для дорожек клипов.
        ClipTrack& clipA = doc.EnsureClipTrack(cubeId);
        doc.EnsureClipTrack(cameraId);
        doc.EnsureClipTrack(lightId);
        clipA.Blocks.push_back(ClipBlock{"A", 0, 0.0f, 1.0f, 1.0f, 0.0f, true});
        Check(doc.FindClipTrack(cubeId) == &clipA, "ссылка на дорожку клипов тоже пережила добавление");
        Check(doc.FindClipTrack(cubeId)->Blocks.size() == 1, "блок лёг в нужную дорожку");
    }

    // --- Подындекс: кости и блендшейпы ---
    // У костных и морф-дорожек к паре «объект + свойство» добавляется третий
    // ключ — номер кости или цели. Без него две кости одного персонажа
    // схлопнулись бы в одну дорожку.
    {
        AnimationDocument doc;
        doc.EnsureTrack(cubeId, Property::BoneRotation, 3);
        doc.EnsureTrack(cubeId, Property::BoneRotation, 5);
        Check(doc.Tracks.size() == 2, "две кости — две дорожки поворота");
        Check(doc.FindTrack(cubeId, Property::BoneRotation, 3) !=
              doc.FindTrack(cubeId, Property::BoneRotation, 5), "дорожки костей различаются");

        doc.EnsureTrack(cubeId, Property::MorphWeight, 0);
        doc.EnsureTrack(cubeId, Property::MorphWeight, 1);
        Check(doc.Tracks.size() == 4, "две морф-цели — две дорожки веса");

        // У обычного свойства подындекс должен игнорироваться, иначе один и тот
        // же «Location» завёлся бы дважды из-за случайно переданного номера.
        doc.EnsureTrack(cubeId, Property::Position, 7);
        doc.EnsureTrack(cubeId, Property::Position, -1);
        Check(doc.Tracks.size() == 5, "у обычного свойства подындекс не создаёт вторую дорожку");
    }

    // --- Ключи и навигация по ним ---
    {
        AnimationDocument doc;
        Track& t = doc.EnsureTrack(cubeId, Property::Position);
        t.Channels[0].SetKey(1.0f, 0.0f);
        t.Channels[1].SetKey(2.0f, 0.0f);

        Check(doc.HasKeyAt(t, 1.0f), "ключ на времени первого канала найден");
        Check(doc.HasKeyAt(t, 2.0f), "ключ на времени второго канала найден");
        Check(!doc.HasKeyAt(t, 1.5f), "между ключами ключа нет");
        // Совпадение времени с точностью до эпсилона — это тот же кадр: иначе
        // клик по ромбу промахивался бы мимо собственного ключа из-за float.
        Check(doc.HasKeyAt(t, 1.0f + Curve::kTimeEpsilon * 0.5f), "ключ найден с точностью до эпсилона");

        Check(doc.RemoveKeysAt(t, 1.0f) == 1, "снят ровно один ключ");
        Check(!doc.HasKeyAt(t, 1.0f), "снятый ключ пропал");
        Check(doc.RemoveKeysAt(t, 42.0f) == 0, "снятие с пустого времени ничего не делает");

        // Навигация идёт по ВСЕМУ документу, а не по одной дорожке: кнопки
        // «предыдущий/следующий ключ» на транспорте не знают про выделение.
        AnimationDocument nav;
        nav.EnsureTrack(cubeId, Property::Position).Channels[0].SetKey(1.0f, 0.0f);
        nav.EnsureTrack(cameraId, Property::CameraFov).Channels[0].SetKey(2.5f, 50.0f);
        nav.EnsureClipTrack(lightId).Blocks.push_back(ClipBlock{"Idle", 0, 4.0f, 1.0f, 1.0f, 0.25f, true});
        nav.Markers.push_back(Marker{"Удар", 0.5f, 0u});

        float t2 = 0.0f;
        Check(nav.NextKeyTime(0.0f, t2) && std::fabs(t2 - 0.5f) < 1e-4f, "следующей найдена метка");
        Check(nav.NextKeyTime(0.6f, t2) && std::fabs(t2 - 1.0f) < 1e-4f, "следующим найден ключ другой дорожки");
        Check(nav.NextKeyTime(1.5f, t2) && std::fabs(t2 - 2.5f) < 1e-4f, "следующим найден ключ камеры");
        Check(nav.NextKeyTime(3.0f, t2) && std::fabs(t2 - 4.0f) < 1e-4f, "следующим найдено начало блока клипа");
        Check(nav.NextKeyTime(4.5f, t2) && std::fabs(t2 - 5.0f) < 1e-4f, "следующим найден конец блока клипа");
        Check(!nav.NextKeyTime(9.0f, t2), "за последним событием следующего нет");
        Check(nav.PrevKeyTime(1.0f, t2) && std::fabs(t2 - 0.5f) < 1e-4f, "предыдущее событие найдено");
        // Строгое неравенство: иначе «следующий ключ», стоя НА ключе, никуда бы
        // не двигал головку.
        Check(nav.NextKeyTime(1.0f, t2) && t2 > 1.0f, "стоя на ключе, переходим к следующему, а не к себе");
        Check(!nav.PrevKeyTime(0.5f, t2), "перед первым событием предыдущего нет");
    }

    // --- Заглушка и замок ---
    {
        AnimationDocument doc;
        Track& t = doc.EnsureTrack(cubeId, Property::Position);
        t.Channels[0].SetKey(0.0f, 100.0f, Interp::Linear);

        scene->Get(cubeId).GetTransform().Position.x = 0.0f;
        t.Muted = true;
        doc.Apply(*scene, 0.0f, true);
        CheckNear(scene->Get(cubeId).GetTransform().Position.x, 0.0f, 1e-4f,
                  "заглушенная дорожка не применяется");
        t.Muted = false;
        doc.Apply(*scene, 0.0f, true);
        CheckNear(scene->Get(cubeId).GetTransform().Position.x, 100.0f, 1e-4f,
                  "снятие заглушки возвращает дорожку в работу");

        // Замок защищает от АВТО-ключа: аниматор запирает готовую дорожку, чтобы
        // случайное движение объекта её не переписало.
        t.Locked = true;
        scene->Get(cubeId).GetTransform().Position.x = 7.0f;
        Check(doc.KeyExistingTracks(*scene, cubeId, 1.0f) == 0, "авто-ключ не трогает запертую дорожку");
        Check(!doc.HasKeyAt(t, 1.0f), "на запертой дорожке ключ не появился");
        t.Locked = false;
        Check(doc.KeyExistingTracks(*scene, cubeId, 1.0f) == 1, "после снятия замка авто-ключ работает");
    }

    // --- Авто-ключ ставит только на СУЩЕСТВУЮЩИЕ дорожки ---
    {
        AnimationDocument doc;
        doc.EnsureTrack(cubeId, Property::Position);
        Check(doc.KeyExistingTracks(*scene, cubeId, 0.0f) == 1,
              "авто-ключ прошёл по единственной дорожке");
        Check(doc.Tracks.size() == 1,
              "авто-ключ не заводит дорожки на всё подряд");
        Check(doc.KeyExistingTracks(*scene, lightId, 0.0f) == 0,
              "у объекта без дорожек авто-ключу делать нечего");
    }

    // --- Дорожки клипов ---
    {
        AnimationDocument doc;
        ClipTrack& ct = doc.EnsureClipTrack(cubeId);
        Check(doc.ClipTracks.size() == 1, "дорожка клипов создана");
        Check(&doc.EnsureClipTrack(cubeId) == &ct, "повторный вызов отдаёт ту же дорожку клипов");
        Check(doc.FindClipTrack(lightId) == nullptr, "чужой дорожки клипов нет");

        ct.Blocks.push_back(ClipBlock{"Walk", 0, 0.0f, 2.0f, 1.0f, 0.25f, true});
        ct.Blocks.push_back(ClipBlock{"Run", 1, 3.0f, 2.0f, 1.5f, 0.3f, false});
        CheckNear(doc.ContentEnd(), 5.0f, 1e-4f, "конец содержимого учитывает блоки клипов");

        // Пробел между блоками — это осознанная пауза, а не «доиграй предыдущий».
        // Проверяем через ContentEnd и через удаление дорожки: сама поза
        // применяется только на живом скелете (см. D3D_BONE_TEST).
        doc.RemoveClipTrack(ct.Id);
        Check(doc.ClipTracks.empty(), "дорожка клипов удаляется по идентификатору");
    }

    // --- Удаление ---
    {
        AnimationDocument doc;
        const int keep = doc.EnsureTrack(cameraId, Property::Position).Id;
        const int drop = doc.EnsureTrack(cubeId, Property::Position).Id;
        doc.EnsureTrack(cubeId, Property::Rotation);
        doc.EnsureClipTrack(cubeId);

        doc.RemoveTrack(drop);
        Check(doc.Tracks.size() == 2, "удалилась ровно одна дорожка");
        Check(doc.TrackById(keep) != nullptr, "чужая дорожка уцелела");

        // Удаление объекта из сцены обязано унести и дорожки свойств, и клипы:
        // осиротевшая дорожка каждый кадр искала бы несуществующую сущность.
        doc.RemoveTracksOf(cubeId);
        Check(doc.Tracks.size() == 1, "дорожки свойств удалённого объекта убраны");
        Check(doc.ClipTracks.empty(), "дорожка клипов удалённого объекта убрана");
        Check(doc.TrackById(keep) != nullptr, "дорожки другого объекта не задеты");
    }

    // --- Применение: несколько объектов не путаются ---
    {
        AnimationDocument doc;
        doc.EnsureTrack(cubeId, Property::Position).Channels[0].SetKey(0.0f, 11.0f, Interp::Linear);
        doc.EnsureTrack(cameraId, Property::Position).Channels[0].SetKey(0.0f, 22.0f, Interp::Linear);
        doc.EnsureTrack(lightId, Property::LightIntensity).Channels[0].SetKey(0.0f, 3.3f, Interp::Linear);
        doc.Apply(*scene, 0.0f, true);
        CheckNear(scene->Get(cubeId).GetTransform().Position.x, 11.0f, 1e-4f, "куб получил своё значение");
        CheckNear(scene->Get(cameraId).GetTransform().Position.x, 22.0f, 1e-4f, "камера получила своё");
        const LightComponent& lc =
            scene->Registry().get<LightComponent>(scene->Get(lightId).Entity());
        CheckNear(lc.Intensity, 3.3f, 1e-4f, "свет получил свою интенсивность");

        // Дорожка объекта, которого в сцене нет, не должна ронять применение.
        doc.EnsureTrack(4242, Property::Position).Channels[0].SetKey(0.0f, 1.0f);
        doc.Apply(*scene, 0.0f, true);
        Check(true, "дорожка несуществующего объекта не ломает применение");
    }

    // --- Копирование, растяжение и запекание ---
    {
        AnimationDocument doc;
        Track& pos = doc.EnsureTrack(cubeId, Property::Position);
        pos.Channels[0].SetKey(1.0f, 10.0f, Interp::Linear);
        pos.Channels[0].SetKey(2.0f, 20.0f, Interp::Bezier);
        pos.Channels[0].AtMutable(1).OutTangent = 5.0f;
        pos.Channels[1].SetKey(1.5f, 7.0f, Interp::Constant);
        // Ключ ВНЕ диапазона копирования — он не должен попасть в буфер.
        pos.Channels[0].SetKey(9.0f, 99.0f, Interp::Linear);

        KeyClipboard clip;
        Check(doc.CopyKeys(cubeId, {}, 1.0f, 2.0f, clip) == 3, "скопированы ключи из диапазона");
        Check(clip.Count() == 3, "в буфере ровно они");

        // Смещения считаются от ЛЕВОГО КРАЯ диапазона, а не от первого ключа:
        // иначе пауза в начале выделения пропадала бы при вставке.
        float minOffset = 1e9f;
        for (const ClipboardKey& k : clip.Keys) minOffset = std::min(minOffset, k.Offset);
        CheckNear(minOffset, 0.0f, 1e-4f, "смещения отсчитаны от начала диапазона");

        // Вставка на ДРУГОЙ объект — ради этого буфер и хранит свойство, а не
        // ссылку на дорожку.
        Check(doc.PasteKeys(cameraId, clip, 5.0f) == 3, "буфер вставился на другой объект");
        const Track* pasted = doc.FindTrack(cameraId, Property::Position);
        Check(pasted != nullptr, "у получателя появилась дорожка позиции");
        if (pasted) {
            Check(pasted->Channels[0].Count() == 2, "первый канал получил свои ключи");
            Check(pasted->Channels[1].Count() == 1, "второй канал получил свой ключ");
            CheckNear(pasted->Channels[0].At(0).Time, 5.0f, 1e-4f, "первый ключ лёг на время вставки");
            CheckNear(pasted->Channels[0].At(1).Time, 6.0f, 1e-4f, "рисунок во времени сохранился");
            CheckNear(pasted->Channels[0].At(1).Value, 20.0f, 1e-4f, "значение сохранилось");
            Check(pasted->Channels[0].At(1).Mode == Interp::Bezier, "режим интерполяции сохранился");
            CheckNear(pasted->Channels[0].At(1).OutTangent, 5.0f, 1e-4f, "касательная сохранилась");
            Check(pasted->Channels[1].At(0).Mode == Interp::Constant, "ступенька сохранилась");
        }

        // Растяжение вдвое вокруг времени 1.0: ключ на 2.0 уезжает на 3.0,
        // ключ на 1.0 остаётся на месте (он и есть точка опоры).
        AnimationDocument st;
        Track& t = st.EnsureTrack(cubeId, Property::Position);
        t.Channels[0].SetKey(1.0f, 0.0f, Interp::Bezier);
        t.Channels[0].SetKey(2.0f, 10.0f, Interp::Bezier);
        t.Channels[0].AtMutable(1).InTangent = 8.0f;
        Check(st.ScaleKeyTimes(cubeId, {}, 0.0f, 10.0f, 1.0f, 2.0f) == 2, "растянулись оба ключа");
        CheckNear(t.Channels[0].At(0).Time, 1.0f, 1e-4f, "ключ в точке опоры не сдвинулся");
        CheckNear(t.Channels[0].At(1).Time, 3.0f, 1e-4f, "второй ключ отъехал вдвое дальше");
        // Касательная задана в «значение за секунду»: растянув время вдвое,
        // наклон надо уполовинить, иначе форма кривой поедет.
        CheckNear(t.Channels[0].At(1).InTangent, 4.0f, 1e-4f, "касательная пересчитана под новое время");

        // Сжатие обратно возвращает исходный тайминг.
        st.ScaleKeyTimes(cubeId, {}, 0.0f, 10.0f, 1.0f, 0.5f);
        CheckNear(t.Channels[0].At(1).Time, 2.0f, 1e-4f, "обратное сжатие вернуло тайминг");
        CheckNear(t.Channels[0].At(1).InTangent, 8.0f, 1e-4f, "и касательную");

        // Запекание: кривая превращается в ключи по кадрам, а форма остаётся.
        AnimationDocument bk;
        Track& curve = bk.EnsureTrack(cubeId, Property::Position);
        curve.Channels[0].SetKey(0.0f, 0.0f, Interp::EaseInOut);
        curve.Channels[0].SetKey(1.0f, 10.0f, Interp::EaseInOut);
        // Значения ДО запекания — с ними и сверяем результат.
        float before[5];
        for (int i = 0; i < 5; ++i) before[i] = curve.Channels[0].Evaluate((float)i * 0.25f);
        // Ключ за пределами диапазона — его запекание трогать не должно.
        curve.Channels[0].SetKey(5.0f, 42.0f, Interp::Linear);

        Check(bk.BakeTrack(curve, 0.0f, 1.0f, 24.0f) == 25, "запеклись все кадры диапазона");
        Check(curve.Channels[0].Count() == 26, "ключ вне диапазона уцелел");
        bool shapeKept = true;
        for (int i = 0; i < 5; ++i) {
            const float after = curve.Channels[0].Evaluate((float)i * 0.25f);
            if (std::fabs(after - before[i]) > 0.05f) shapeKept = false;
        }
        Check(shapeKept, "форма кривой после запекания сохранилась");
        bool allLinear = true;
        for (int i = 0; i < curve.Channels[0].Count(); ++i) {
            const Keyframe& k = curve.Channels[0].At(i);
            if (k.Time <= 1.0f + 1e-4f && k.Mode != Interp::Linear) allLinear = false;
        }
        Check(allLinear, "запечённые ключи линейные — форму задаёт их частота");
        CheckNear(curve.Channels[0].At(25).Value, 42.0f, 1e-4f, "ключ вне диапазона не тронут");
    }

    // --- Круг через файл проекта ---
    // Самое важное для дорожек: всё, что аниматор настроил, обязано пережить
    // сохранение. Молча теряющееся поле выглядит как «программа сломалась
    // сама по себе» — воспроизвести такое пользователь не сможет.
    {
        AnimationDocument doc;
        doc.Fps = 30.0f;
        doc.Duration = 9.0f;

        Track& pos = doc.EnsureTrack(cubeId, Property::Position);
        pos.Muted = true;
        pos.Locked = true;
        pos.Expanded = false;
        pos.Channels[0].SetKey(0.0f, 1.0f, Interp::EaseInOut);
        pos.Channels[0].SetKey(2.0f, 5.0f, Interp::Bezier);
        pos.Channels[0].AtMutable(1).InTangent = 3.5f;
        pos.Channels[0].AtMutable(1).OutTangent = -1.25f;
        pos.Channels[2].SetKey(1.0f, 8.0f, Interp::Constant);

        Track& bone = doc.EnsureTrack(cubeId, Property::BoneRotation, 4);
        bone.JointName = "bone4";
        bone.Channels[2].SetKey(0.5f, 45.0f, Interp::Linear);

        Track& morph = doc.EnsureTrack(cubeId, Property::MorphWeight, 1);
        morph.Channels[0].SetKey(0.0f, 0.25f, Interp::Linear);

        ClipTrack& clips = doc.EnsureClipTrack(cameraId);
        clips.Muted = true;
        clips.Blocks.push_back(ClipBlock{"Walk", 2, 1.5f, 3.25f, 1.75f, 0.4f, false});

        doc.Markers.push_back(Marker{"Смена плана", 4.0f, 0xFF00FF00u});

        const fs::path path = fs::temp_directory_path() / "director3d_tracks.d3dproj";
        std::string err;
        Check(ProjectFile::Save(path.string(), *scene, doc, 0.0f, err), "проект с дорожками сохранён");

        std::unique_ptr<Scene> loadedScene;
        AnimationDocument loaded;
        float playhead = 0.0f;
        Check(ProjectFile::Load(path.string(), loadedScene, loaded, playhead, err),
              "проект с дорожками загружен");

        Check(loaded.Tracks.size() == doc.Tracks.size(), "число дорожек сохранилось");
        const Track* lp = loaded.FindTrack(cubeId, Property::Position);
        Check(lp != nullptr, "дорожка позиции нашлась после загрузки");
        if (lp) {
            Check(lp->Muted, "заглушка сохранилась");
            Check(lp->Locked, "замок сохранился");
            Check(!lp->Expanded, "свёрнутость дорожки сохранилась");
            Check(lp->ChannelCount() == 3, "число каналов сохранилось");
            Check(lp->Channels[0].Count() == 2, "число ключей сохранилось");
            Check(lp->Channels[0].At(0).Mode == Interp::EaseInOut, "режим интерполяции сохранился");
            Check(lp->Channels[0].At(1).Mode == Interp::Bezier, "режим Безье сохранился");
            CheckNear(lp->Channels[0].At(1).InTangent, 3.5f, 1e-4f, "входная касательная сохранилась");
            CheckNear(lp->Channels[0].At(1).OutTangent, -1.25f, 1e-4f, "выходная касательная сохранилась");
            Check(lp->Channels[1].Empty(), "пустой канал остался пустым");
            CheckNear(lp->Channels[2].At(0).Value, 8.0f, 1e-4f, "ключ третьего канала сохранился");
        }

        const Track* lb = loaded.FindTrack(cubeId, Property::BoneRotation, 4);
        Check(lb != nullptr, "костная дорожка нашлась по кости после загрузки");
        if (lb) Check(lb->JointName == "bone4", "имя кости сохранилось");

        // Морф-дорожка адресуется номером цели ровно так же, как костная —
        // номером кости. Потеря номера означает, что после открытия проекта
        // мимика перестаёт применяться, причём молча.
        const Track* lm = loaded.FindTrack(cubeId, Property::MorphWeight, 1);
        Check(lm != nullptr, "морф-дорожка нашлась по номеру цели после загрузки");
        if (lm) CheckNear(lm->Channels[0].At(0).Value, 0.25f, 1e-4f, "ключ веса блендшейпа сохранился");

        Check(loaded.ClipTracks.size() == 1, "дорожка клипов сохранилась");
        if (!loaded.ClipTracks.empty()) {
            const ClipTrack& lc = loaded.ClipTracks[0];
            Check(lc.Muted, "заглушка дорожки клипов сохранилась");
            Check(lc.Blocks.size() == 1, "блок клипа сохранился");
            if (!lc.Blocks.empty()) {
                const ClipBlock& b = lc.Blocks[0];
                Check(b.Name == "Walk", "имя блока сохранилось");
                Check(b.ClipIndex == 2, "индекс клипа сохранился");
                CheckNear(b.Start, 1.5f, 1e-4f, "начало блока сохранилось");
                CheckNear(b.Duration, 3.25f, 1e-4f, "длительность блока сохранилась");
                CheckNear(b.Speed, 1.75f, 1e-4f, "скорость блока сохранилась");
                CheckNear(b.BlendIn, 0.4f, 1e-4f, "кросс-фейд блока сохранился");
                Check(!b.Loop, "выключенный цикл блока сохранился");
            }
        }

        Check(loaded.Markers.size() == 1, "метка сохранилась");
        if (!loaded.Markers.empty()) {
            Check(loaded.Markers[0].Name == "Смена плана", "имя метки сохранилось");
            CheckNear(loaded.Markers[0].Time, 4.0f, 1e-4f, "время метки сохранилось");
        }

        // Счётчик идентификаторов обязан быть больше всех занятых: иначе
        // следующая созданная дорожка получила бы чужой номер и склеилась с ней.
        int maxId = 0;
        for (const Track& t : loaded.Tracks) maxId = std::max(maxId, t.Id);
        for (const ClipTrack& t : loaded.ClipTracks) maxId = std::max(maxId, t.Id);
        Check(loaded.NextId() > maxId, "счётчик идентификаторов после загрузки свободен");
        const int freshId = loaded.EnsureTrack(lightId, Property::LightIntensity).Id;
        Check(loaded.TrackById(freshId) == &loaded.Tracks.back(),
              "новая дорожка после загрузки не склеилась с существующей");

        std::error_code ec;
        fs::remove(path, ec);
    }
}

} // namespace

int RunSelfTest() {
    std::printf("Director 3D — самотест ядра анимации\n");
    std::printf("=====================================\n");

    TestCurves();
    TestPlayback();
    TestDocument();
    TestTracks();
    TestFileDialog();
    TestLocalization();
    TestCameraTrack();
    TestClipTracks();
    TestAudioTrackTimeline();
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
