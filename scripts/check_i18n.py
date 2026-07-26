#!/usr/bin/env python3
# ============================================================================
#  Проверка полноты перевода: сверяет строки из T("...") в исходниках со
#  словарём assets/i18n/en.json.
#
#  Зачем отдельный скрипт, а не проверка внутри программы. Ключ перевода — сама
#  русская строка, поэтому «список всех ключей» существует только в исходном
#  коде. Программа во время работы видит лишь те строки, которые реально
#  запрашивались: не открыл окно настроек — не узнал, что там дыра. Полное
#  покрытие можно посчитать только по исходникам, то есть до сборки.
#
#  Внутри программы остаётся вторая, дополняющая проверка — i18n::MissingKeys()
#  ловит то, что этот разбор пропустил (строки, собранные во время работы).
#
#  Запуск: python3 scripts/check_i18n.py   (0 — всё на месте, 1 — расхождения)
# ============================================================================
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DICT = ROOT / "assets" / "i18n" / "en.json"
SOURCES = sorted((ROOT / "src").rglob("*.cpp")) + sorted((ROOT / "src").rglob("*.h"))

# Строковый литерал целиком, вместе с экранированием: \" не должна обрывать
# разбор, иначе T("Кадр \"%d\"") распадётся на куски.
LITERAL = r'"(?:[^"\\]|\\.)*"'
# Соседние литералы C++ склеиваются КОМПИЛЯТОРОМ, до вызова: T("а" "б")
# передаёт в функцию "аб". Поэтому ловим всю цепочку, а не первый кусок —
# иначе ключи получатся обрезанными и не совпадут ни с чем.
CALL = re.compile(r'\bT\(\s*(' + LITERAL + r'(?:\s*' + LITERAL + r')*)\s*[,)]')
# Комментарии и сами файлы локализации из разбора исключаются: в них литералы
# есть, но они не надписи (в Localization.h примеры в комментарии).
LINE_COMMENT = re.compile(r'//[^\n]*')
BLOCK_COMMENT = re.compile(r'/\*.*?\*/', re.S)
# Спецификаторы printf. Перевод обязан повторять их ОДИН В ОДИН и в том же
# порядке: строка уходит в ImGui::Text как формат, а аргументы к ней собирает
# вызывающий код. Лишний %d в переводе прочитает из стека мусор, пропущенный
# %s — молча потеряет значение. Это единственное расхождение в словаре,
# которое приводит не к некрасивой надписи, а к падению.
SPEC = re.compile(r'%[-+ #0]*[0-9*]*(?:\.[0-9*]+)?(?:hh|h|ll|l|L|z|j|t)?[diouxXeEfgGaAcspn%]')


def unescape(literal_chain: str) -> str:
    """Склеивает цепочку литералов и разворачивает экранирование, получая ту же
    строку, которую увидит T() во время работы."""
    parts = re.findall(LITERAL, literal_chain)
    joined = "".join(p[1:-1] for p in parts)
    return (joined.replace(r"\n", "\n").replace(r"\t", "\t")
                  .replace(r"\"", '"').replace(r"\\", "\\"))


# Подписи свойств лежат таблицей и показываются как T(info.Label) — ключа в
# виде литерала рядом с T() там нет, разбор вызовов их не увидит. Таблица
# известна и стабильна, поэтому её третья колонка читается отдельно.
TABLE_ROW = re.compile(r'\{Property::\w+,\s*"[^"]*",\s*"([^"]*)"')


def collect_table_keys(keys):
    path = ROOT / "src" / "anim" / "Binding.cpp"
    if not path.exists():
        return
    for label in TABLE_ROW.findall(path.read_text(encoding="utf-8")):
        if label:
            keys.setdefault(label, "src/anim/Binding.cpp")


def collect_keys():
    keys = {}
    collect_table_keys(keys)
    for path in SOURCES:
        if path.name.startswith("Localization."):
            continue
        text = path.read_text(encoding="utf-8")
        text = BLOCK_COMMENT.sub(" ", text)
        text = LINE_COMMENT.sub(" ", text)
        for match in CALL.finditer(text):
            key = unescape(match.group(1))
            if key:
                keys.setdefault(key, path.relative_to(ROOT).as_posix())
    return keys


def main() -> int:
    if not DICT.exists():
        print(f"нет словаря {DICT}")
        return 1
    dictionary = json.loads(DICT.read_text(encoding="utf-8"))
    keys = collect_keys()

    missing = sorted(k for k in keys if k not in dictionary)
    extra = sorted(k for k in dictionary if k not in keys)
    # Перевод, совпадающий с оригиналом, — обычно забытая строка, но не всегда:
    # «FPS», «glTF», «%.2f» одинаковы на обоих языках. Поэтому это замечание,
    # а не ошибка.
    identical = sorted(k for k, v in dictionary.items() if k == v and k in keys)

    print(f"строк в коде: {len(keys)}   в словаре: {len(dictionary)}")
    for title, items in (("НЕТ ПЕРЕВОДА", missing), ("ЛИШНИЕ В СЛОВАРЕ", extra)):
        if items:
            print(f"\n{title} ({len(items)}):")
            for k in items[:40]:
                where = keys.get(k, "—")
                print(f"  {k!r}  [{where}]")
            if len(items) > 40:
                print(f"  … ещё {len(items) - 40}")
    if identical:
        print(f"\nперевод совпадает с оригиналом ({len(identical)}): "
              + ", ".join(repr(k) for k in identical[:10]))

    mismatched = [(k, v) for k, v in sorted(dictionary.items())
                  if SPEC.findall(k) != SPEC.findall(v)]
    if mismatched:
        print(f"\nРАСХОЖДЕНИЕ В ФОРМАТЕ ({len(mismatched)}):")
        for k, v in mismatched:
            print(f"  {k!r} {SPEC.findall(k)}")
            print(f"  {v!r} {SPEC.findall(v)}")

    if missing or extra or mismatched:
        return 1
    print("покрытие полное")
    return 0


if __name__ == "__main__":
    sys.exit(main())
