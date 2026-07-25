#include "render/SequenceExporter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "render/StageRenderer.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/core/Application.h"
#include "sage/core/Log.h"
#include "sage/render/Screenshot.h"
#include "sage/scene/Scene.h"

namespace fs = std::filesystem;

namespace d3d {

bool SequenceExporter::Begin(const Settings& settings, const AnimationDocument& doc,
                             Scene& scene, std::string& err) {
    m_settings = settings;
    m_failed = false;
    m_error.clear();
    m_active = false;

    if (m_settings.Width < 16 || m_settings.Height < 16) {
        err = "слишком маленькое разрешение кадра";
        return false;
    }
    // Проверяем камеру ДО создания каталога: иначе после отказа оставался бы
    // пустой каталог рендера.
    StageRenderer::CameraFrameInfo frame = StageRenderer::CameraFrameOf(
        scene, m_settings.CameraId, (float)m_settings.Width / (float)m_settings.Height);
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
    m_active = true;

    LOG_INFO("Export") << "Экспорт секвенции: " << m_total << " кадр(ов) "
                       << m_settings.Width << "x" << m_settings.Height
                       << " -> " << m_settings.OutputDir;
    return true;
}

bool SequenceExporter::Step(Scene& scene, StageRenderer& renderer, AnimationDocument& doc) {
    if (!m_active) return false;

    if (!m_target || m_target->Width() != m_settings.Width || m_target->Height() != m_settings.Height) {
        m_target.emplace(m_settings.Width, m_settings.Height);
    }

    const int batch = std::max(m_settings.FramesPerStep, 1);
    for (int i = 0; i < batch && m_current < m_total; ++i) {
        const int frame = m_firstFrame + m_current;
        const float time = (float)frame / m_fps;

        // Ставим мир ровно на этот момент. seeking=true принципиально: поза
        // скелетных клипов не должна зависеть от того, какой кадр рисовали до
        // этого, иначе секвенция «поплывёт» при любом пропуске.
        doc.Apply(scene, time, /*seeking=*/true);
        sage::anim::UpdateAnimators(scene, 0.0f);

        LightingEnvironment env = CollectVisibleLighting(scene);
        renderer.RenderShadow(scene, env);
        if (!renderer.RenderToTarget(scene, env, m_settings.CameraId, *m_target)) {
            m_failed = true;
            m_error = "камера пропала посреди экспорта";
            m_active = false;
            m_target.reset();
            return false;
        }

        // Читаем ИМЕННО из буфера рендера, а не из экрана: разрешение экспорта
        // не связано с размером окна, и панели инструмента в кадр не попадают.
        m_target->Bind();
        char name[64];
        std::snprintf(name, sizeof(name), "%s_%05d.png", m_settings.BaseName.c_str(), frame);
        const fs::path out = fs::path(m_settings.OutputDir) / name;
        SaveScreenshot(out.string(), m_settings.Width, m_settings.Height);

        ++m_current;
    }

    sage::Application::Get().Device().BindDefaultFramebuffer();

    if (m_current >= m_total) {
        m_active = false;
        m_target.reset(); // экспорт закончен — VRAM под кадр больше не нужна
        LOG_INFO("Export") << "Экспорт завершён: " << m_total << " кадр(ов) в " << m_settings.OutputDir;
        return false;
    }
    return true;
}

void SequenceExporter::Cancel() {
    if (!m_active) return;
    m_active = false;
    m_target.reset();
    LOG_INFO("Export") << "Экспорт прерван на кадре " << m_current << " из " << m_total;
}

} // namespace d3d
