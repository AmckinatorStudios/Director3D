cmake_minimum_required(VERSION 3.18)
project(Director3D LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Как и у движка: без явного выбора получилась бы сборка без оптимизаций (-O0),
# а Director 3D крутит рендер сцены и вычисление кривых каждый кадр.
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Тип сборки (по умолчанию Release)" FORCE)
    message(STATUS "Director3D: CMAKE_BUILD_TYPE не задан — использую Release")
endif()

# ============================================================================
#  Поиск SAGE Engine
#
#  Director 3D — приложение ПОВЕРХ движка, а не его форк: весь рендер, сцена/
#  ECS, скелетная анимация, ресурсы и звук берутся из sage::engine. Движок
#  подключается исходниками (add_subdirectory) — так инструмент собирается одной
#  командой вместе со свежим движком, без установки и без копий кода.
#
#  Порядок поиска:
#    1) -DSAGE_ENGINE_DIR=<путь>        — явно заданный чекаут;
#    2) ../SAGE-Engine, ../sage-engine  — соседний каталог (обычная раскладка);
#    3) FetchContent с GitHub           — если рядом ничего нет.
# ============================================================================
set(SAGE_ENGINE_DIR "" CACHE PATH "Путь к чекауту SAGE Engine (пусто — искать рядом/скачать)")
set(SAGE_ENGINE_REPO "https://github.com/AmckinatorStudios/SAGE-Engine.git"
    CACHE STRING "Репозиторий SAGE Engine для автозагрузки")
set(SAGE_ENGINE_TAG "main" CACHE STRING "Ветка/тег SAGE Engine для автозагрузки")

if (NOT SAGE_ENGINE_DIR)
    foreach(candidate
            "${CMAKE_CURRENT_SOURCE_DIR}/../SAGE-Engine"
            "${CMAKE_CURRENT_SOURCE_DIR}/../sage-engine"
            "${CMAKE_CURRENT_SOURCE_DIR}/../SAGEEngine")
        if (EXISTS "${candidate}/engine/CMakeLists.txt")
            get_filename_component(SAGE_ENGINE_DIR "${candidate}" ABSOLUTE)
            break()
        endif()
    endforeach()
endif()

# Движку говорим собирать ТОЛЬКО библиотеку: редактор сцен, плеер, игры-примеры
# и его юнит-тесты — не наши цели, их сборка только тратила бы время.
set(SAGE_BUILD_RUNTIME OFF CACHE BOOL "" FORCE)
set(SAGE_BUILD_EDITOR  OFF CACHE BOOL "" FORCE)
set(SAGE_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(SAGE_BUILD_TESTS   OFF CACHE BOOL "" FORCE)

if (SAGE_ENGINE_DIR AND EXISTS "${SAGE_ENGINE_DIR}/engine/CMakeLists.txt")
    message(STATUS "Director3D: SAGE Engine найден в ${SAGE_ENGINE_DIR}")
    # Бинарная директория задана явно: движок лежит ВНЕ дерева нашего проекта,
    # поэтому CMake не может вывести её сам.
    add_subdirectory("${SAGE_ENGINE_DIR}" "${CMAKE_BINARY_DIR}/sage-engine")
else()
    message(STATUS "Director3D: SAGE Engine рядом не найден — качаю ${SAGE_ENGINE_REPO} (${SAGE_ENGINE_TAG})")
    include(FetchContent)
    FetchContent_Declare(sage_engine_src
        GIT_REPOSITORY ${SAGE_ENGINE_REPO}
        GIT_TAG ${SAGE_ENGINE_TAG})
    FetchContent_MakeAvailable(sage_engine_src)
endif()

if (NOT TARGET sage::engine)
    message(FATAL_ERROR
        "Director3D: цель sage::engine не создана. Укажите чекаут движка: "
        "cmake -B build -DSAGE_ENGINE_DIR=/путь/к/SAGE-Engine")
endif()

# ============================================================================
#  Director3D — исполняемый файл инструмента
#
#  Раскладка исходников повторяет разделение ответственности:
#    anim/    — модель данных анимации (кривые, дорожки, документ) — без UI и GL;
#    project/ — undo/redo и файл проекта .d3dproj;
#    render/  — превью-рендер сцены и экспорт секвенции кадров;
#    ui/      — ImGui-оболочка: тема, иконки, панели.
#  Ядро anim/ намеренно не знает ни про ImGui, ни про OpenGL — оно проверяется
#  headless-самотестом (--self-test) и переиспользуемо вне редактора.
# ============================================================================
# WIN32 — приложение с ОКНОМ, а не консольное. Без этого рядом с программой
# вылезает чёрное окно терминала: на Windows это выглядит как ошибка, а закрыть
# его нельзя — оно и есть процесс. Весь вывод и без него пишется в
# director3d.log рядом с программой.
#
# На Linux флаг игнорируется, поэтому ветвления здесь не нужно.
add_executable(Director3D WIN32
    src/main.cpp
    src/DirectorLayer.cpp
    src/anim/Curve.cpp
    src/anim/Binding.cpp
    src/anim/BonePose.cpp
    src/anim/Constraints.cpp
    src/anim/AnimationDocument.cpp
    src/anim/Playback.cpp
    src/anim/AudioTrack.cpp
    src/project/UndoStack.cpp
    src/project/Project.cpp
    src/render/StageRenderer.cpp
    src/export/GltfExporter.cpp
    src/render/SequenceExporter.cpp
    src/render/VideoWriter.cpp
    src/ui/Theme.cpp
    src/ui/FileDialog.cpp
    src/ui/Localization.cpp
    src/ui/Icons.cpp
    src/ui/panels/MenuBarPanel.cpp
    src/ui/panels/ToolbarPanel.cpp
    src/ui/panels/ScenePanel.cpp
    src/ui/panels/AssetsPanel.cpp
    src/ui/panels/StagePanel.cpp
    src/ui/panels/TimelinePanel.cpp
    src/ui/panels/PropertiesPanel.cpp
    src/ui/panels/WorldPanel.cpp
    src/ui/panels/ProfilerPanel.cpp
    src/ui/panels/StatusBarPanel.cpp
    src/ui/panels/DialogsPanel.cpp
    src/SelfTest.cpp
)

target_include_directories(Director3D PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(Director3D PRIVATE
    sage::engine          # окно, рендер, сцена/ECS, скелетная анимация, звук
    imgui                 # весь интерфейс инструмента
    imguizmo              # манипулятор объекта во вьюпорте
    nlohmann_json::nlohmann_json # файлы проекта .d3dproj
)

# Ассеты инструмента (шрифт интерфейса) — рядом с бинарником, чтобы Director 3D
# запускался из каталога сборки без дополнительных шагов.
add_custom_command(TARGET Director3D POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:Director3D>/assets
    COMMENT "Копирование ассетов Director 3D рядом с бинарником")

# Самотест ядра анимации (кривые/дорожки/документ/undo/проект) — без окна и GL,
# поэтому гоняется в CI на обычном раннере.
enable_testing()
add_test(NAME director3d_self_test COMMAND Director3D --self-test)
