#include "render/VideoWriter.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#include "sage/core/Log.h"

#ifdef _WIN32
#define D3D_POPEN _popen
#define D3D_PCLOSE _pclose
// Windows различает текстовый и двоичный режим: без 'b' каждый байт 0x0A в
// кадре превратился бы в 0x0D 0x0A, и поток кадров съехал бы на первом же
// пикселе.
#define D3D_PIPE_WRITE_MODE "wb"
#else
#define D3D_POPEN popen
#define D3D_PCLOSE pclose
// POSIX-режимы popen — только "r"/"w" (+"e"); glibc на "wb" возвращает NULL с
// EINVAL, то есть кодировщик молча не запускается. Двоичного режима здесь нет.
#define D3D_PIPE_WRITE_MODE "w"
#endif

namespace fs = std::filesystem;

namespace d3d {

namespace {

// Запускает команду и возвращает её первую строку вывода. Пусто, если команду
// не удалось выполнить.
std::string RunAndReadLine(const char* command) {
    std::FILE* pipe = D3D_POPEN(command, "r");
    if (!pipe) return {};
    char buffer[512] = {};
    const char* line = std::fgets(buffer, sizeof(buffer), pipe);
    D3D_PCLOSE(pipe);
    if (!line) return {};
    std::string out(buffer);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

// Экранирование пути для командной строки. Путь приходит от пользователя, и
// пробел или кавычка в нём не должны ни ломать команду, ни давать возможность
// дописать к ней произвольный shell-вызов.
std::string Quote(const std::string& value) {
#ifdef _WIN32
    // cmd.exe: двойные кавычки, внутренние удваиваются.
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
#else
    // POSIX-оболочка: одинарные кавычки отключают ВСЮ подстановку; единственный
    // спецслучай — сама одинарная кавычка, её закрываем и вставляем экранированной.
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
#endif
}

} // namespace

bool VideoWriter::FfmpegAvailable() {
    return !FfmpegVersion().empty();
}

const std::string& VideoWriter::FfmpegVersion() {
    // Проверяем один раз за запуск: поиск и старт процесса на каждый вопрос —
    // это заметная задержка в диалоге настроек, который перерисовывается каждый кадр.
    static const std::string version = [] {
#ifdef _WIN32
        std::string line = RunAndReadLine("ffmpeg -version 2>NUL");
#else
        std::string line = RunAndReadLine("ffmpeg -version 2>/dev/null");
#endif
        if (line.rfind("ffmpeg version", 0) != 0) return std::string{};
        LOG_INFO("Video") << "Найден кодировщик: " << line;
        return line;
    }();
    return version;
}

std::string VideoWriter::BuildCommand(const Settings& s) {
    std::ostringstream cmd;
    cmd << "ffmpeg -hide_banner -loglevel error -y";

    // --- Вход 1: сырые кадры из stdin ---
    // Опции входа обязаны идти ПЕРЕД своим -i, иначе ffmpeg отнесёт их к выходу.
    cmd << " -f rawvideo -pixel_format rgb24"
        << " -video_size " << s.Width << "x" << s.Height
        << " -framerate " << s.Fps
        << " -i -";

    // --- Вход 2: звук (если есть) ---
    const bool withAudio = !s.AudioPath.empty();
    if (withAudio) {
        // Звук подрезаем с того момента, которому соответствует первый кадр:
        // ролик может рендериться не с нуля, и без сдвига звук уехал бы.
        const float seek = s.StartTime - s.AudioOffset;
        if (seek > 0.0f) cmd << " -ss " << seek;
        cmd << " -i " << Quote(s.AudioPath);
    }

    // --- Кодирование видео ---
    // yuv420p обязателен для совместимости: без него QuickTime и часть плееров
    // и браузеров просто не открывают файл, хотя ffmpeg его пишет.
    cmd << " -c:v libx264 -preset medium -crf " << s.Quality
        << " -pix_fmt yuv420p -movflags +faststart";

    if (withAudio) {
        // -shortest: ролик кончается вместе с картинкой, даже если звук длиннее.
        cmd << " -c:a aac -b:a 192k -shortest";
    }

    cmd << " " << Quote(s.OutputPath);
    return cmd.str();
}

bool VideoWriter::Begin(const Settings& settings, std::string& err) {
    if (m_pipe) {
        err = "запись уже идёт";
        return false;
    }
    if (!FfmpegAvailable()) {
        err = "не найден ffmpeg — он нужен для записи MP4. "
              "Ubuntu/Debian: sudo apt install ffmpeg; macOS: brew install ffmpeg; "
              "Windows: winget install ffmpeg. "
              "Либо выберите вывод секвенцией PNG — она работает без него.";
        return false;
    }
    if (settings.Width < 16 || settings.Height < 16) {
        err = "слишком маленькое разрешение кадра";
        return false;
    }
    // H.264 не кодирует нечётные размеры при yuv420p. Ловим здесь, а не через
    // невнятное падение кодировщика на середине рендера.
    if (settings.Width % 2 != 0 || settings.Height % 2 != 0) {
        err = "ширина и высота должны быть чётными (требование H.264)";
        return false;
    }

    m_settings = settings;
    std::error_code ec;
    const fs::path out(settings.OutputPath);
    if (out.has_parent_path()) {
        fs::create_directories(out.parent_path(), ec);
        if (ec) {
            err = "не удалось создать каталог " + out.parent_path().string() + ": " + ec.message();
            return false;
        }
    }

    const std::string command = BuildCommand(settings);
    LOG_INFO("Video") << "Запуск кодировщика: " << command;

    m_pipe = D3D_POPEN(command.c_str(), D3D_PIPE_WRITE_MODE);
    if (!m_pipe) {
        err = "не удалось запустить ffmpeg";
        return false;
    }
    m_frameBytes = (size_t)settings.Width * settings.Height * 3u;
    return true;
}

bool VideoWriter::WriteFrame(const unsigned char* rgb) {
    if (!m_pipe || !rgb) return false;
    const size_t written = std::fwrite(rgb, 1, m_frameBytes, m_pipe);
    if (written != m_frameBytes) {
        // Обрыв канала означает, что ffmpeg завершился (чаще всего с ошибкой).
        // Продолжать писать бессмысленно — вызывающий останавливает экспорт.
        LOG_ERROR("Video") << "Кодировщик оборвал приём кадров (" << written << " из "
                           << m_frameBytes << " байт)";
        return false;
    }
    return true;
}

bool VideoWriter::Finish(std::string& err) {
    if (!m_pipe) {
        err = "запись не запускалась";
        return false;
    }
    const int status = D3D_PCLOSE(m_pipe);
    m_pipe = nullptr;

    if (status != 0) {
        err = "ffmpeg завершился с кодом " + std::to_string(status) +
              " — подробности в его выводе выше";
        LOG_ERROR("Video") << err;
        return false;
    }

    std::error_code ec;
    const uintmax_t size = fs::file_size(m_settings.OutputPath, ec);
    if (ec || size == 0) {
        err = "файл " + m_settings.OutputPath + " не создан или пуст";
        LOG_ERROR("Video") << err;
        return false;
    }
    LOG_INFO("Video") << "Видео записано: " << m_settings.OutputPath << " ("
                      << (double)size / (1024.0 * 1024.0) << " МБ)";
    return true;
}

void VideoWriter::Cancel() {
    if (!m_pipe) return;
    D3D_PCLOSE(m_pipe);
    m_pipe = nullptr;
    // Недописанный файл оставлять нельзя: он выглядит как готовый результат,
    // но не открывается.
    std::error_code ec;
    fs::remove(m_settings.OutputPath, ec);
    LOG_INFO("Video") << "Запись видео прервана, неполный файл удалён";
}

VideoWriter::~VideoWriter() {
    if (m_pipe) Cancel();
}

} // namespace d3d
