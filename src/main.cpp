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
#include <cstdlib>
#include <cstring>
#include <memory>

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

    auto* app = new sage::Application(config);
    app->PushLayer(std::make_unique<d3d::DirectorLayer>());
    return app;
}

SAGE_MAIN()
