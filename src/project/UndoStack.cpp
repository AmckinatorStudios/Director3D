#include "project/UndoStack.h"

namespace d3d {

void UndoStack::Push(std::string snapshot) {
    m_undo.push_back(std::move(snapshot));
    // Новая правка после отмены обрывает будущее: повторять больше нечего —
    // ветка, из которой пришли, теперь недостижима.
    m_redo.clear();
    if (m_undo.size() > kMaxDepth) m_undo.erase(m_undo.begin());
}

void UndoStack::Capture(std::string snapshot) {
    // Повторный Capture внутри одного жеста (мышь ведут — виджет активен
    // каждый кадр) не должен затирать самое раннее «до».
    if (m_hasPending) return;
    m_pending = std::move(snapshot);
    m_hasPending = true;
}

void UndoStack::Commit() {
    if (!m_hasPending) return;
    m_hasPending = false;
    Push(std::move(m_pending));
    m_pending.clear();
}

void UndoStack::DropPending() {
    m_hasPending = false;
    m_pending.clear();
}

bool UndoStack::Undo(const std::string& current, std::string& outSnapshot) {
    if (m_undo.empty()) return false;
    outSnapshot = std::move(m_undo.back());
    m_undo.pop_back();
    m_redo.push_back(current);
    if (m_redo.size() > kMaxDepth) m_redo.erase(m_redo.begin());
    return true;
}

bool UndoStack::Redo(const std::string& current, std::string& outSnapshot) {
    if (m_redo.empty()) return false;
    outSnapshot = std::move(m_redo.back());
    m_redo.pop_back();
    m_undo.push_back(current);
    if (m_undo.size() > kMaxDepth) m_undo.erase(m_undo.begin());
    return true;
}

void UndoStack::Clear() {
    m_undo.clear();
    m_redo.clear();
    DropPending();
}

size_t UndoStack::MemoryBytes() const {
    size_t bytes = m_pending.size();
    for (const std::string& s : m_undo) bytes += s.size();
    for (const std::string& s : m_redo) bytes += s.size();
    return bytes;
}

} // namespace d3d
