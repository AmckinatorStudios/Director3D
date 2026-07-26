// ============================================================================
//  Director 3D — инструмент создания 3D-анимации на движке SAGE Engine.
//
//  Это ОТДЕЛЬНОЕ приложение поверх движка, а не его форк: линкует ту же
//  библиотеку sage::engine, что и игры с редактором, и добавляет свой слой с
//  интерфейсом на ImGui. Движок отвечает за окно, рендер, сцену/ECS, скелетную
//  анимацию, ресурсы и звук; Director 3D — за таймлайн, кривые, транспорт и
//  вывод готового ролика.
//
//  Точку входа даёт движок (SAGE_MAIN): он владеет главным циклом и обработкой
//  фатальных ошибок, а приложение лишь возвращает сконфигурированный
//  Application со своим слоем.
// ============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "sage/core/GameModule.h"
#include "sage/core/Log.h"

#include "DirectorLayer.h"
#include "SelfTest.h"

sage::Application* sage::CreateApplication(int argc, char** argv) {
    // Самотест ядра анимации гоняется БЕЗ окна и без OpenGL, поэтому он
    // обрабатывается до создания Application — и работает на CI-раннере без
    // видеокарты и без иксов.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) {
            Log::Init("director3d_selftest.log");
            std::exit(d3d::RunSelfTest());
        }
    }

    // Первый аргумент без дефиса — проект, который надо открыть при запуске.
    // Так работает открытие двойным щелчком по .d3dproj и «Открыть с помощью»
    // в проводнике; без этого файл проекта нельзя открыть иначе как из меню.
    std::string startupProject;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-') {
            startupProject = argv[i];
            break;
        }
    }

    // Пакетный рендер: открыть проект, снять ролик, выйти. Нужен там, где за
    // мышью никого нет, — сборочная машина, очередь роликов в скрипте, проверка
    // «весь конвейер жив» в CI.
    d3d::RenderJob job;
    auto next = [&](int& i) -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--render") == 0) {
            if (const char* v = next(i)) { job.Active = true; job.Output = v; }
        } else if (std::strcmp(arg, "--showcase") == 0) {
            job.Showcase = true;
        } else if (std::strcmp(arg, "--width") == 0) {
            if (const char* v = next(i)) job.Width = std::atoi(v);
        } else if (std::strcmp(arg, "--height") == 0) {
            if (const char* v = next(i)) job.Height = std::atoi(v);
        } else if (std::strcmp(arg, "--fps") == 0) {
            if (const char* v = next(i)) job.Fps = (float)std::atof(v);
        } else if (std::strcmp(arg, "--start") == 0) {
            if (const char* v = next(i)) job.StartTime = (float)std::atof(v);
        } else if (std::strcmp(arg, "--end") == 0) {
            if (const char* v = next(i)) job.EndTime = (float)std::atof(v);
        } else if (std::strcmp(arg, "--samples") == 0) {
            if (const char* v = next(i)) job.Samples = std::atoi(v);
        } else if (std::strcmp(arg, "--quality") == 0) {
            if (const char* v = next(i)) job.Quality = std::atoi(v);
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            std::printf(
                "Director 3D\n"
                "  Director3D [проект.d3dproj]            открыть проект в интерфейсе\n"
                "  Director3D --self-test                 самотест ядра, без окна и OpenGL\n"
                "  Director3D --render <файл|каталог> [...] снять ролик и выйти\n"
                "\nПакетный рендер:\n"
                "  --render <путь>   .mp4 — готовый ролик, иначе каталог с секвенцией PNG\n"
                "  --showcase        снять встроенную демо-постановку вместо проекта\n"
                "  --width/--height  разрешение (по умолчанию 1920x1080)\n"
                "  --fps <ч>         частота кадров (по умолчанию — из проекта)\n"
                "  --start/--end <с> диапазон в секундах (--end 0 — до конца ролика)\n"
                "  --samples <н>     сглаживание накоплением, 1 — выключено\n"
                "  --quality <crf>   качество H.264: 18 без потерь, 23 обычное\n"
                "\nMP4 требует ffmpeg в PATH. Окно создаётся всегда (нужен контекст\n"
                "OpenGL), но под xvfb-run экран и видеокарта не нужны.\n");
            std::exit(0);
        }
    }

    Log::Init("director3d.log");
    LOG_INFO("Director") << "Director 3D запускается...";

    sage::AppConfig config;
    config.Width = 1600;
    config.Height = 950;
    config.Title = "Director 3D";
    // Размер окна переопределяется переменными окружения. Нужно проверкам без
    // человека за монитором: под программным OpenGL (headless-раннер, xvfb)
    // полноразмерное окно с SSAO и bloom рисуется единицы кадров в минуту, и
    // на маленьком окне то же самое проверяется за секунды.
    if (const char* w = std::getenv("D3D_WINDOW_WIDTH")) config.Width = std::atoi(w);
    if (const char* h = std::getenv("D3D_WINDOW_HEIGHT")) config.Height = std::atoi(h);
    // Сглаживание экранного буфера не нужно: вся 3D-картинка идёт через
    // offscreen-буферы инструмента, а окно только показывает готовые текстуры.
    config.Msaa = 0;

    // Пакетному рендеру окно нужно только ради контекста OpenGL — на экране ему
    // делать нечего, а большое окно под программным рендерером стоит секунд.
    if (job.Active) { config.Width = 640; config.Height = 400; }

    auto* app = new sage::Application(config);
    app->PushLayer(std::make_unique<d3d::DirectorLayer>(startupProject, std::move(job)));
    return app;
}

SAGE_MAIN()
