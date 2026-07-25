#include "ui/panels/DialogsPanel.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

#include "imgui.h"
#include "imgui_stdlib.h"

#include "project/Project.h"
#include "sage/core/Version.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace fs = std::filesystem;

namespace d3d {

namespace {

const char* TitleOf(Dialog dialog) {
    switch (dialog) {
        case Dialog::NewProject:       return "Новый проект";
        case Dialog::OpenProject:      return "Открыть проект";
        case Dialog::SaveProjectAs:    return "Сохранить проект как";
        case Dialog::ImportAsset:      return "Импорт";
        case Dialog::ExportScene:      return "Экспорт сцены (.sage)";
        case Dialog::RenderSettings:   return "Настройки рендера";
        case Dialog::TimelineSettings: return "Настройки таймлайна";
        case Dialog::About:            return "О программе Director 3D";
        case Dialog::Shortcuts:        return "Горячие клавиши";
        default:                       return "";
    }
}

// Поле ввода пути с подсказкой о состоянии файла: зелёная — файл есть, серая —
// будет создан, красная — путь заведомо нерабочий. Пользователь видит проблему
// ДО нажатия кнопки, а не в виде ошибки после.
void PathField(const char* label, char* buffer, size_t size, bool mustExist) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##path", buffer, size);

    const fs::path path(buffer);
    std::error_code ec;
    if (buffer[0] == '\0') {
        ImGui::TextDisabled("Введите путь к файлу");
        return;
    }
    if (fs::exists(path, ec)) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Good);
        ImGui::Text("Файл найден");
        ImGui::PopStyleColor();
    } else if (mustExist) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Record);
        ImGui::Text("Файла нет по этому пути");
        ImGui::PopStyleColor();
    } else if (path.has_parent_path() && !fs::is_directory(path.parent_path(), ec)) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Record);
        ImGui::Text("Каталога %s не существует", path.parent_path().string().c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("Будет создан новый файл");
    }
}

bool PathExists(const char* buffer) {
    std::error_code ec;
    return buffer[0] != '\0' && fs::exists(fs::path(buffer), ec);
}

} // namespace

void DialogsPanel::Open(Dialog dialog) {
    m_requested = dialog;
    m_error.clear();
}

void DialogsPanel::Close() {
    m_active = Dialog::None;
    ImGui::CloseCurrentPopup();
}

bool DialogsPanel::BeginModal(const char* title) {
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
    return ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
}

void DialogsPanel::Draw(DirectorHost& host) {
    // OpenPopup можно звать только внутри кадра, а запрос приходит из меню и
    // кнопок в любой момент — поэтому он копится в m_requested.
    if (m_requested != Dialog::None) {
        m_active = m_requested;
        m_requested = Dialog::None;
        ImGui::OpenPopup(TitleOf(m_active));

        // Предзаполняем поля разумными значениями: пустая строка в диалоге
        // сохранения — это лишняя работа пользователю на ровном месте.
        if (m_active == Dialog::SaveProjectAs && m_path[0] == '\0') {
            std::snprintf(m_path, sizeof(m_path), "%s%s",
                          host.Document().Name.c_str(), ProjectFile::kExtension);
        }
        if (m_active == Dialog::NewProject) {
            std::snprintf(m_projectName, sizeof(m_projectName), "My_Animation_Project");
        }
    }

    switch (m_active) {
        case Dialog::NewProject:       DrawNewProject(host); break;
        case Dialog::OpenProject:      DrawOpenProject(host); break;
        case Dialog::SaveProjectAs:    DrawSaveProjectAs(host); break;
        case Dialog::ImportAsset:      DrawImportAsset(host); break;
        case Dialog::ExportScene:      DrawExportScene(host); break;
        case Dialog::RenderSettings:   DrawRenderSettings(host); break;
        case Dialog::TimelineSettings: DrawTimelineSettings(host); break;
        case Dialog::About:            DrawAbout(host); break;
        case Dialog::Shortcuts:        DrawShortcuts(host); break;
        default: break;
    }
}

void DialogsPanel::DrawNewProject(DirectorHost& host) {
    if (!BeginModal(TitleOf(Dialog::NewProject))) return;

    if (host.Dirty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Warning);
        ImGui::TextWrapped("В текущем проекте есть несохранённые изменения — они будут потеряны.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    ImGui::TextUnformatted("Название проекта");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##name", m_projectName, sizeof(m_projectName));
    ImGui::Spacing();
    ImGui::TextDisabled("Новая сцена содержит камеру, свет и пол — можно сразу анимировать.");
    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::Button("Создать", ImVec2(120.0f, 0.0f))) {
        host.NewProject();
        host.Document().Name = m_projectName;
        host.SetStatus("Новый проект создан");
        Close();
    }
    ImGui::SameLine();
    if (ImGui::Button("Отмена", ImVec2(120.0f, 0.0f))) Close();
    ImGui::EndPopup();
}

void DialogsPanel::DrawOpenProject(DirectorHost& host) {
    if (!BeginModal(TitleOf(Dialog::OpenProject))) return;

    PathField("Путь к файлу проекта (.d3dproj)", m_path, sizeof(m_path), /*mustExist=*/true);
    if (!m_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Record);
        ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::PopStyleColor();
    }

    // Список проектов в текущем каталоге ассетов — чаще всего нужный файл там.
    ImGui::Spacing();
    ImGui::TextDisabled("Проекты в %s:", host.AssetsDir().string().c_str());
    ImGui::BeginChild("##list", ImVec2(0.0f, 110.0f), true);
    std::error_code ec;
    bool anyFound = false;
    if (fs::is_directory(host.AssetsDir(), ec)) {
        for (const fs::directory_entry& entry : fs::directory_iterator(host.AssetsDir(), ec)) {
            if (entry.is_directory(ec) || entry.path().extension() != ProjectFile::kExtension) continue;
            anyFound = true;
            if (ImGui::Selectable(entry.path().filename().string().c_str())) {
                std::snprintf(m_path, sizeof(m_path), "%s", entry.path().string().c_str());
            }
        }
    }
    if (!anyFound) ImGui::TextDisabled("Здесь нет файлов .d3dproj");
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::BeginDisabled(!PathExists(m_path));
    if (ImGui::Button("Открыть", ImVec2(120.0f, 0.0f))) {
        std::string err;
        if (host.OpenProject(m_path, err)) {
            host.SetStatus("Проект открыт");
            Close();
        } else {
            m_error = "Не удалось открыть: " + err;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Отмена", ImVec2(120.0f, 0.0f))) Close();
    ImGui::EndPopup();
}

void DialogsPanel::DrawSaveProjectAs(DirectorHost& host) {
    if (!BeginModal(TitleOf(Dialog::SaveProjectAs))) return;

    PathField("Куда сохранить (.d3dproj)", m_path, sizeof(m_path), /*mustExist=*/false);
    if (!m_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Record);
        ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::PopStyleColor();
    }
    if (PathExists(m_path)) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Warning);
        ImGui::TextWrapped("Файл существует и будет перезаписан.");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    ImGui::BeginDisabled(m_path[0] == '\0');
    if (ImGui::Button("Сохранить", ImVec2(120.0f, 0.0f))) {
        // Расширение дописываем сами: пользователь набирает имя, а не формат.
        fs::path path(m_path);
        if (path.extension() != ProjectFile::kExtension) path += ProjectFile::kExtension;
        std::string err;
        if (host.SaveProject(path, err)) {
            host.SetStatus("Проект сохранён");
            Close();
        } else {
            m_error = "Не удалось сохранить: " + err;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Отмена", ImVec2(120.0f, 0.0f))) Close();
    ImGui::EndPopup();
}

void DialogsPanel::DrawImportAsset(DirectorHost& host) {
    if (!BeginModal(TitleOf(Dialog::ImportAsset))) return;

    ImGui::TextWrapped("Модели .glb / .gltf импортируются как персонажи со скелетной анимацией, "
                       ".obj — как статическая геометрия. Звуковые файлы становятся звуковой "
                       "дорожкой ролика.");
    ImGui::Spacing();
    PathField("Путь к файлу", m_path, sizeof(m_path), /*mustExist=*/true);
    if (!m_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Record);
        ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    ImGui::BeginDisabled(!PathExists(m_path));
    if (ImGui::Button("Импортировать", ImVec2(140.0f, 0.0f))) {
        const fs::path path(m_path);
        const std::string ext = path.extension().string();
        if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac") {
            host.PushUndo();
            if (host.Document().Audio.Load(path.string())) {
                host.SetStatus("Звуковая дорожка загружена");
                Close();
            } else {
                m_error = "Не удалось прочитать звуковой файл";
            }
        } else if (host.ImportAsset(path) >= 0) {
            host.SetStatus("Импортировано: " + path.filename().string());
            Close();
        } else {
            m_error = "Не удалось импортировать — формат не поддерживается или файл повреждён";
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Отмена", ImVec2(120.0f, 0.0f))) Close();
    ImGui::EndPopup();
}

void DialogsPanel::DrawExportScene(DirectorHost& host) {
    if (!BeginModal(TitleOf(Dialog::ExportScene))) return;

    ImGui::TextWrapped("Сохраняет ТОЛЬКО сцену в формате движка (.sage) — без анимации. "
                       "Такой файл открывается редактором SAGE и грузится в игру.");
    ImGui::Spacing();
    PathField("Куда сохранить (.sage)", m_path, sizeof(m_path), /*mustExist=*/false);
    if (!m_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Record);
        ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    ImGui::BeginDisabled(m_path[0] == '\0');
    if (ImGui::Button("Экспортировать", ImVec2(140.0f, 0.0f))) {
        fs::path path(m_path);
        if (path.extension() != ".sage") path += ".sage";
        std::string err;
        if (host.ExportSceneToEngine(path, err)) {
            host.SetStatus("Сцена экспортирована");
            Close();
        } else {
            m_error = "Не удалось экспортировать: " + err;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Отмена", ImVec2(120.0f, 0.0f))) Close();
    ImGui::EndPopup();
}

void DialogsPanel::DrawRenderSettings(DirectorHost& host) {
    if (!BeginModal(TitleOf(Dialog::RenderSettings))) return;

    SequenceExporter::Settings& settings = host.RenderSettings();
    const AnimationDocument& doc = host.Document();

    ImGui::TextUnformatted("Разрешение кадра");
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragInt("##w", &settings.Width, 1.0f, 16, 7680, "%d px");
    ImGui::SameLine();
    ImGui::TextUnformatted("x");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragInt("##h", &settings.Height, 1.0f, 16, 4320, "%d px");

    ImGui::SameLine(0.0f, 12.0f);
    if (ImGui::SmallButton("1080p")) { settings.Width = 1920; settings.Height = 1080; }
    ImGui::SameLine();
    if (ImGui::SmallButton("720p")) { settings.Width = 1280; settings.Height = 720; }
    ImGui::SameLine();
    if (ImGui::SmallButton("4K")) { settings.Width = 3840; settings.Height = 2160; }

    ImGui::Spacing();
    ImGui::TextUnformatted("Диапазон");
    ImGui::SetNextItemWidth(150.0f);
    ImGui::DragFloat("Начало", &settings.StartTime, 0.05f, 0.0f, doc.Duration, "%.2f c");
    ImGui::SetNextItemWidth(150.0f);
    ImGui::DragFloat("Конец", &settings.EndTime, 0.05f, 0.0f, doc.Duration, "%.2f c");
    ImGui::TextDisabled("Конец = 0 означает «до конца ролика» (%.2f c)", (double)doc.Duration);

    ImGui::Spacing();
    std::snprintf(m_renderDir, sizeof(m_renderDir), "%s", settings.OutputDir.c_str());
    ImGui::TextUnformatted("Каталог вывода");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##outdir", m_renderDir, sizeof(m_renderDir))) settings.OutputDir = m_renderDir;

    std::snprintf(m_renderName, sizeof(m_renderName), "%s", settings.BaseName.c_str());
    ImGui::TextUnformatted("Имя файлов");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##outname", m_renderName, sizeof(m_renderName))) settings.BaseName = m_renderName;

    ImGui::Spacing();
    ImGui::SetNextItemWidth(150.0f);
    ImGui::DragInt("Кадров за шаг", &settings.FramesPerStep, 0.2f, 1, 30);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Больше — быстрее экспорт, но интерфейс отзывается реже.");
    }

    ImGui::Spacing();
    const float end = settings.EndTime > settings.StartTime ? settings.EndTime : doc.Duration;
    const int frames = (int)((end - settings.StartTime) * doc.Fps) + 1;
    ImGui::TextDisabled("Будет записано примерно %d кадр(ов) в %s_00000.png …",
                        frames > 0 ? frames : 0, settings.BaseName.c_str());

    ImGui::Separator();
    if (ImGui::Button("Рендерить", ImVec2(140.0f, 0.0f))) {
        host.StartRender();
        Close();
    }
    ImGui::SameLine();
    if (ImGui::Button("Закрыть", ImVec2(120.0f, 0.0f))) Close();
    ImGui::EndPopup();
}

void DialogsPanel::DrawTimelineSettings(DirectorHost& host) {
    if (!BeginModal(TitleOf(Dialog::TimelineSettings))) return;

    AnimationDocument& doc = host.Document();

    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::DragFloat("Длительность", &doc.Duration, 0.1f, 0.1f, 36000.0f, "%.2f c")) host.PushUndo();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::DragFloat("Кадров в секунду", &doc.Fps, 0.5f, 1.0f, 240.0f, "%.0f")) host.PushUndo();
    ImGui::TextDisabled("Смена частоты не двигает ключи: время хранится в секундах.");

    ImGui::Spacing();
    ImGui::Checkbox("Зациклить проигрывание", &host.Transport().Loop);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat("Скорость", &host.Transport().Speed, 0.01f, -4.0f, 4.0f, "%.2fx");
    ImGui::TextDisabled("Отрицательная скорость проигрывает ролик назад.");

    ImGui::Spacing();
    ImGui::SeparatorText("Метки");
    if (doc.Markers.empty()) {
        ImGui::TextDisabled("Меток нет. Animation > Add Marker ставит метку на текущем кадре.");
    } else {
        for (size_t i = 0; i < doc.Markers.size(); ++i) {
            ImGui::PushID((int)i);
            ImGui::SetNextItemWidth(200.0f);
            // Через imgui_stdlib: std::string растёт сам, без ручного буфера и
            // без риска записать мимо capacity.
            ImGui::InputText("##mname", &doc.Markers[i].Name);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::DragFloat("##mtime", &doc.Markers[i].Time, 0.01f, 0.0f, doc.Duration, "%.2f c");
            ImGui::SameLine();
            if (ImGui::SmallButton("Удалить")) {
                host.PushUndo();
                doc.Markers.erase(doc.Markers.begin() + (long)i);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Закрыть", ImVec2(120.0f, 0.0f))) Close();
    ImGui::EndPopup();
}

void DialogsPanel::DrawAbout(DirectorHost& host) {
    (void)host;
    if (!BeginModal(TitleOf(Dialog::About))) return;

    if (ImFont* big = Theme::MonoFont()) ImGui::PushFont(big);
    ImGui::TextUnformatted("Director 3D");
    if (Theme::MonoFont()) ImGui::PopFont();

    ImGui::TextDisabled("Версия 0.1.0 — инструмент 3D-анимации");
    ImGui::Spacing();
    ImGui::TextWrapped("Собран на движке SAGE Engine %s. Сцена, рендер, скелетная анимация, "
                       "ресурсы и звук — движковые; Director 3D добавляет поверх них таймлайн, "
                       "кривые, транспорт и экспорт секвенции.", kSageEngineVersion);
    ImGui::Spacing();
    ImGui::SeparatorText("Форматы");
    ImGui::BulletText(".d3dproj — проект (сцена + анимация)");
    ImGui::BulletText(".sage — сцена движка (экспорт и импорт)");
    ImGui::BulletText(".glb / .gltf / .obj — модели");
    ImGui::BulletText("PNG-секвенция — результат рендера");
    ImGui::Spacing();
    ImGui::TextDisabled("Интерфейс: Dear ImGui. Манипулятор: ImGuizmo.");

    ImGui::Separator();
    if (ImGui::Button("Закрыть", ImVec2(120.0f, 0.0f))) Close();
    ImGui::EndPopup();
}

void DialogsPanel::DrawShortcuts(DirectorHost& host) {
    (void)host;
    if (!BeginModal(TitleOf(Dialog::Shortcuts))) return;

    // Таблица вместо текста: колонки не разъезжаются при любом шрифте.
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        auto row = [](const char* key, const char* what) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Accent);
            ImGui::TextUnformatted(key);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(what);
        };

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::SeparatorText("Файл");
        ImGui::TableNextColumn();
        row("Ctrl+N / Ctrl+O / Ctrl+S", "Новый / открыть / сохранить");
        row("Ctrl+Z / Ctrl+Y", "Отменить / повторить");
        row("Ctrl+D / Del", "Дублировать / удалить объект");

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::SeparatorText("Вьюпорт");
        ImGui::TableNextColumn();
        row("ПКМ + WASD / QE", "Осмотр и полёт камерой");
        row("СКМ", "Панорама");
        row("Колесо", "Приблизить / отдалить");
        row("Q / W / E / R", "Выбор / перемещение / поворот / масштаб");
        row("F", "Навести камеру на выбранное");
        row("G", "Показать или скрыть сетку");

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::SeparatorText("Анимация");
        ImGui::TableNextColumn();
        row("Пробел", "Проигрывание / пауза");
        row("Shift+Пробел", "Стоп и в начало");
        row("← / →", "Кадр назад / вперёд");
        row("Home / End", "В начало / в конец ролика");
        row(", / .", "Предыдущий / следующий ключ");
        row("K", "Поставить ключ выбранному");
        row("Ctrl+K", "Включить авто-ключ");
        row("L", "Зациклить проигрывание");
        row("M", "Поставить метку");
        row("F12", "Отрендерить секвенцию");

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::SeparatorText("Таймлайн");
        ImGui::TableNextColumn();
        row("Колесо", "Зум вокруг курсора");
        row("Shift+Колесо / СКМ", "Прокрутка по времени");
        row("Ctrl+клик", "Добавить ключ к выделению");
        row("Alt при перетаскивании", "Без прилипания к кадрам");
        row("Del", "Удалить выбранные ключи");

        ImGui::EndTable();
    }

    ImGui::Separator();
    if (ImGui::Button("Закрыть", ImVec2(120.0f, 0.0f))) Close();
    ImGui::EndPopup();
}

} // namespace d3d
