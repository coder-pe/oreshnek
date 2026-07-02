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
- `corpus/` — semillas. Cada fichero lleva un byte de ruteo inicial: par → modo
  una-pasada (el resto es la petición HTTP cruda), impar → modo incremental.
- `regressions/` — reproductores de crashes hallados por el fuzzer; se añaden
  aquí para que `fuzz_replay_test` los cubra a perpetuidad.

## Ejecutar el fuzzer (campaña)

En macOS Apple clang no trae el runtime de libFuzzer; usa el LLVM de Homebrew:

```bash
cmake -B build-fuzz -DORESHNEK_FUZZ=ON \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
cmake --build build-fuzz --target fuzz_http_parser
./build-fuzz/fuzz_http_parser -max_total_time=60 tests/fuzz/corpus
```

En Linux basta el `clang` del sistema. Un crash deja un fichero `crash-*`:
cópialo a `tests/fuzz/regressions/` y confirma que `fuzz_replay_test` lo
reproduce (idealmente tras corregir el parser).

## Replay determinista (ctest)

```bash
cmake -B build && cmake --build build --target fuzz_replay_test
ctest --test-dir build -R fuzz_replay --output-on-failure
```
