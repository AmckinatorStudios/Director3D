#pragma once
#include <optional>
#include <string>

#include "anim/AnimationDocument.h"
#include "sage/render/Framebuffer.h"

class Scene;

namespace d3d {

class StageRenderer;

// ---------------------------------------------------------------------------
// SequenceExporter — вывод готового ролика в файлы.
//
// Пишет секвенцию PNG (кадр = файл вида name_0001.png) — универсальный формат,
// который принимает любой монтажный пакет и любой сборщик видео (ffmpeg). Свой
// видеокодек инструмент не тащит: это отдельная большая зависимость, а секвенция
// не теряет качество и позволяет пересобрать ролик с любым битрейтом.
//
// Экспорт идёт ПО КАДРАМ и синхронно, кадр за кадром: для каждого номера кадра
// время ставится точно (time = frame / fps), документ применяется в режиме
// перемотки (позы клипов встают на своё место независимо от истории), сцена
// рисуется в буфер нужного разрешения и читается в файл. Поэтому результат не
// зависит ни от частоты кадров экрана, ни от того, тормозила ли машина, — в
// отличие от «записи с экрана».
//
// Экспорт растянут по кадрам приложения (Step вызывается из главного цикла),
// чтобы окно не висело намертво на длинном ролике и показывало прогресс.
// ---------------------------------------------------------------------------
class SequenceExporter {
public:
    struct Settings {
        std::string OutputDir = "render";
        std::string BaseName = "frame";
        int Width = 1920;
        int Height = 1080;
        float StartTime = 0.0f;
        float EndTime = 0.0f;   // 0 — до конца ролика
        int CameraId = -1;      // с какой камеры снимаем
        // Сколько кадров писать за один кадр приложения: больше — быстрее
        // экспорт, но интерфейс отзывается реже.
        int FramesPerStep = 1;
    };

    // Готовит экспорт: создаёт каталог, считает диапазон кадров. err — причина
    // отказа (нет камеры, каталог не создать). Возвращает false, не начав.
    bool Begin(const Settings& settings, const AnimationDocument& doc, Scene& scene, std::string& err);

    // Пишет очередную порцию кадров. Возвращает false, когда экспорт завершён
    // или прерван; подробности — в Failed()/Error().
    bool Step(Scene& scene, StageRenderer& renderer, AnimationDocument& doc);

    void Cancel();

    bool Active() const { return m_active; }
    bool Failed() const { return m_failed; }
    const std::string& Error() const { return m_error; }
    int CurrentFrame() const { return m_current; }
    int TotalFrames() const { return m_total; }
    float Progress() const { return m_total > 0 ? (float)m_current / (float)m_total : 0.0f; }
    const std::string& OutputDir() const { return m_settings.OutputDir; }

private:
    Settings m_settings;
    // Буфер рендера живёт на всё время экспорта и освобождается по его
    // окончании: пересоздавать текстуры на каждый кадр — сотни лишних
    // аллокаций в GPU-памяти подряд, держать вечно — занятая VRAM без дела.
    std::optional<Framebuffer> m_target;
    bool m_active = false;
    bool m_failed = false;
    std::string m_error;
    int m_current = 0;
    int m_total = 0;
    int m_firstFrame = 0;
    float m_fps = 24.0f;
};

} // namespace d3d
