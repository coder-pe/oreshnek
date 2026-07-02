# Plan: validación de carga (wrk) y fuzzing del parser (libFuzzer)

Cierre de dos de los tres bloqueantes de producción identificados en la
evaluación: (1) el claim de "alto rendimiento" no está medido y (2) el
`HttpParser` —superficie de ataque #1— no está fuzzeado. Este documento define
qué construir, cómo, y los criterios de aceptación. **No implementa nada aún**;
se ejecuta tras tu visto bueno.

Herramientas acordadas: **wrk** para carga, **libFuzzer + ASan/UBSan** para fuzz.

---

## Restricciones de toolchain (verificadas en esta máquina)

- **Apple clang NO trae el runtime de libFuzzer** (`libclang_rt.fuzzer_osx.a` no
  existe). En macOS el target de fuzzing se compila con el **LLVM de Homebrew**,
  ya presente: `/opt/homebrew/opt/llvm/bin/clang++` (probado: compila y corre
  `-fsanitize=fuzzer,address`). En Linux basta el `clang` del sistema.
- **wrk no está instalado** → `brew install wrk` (macOS) / `apt-get install wrk`
  (CI Linux). Es una herramienta externa, no una dependencia del framework: no
  entra en el árbol de `include/`+`src/`, solo en `tools/` y CI.
- Política de dependencias intacta: ni wrk ni libFuzzer se enlazan en la
  librería `oreshnek`; son andamiaje de test/CI.

---

## Parte A — Fuzzing del `HttpParser` (libFuzzer)

### A.1 Harness

Nuevo `tests/fuzz/fuzz_http_parser.cpp` con `LLVMFuzzerTestOneInput`:

- Copia los bytes de entrada a un **buffer mutable** (`std::string`/`std::vector<char>`),
  porque la ruta chunked compacta el cuerpo *in place* (mutación del buffer);
  un `string_view` sobre un literal const sería UB.
- Ejercita el `HttpParser` en dos modos, decididos por el primer byte del input:
  1. **De una pasada**: `parse_request(view, bytes_processed, req)` sobre todo el buffer.
  2. **Incremental**: alimenta el buffer en trozos (simula lecturas parciales de
     socket / pipelining), llamando `parse_request` repetidamente y consumiendo
     `bytes_processed`, con `reset()` entre requests completas.
- Aserciones de invariantes (no solo "no crashea"):
  - `bytes_processed <= raw_buffer.size()` siempre.
  - En estado `COMPLETE`, `bytes_processed > 0` (progreso garantizado).
  - Estado `ERROR` es terminal: no vuelve a `COMPLETE` sin `reset()`.
  - Nunca se superan `MAX_HEADER_BYTES` / `MAX_BODY_BYTES` sin pasar a `ERROR`.

### A.2 Corpus semilla + regresión

- `tests/fuzz/corpus/` con semillas: GET simple, POST con `Content-Length`,
  `Transfer-Encoding: chunked` válido, pipelining, `Expect: 100-continue`, HEAD,
  y casos límite (header al borde de 64 KiB, body al borde de 8 MiB).
- `tests/fuzz/regressions/` para todo crash que encuentre el fuzzer: cada input
  reproductor se guarda y se re-ejecuta como **test determinista en `ctest`**
  (sin fuzzer), de modo que CI en Apple clang también protege contra regresión.

### A.3 CMake

- Opción `ORESHNEK_FUZZ=OFF` (default). Cuando `ON`:
  - Exige un compilador con libFuzzer; si no, error claro apuntando al LLVM de brew.
  - Target `fuzz_http_parser` con `-fsanitize=fuzzer,address,undefined`.
- El **replay de regresiones** se compila siempre (sin `-fsanitize=fuzzer`,
  linkando un `main` que itera `tests/fuzz/regressions/*`) y entra en `ctest`
  como `fuzz_regression_test` → protección continua sin toolchain especial.

### A.4 Ejecución y CI

```bash
# Local (macOS), campaña corta:
cmake -B build-fuzz -DORESHNEK_FUZZ=ON \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
cmake --build build-fuzz --target fuzz_http_parser
./build-fuzz/fuzz_http_parser -max_total_time=60 tests/fuzz/corpus
```

- CI (Linux, clang del sistema): job de fuzz corto (p.ej. 120 s) por PR +
  campaña larga programada (nightly, minimizando corpus). Cualquier crash →
  artefacto subido + semilla añadida a `regressions/`.

### A.5 Criterios de aceptación (Parte A)

1. `fuzz_http_parser` compila y corre bajo libFuzzer+ASan+UBSan.
2. Campaña de ≥5 min sin crash/leak/UB partiendo del corpus semilla; si aparece
   alguno, se corrige y su reproductor queda en `regressions/` (verde en ctest).
3. `fuzz_regression_test` integrado en `ctest` y verde en build normal.

---

## Parte B — Validación de carga (wrk)

### B.1 Andamiaje

- `tools/loadtest/` con:
  - `run.sh <url> [--soak]`: lanza `oreshnek_server` con una config de carga
    conocida, espera a `/health`, corre los escenarios wrk y vuelca resultados.
  - Scripts Lua wrk: `get_json.lua` (JSON pequeño, camino caliente),
    `keepalive.lua` (reutilización de conexión), `pipeline.lua` (pipelining),
    `static.lua` (fichero estático vía `sendfile`).
- Perfilado en dos ejes: **throughput/latencia** (ráfaga corta, p50/p90/p99) y
  **estabilidad** (soak de 10–30 min observando RSS y `/metrics`).

### B.2 Qué se mide y con qué se cruza

- De wrk: req/s, latencia p50/p90/p99, errores, timeouts.
- De `/metrics` (ya expuesto): `requests_total`, clases de respuesta,
  histograma de latencia, `connections_active`, y —clave tras el último
  cambio— `workers_in_flight` y `load_shed_total` para confirmar que bajo
  saturación el shedding actúa y el pool no se desborda.
- **Estabilidad de memoria**: RSS al inicio vs. final del soak; criterio = sin
  crecimiento monótono (descarta fugas/lifetime en el camino caliente). Un soak
  bajo un build ASan (sin fuzzer) da además detección de use-after-free real
  bajo carga sostenida.

### B.3 Escenarios

1. **JSON caliente** keep-alive, concurrencia creciente (c = 50/200/1000) →
   curva throughput/latencia y punto de saturación.
2. **Pipelining** → valida el orden de respuestas HTTP/1.1 bajo presión.
3. **Estático + Range** → valida ruta `sendfile`/206 bajo carga.
4. **Soak** 10–30 min a concurrencia media → estabilidad de RSS y ausencia de
   degradación de latencia (fragmentación, fugas, crecimiento de buckets).

### B.4 Línea base y criterios de aceptación (Parte B)

- Como el objetivo aquí es *validación*, no récord, los umbrales se fijan como
  **línea base reproducible** (se registran los números de esta máquina en el
  README/ANALYSIS y se vigilan regresiones), más criterios cualitativos duros:
  1. 0 errores/timeouts a concurrencia objetivo sostenida (excluyendo 503 de
     shedding deliberado).
  2. RSS estable en el soak (sin crecimiento monótono).
  3. Bajo sobrecarga deliberada, el server responde 503 (shedding) en vez de
     estancarse, y `load_shed_total` crece mientras `workers_in_flight` se
     mantiene acotado ≤ cap.
  4. p99 no se dispara de forma no acotada al subir la concurrencia por debajo
     del punto de saturación.

### B.5 Comparación honesta con terceros (opcional, fase posterior)

Un arnés tipo TechEmpower queda fuera de este entregable; se anota como paso
futuro una vez fijada la línea base propia.

---

## Orden de implementación propuesto

1. **Parte A (fuzz)** primero: mayor valor de seguridad, contenida, y deja
   `fuzz_regression_test` protegiendo en CI desde ya.
2. **Parte B (carga)**: andamiaje `tools/loadtest/` + línea base documentada.
3. Integración de ambos en el pipeline de CI (job de fuzz corto por PR + soak/
   fuzz largo nightly). *(Depende del CI, que es el tercer bloqueante pendiente.)*

Todo el trabajo en una rama de feature; cada commit compila y mantiene verde el
gate de sanitizers y `ctest`.
