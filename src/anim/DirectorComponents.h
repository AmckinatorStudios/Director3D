#pragma once
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sage/anim/Skeleton.h"

#include "anim/Constraints.h" // ConstraintComponent — тоже компонент инструмента

// ---------------------------------------------------------------------------
// Собственные компоненты Director 3D.
//
// SAGE — настоящая ECS: состав сущности собирается из компонентов, и любое
// приложение поверх движка добавляет СВОИ, не трогая ядро (см. комментарий в
// sage/scene/Components.h). Инструменту анимации нужны данные, которых у
// игрового движка нет и быть не должно — киношные параметры камеры, пост-
// обработка на конкретной камере, видимость/блокировка элемента в аутлайнере.
// Живут они здесь и сериализуются в .d3dproj самим Director 3D, а движковый
// SceneSerializer их просто не видит — .sage-сцена остаётся совместимой.
// ---------------------------------------------------------------------------
namespace d3d {

// Параметры камеры, которых нет в движковом CameraComponent: то, чем оперирует
// не программист, а оператор — дистанция фокуса, диафрагма, глубина резкости,
// рамка кадра, тряска. Пост-обработка тоже здесь: в кино это свойство КАМЕРЫ
// (у крупного плана свой грейд), а не глобальная настройка приложения.
struct CineCameraComponent {
    // --- Оптика ---
    float FocusDistance = 15.0f; // метры до плоскости фокуса
    bool AutoFocus = true;       // фокус наводится на выбранный объект автоматически
    float Aperture = 2.8f;       // f-число: меньше — сильнее размытие фона
    bool DepthOfField = true;

    // --- Кадр ---
    bool FilmBack = true;        // рамка кадра (безопасная зона) во вьюпорте
    float AspectRatio = 1.85f;   // соотношение сторон рамки кадра

    // --- Тряска камеры («живая» камера с рук) ---
    bool CameraShake = false;
    float ShakeAmplitude = 0.05f; // метры
    float ShakeFrequency = 6.0f;  // колебаний в секунду

    // --- Пост-обработка кадра этой камеры ---
    bool Bloom = true;              float BloomIntensity = 0.60f;
    bool MotionBlur = true;         float MotionBlurAmount = 0.50f;
    bool ColorGrading = true;       float ColorGradingAmount = 1.00f;
    bool Vignette = true;           float VignetteAmount = 0.30f;
    bool ChromaticAberration = true; float ChromaticAmount = 0.20f;
};

// Состояние элемента в аутлайнере: глазок (видимость) и замок (защита от
// случайного перетаскивания гизмо). Это свойство ПРОСМОТРА, а не игры —
// поэтому компонент инструмента, а не движка.
struct StageItemComponent {
    bool Visible = true;
    bool Locked = false;
};

// Пометка «этот объект создан импортом файла» — Assets-панель показывает, из
// чего собрана сцена, а повторный импорт того же файла переиспользует путь.
struct SourceAssetComponent {
    std::string Path;
};

// Поза персонажа, поставленная РУКАМИ, — то, чем ручная анимация костей
// отличается от проигрывания готового клипа.
//
// Хранит переопределения локальных TRS костей (sage::anim::JointPose): движок
// применяет их поверх позы клипа, покостно и покомпонентно. Поэтому руками
// правится ровно то, что нужно, а остальное продолжает вести клип — можно
// взять готовую ходьбу и доработать поворот головы, не трогая ноги.
//
// Компонент инструмента, а не движка: это АВТОРСКИЕ данные, они живут в
// .d3dproj рядом с кривыми. В сцену (.sage) они не уезжают — там персонаж
// остаётся с обычными клипами.
//
// Значения здесь — состояние на ТЕКУЩЕМ кадре: их пишет AnimationDocument::Apply
// из кривых костных дорожек, а гизмо правит напрямую (и дальше это состояние
// снимается ключом). Ровно та же схема, что у Transform обычного объекта, —
// сцена всегда хранит «как сейчас», документ хранит «как во времени».
struct PoseComponent {
    std::vector<sage::anim::JointPose> Joints;

    // Гарантирует нужный размер, не теряя уже поставленного. Скелет становится
    // известен только после загрузки модели, поэтому вектор растёт лениво.
    sage::anim::JointPose& Ensure(int joint, int skeletonSize) {
        if ((int)Joints.size() < skeletonSize) Joints.resize((size_t)skeletonSize);
        return Joints[(size_t)joint];
    }

    const sage::anim::JointPose* Find(int joint) const {
        if (joint < 0 || joint >= (int)Joints.size()) return nullptr;
        return &Joints[(size_t)joint];
    }

    bool Any() const {
        for (const sage::anim::JointPose& j : Joints) {
            if (j.Any()) return true;
        }
        return false;
    }
};

} // namespace d3d
