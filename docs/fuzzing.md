# Фаззинг парсера и анализатора

Покрытийно-управляемый фаззинг **переднего края** Codons: строгий разбор
(`bio_parse`) и анализ (`bio_analyze`). Модуль намеренно отделён от
`tests/fuzz_diff.py`, который сравнивает *исполняющие* бэкенды (JIT против
интерпретатора) — здесь исполнение и JIT не участвуют вовсе.

Входная точка — `bio_load(const char *path, int repair, BioError *error)`
(`include/bio.h:71`): «загрузить и проверить без выделения исполняемой
памяти». Она вызывает `bio_parse` + `bio_analyze` и не строит машинный код,
поэтому покрытие относится только к парсеру и анализатору. Клиентский режим
`codons check <file.dna>` проходит ровно по этому пути.

## Два инструмента

| Файл | Тип | Где запускается |
| --- | --- | --- |
| `tests/fuzz/parser_fuzz.c` | libFuzzer-таргет (`LLVMFuzzerTestOneInput`) | Linux / CI, Clang `-fsanitize=fuzzer` |
| `tests/fuzz/coverage_fuzzer.py` | gcov-управляемый мутационный фаззер | локально, любой платформой с gcov |

## Корпус

`tests/fuzz/corpus/` — 30 исходных файлов: валидные примеры (например
`factorial.dna`, `list_filter.dna`, `fib64.dna`) плюс намеренно битые фрагменты
(`broken.dna`, `garbage.dna`, `infinite.dna`). Корпус — это и стартовые точки
мутаций, и регрессионная база: каждый «интересный» вход, выросший покрытие,
добавляется обратно в корпус прямо во время прогона.

## 1. parser_fuzz.c (libFuzzer)

Таргет пишет входной буфер fuzzer-а во временный файл
`.codons_fuzz_<pid>.dna` и прогоняет его через `bio_load` дважды: строго
(`repair=0`) и в режиме восстановления (`repair=1`). Каждый успешный результат
освобождается через `bio_program_release`, временный файл удаляется. Так как
`bio_load` принимает путь, а не буфер, запись во временный файл обязательна.

Сборка на Linux/CI (на Windows Clang нет `-fsanitize=fuzzer`):

```sh
clang -g -O1 -std=c11 -fsanitize=fuzzer,address,undefined \
  -fno-omit-frame-pointer -Iinclude \
  tests/fuzz/parser_fuzz.c src/parser.c src/analyze.c src/core.c src/runtime.c \
  -o build/parser_fuzz
./build/parser_fuzz tests/fuzz/corpus -max_total_time=60 -print_final_stats=1
```

Локально на Windows ограничиваемся проверкой сборки без предупреждений —
таргет должен компилироваться начисто под `-Werror` обеими цепочками
(Clang 20 и mingw64 GCC):

```sh
clang -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -c tests/fuzz/parser_fuzz.c
gcc   -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -c tests/fuzz/parser_fuzz.c
```

## 2. coverage_fuzzer.py (gcov)

Запускаемый локальный фаззер. Цикл: `мутация → codons check <вход> → gcov →
сравнение покрытия → сохранить вход, если покрытие выросло`. Сигнал покрытия —
агрегат gcov по трём единицам трансляции: `src/parser.c`, `src/analyze.c`,
`src/core.c` (обёртка `bio_load` находится в `core.c`).

Бинарник собирается **один раз** в отдельную gcov-инструментированную папку
`build-fuzz/` с флагом `--coverage`. Счётчики `.gcda` накапливаются между
запусками `codons check`, поэтому после каждой мутации вызывается
`gcov -f -n` и парсятся строки `Lines executed: X% of N` по каждой функции.
Вход считается «интересным», когда выросло число покрытых строк или функций.

Сборка под gcov (Windows / MinGW-w64):

```sh
cmake -S . -B build-fuzz -G "MinGW Makefiles" \
  -DCMAKE_C_COMPILER=C:/ProgramData/mingw64/mingw64/bin/gcc.exe \
  -DCMAKE_C_FLAGS="--coverage -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="--coverage"
cmake --build build-fuzz
```

Запуск:

```sh
python tests/fuzz/coverage_fuzzer.py --iterations 600
python tests/fuzz/coverage_fuzzer.py --iterations 2000 --seed 42
```

Отчёт пишется в `build-fuzz/coverage_fuzzer_report.txt`, найденные входы —
в `build-fuzz/coverage_interesting/` (выросшее покрытие) и
`build-fuzz/coverage_crashes/` (краши/зависания). Бинарник и `gcov` находятся
автоматически; `--no-reset` оставляет накопленные `.gcda` без очистки.

> Деталь реализации: stdout/stderr дочерних процессов перенаправляются во
> временные **файлы**, а не читаются через канал — захват через пайп
> заблокирован в песочнице. Файлы вычитываются уже после завершения процесса.

## Результаты прогона

`--iterations 600`, gcov от mingw64, суммарно по трём единицам трансляции:

| Метрика | До фаззинга | После | Прирост |
| --- | --- | --- | --- |
| Строки | 1118 / 2814 (39,7 %) | 1251 / 2814 (44,5 %) | **+133 строки** |
| Функции | 28 / 48 (58,3 %) | 28 / 48 (58,3 %) | +0 |

Покрытие по функциям насытилось уже на стартовом корпусе (передний край мал и
весь достижим из валидных примеров); прирост сосредоточен на уровне строк —
в новых ветках парсера и анализатора.

После прогона, по источникам:

| Единица трансляции | Строки | Функции |
| --- | --- | --- |
| `src/parser.c` | 805 / 1190 (67,6 %) | 12 / 13 |
| `src/analyze.c` | 320 / 443 (72,2 %) | 7 / 7 |
| `src/core.c` | 126 / 1181 (10,7 %) | 9 / 28 |

Итоги фаззинга: краши **0**, зависания **0**, «интересных» входов **17**
(корпус вырос с 30 до 47 файлов).

## Найденные проблемы

Сбоев нет. Фаззер не поймал ни крашей, ни зависаний парсера и анализатора
(входы с кодом возврата вне `{0, 1}` отсутствуют). По соглашению проекта
ошибки в `src/parser.c` / `src/analyze.c` не правятся — только фиксируются;
сейчас фиксировать нечего.

## Отличие от tests/fuzz_diff.py

| | `tests/fuzz_diff.py` | `tests/fuzz/coverage_fuzzer.py` |
| --- | --- | --- |
| Цель | сходимость JIT и интерпретатора | покрытие парсера/анализатора |
| Путь | компиляция + исполнение | `bio_load` (без JIT, без исполнения) |
| Сигнал | расхождение результатов бэкендов | прирост покрытия gcov |
| Оракул | детерминированный | покрытийный (growth) |

## Интеграция в CMake

Обвязка таргета уже присутствует в `CMakeLists.txt` и держит компиляцию начисто
под `-Werror` (`/W4 /WX` под MSVC) на каждой сборке — это и есть проверка
приёмки (a):

```cmake
add_library(parser_fuzz_obj OBJECT tests/fuzz/parser_fuzz.c)
target_include_directories(parser_fuzz_obj PRIVATE include)
if(MSVC)
  target_compile_options(parser_fuzz_obj PRIVATE /W4 /WX)
else()
  target_compile_options(parser_fuzz_obj PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
# внутри if(Python3_Interpreter_FOUND):
add_test(NAME fuzz_coverage_selfcheck COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/coverage_fuzzer.py" --help)
```

Для реального запуска libFuzzer на Linux/CI добавляется исполняемая цель и
удобная цель `fuzz`, включаемая опцией `CODONS_FUZZ` (по умолчанию OFF, так
как требует Clang с `-fsanitize=fuzzer`, недоступного в Windows-сборке):

```cmake
option(CODONS_FUZZ "Build the libFuzzer parser/analyzer harness (Linux/CI Clang)" OFF)
if(CODONS_FUZZ)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "CODONS_FUZZ requires Clang (-fsanitize=fuzzer)")
  endif()
  add_executable(fuzz_parser
    tests/fuzz/parser_fuzz.c
    src/parser.c src/analyze.c src/core.c src/runtime.c)
  target_include_directories(fuzz_parser PRIVATE include)
  target_compile_options(fuzz_parser PRIVATE
    -g -O1 -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
  target_link_options(fuzz_parser PRIVATE -fsanitize=fuzzer,address,undefined)
  add_custom_target(fuzz
    COMMAND fuzz_parser tests/fuzz/corpus -max_total_time=60
    DEPENDS fuzz_parser)
endif()
```

Итог: `cmake -B build-linux -DCODONS_FUZZ=ON` даёт цель `fuzz_parser`, а
`cmake --build . --target fuzz` прогоняет корпус минуту и печатает итоговую
статистику покрытия.