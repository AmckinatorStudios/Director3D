#pragma once
#include <string>
#include <vector>

#include "sage/scene/Scene.h"

namespace d3d {

class DirectorHost;

// Дерево сцены («Scene» в референсе): объекты, разложенные по категориям
// (Cameras / Characters / Environment / Lights / Effects), с поиском, глазком
// видимости и переносом мышью для смены родителя.
//
// Категории НЕ хранятся в данных — они выводятся из состава компонентов
// сущности. Так дерево всегда отражает реальную сцену: навесил свет — объект
// сам переехал в Lights, и не бывает «папки», которая врёт о содержимом.
class ScenePanel {
public:
    void Draw(DirectorHost& host);

private:
    // Категория, в которую попадает сущность.
    enum class Category { Cameras, Characters, Environment, Lights, Effects, Other, Count };
    static Category CategoryOf(Scene& scene, entt::entity e);
    static const char* CategoryName(Category c);

    // Рисует одну сущность и её потомков.
    void DrawEntity(DirectorHost& host, Scene& scene, entt::entity e, bool insideCategory);
    bool MatchesFilter(Scene& scene, entt::entity e) const;

    std::string m_filter;
    int m_renaming = -1;          // id объекта, имя которого правят на месте
    char m_renameBuffer[128] = {};
    int m_dragSource = -1;        // id объекта, который тащат мышью
};

} // namespace d3d
