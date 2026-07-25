#pragma once
#include <cstdio>
#include <string>
#include <vector>

namespace d3d {

// ---------------------------------------------------------------------------
// VideoWriter — запись кадров в видеофайл (H.264 в MP4) через ffmpeg.
//
// Почему через внешний ffmpeg, а не своим кодеком: H.264 — это либо libx264
// (GPL, отдельная зависимость с нетривиальной сборкой под три платформы), либо
// аппаратные API, у каждого свои. Тащить это в инструмент ради одной кнопки
// «сохранить видео» — плохой размен: ffmpeg стоит почти везде, ставится одной
// командой, и его наличие проверяется до начала рендера, а не в конце.
//
// Кадры уходят ffmpeg'у В STDIN сырыми (rawvideo RGB24) — без промежуточных
// PNG. Это принципиально: кодирование сотен кадров через файлы на диске тратит
// на порядок больше времени и места, а качество остаётся тем же.
//
// Если у проекта есть звуковая дорожка, она подмешивается вторым входом, и на
// выходе получается готовый ролик со звуком.
// ---------------------------------------------------------------------------
class VideoWriter {
public:
    struct Settings {
        std::string OutputPath = "render.mp4";
        int Width = 1920;
        int Height = 1080;
        float Fps = 24.0f;
        // Постоянное качество (CRF): 0 — без потерь, 18 — визуально без потерь,
        // 23 — обычное, 28 — заметно хуже. Битрейт кодек выберет сам.
        int Quality = 18;
        // Звук: путь к файлу и его смещение на таймлайне (секунды). Пусто — без звука.
        std::string AudioPath;
        float AudioOffset = 0.0f;
        // Время начала ролика: нужно, чтобы звук совпал с картинкой при
        // экспорте не с нуля.
        float StartTime = 0.0f;
    };

    ~VideoWriter();
    VideoWriter(const VideoWriter&) = delete;
    VideoWriter& operator=(const VideoWriter&) = delete;
    VideoWriter() = default;

    // Есть ли ffmpeg в PATH. Проверяется один раз и запоминается: спрашивать
    // это на каждый экспорт — лишний запуск процесса.
    static bool FfmpegAvailable();
    // Версия ffmpeg одной строкой (для окна настроек и лога). Пусто, если его нет.
    static const std::string& FfmpegVersion();

    // Запускает ffmpeg и открывает поток кадров. false + err, если ffmpeg
    // отсутствует, путь не создать или процесс не запустился.
    bool Begin(const Settings& settings, std::string& err);

    // Пишет один кадр. Данные — RGB24, ровно Width*Height*3 байт, СВЕРХУ ВНИЗ.
    // false, если поток оборвался (ffmpeg упал) — вызывающий обязан остановиться.
    bool WriteFrame(const unsigned char* rgb);

    // Закрывает поток и дожидается завершения ffmpeg. Возвращает false, если
    // кодировщик завершился с ошибкой; err содержит код возврата.
    bool Finish(std::string& err);

    // Аварийное закрытие без ожидания результата (отмена экспорта).
    void Cancel();

    bool Active() const { return m_pipe != nullptr; }
    const std::string& OutputPath() const { return m_settings.OutputPath; }

    // Сборка командной строки ffmpeg. Вынесена и публична, чтобы её можно было
    // проверить самотестом без запуска процесса: порядок аргументов у ffmpeg
    // значим (опции входа идут ПЕРЕД своим -i), и ошибка здесь ловится только
    // на живом запуске, что для теста слишком дорого.
    static std::string BuildCommand(const Settings& settings);

private:
    std::FILE* m_pipe = nullptr;
    Settings m_settings;
    size_t m_frameBytes = 0;
};

} // namespace d3d
