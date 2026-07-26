#include "ui/Theme.h"

#include <cstdio>

namespace d3d::Theme {

namespace {
ImFont* g_small = nullptr;
ImFont* g_mono = nullptr;

// Пробует загрузить шрифт по списку путей: сначала свой (лежит рядом с
// бинарником), потом системные. Возвращает nullptr, если ничего не нашлось —
// тогда остаётся встроенный ProggyClean (только латиница).
ImFont* TryLoad(const char* const* paths, int count, float size, const ImWchar* ranges) {
    ImGuiIO& io = ImGui::GetIO();
    for (int i = 0; i < count; ++i) {
        std::FILE* probe = std::fopen(paths[i], "rb");
        if (!probe) continue;
        std::fclose(probe);
        return io.Fonts->AddFontFromFileTTF(paths[i], size, nullptr, ranges);
    }
    return nullptr;
}

const char* const kUiFontCandidates[] = {
    "assets/fonts/director-ui.ttf", // свой — копируется рядом с бинарником
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "C:/Windows/Fonts/segoeui.ttf",
    "C:/Windows/Fonts/arial.ttf",
};

} // namespace

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    // Кириллица нужна: подписи инструмента и имена объектов сцены по-русски
    // иначе превратились бы в «???».
    const ImWchar* ranges = io.Fonts->GetGlyphRangesCyrillic();
    const int count = (int)(sizeof(kUiFontCandidates) / sizeof(kUiFontCandidates[0]));

    TryLoad(kUiFontCandidates, count, 16.0f, ranges);      // основной
    g_small = TryLoad(kUiFontCandidates, count, 12.0f, ranges); // линейка/подписи каналов
    g_mono  = TryLoad(kUiFontCandidates, count, 22.0f, ranges); // таймкод на транспорте
}

ImFont* SmallFont() { return g_small; }
ImFont* MonoFont() { return g_mono; }

void Apply() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark();

    // --- Метрики: сдержанные скругления, плотная, но не тесная посадка ---
    style.WindowRounding = 0.0f;   // панели стыкуются встык, как в референсе
    style.ChildRounding = 3.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.ScrollbarRounding = 5.0f;
    style.WindowPadding = ImVec2(8, 8);
    style.FramePadding = ImVec2(7, 4);
    style.CellPadding = ImVec2(6, 3);
    style.ItemSpacing = ImVec2(7, 5);
    style.ItemInnerSpacing = ImVec2(5, 4);
    style.IndentSpacing = 16.0f;
    style.ScrollbarSize = 11.0f;
    style.GrabMinSize = 9.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.SeparatorTextBorderSize = 1.0f;

    // --- Палитра ---
    // Значения повторяют Theme::Colors, но в ImVec4: ImGui хранит стиль во
    // float-цветах, а ImDrawList принимает упакованные ImU32. Держим их рядом,
    // чтобы расхождение было видно глазом при правке.
    const ImVec4 bg0(0.114f, 0.125f, 0.137f, 1.00f); // фон панелей
    const ImVec4 bg1(0.149f, 0.164f, 0.180f, 1.00f); // поля ввода, шапки
    const ImVec4 bg2(0.196f, 0.212f, 0.231f, 1.00f); // наведение
    const ImVec4 bgDeep(0.090f, 0.098f, 0.106f, 1.00f);
    const ImVec4 accent(0.235f, 0.627f, 0.910f, 1.00f);
    const ImVec4 accentHi(0.353f, 0.706f, 0.949f, 1.00f);
    const ImVec4 text(0.890f, 0.902f, 0.910f, 1.00f);
    const ImVec4 textDim(0.553f, 0.580f, 0.604f, 1.00f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = textDim;
    c[ImGuiCol_WindowBg] = bg0;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.106f, 0.118f, 0.129f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.227f, 0.243f, 0.259f, 0.85f);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = bg1;
    c[ImGuiCol_FrameBgHovered] = bg2;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.235f, 0.255f, 0.278f, 1.00f);
    c[ImGuiCol_TitleBg] = bgDeep;
    c[ImGuiCol_TitleBgActive] = bg1;
    c[ImGuiCol_TitleBgCollapsed] = bgDeep;
    c[ImGuiCol_MenuBarBg] = ImVec4(0.098f, 0.106f, 0.118f, 1.00f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.20f);
    c[ImGuiCol_ScrollbarGrab] = bg2;
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.290f, 0.310f, 0.333f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = accent;
    c[ImGuiCol_CheckMark] = accentHi;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accentHi;
    c[ImGuiCol_Button] = bg1;
    c[ImGuiCol_ButtonHovered] = bg2;
    c[ImGuiCol_ButtonActive] = ImVec4(0.216f, 0.408f, 0.588f, 1.00f);
    c[ImGuiCol_Header] = ImVec4(0.180f, 0.286f, 0.392f, 1.00f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.216f, 0.353f, 0.478f, 1.00f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.235f, 0.416f, 0.573f, 1.00f);
    c[ImGuiCol_Separator] = c[ImGuiCol_Border];
    c[ImGuiCol_SeparatorHovered] = accent;
    c[ImGuiCol_SeparatorActive] = accentHi;
    c[ImGuiCol_ResizeGrip] = ImVec4(0.255f, 0.275f, 0.302f, 0.60f);
    c[ImGuiCol_ResizeGripHovered] = accent;
    c[ImGuiCol_ResizeGripActive] = accentHi;
    c[ImGuiCol_Tab] = ImVec4(0.129f, 0.141f, 0.153f, 1.00f);
    c[ImGuiCol_TabHovered] = bg2;
    c[ImGuiCol_TabActive] = bg0;
    c[ImGuiCol_TabUnfocused] = ImVec4(0.118f, 0.129f, 0.141f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.145f, 0.157f, 0.169f, 1.00f);
    c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    c[ImGuiCol_DockingEmptyBg] = bgDeep;
    c[ImGuiCol_PlotLines] = accentHi;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_TableHeaderBg] = bg1;
    c[ImGuiCol_TableBorderStrong] = c[ImGuiCol_Border];
    c[ImGuiCol_TableBorderLight] = ImVec4(0.196f, 0.208f, 0.220f, 0.60f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.020f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_DragDropTarget] = accentHi;
    c[ImGuiCol_NavHighlight] = accentHi;

    // Панель, вытащенная в отдельное окно ОС, должна выглядеть как обычное
    // окно — без скруглений и без прозрачности.
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        c[ImGuiCol_WindowBg].w = 1.0f;
    }
}

ImU32 BlockColor(int key) {
    // Палитра подобрана вручную, а не выведена из хеша: случайные цвета дают
    // и ядовитые, и почти одинаковые пары, а на монтаже соседние планы обязаны
    // различаться с одного взгляда.
    static const ImU32 kPalette[] = {
        0xFF8A4A9A, 0xFF4A8A5A, 0xFF3C7FD9, 0xFFD9A03C,
        0xFFC2543C, 0xFF3CB0B0, 0xFF8A7A3C, 0xFF6A5AC8,
    };
    constexpr int kCount = (int)(sizeof(kPalette) / sizeof(kPalette[0]));
    // Отрицательные ключи (камеры нет) не должны уводить индекс в минус.
    const int index = ((key % kCount) + kCount) % kCount;
    return kPalette[index];
}

} // namespace d3d::Theme
