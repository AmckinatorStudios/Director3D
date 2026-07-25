#pragma once
#include <memory>
#include <string>

#include "anim/AnimationDocument.h"

class Scene;

// ---------------------------------------------------------------------------
// Файл проекта Director 3D (.d3dproj) — один JSON, в котором лежит и сцена, и
// анимация, и настройки монтажа.
//
// Сцена внутри сохраняется движковым SceneSerializer (тот же формат, что и
// .sage), поэтому проект не форкает формат сцены: объекты, меши, материалы,
// свет и иерархия остаются движковыми и их понимает редактор SAGE. Сверху
// Director 3D добавляет свои разделы:
//   "director"  — компоненты инструмента (киношная камера, видимость);
//   "animation" — дорожки, ключи, блоки клипов, метки, звук;
//   "timeline"  — fps, длина, положение головки.
//
// Один файл вместо связки «сцена + отдельный файл анимации» выбран сознательно:
// анимация ссылается на объекты сцены по id, и разъехавшиеся половинки давали
// бы молча сломанный проект. Экспорт «только сцены» в .sage при этом остаётся
// (см. SaveSceneOnly) — им и обмениваются с движком.
// ---------------------------------------------------------------------------
namespace d3d {

struct ProjectFile {
    // Сохраняет сцену + документ + состояние монтажа. err — текст ошибки.
    static bool Save(const std::string& path, Scene& scene, const AnimationDocument& doc,
                     float playheadTime, std::string& err);

    // Загружает проект: создаёт НОВУЮ сцену (возвращается через outScene) и
    // заполняет документ. Сцена создаётся, а не правится на месте, потому что
    // движковый SceneSerializer::Load отдаёт готовый unique_ptr<Scene>, и это
    // же гарантирует, что от прошлого проекта ничего не осталось.
    static bool Load(const std::string& path, std::unique_ptr<Scene>& outScene,
                     AnimationDocument& doc, float& outPlayheadTime, std::string& err);

    // Экспорт только сцены в движковый .sage (без анимации) — чтобы открыть в
    // SAGE Editor или загрузить в игру.
    static bool SaveSceneOnly(const std::string& path, Scene& scene, std::string& err);

    // Снимки в память для undo/redo и для отката сцены после предпросмотра.
    // Формат тот же, что и у файла, — один код сериализации на оба применения.
    static std::string SnapshotToString(Scene& scene, const AnimationDocument& doc);
    static bool RestoreFromString(const std::string& json, std::unique_ptr<Scene>& outScene,
                                  AnimationDocument& doc, std::string& err);

    static constexpr const char* kExtension = ".d3dproj";
    static constexpr int kFormatVersion = 1;
};

} // namespace d3d
