#pragma once

namespace d3d {

class DirectorHost;

// Главное меню приложения (File / Edit / Create / View / Tools / Animation /
// Render / Window / Help). Рисуется внутри окна дока хоста, поэтому это
// BeginMenuBar, а не BeginMainMenuBar — иначе полоса меню жила бы отдельно от
// раскладки панелей и не совпадала бы с ней по ширине при multi-viewport.
class MenuBarPanel {
public:
    void Draw(DirectorHost& host);

    // Показывать ли окно «О программе» / «Горячие клавиши» — состояние живёт
    // в панели диалогов, меню только просит его открыть (host.OpenDialog).
};

} // namespace d3d
