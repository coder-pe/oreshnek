# Fuzzing del `HttpParser`

Fuzzing del parser HTTP (la superficie de ataque #1) con **libFuzzer + ASan/UBSan**.
Plan completo en [`docs/LOAD_AND_FUZZ_PLAN.md`](../../docs/LOAD_AND_FUZZ_PLAN.md).

## Piezas

- `parser_fuzz_target.h` — cuerpo compartido del fuzzer. Además de "no crashea",
  comprueba invariantes (`consumed <= tamaño`, `COMPLETE ⇔ complete`, progreso,
  `ERROR` terminal). Cubre dos modos: una pasada y alimentación incremental
  (espeja `Connection::parse_next`, con pipelining).
- `fuzz_http_parser.cpp` — punto de entrada libFuzzer (`LLVMFuzzerTestOneInput`).
- `fuzz_replay.cpp` — replay determinista de `corpus/` y `regressions/`, cableado
  en `ctest` como `fuzz_replay_test`; protege contra regresiones **sin** libFuzzer
  (p.ej. Apple clang).
- `corpus/` — semillas curadas, versionadas en git. Cada fichero lleva un byte
  de ruteo inicial: par → modo una-pasada (el resto es la petición HTTP
  cruda), impar → modo incremental.
- `corpus_growth/` — scratch **gitignored** donde libFuzzer escribe los
  inputs nuevos que descubre durante una campaña (no se commitea; ver más
  abajo por qué no se le pasa `corpus/` directo).
- `regressions/` — reproductores de crashes hallados por el fuzzer; se añaden
  aquí para que `fuzz_replay_test` los cubra a perpetuidad.

## Ejecutar el fuzzer (campaña)

**libFuzzer escribe en el primer directorio que le pasas** (los inputs que
descubre y que aumentan cobertura quedan ahí) y solo *lee* de los siguientes.
Pásale siempre `tests/fuzz/corpus_growth` primero y `tests/fuzz/corpus`
después — si le pasas solo `tests/fuzz/corpus`, una campaña larga te deja
miles de ficheros nuevos mezclados con las semillas curadas y versionadas.

En macOS Apple clang no trae el runtime de libFuzzer; usa el LLVM de Homebrew:

```bash
cmake -B build-fuzz -DORESHNEK_FUZZ=ON \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
cmake --build build-fuzz --target fuzz_http_parser
mkdir -p tests/fuzz/corpus_growth
./build-fuzz/fuzz_http_parser -max_total_time=60 \
    tests/fuzz/corpus_growth tests/fuzz/corpus
```

En Linux basta el `clang` del sistema, siempre que su `compiler-rt` incluya el
runtime de fuzzer:

```bash
# Debian / Ubuntu
sudo apt-get install -y clang

# Fedora / RHEL / CentOS Stream
sudo dnf install -y clang compiler-rt

# Arch Linux
sudo pacman -S --needed clang compiler-rt

cmake -B build-fuzz -DORESHNEK_FUZZ=ON
cmake --build build-fuzz --target fuzz_http_parser
mkdir -p tests/fuzz/corpus_growth
./build-fuzz/fuzz_http_parser -max_total_time=60 \
    tests/fuzz/corpus_growth tests/fuzz/corpus
```

Guía completa paso a paso (VPS Ubuntu/Debian, incluida la campaña larga con
evidencia archivada):
[`docs/RUNBOOK_UBUNTU_LOAD_FUZZ.md`](../../docs/RUNBOOK_UBUNTU_LOAD_FUZZ.md).

Verifica antes de una campaña larga que el runtime está presente
(`find / -name 'libclang_rt.fuzzer*' 2>/dev/null`); si no aparece nada, falta
el paquete `compiler-rt`/`libclang-rt-<ver>-dev` de tu distro. Lista completa
de dependencias por sistema operativo: [`docs/DEPENDENCIES.md`](../../docs/DEPENDENCIES.md).

Un crash deja un fichero `crash-*`: cópialo a `tests/fuzz/regressions/` y
confirma que `fuzz_replay_test` lo reproduce (idealmente tras corregir el
parser).

## Archivar una campaña (evidencia, no solo "pasó")

Para que una corrida cuente como evidencia del criterio de aceptación A.5.2
(≥5 min sin crash/leak/UB) hay que archivar su salida cruda, no solo anotar
que pasó. Procedimiento y formato en
[`campaigns/README.md`](campaigns/README.md); el resumen legible de cada
campaña archivada va en la tabla de "Estado actual" de
[`docs/LOAD_AND_FUZZ_PLAN.md`](../../docs/LOAD_AND_FUZZ_PLAN.md).

## Replay determinista (ctest)

```bash
cmake -B build && cmake --build build --target fuzz_replay_test
ctest --test-dir build -R fuzz_replay --output-on-failure
```
