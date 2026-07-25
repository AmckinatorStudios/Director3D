#pragma once
#include <cstddef>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// UndoStack — отмена/повтор по СНИМКАМ состояния.
//
// Инструмент анимации правит одно и то же состояние из десятка мест: гизмо во
// вьюпорте, поля инспектора, перетаскивание ключей, блоки клипов, дерево сцены.
// Команда на каждую правку — это десяток классов, каждый со своим обратным
// действием, и любая забытая пара «сделать/отменить» превращается в тихую
// порчу проекта. Снимок целиком (JSON проекта, см. ProjectFile::SnapshotToString)
// нельзя забыть реализовать: он один на все правки и по построению точен.
//
// Цена — память и время сериализации. Поэтому:
//   • стек ограничен по числу шагов (kMaxDepth) — старое вытесняется;
//   • «размазанные» правки (тянут ползунок, тащат ключ мышью) кладут ОДИН
//     снимок на весь жест: Capture запоминает состояние «до» в момент захвата
//     виджета, Commit кладёт его в стек в момент первого реального изменения.
//     Без этой пары каждый кадр перетаскивания стал бы отдельным шагом отмены.
// ---------------------------------------------------------------------------
namespace d3d {

class UndoStack {
public:
    // Кладёт снимок состояния «до правки». Зовётся ПЕРЕД изменением.
    void Push(std::string snapshot);

    // Запоминает состояние «до» без записи в стек (начало жеста).
    void Capture(std::string snapshot);
    // Кладёт запомненное в стек (факт изменения). Без Capture — ничего не делает.
    void Commit();
    // Отменяет незакоммиченный Capture (жест закончился без изменений).
    void DropPending();
    bool HasPending() const { return m_hasPending; }

    // Возвращает снимок, к которому надо откатиться, и переносит текущее
    // состояние в стек повтора. current — состояние ПРЯМО СЕЙЧАС (его надо
    // передать, потому что стек хранит только «до», а не «после»).
    bool Undo(const std::string& current, std::string& outSnapshot);
    bool Redo(const std::string& current, std::string& outSnapshot);

    bool CanUndo() const { return !m_undo.empty(); }
    bool CanRedo() const { return !m_redo.empty(); }

    void Clear();
    size_t UndoDepth() const { return m_undo.size(); }
    size_t RedoDepth() const { return m_redo.size(); }
    // Сколько памяти занимают снимки — показывается в статус-баре, чтобы
    // «инструмент съел гигабайт» не было сюрпризом.
    size_t MemoryBytes() const;

    static constexpr size_t kMaxDepth = 64;

private:
    std::vector<std::string> m_undo;
    std::vector<std::string> m_redo;
    std::string m_pending;
    bool m_hasPending = false;
};

} // namespace d3d
