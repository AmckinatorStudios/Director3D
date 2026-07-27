#include "render/SequenceExporter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "render/StageRenderer.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/core/Application.h"
#include "sage/core/Log.h"
#include "sage/render/ParticleECS.h"
#include "sage/render/Screenshot.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Scene.h"

namespace fs = std::filesystem;

namespace d3d {

bool SequenceExporter::Begin(const Settings& settings, const AnimationDocument& doc,
                             Scene& scene, std::string& err) {
    m_settings = settings;
    m_failed = false;
    m_error.clear();
    m_active = false;
    m_resultPath.clear();

    if (m_settings.Width < 16 || m_settings.Height < 16) {
        err = "слишком маленькое разрешение кадра";
        return false;
    }
    // Проверяем камеру ДО создания каталога и запуска кодировщика: иначе после
    // отказа оставался бы пустой каталог рендера и повисший процесс.
    StageRenderer::CameraFrameInfo frame = StageRenderer::CameraFrameOf(
        scene, doc.CameraAt(m_settings.StartTime, m_settings.CameraId),
        (float)m_settings.Width / (float)m_settings.Height);
    if (!frame.HasCamera) {
        err = "в сцене нет камеры — снимать нечем";
        return false;
    }

    std::error_code ec;
    fs::create_directories(m_settings.OutputDir, ec);
    if (ec) {
        err = "не удалось создать каталог " + m_settings.OutputDir + ": " + ec.message();
        return false;
    }

    m_fps = doc.Fps > 0.0f ? doc.Fps : 24.0f;
    const float end = m_settings.EndTime > m_settings.StartTime ? m_settings.EndTime : doc.Duration;
    m_firstFrame = (int)std::lround(std::max(m_settings.StartTime, 0.0f) * m_fps);
    const int lastFrame = (int)std::lround(end * m_fps);
    m_total = std::max(lastFrame - m_firstFrame + 1, 1);
    m_current = 0;

    if (m_settings.OutputFormat == Format::Mp4) {
        VideoWriter::Settings video;
        video.OutputPath = (fs::path(m_settings.OutputDir) / (m_settings.BaseName + ".mp4")).string();
        video.Width = m_settings.Width;
        video.Height = m_settings.Height;
        video.Fps = m_fps;
        video.Quality = m_settings.Quality;
        video.StartTime = (float)m_firstFrame / m_fps;
        if (m_settings.IncludeAudio && doc.Audio.Loaded() && !doc.Audio.Muted) {
            video.AudioPath = doc.Audio.Path();
            video.AudioOffset = doc.Audio.Offset;
        }
        if (!m_video.Begin(video, err)) return false;
        m_resultPath = video.OutputPath;
        // Буфер под один кадр в RGB — переиспользуется до конца экспорта.
        m_frameBuffer.assign((size_t)m_settings.Width * m_settings.Height * 3u, 0);
    } else {
        m_resultPath = m_settings.OutputDir;
    }

    m_active = true;
    LOG_INFO("Export") << "Экспорт: " << m_total << " кадр(ов) " << m_settings.Width << "x"
                       << m_settings.Height << " -> " << m_resultPath;
    return true;
}

bool SequenceExporter::Step(Scene& scene, StageRenderer& renderer, AnimationDocument& doc) {
    if (!m_active) return false;

    if (!m_target || m_target->Width() != m_settings.Width || m_target->Height() != m_settings.Height) {
        m_target.emplace(m_settings.Width, m_settings.Height);
    }

    auto fail = [&](const std::string& reason) {
        m_failed = true;
        m_error = reason;
        m_active = false;
        m_video.Cancel();
        m_target.reset();
        return false;
    };

    const int batch = std::max(m_settings.FramesPerStep, 1);
    for (int i = 0; i < batch && m_current < m_total; ++i) {
        const int frame = m_firstFrame + m_current;
        const float time = (float)frame / m_fps;

        // Ставим мир ровно на этот момент. seeking=true принципиально: поза
        // скелетных клипов не должна зависеть от того, какой кадр рисовали до
        // этого, иначе секвенция «поплывёт» при любом пропуске.
        doc.Apply(scene, time, /*seeking=*/true);
        sage::anim::UpdateAnimators(scene, 0.0f);

        // Частицы шагаем САМИ, на длительность одного кадра ролика.
        //
        // Всё остальное в кадре ставится по абсолютному времени, а поток частиц
        // так поставить нельзя: это симуляция, у неё есть только «продвинуть на
        // dt». Раньше её двигал главный цикл своим dt, и получалась ерунда —
        // экспорт пишет FramesPerStep кадров за один кадр приложения, то есть
        // на восемь записанных кадров приходился один шаг симуляции неизвестной
        // длины. В ролике это выглядело как замерший или дёргающийся эмиттер, а
        // на быстрой машине результат отличался от результата на медленной.
        //
        // Шаг ровно 1/fps делает поток и правильным по скорости, и
        // воспроизводимым: тот же проект даёт тот же ролик.
        sage::fx::UpdateEmitters(scene, renderer.Particles(), 1.0f / m_fps);

        LightingEnvironment env = CollectVisibleLighting(scene);

        // Камера выбирается НА КАЖДОМ КАДРЕ, а не один раз на весь экспорт:
        // в этом и состоит монтаж. Настройка из диалога рендера остаётся
        // запасной — ею снимается всё, где склеек нет.
        //
        // Выбор стоит ДО прохода теней: каскады строятся под камеру кадра, и
        // считать их до того, как известно, какая камера снимает, не из чего.
        const int cameraId = doc.CameraAt(time, m_settings.CameraId);

        ShadowMap::CameraView shadowCam;
        const bool haveShadowCam = renderer.CascadeViewOf(
            scene, cameraId,
            (float)m_target->Width() / (float)std::max(m_target->Height(), 1), shadowCam);
        renderer.RenderShadow(scene, env, haveShadowCam ? &shadowCam : nullptr);

        const int samples = std::max(m_settings.Samples, 1);
        if (samples == 1) {
            if (!renderer.RenderToTarget(scene, env, cameraId, *m_target)) {
                return fail("камера пропала посреди экспорта");
            }
        } else if (!RenderAccumulated(scene, renderer, env, cameraId, samples)) {
            return fail("камера пропала посреди экспорта");
        }

        // Кадр записан — шаг времени закрыт, положение объектов уходит в
        // историю смаза движения. Здесь на шаг приходится ровно один вид,
        // поэтому вызов стоит сразу после отрисовки.
        renderer.EndTimeStep();

        // Читаем ИМЕННО из буфера рендера, а не из экрана: разрешение экспорта
        // не связано с размером окна, и панели инструмента в кадр не попадают.
        m_target->Bind();
        if (m_settings.OutputFormat == Format::Mp4) {
            sage::rhi::GraphicsDevice::Get().ReadPixelsRGB(0, 0, m_settings.Width, m_settings.Height,
                                                           m_frameBuffer.data());
            // GPU отдаёт строки снизу вверх, а кодировщик ждёт сверху вниз.
            // Переворачиваем на месте, меняя строки местами.
            const size_t stride = (size_t)m_settings.Width * 3u;
            for (int y = 0; y < m_settings.Height / 2; ++y) {
                unsigned char* top = m_frameBuffer.data() + (size_t)y * stride;
                unsigned char* bottom = m_frameBuffer.data() +
                                        (size_t)(m_settings.Height - 1 - y) * stride;
                std::swap_ranges(top, top + stride, bottom);
            }
            if (!m_video.WriteFrame(m_frameBuffer.data())) {
                return fail("кодировщик оборвал приём кадров — проверьте вывод ffmpeg в логе");
            }
        } else {
            char name[64];
            std::snprintf(name, sizeof(name), "%s_%05d.png", m_settings.BaseName.c_str(), frame);
            const fs::path out = fs::path(m_settings.OutputDir) / name;
            SaveScreenshot(out.string(), m_settings.Width, m_settings.Height);
        }

        ++m_current;
    }

    sage::Application::Get().Device().BindDefaultFramebuffer();

    if (m_current >= m_total) {
        m_active = false;
        m_target.reset(); // экспорт закончен — VRAM под кадр больше не нужна
        if (m_settings.OutputFormat == Format::Mp4) {
            std::string err;
            if (!m_video.Finish(err)) {
                m_failed = true;
                m_error = err;
                return false;
            }
        }
        LOG_INFO("Export") << "Экспорт завершён: " << m_total << " кадр(ов) в " << m_resultPath;
        return false;
    }
    return true;
}

bool SequenceExporter::RenderAccumulated(Scene& scene, StageRenderer& renderer,
                                         const LightingEnvironment& env, int cameraId,
                                         int samples) {
    const size_t pixels = (size_t)m_settings.Width * m_settings.Height * 3u;
    if (m_accum.size() != pixels) m_accum.assign(pixels, 0.0f);
    else std::fill(m_accum.begin(), m_accum.end(), 0.0f);
    if (m_frameBuffer.size() != pixels) m_frameBuffer.assign(pixels, 0);

    for (int s = 0; s < samples; ++s) {
        // Последовательность Холтона по основаниям 2 и 3: точки ложатся в
        // пиксель равномерно при ЛЮБОМ числе выборок, в отличие от регулярной
        // сетки (она требует полного квадрата) и случайных точек (те сбиваются
        // в кучки, и часть пикселя остаётся неохваченной).
        auto halton = [](int index, int base) {
            float result = 0.0f, f = 1.0f;
            for (int i = index + 1; i > 0; i /= base) {
                f /= (float)base;
                result += f * (float)(i % base);
            }
            return result;
        };
        const glm::vec2 jitter(halton(s, 2) - 0.5f, halton(s, 3) - 0.5f);

        if (!renderer.RenderToTarget(scene, env, cameraId, *m_target, jitter)) {
            return false;
        }
        m_target->Bind();
        sage::rhi::GraphicsDevice::Get().ReadPixelsRGB(0, 0, m_settings.Width, m_settings.Height,
                                                       m_frameBuffer.data());
        for (size_t i = 0; i < pixels; ++i) m_accum[i] += (float)m_frameBuffer[i];
    }

    const float inv = 1.0f / (float)samples;
    for (size_t i = 0; i < pixels; ++i) {
        m_frameBuffer[i] = (unsigned char)std::lround(std::min(m_accum[i] * inv, 255.0f));
    }
    return true;
}

void SequenceExporter::Cancel() {
    if (!m_active) return;
    m_active = false;
    m_video.Cancel();
    m_target.reset();
    LOG_INFO("Export") << "Экспорт прерван на кадре " << m_current << " из " << m_total;
}


// ============================================================================
//  Очередь заданий
// ============================================================================

int RenderQueue::Remaining() const {
    int left = 0;
    for (const RenderQueueJob& job : Jobs) {
        if (!job.Done) ++left;
    }
    return left;
}

bool RenderQueue::TakeNext(SequenceExporter::Settings& outSettings) {
    for (size_t i = 0; i < Jobs.size(); ++i) {
        if (Jobs[i].Done) continue;
        m_current = (int)i;
        outSettings = Jobs[i].Settings;
        return true;
    }
    m_current = -1;
    return false;
}

void RenderQueue::FinishCurrent(const std::string& error) {
    if (m_current < 0 || m_current >= (int)Jobs.size()) return;
    RenderQueueJob& job = Jobs[(size_t)m_current];
    job.Done = true;
    // Пустая ошибка — успех. Отдельного флага нет намеренно: два поля,
    // означающие одно и то же, рано или поздно разъезжаются.
    job.Result = error.empty() ? std::string("Готово") : error;
    m_current = -1;
}

void RenderQueue::Reset() {
    for (RenderQueueJob& job : Jobs) {
        job.Done = false;
        job.Result.clear();
    }
    m_current = -1;
}

} // namespace d3d
