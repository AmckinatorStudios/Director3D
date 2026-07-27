#include "ui/panels/DialogsPanel.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_stdlib.h"

#include "project/Project.h"
#include "render/VideoWriter.h"
#include "sage/core/Version.h"
#include "ui/Icons.h"
#include "ui/FileDialog.h"
#include "ui/Localization.h"
#include "ui/Theme.h"

namespace fs = std::filesystem;

namespace d3d {

namespace {

const char* TitleOf(Dialog dialog) {
    switch (dialog) {
        case Dialog::NewProject:       return T("Новый проект");
        case Dialog::OpenProject:      return T("Открыть проект");
        case Dialog::SaveProjectAs:    return T("Сохранить проект как");
        case Dialog::ImportAsset:      return T("Импорт");
        case Dialog::ExportScene:      return T("Экспорт сцены (.sage)");
        case Dialog::ExportGltf:       return T("Экспорт анимации (.glb)");
        case Dialog::RenderSettings:   return T("Настройки рендера");
        case Dialog::TimelineSettings: return T("Настройки таймлайна");
        case Dialog::About:            return T("О программе Director 3D");
        case Dialog::Shortcuts:        return T("Горячие клавиши");
        default:                       return "";
    }
}

// Описание системного диалога для кнопки «Обзор». Пустой Title — кнопки нет.
struct BrowseSpec {
    const char* Title = nullptr;
    std::vector<filedialog::Filter> Filters;
    bool Save = false;
    std::string SuggestedName;
};

// Поле ввода пути с подсказкой о состоянии файла: зелёная — файл есть, серая —
// будет создан, красная — путь заведомо нерабочий. Пользователь видит проблему
// ДО нажатия кнопки, а не в виде ошибки после. Ввод строкой остаётся всегда:
// системный диалог — это удобство поверх него, а не замена ему.
void PathField(const char* label, char* buffer, size_t size, bool mustExist,
               const BrowseSpec& browse = {}) {
    ImGui::TextUnformatted(label);

    // Кнопка «Обзор» есть только если системному диалогу есть чем открыться.
    // Показывать неработающую кнопку хуже, чем не показывать никакой: ввод
    // строкой остаётся рабочим путём, а нажатие в пустоту выглядит поломкой.
    const bool browsable = browse.Title && filedialog::Available();
    const float browseWidth = browsable ? 90.0f : 0.0f;
    ImGui::SetNextItemWidth(browsable ? -(browseWidth + ImGui::GetStyle().ItemSpacing.x) : -1.0f);
    ImGui::InputText("##path", buffer, size);

    if (browsable) {
        ImGui::SameLine();
        if (ImGui::Button(T("Обзор…"), ImVec2(browseWidth, 0.0f))) {
            // Стартовый каталог — тот, что уже введён: диалог должен
            // открываться там, где человек работает, а не в домашней папке.
            std::error_code ec;
            fs::path start(buffer);
            if (!start.empty() && !fs::is_directory(start, ec)) start = start.parent_path();
            std::string picked;
            const bool got = browse.Save
                ? filedialog::SaveFile(browse.Title, browse.Filters, start.string(),
                                       browse.SuggestedName, picked)
                : filedialog::OpenFile(browse.Title, browse.Filters, start.string(), picked);
            if (got) {
                std::snprintf(buffer, size, "%s", picked.c_str());
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(T("Открыть системный диалог (%s)"), filedialog::Backend().c_str());
        }
    }

    const fs::path path(buffer);
    std::error_code ec;
    if (buffer[0] == '\0') {
        ImGui::TextDisabled("%s", T("Введите путь к файлу"));
        return;
    }
    if (fs::exists(path, ec)) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Good);
        ImGui::Text("%s", T("Файл найден"));
        ImGui::PopStyleColor();
    } else if (mustExist) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Record);
        ImGui::Text("%s", T("Файла нет по этому пути"));
        ImGui::PopStyleColor();
    } else if (path.has_parent_path() && !fs::is_directory(path.parent_path(), ec)) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Record);
        ImGui::Text(T("Каталога %s не существует"), path.parent_path().string().c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("%s", T("Будет создан новый файл"));
    }
}

bool PathExists(const char* buffer) {
    std::error_code ec;
    return buffer[0] != '\0' && fs::exists(fs::path(buffer), ec);
}

// Длительность по-человечески: «45 с», «3 мин 20 с», «1 ч 05 мин». Секунды
// числом («осталось 212 секунд») читатель всё равно переводит в минуты в уме —
// пусть это делает программа.
std::string HumanDuration(float seconds) {
    if (seconds < 0.0f) return "—";
    const int total = (int)(seconds + 0.5f);
    char out[64];
    if (total < 60) {
        std::snprintf(out, sizeof(out), T("%d с"), total);
    } else if (total < 3600) {
        std::snprintf(out, sizeof(out), T("%d мин %02d с"), total / 60, total % 60);
    } else {
        std::snprintf(out, sizeof(out), T("%d ч %02d мин"), total / 3600, (total % 3600) / 60);
    }
    return out;
}

// Заголовок окна с постоянным идентификатором: видимая часть переводится, а
// «###id» держит ImGui-состояние окна на месте при смене языка.
std::string TitleWithId(const char* text, const char* id) {
    return std::string(T(text)) + "###" + id;
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
        case Dialog::ExportGltf:       DrawExportGltf(host); break;
        case Dialog::RenderSettings:   DrawRenderSettings(host); break;
        case Dialog::TimelineSettings: DrawTimelineSettings(host); break;
        case Dialog::About:            DrawAbout(host); break;
        case Dialog::Shortcuts:        DrawShortcuts(host); break;
        default: break;
    }

    // Эти два окна открывает ход дела, а не человек, поэтому они вне switch и
    // рисуются всегда. Порядок важен: прогресс должен успеть закрыться прежде,
    // чем откроется итог.
    DrawRenderProgress(host);
    DrawRenderOutcome(host);
}

// ============================================================================
//  Ход рендера
// ============================================================================

void DialogsPanel::DrawRenderProgress(DirectorHost& host) {
    SequenceExporter& exporter = host.Exporter();
    const std::string title = TitleWithId("Идёт рендер", "d3d_render_progress");

    if (exporter.Active() && !m_progressOpen) {
        m_progressOpen = true;
        ImGui::OpenPopup(title.c_str());
    }
    if (!m_progressOpen) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(470.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoSavedSettings)) {
        m_progressOpen = false; // окно закрылось само — состояние надо вернуть
        return;
    }

    // Рендер кончился, пока окно было открыто: закрываем и уступаем место итогу.
    if (!exporter.Active()) {
        m_progressOpen = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    const int current = exporter.CurrentFrame();
    const int total = exporter.TotalFrames();

    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "%.0f%%", (double)(exporter.Progress() * 100.0f));
    ImGui::ProgressBar(exporter.Progress(), ImVec2(-1.0f, 22.0f), overlay);

    ImGui::Spacing();
    ImGui::Text(T("Кадр %d из %d"), current, total);

    // Прошедшее время — всегда, оставшееся — как только его есть из чего
    // посчитать. Пока оценки нет, честно пишем «считаю»: пустое место на этой
    // строке человек читает как «программа не отвечает».
    const float remaining = exporter.RemainingSeconds();
    ImGui::TextDisabled(T("Прошло: %s     Осталось: %s"), HumanDuration(exporter.ElapsedSeconds()).c_str(),
                        remaining < 0.0f ? T("считаю…") : HumanDuration(remaining).c_str());

    ImGui::Spacing();
    ImGui::TextDisabled("%s", T("Пишется в:"));
    ImGui::TextWrapped("%s", exporter.ResultPath().c_str());

    ImGui::Spacing();
    ImGui::TextDisabled("%s", T("Пока идёт рендер, сцена стоит на снимаемом кадре — это нормально."));

    ImGui::Separator();
    if (ImGui::Button(T("Отменить рендер"), ImVec2(170.0f, 0.0f))) {
        host.CancelRender();
        m_progressOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ============================================================================
//  Итог рендера
// ============================================================================

void DialogsPanel::DrawRenderOutcome(DirectorHost& host) {
    RenderOutcome& outcome = host.LastRender();
    const std::string title = TitleWithId("Рендер", "d3d_render_outcome");

    // Ждём, пока закроется всё остальное: открывать модалку поверх модалки
    // прогресса, которая закрывается в этом же кадре, — верный способ получить
    // окно, которое нельзя закрыть.
    const bool anyPopup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                                          ImGuiPopupFlags_AnyPopupLevel);
    if (outcome.Shown && !m_outcomeOpen && !anyPopup) {
        m_outcomeOpen = true;
        ImGui::OpenPopup(title.c_str());
    }
    if (!m_outcomeOpen) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(470.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoSavedSettings)) {
        m_outcomeOpen = false;
        outcome.Shown = false;
        return;
    }

    if (outcome.Ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Good);
        ImGui::TextUnformatted(T("Готово."));
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Text(T("Кадров: %d     Времени: %s"), outcome.Frames,
                    HumanDuration(outcome.Seconds).c_str());
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Record);
        ImGui::TextUnformatted(T("Рендер не завершён."));
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::TextWrapped("%s", outcome.Message.c_str());
    }

    if (!outcome.Path.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", T("Результат:"));
        ImGui::TextWrapped("%s", outcome.Path.c_str());
    }

    ImGui::Separator();

    // «Открыть папку» есть только если результат существует: предлагать открыть
    // то, чего нет, — обещание, которое кнопка не выполнит.
    std::error_code ec;
    const bool exists = !outcome.Path.empty() && fs::exists(fs::path(outcome.Path), ec);
    ImGui::BeginDisabled(!exists);
    if (ImGui::Button(T("Открыть папку"), ImVec2(150.0f, 0.0f))) {
        if (!filedialog::RevealInFileManager(outcome.Path)) {
            host.SetStatus(T("Не удалось открыть проводник — путь показан выше"));
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Закрыть"), ImVec2(120.0f, 0.0f))) {
        m_outcomeOpen = false;
        outcome.Shown = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DialogsPanel::DrawNewProject(DirectorHost& host) {
    if (!BeginModal(TitleOf(Dialog::NewProject))) return;

    if (host.Dirty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Warning);
        ImGui::TextWrapped("%s", T("В текущем проекте есть несохранённые изменения — они будут потеряны."));
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    ImGui::TextUnformatted(T("Название проекта"));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##name", m_projectName, sizeof(m_projectName));
    ImGui::Spacing();
    ImGui::TextDisabled("%s", T("Новая сцена содержит камеру, свет и пол — можно сразу анимировать."));
    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::Button(T("Создать"), ImVec2(120.0f, 0.0f))) {
        host.NewProject();
        host.Document().Name = m_projectName;
        host.SetStatus(T("Новый проект создан"));
        Close();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Отмена"), ImVec2(120.0f, 0.0f))) Close();
    ImGui::EndPopup();
}

void DialogsPanel::DrawOpenProject(DirectorHost& host) {
    if (!BeginModal(TitleOf(Dialog::OpenProject))) return;

    PathField(T("Путь к файлу проекта (.d3dproj)"), m_path, sizeof(m_path), /*mustExist=*/true,
              BrowseSpec{T("Открыть проект Director 3D"),
                         {{T("Проекты Director 3D"), "*.d3dproj"}}, false, ""});
    if (!m_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Record);
        ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::PopStyleColor();
    }

    // Список проектов в текущем каталоге ассетов — чаще всего нужный файл там.
    ImGui::Spacing();
    ImGui::TextDisabled(T("Проекты в %s:"), host.AssetsDir().string().c_str());
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
    if (!anyFound) ImGui::TextDisabled("%s", T("Здесь нет файлов .d3dproj"));
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::BeginDisabled(!PathExists(m_path));
    if (ImGui::Button(T("Открыть"), ImVec2(120.0f, 0.0f))) {
        std::string err;
        if (host.OpenProject(m_path, err)) {
            host.SetStatus(T("Проект открыт"));
            Close();
        } else {
            m_error = T("Не удалось открыть: ") + err;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Отмена"), ImVec2(120.0f, 0.0f))) Close();
    ImGui::EndPopup();
}

void DialogsPanel::DrawSaveProjectAs(DirectorHost& host) {
    if (!BeginModal(TitleOf(Dialog::SaveProjectAs))) return;

    PathField(T("Куда сохранить (.d3dproj)"), m_path, sizeof(m_path), /*mustExist=*/false,
              BrowseSpec{T("Сохранить проект как"),
                         {{T("Проекты Director 3D"), "*.d3dproj"}}, true, "project.d3dproj"});
    if (!m_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Record);
        ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::PopStyleColor();
    }
    if (PathExists(m_path)) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Warning);
        ImGui::TextWrapped("%s", T("Файл существует и будет перезаписан."));
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    ImGui::BeginDisabled(m_path[0] == '\0');
    if (ImGui::Button(T("Сохранить"), ImVec2(120.0f, 0.0f))) {
        // Расширение дописываем сами: пользователь набирает имя, а не формат.
        fs::path path(m_path);
        if (path.extension() != ProjectFile::kExtension) path += ProjectFile::kExtension;
        std::string err;
        if (host.SaveProject(path, err)) {
            host.SetStatus(T("Проект сохранён"));
            Close();
        } else {
            m_error = T("Не удалось сохранить: ") + err;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Отмена"), ImVec2(120.0f, 0.0f))) Close();
    ImGui::EndPopup();
}

void DialogsPanel::DrawImportAsset(DirectorHost& host) {
    if (!BeginModal(TitleOf(Dialog::ImportAsset))) return;

    ImGui::TextWrapped("%s", T("Модели .glb / .gltf импортируются как персонажи со скелетной анимацией, "
                       ".obj — как статическая геометрия. Звуковые файлы становятся звуковой "
                       "дорожкой ролика."));
    ImGui::Spacing();
    PathField(T("Путь к файлу"), m_path, sizeof(m_path), /*mustExist=*/true,
              BrowseSpec{T("Импорт ассета"),
                         {{T("Модели и звук"), "*.glb *.gltf *.obj *.wav *.mp3 *.flac"},
                          {T("Все файлы"), "*"}}, false, ""});
    if (!m_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Record);
        ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    ImGui::BeginDisabled(!PathExists(m_path));
    if (ImGui::Button(T("Импортировать"), ImVec2(140.0f, 0.0f))) {
        const fs::path path(m_path);
        const std::string ext = path.extension().string();
        if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac") {
            host.PushUndo();
            if (host.Document().Audio.Load(path.string())) {
                host.SetStatus(T("Звуковая дорожка загружена"));
                Close();
            } else {
                m_error = T("Не удалось прочитать звуковой файл");
            }
        } else if (host.ImportAsset(path) >= 0) {
            host.SetStatus(T("Импортировано: ") + path.filename().string());
            Close();
        } else {
            m_error = T("Не удалось импортировать — формат не поддерживается или файл повреждён");
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Отмена"), ImVec2(120.0f, 0.0f))) Close();
    ImGui::EndPopup();
}

void DialogsPanel::DrawExportScene(DirectorHost& host) {
    if (!BeginModal(TitleOf(Dialog::ExportScene))) return;

    ImGui::TextWrapped("%s", T("Сохраняет ТОЛЬКО сцену в формате движка (.sage) — без анимации. "
                       "Такой файл открывается редактором SAGE и грузится в игру."));
    ImGui::Spacing();
    PathField(T("Куда сохранить (.sage)"), m_path, sizeof(m_path), /*mustExist=*/false,
              BrowseSpec{T("Экспорт сцены в формат движка"),
                         {{T("Сцены SAGE"), "*.sage"}}, true, "scene.sage"});
    if (!m_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Record);
        ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    ImGui::BeginDisabled(m_path[0] == '\0');
    if (ImGui::Button(T("Экспортировать"), ImVec2(140.0f, 0.0f))) {
        fs::path path(m_path);
        if (path.extension() != ".sage") path += ".sage";
        std::string err;
        if (host.ExportSceneToEngine(path, err)) {
            host.SetStatus(T("Сцена экспортирована"));
            Close();
        } else {
            m_error = T("Не удалось экспортировать: ") + err;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Отмена"), ImVec2(120.0f, 0.0f))) Close();
    ImGui::EndPopup();
}

void DialogsPanel::DrawExportGltf(DirectorHost& host) {
    if (!BeginModal(TitleOf(Dialog::ExportGltf))) return;

    ImGui::TextWrapped("%s", T("Движение объектов, камер и костей в формате glTF 2.0 — его читают "
                       "Blender, игровые движки и браузер."));
    ImGui::Spacing();
    // Границы формата названы прямо в диалоге, а не в документации: узнать о
    // них после экспорта, из чужой программы, куда обиднее.
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::TextDim);
    ImGui::TextWrapped("%s", T("Геометрия НЕ выгружается: возьмите свою модель и наложите на неё "
                       "это движение. Угол обзора, свет и пост-обработка в glTF не "
                       "анимируются — эти дорожки остаются в проекте."));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    PathField(T("Куда сохранить (.glb)"), m_path, sizeof(m_path), /*mustExist=*/false,
              BrowseSpec{T("Экспорт анимации в glTF"),
                         {{T("Анимация glTF"), "*.glb"}}, true, "animation.glb"});
    if (!m_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Record);
        ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    ImGui::BeginDisabled(m_path[0] == '\0');
    if (ImGui::Button(T("Экспортировать"), ImVec2(140.0f, 0.0f))) {
        fs::path path(m_path);
        if (path.extension() != ".glb") path += ".glb";
        std::string err;
        if (host.ExportAnimationToGltf(path, err)) {
            host.SetStatus(T("Анимация экспортирована"));
            Close();
        } else {
            m_error = T("Не удалось экспортировать: ") + err;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Отмена"), ImVec2(120.0f, 0.0f))) Close();
    ImGui::EndPopup();
}

void DialogsPanel::DrawRenderSettings(DirectorHost& host) {
    if (!BeginModal(TitleOf(Dialog::RenderSettings))) return;

    SequenceExporter::Settings& settings = host.RenderSettings();
    const AnimationDocument& doc = host.Document();

    // --- Формат вывода ---
    // Он первый, потому что от него зависит смысл остальных полей: у MP4 «имя
    // файлов» — это имя одного ролика, у секвенции — префикс сотен PNG.
    const bool ffmpeg = VideoWriter::FfmpegAvailable();
    ImGui::TextUnformatted(T("Формат вывода"));
    bool mp4 = settings.OutputFormat == SequenceExporter::Format::Mp4;
    if (ImGui::RadioButton(T("Видео MP4 (H.264)"), mp4)) {
        settings.OutputFormat = SequenceExporter::Format::Mp4;
        mp4 = true;
    }
    ImGui::SameLine(0.0f, 18.0f);
    if (ImGui::RadioButton(T("Секвенция PNG"), !mp4)) {
        settings.OutputFormat = SequenceExporter::Format::PngSequence;
        mp4 = false;
    }

    if (mp4 && !ffmpeg) {
        // Не запрещаем выбор — просто честно говорим, почему рендер не пойдёт и
        // что с этим делать. Иначе пользователь упрётся в ошибку уже после
        // нажатия «Рендерить».
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Record);
        ImGui::TextWrapped("%s", T("ffmpeg не найден — MP4 записать нечем."));
        ImGui::PopStyleColor();
        ImGui::TextDisabled("Ubuntu/Debian: sudo apt install ffmpeg · macOS: brew install ffmpeg · "
                            "Windows: winget install ffmpeg");
        ImGui::TextDisabled("%s", T("Либо выберите секвенцию PNG — она работает без ffmpeg."));
    } else if (mp4) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Good);
        ImGui::TextWrapped("%s", VideoWriter::FfmpegVersion().c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("%s", T("Кадр = отдельный файл PNG без потерь: принимается любым монтажом."));
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(T("Разрешение кадра"));
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

    // H.264 в yuv420p не кодирует нечётные стороны. Предупреждаем и чиним одной
    // кнопкой, а не роняем рендер посреди ролика.
    const bool oddSize = (settings.Width % 2 != 0) || (settings.Height % 2 != 0);
    if (mp4 && oddSize) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Warning);
        ImGui::TextWrapped("%s", T("H.264 требует чётных сторон кадра."));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Исправить"))) {
            settings.Width &= ~1;
            settings.Height &= ~1;
        }
    }

    if (mp4) {
        ImGui::Spacing();
        ImGui::SetNextItemWidth(220.0f);
        // CRF: меньше — лучше картинка и больше файл. Диапазон сознательно
        // сужен до вменяемого: ниже 14 растёт только размер, выше 28 — заметные
        // артефакты на градиентах неба и в размытии.
        ImGui::SliderInt(T("Качество (CRF)"), &settings.Quality, 14, 28);
        const char* hint = settings.Quality <= 17   ? T("почти без потерь, файл крупный")
                           : settings.Quality <= 20 ? T("мастер-качество для монтажа")
                           : settings.Quality <= 23 ? T("обычное качество для показа")
                                                    : T("лёгкий файл, видны артефакты");
        ImGui::SameLine();
        ImGui::TextDisabled("— %s", hint);

        const bool hasAudio = doc.Audio.Loaded();
        ImGui::BeginDisabled(!hasAudio);
        bool includeAudio = settings.IncludeAudio && hasAudio;
        if (ImGui::Checkbox(T("Вшить звуковую дорожку"), &includeAudio)) settings.IncludeAudio = includeAudio;
        ImGui::EndDisabled();
        if (!hasAudio) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", T("(звук в проект не загружен)"));
        } else if (doc.Audio.Muted) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", T("(дорожка заглушена — в ролик не попадёт)"));
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(T("Диапазон"));
    ImGui::SetNextItemWidth(150.0f);
    ImGui::DragFloat(T("Начало"), &settings.StartTime, 0.05f, 0.0f, doc.Duration, T("%.2f c"));
    ImGui::SetNextItemWidth(150.0f);
    ImGui::DragFloat(T("Конец"), &settings.EndTime, 0.05f, 0.0f, doc.Duration, T("%.2f c"));
    ImGui::TextDisabled(T("Конец = 0 означает «до конца ролика» (%.2f c)"), (double)doc.Duration);

    ImGui::Spacing();
    std::snprintf(m_renderDir, sizeof(m_renderDir), "%s", settings.OutputDir.c_str());
    ImGui::TextUnformatted(T("Каталог вывода"));

    // Кнопка выбора каталога — обязательная часть, а не украшение. Раньше здесь
    // было только поле ввода: чтобы положить ролик в нужное место, приходилось
    // знать полный путь наизусть и набирать его руками, с обратными слэшами и
    // кириллицей в имени пользователя. Именно на этом шаге рендер и упирался.
    const bool browsable = filedialog::Available();
    const float browseWidth = browsable ? 90.0f : 0.0f;
    ImGui::SetNextItemWidth(browsable ? -(browseWidth + ImGui::GetStyle().ItemSpacing.x) : -1.0f);
    if (ImGui::InputText("##outdir", m_renderDir, sizeof(m_renderDir))) settings.OutputDir = m_renderDir;
    if (browsable) {
        ImGui::SameLine();
        if (ImGui::Button(T("Обзор…"), ImVec2(browseWidth, 0.0f))) {
            std::string picked;
            if (filedialog::PickFolder(T("Куда сохранять рендер"), settings.OutputDir, picked)) {
                settings.OutputDir = picked;
                std::snprintf(m_renderDir, sizeof(m_renderDir), "%s", picked.c_str());
            }
        }
    }

    // Показываем ПОЛНЫЙ путь: относительный («render») человек читает как
    // «где-то рядом», и после рендера начинается поиск файла по диску.
    {
        std::error_code ec;
        const fs::path dir(settings.OutputDir);
        const fs::path full = dir.is_absolute() ? dir : fs::absolute(dir, ec);
        if (!ec && full != dir) ImGui::TextDisabled("%s", full.string().c_str());
        if (fs::exists(full, ec)) {
            ImGui::TextDisabled("%s", T("Каталог существует — файлы лягут в него."));
        } else {
            ImGui::TextDisabled("%s", T("Каталога ещё нет — будет создан при рендере."));
        }
    }

    std::snprintf(m_renderName, sizeof(m_renderName), "%s", settings.BaseName.c_str());
    ImGui::TextUnformatted(mp4 ? T("Имя ролика") : T("Имя файлов"));
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##outname", m_renderName, sizeof(m_renderName))) settings.BaseName = m_renderName;

    ImGui::Spacing();
    ImGui::SetNextItemWidth(220.0f);
    // Сглаживание накоплением. Осмысленные значения — степени двойки; ползунок
    // по «числу выборок» честнее выпадающего списка «низкое/среднее/высокое»:
    // здесь прямо видно, во сколько раз вырастет время рендера.
    ImGui::SliderInt(T("Сглаживание"), &settings.Samples, 1, 16, settings.Samples > 1 ? T("%d выборок") : T("выкл"));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Кадр снимается несколько раз с микросдвигом и усредняется.\n"
                          "Убирает лесенку и мерцание тонких деталей — в отличие от\n"
                          "экранного сглаживания во вьюпорте. Время рендера растёт\n"
                          "во столько же раз."));
    }
    if (settings.Samples > 1) {
        ImGui::TextDisabled(T("Рендер будет примерно в %d раз(а) дольше"), settings.Samples);
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(150.0f);
    ImGui::DragInt(T("Кадров за шаг"), &settings.FramesPerStep, 0.2f, 1, 30);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Больше — быстрее экспорт, но интерфейс отзывается реже."));
    }

    ImGui::Spacing();
    const float end = settings.EndTime > settings.StartTime ? settings.EndTime : doc.Duration;
    const int frames = (int)((end - settings.StartTime) * doc.Fps) + 1;
    if (mp4) {
        ImGui::TextDisabled(T("Будет записано примерно %d кадр(ов) (%.1f c) в %s/%s.mp4"),
                            frames > 0 ? frames : 0,
                            (double)(frames > 0 ? frames : 0) / (double)(doc.Fps > 0.0f ? doc.Fps : 24.0f),
                            settings.OutputDir.c_str(), settings.BaseName.c_str());
    } else {
        ImGui::TextDisabled(T("Будет записано примерно %d кадр(ов) в %s_00000.png …"),
                            frames > 0 ? frames : 0, settings.BaseName.c_str());
    }

    // --- Очередь заданий ---
    // Рендер блокирует работу, поэтому снять три плана — это трижды дождаться
    // конца и трижды переставить настройки руками. Очередь превращает это в
    // «поставил задания и ушёл».
    ImGui::Spacing();
    RenderQueue& queue = host.Queue();
    char queueLabel[64];
    std::snprintf(queueLabel, sizeof(queueLabel), T("Очередь (%d)"), (int)queue.Jobs.size());
    if (ImGui::CollapsingHeader(queueLabel)) {
        if (queue.Jobs.empty()) {
            ImGui::TextDisabled("%s", T("Заданий нет. Настройте параметры выше и нажмите «В очередь»."));
        }
        for (size_t i = 0; i < queue.Jobs.size(); ++i) {
            RenderQueueJob& job = queue.Jobs[i];
            ImGui::PushID((int)i);
            const bool current = queue.CurrentIndex() == (int)i;
            if (current) ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::Good);
            ImGui::Text("%zu. %s", i + 1, job.Name.c_str());
            if (current) ImGui::PopStyleColor();
            ImGui::SameLine(300.0f);
            if (job.Done) ImGui::TextDisabled("%s", job.Result.c_str());
            else if (current) ImGui::TextDisabled("%s", T("идёт…"));
            else ImGui::TextDisabled("%s", T("ждёт"));
            ImGui::SameLine(430.0f);
            if (ImGui::SmallButton(T("Убрать"))) {
                queue.Jobs.erase(queue.Jobs.begin() + (long)i);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }

        ImGui::Spacing();
        if (ImGui::Button(T("В очередь"), ImVec2(120.0f, 0.0f))) {
            RenderQueueJob job;
            job.Settings = settings;
            // Имя собирается из того, что отличает задания друг от друга:
            // диапазон и разрешение. «Задание 1/2/3» ничего бы не сказало.
            char name[160];
            std::snprintf(name, sizeof(name), T("%s · %dx%d · %.1f–%.1f c"),
                          settings.BaseName.c_str(), settings.Width, settings.Height,
                          (double)settings.StartTime,
                          (double)(settings.EndTime > settings.StartTime ? settings.EndTime
                                                                         : doc.Duration));
            job.Name = name;
            queue.Jobs.push_back(std::move(job));
            host.SetStatus(T("Добавлено в очередь: ") + std::to_string(queue.Jobs.size()));
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Запомнить ТЕКУЩИЕ настройки как задание.\n"
                              "Меняйте диапазон и разрешение и добавляйте ещё."));
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(queue.Jobs.empty());
        if (ImGui::Button(T("Запустить очередь"), ImVec2(160.0f, 0.0f))) {
            host.StartQueue();
            Close();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(queue.Jobs.empty());
        if (ImGui::Button(T("Очистить"), ImVec2(100.0f, 0.0f))) queue.Jobs.clear();
        ImGui::EndDisabled();
    }

    ImGui::Separator();
    // Рендер в MP4 без кодировщика заведомо провалится — кнопку гасим, причина
    // уже написана выше.
    ImGui::BeginDisabled(mp4 && !ffmpeg);
    if (ImGui::Button(T("Рендерить"), ImVec2(140.0f, 0.0f))) {
        host.StartRender();
        Close();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Закрыть"), ImVec2(120.0f, 0.0f))) Close();
    ImGui::EndPopup();
}

void DialogsPanel::DrawTimelineSettings(DirectorHost& host) {
    if (!BeginModal(TitleOf(Dialog::TimelineSettings))) return;

    AnimationDocument& doc = host.Document();

    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::DragFloat(T("Длительность"), &doc.Duration, 0.1f, 0.1f, 36000.0f, T("%.2f c"))) host.PushUndo();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::DragFloat(T("Кадров в секунду"), &doc.Fps, 0.5f, 1.0f, 240.0f, "%.0f")) host.PushUndo();
    ImGui::TextDisabled("%s", T("Смена частоты не двигает ключи: время хранится в секундах."));

    ImGui::Spacing();
    ImGui::Checkbox(T("Зациклить проигрывание"), &host.Transport().Loop);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat(T("Скорость"), &host.Transport().Speed, 0.01f, -4.0f, 4.0f, "%.2fx");
    ImGui::TextDisabled("%s", T("Отрицательная скорость проигрывает ролик назад."));

    ImGui::Spacing();
    ImGui::SeparatorText(T("Метки"));
    if (doc.Markers.empty()) {
        ImGui::TextDisabled("%s", T("Меток нет. Animation > Add Marker ставит метку на текущем кадре."));
    } else {
        for (size_t i = 0; i < doc.Markers.size(); ++i) {
            ImGui::PushID((int)i);
            ImGui::SetNextItemWidth(200.0f);
            // Через imgui_stdlib: std::string растёт сам, без ручного буфера и
            // без риска записать мимо capacity.
            ImGui::InputText("##mname", &doc.Markers[i].Name);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::DragFloat("##mtime", &doc.Markers[i].Time, 0.01f, 0.0f, doc.Duration, T("%.2f c"));
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Удалить"))) {
                host.PushUndo();
                doc.Markers.erase(doc.Markers.begin() + (long)i);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }

    ImGui::Separator();
    if (ImGui::Button(T("Закрыть"), ImVec2(120.0f, 0.0f))) Close();
    ImGui::EndPopup();
}

void DialogsPanel::DrawAbout(DirectorHost& host) {
    (void)host;
    if (!BeginModal(TitleOf(Dialog::About))) return;

    if (ImFont* big = Theme::MonoFont()) ImGui::PushFont(big);
    ImGui::TextUnformatted("Director 3D");
    if (Theme::MonoFont()) ImGui::PopFont();

    ImGui::TextDisabled("%s", T("Версия 0.1.0 — инструмент 3D-анимации"));
    ImGui::Spacing();
    ImGui::TextWrapped(T("Собран на движке SAGE Engine %s. Сцена, рендер, скелетная анимация, "
                       "ресурсы и звук — движковые; Director 3D добавляет поверх них таймлайн, "
                       "кривые, транспорт и экспорт секвенции."), kSageEngineVersion);
    ImGui::Spacing();
    ImGui::SeparatorText(T("Форматы"));
    ImGui::BulletText("%s", T(".d3dproj — проект (сцена + анимация)"));
    ImGui::BulletText("%s", T(".sage — сцена движка (экспорт и импорт)"));
    ImGui::BulletText("%s", T(".glb / .gltf / .obj — модели"));
    ImGui::BulletText("%s", T("PNG-секвенция — результат рендера"));
    ImGui::Spacing();
    ImGui::TextDisabled("%s", T("Интерфейс: Dear ImGui. Манипулятор: ImGuizmo."));

    ImGui::Separator();
    if (ImGui::Button(T("Закрыть"), ImVec2(120.0f, 0.0f))) Close();
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
        ImGui::SeparatorText(T("Файл"));
        ImGui::TableNextColumn();
        row("Ctrl+N / Ctrl+O / Ctrl+S", T("Новый / открыть / сохранить"));
        row("Ctrl+Z / Ctrl+Y", T("Отменить / повторить"));
        row("Ctrl+D / Del", T("Дублировать / удалить объект"));

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::SeparatorText(T("Вьюпорт"));
        ImGui::TableNextColumn();
        row(T("ПКМ + WASD / QE"), T("Осмотр и полёт камерой"));
        row(T("СКМ"), T("Панорама"));
        row(T("Колесо"), T("Приблизить / отдалить"));
        row("Q / W / E / R", T("Выбор / перемещение / поворот / масштаб"));
        row("F", T("Навести камеру на выбранное"));
        row("G", T("Показать или скрыть сетку"));

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::SeparatorText(T("Анимация"));
        ImGui::TableNextColumn();
        row(T("Пробел"), T("Проигрывание / пауза"));
        row(T("Shift+Пробел"), T("Стоп и в начало"));
        row("← / →", T("Кадр назад / вперёд"));
        row("Home / End", T("В начало / в конец ролика"));
        row(", / .", T("Предыдущий / следующий ключ"));
        row("K", T("Поставить ключ выбранному"));
        row("Ctrl+K", T("Включить авто-ключ"));
        row("L", T("Зациклить проигрывание"));
        row("M", T("Поставить метку"));
        row("F12", T("Отрендерить секвенцию"));

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::SeparatorText(T("Таймлайн"));
        ImGui::TableNextColumn();
        row(T("Колесо"), T("Зум вокруг курсора"));
        row(T("Shift+Колесо / СКМ"), T("Прокрутка по времени"));
        row(T("Ctrl+клик"), T("Добавить ключ к выделению"));
        row(T("Alt при перетаскивании"), T("Без прилипания к кадрам"));
        row("Del", T("Удалить выбранные ключи"));

        ImGui::EndTable();
    }

    ImGui::Separator();
    if (ImGui::Button(T("Закрыть"), ImVec2(120.0f, 0.0f))) Close();
    ImGui::EndPopup();
}

} // namespace d3d
