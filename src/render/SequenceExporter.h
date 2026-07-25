#pragma once
#include <optional>
#include <string>
#include <vector>

#include "anim/AnimationDocument.h"
#include "render/VideoWriter.h"
#include "sage/render/Framebuffer.h"

class Scene;
struct LightingEnvironment;

namespace d3d {

class StageRenderer;

// ---------------------------------------------------------------------------
// SequenceExporter — вывод готового ролика в файлы.
//
// Два формата вывода:
//   • MP4 (H.264) — готовый ролик, который можно сразу смотреть и отправлять.
//     Кадры уходят кодировщику потоком, без промежуточных файлов (см.
//     VideoWriter); при наличии звуковой дорожки она подмешивается в тот же файл;
//   • секвенция PNG — кадр в файл вида name_00001.png. Без потерь, принимается
//     любым монтажным пакетом, и работает там, где нет ffmpeg.
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
    // Куда пишем результат.
    enum class Format {
        Mp4,          // готовый ролик H.264 (нужен ffmpeg)
        PngSequence,  // кадр = файл, работает всегда
    };

    struct Settings {
        Format OutputFormat = Format::Mp4;
        std::string OutputDir = "render";
        std::string BaseName = "frame";
        // Качество H.264 (CRF): 18 — визуально без потерь, 23 — обычное.
        int Quality = 18;
        bool IncludeAudio = true; // подмешать звуковую дорожку проекта в MP4
        int Width = 1920;
        int Height = 1080;
        // Сглаживание накоплением: кадр снимается Samples раз с микросдвигом
        // проекции, результаты усредняются. 1 — выключено. Это НЕ экранный
        // фильтр: усреднение убирает и лесенку, и мерцание тонких деталей, и
        // шум шейдеров — ценой линейного роста времени рендера. В чистовом
        // выводе время есть, в интерактивном вьюпорте его нет (там FXAA).
        int Samples = 1;
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

private:
    // Снимает кадр несколько раз с микросдвигом и усредняет прямо в m_frameBuffer.
    // false — камера пропала посреди экспорта.
    bool RenderAccumulated(Scene& scene, StageRenderer& renderer, const LightingEnvironment& env,
                           int samples);

public:

    bool Active() const { return m_active; }
    bool Failed() const { return m_failed; }
    const std::string& Error() const { return m_error; }
    int CurrentFrame() const { return m_current; }
    int TotalFrames() const { return m_total; }
    float Progress() const { return m_total > 0 ? (float)m_current / (float)m_total : 0.0f; }
    const std::string& OutputDir() const { return m_settings.OutputDir; }
    // Человекочитаемый путь результата — то, что показывается в статус-баре.
    const std::string& ResultPath() const { return m_resultPath; }

private:
    Settings m_settings;
    // Буфер рендера живёт на всё время экспорта и освобождается по его
    // окончании: пересоздавать текстуры на каждый кадр — сотни лишних
    // аллокаций в GPU-памяти подряд, держать вечно — занятая VRAM без дела.
    std::optional<Framebuffer> m_target;
    // Кодировщик и буфер чтения кадра. Буфер переиспользуется: выделять
    // мегабайты на каждый кадр — это заметная нагрузка на аллокатор.
    VideoWriter m_video;
    std::vector<unsigned char> m_frameBuffer;
    // Накопитель для сглаживания. Сумма в float: складывать сотни выборок в
    // байтах — это потерять всё, ради чего сглаживание затевалось.
    std::vector<float> m_accum;
    std::string m_resultPath;
    bool m_active = false;
    bool m_failed = false;
    std::string m_error;
    int m_current = 0;
    int m_total = 0;
    int m_firstFrame = 0;
    float m_fps = 24.0f;
};

} // namespace d3d
