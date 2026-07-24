# Plan: validación de carga (wrk) y fuzzing del parser (libFuzzer)

Cierre de dos de los tres bloqueantes de producción identificados en la
evaluación: (1) el claim de "alto rendimiento" (README) no está medido y (2) el
`HttpParser` —superficie de ataque #1— no está fuzzeado. El tercer bloqueante,
CI, queda fuera de este documento (ver "Qué desbloquea el indicador de
producción" más abajo). Este documento define qué construir, cómo, los
criterios de aceptación, y **cómo se documenta la evidencia** de que cada
criterio se cumplió — un harness que compila no es evidencia; una campaña
ejecutada con su salida archivada sí lo es.

Herramientas acordadas: **wrk** para carga, **libFuzzer + ASan/UBSan** para fuzz.

## Estado actual

| Parte | Estado | Detalle |
|---|---|---|
| A — Harness de fuzzing | ✅ Implementado | `tests/fuzz/` (harness, corpus, replay determinista en `ctest`); commit `c271ee8`. |
| A — Campaña larga + evidencia archivada | ⬜ Pendiente | El harness compila y el replay del corpus semilla pasa en `ctest`, pero no hay registro de una campaña extendida (criterio A.5.2). Ver "Recolección y documentación de resultados". |
| B — Andamiaje de carga (`tools/loadtest/`) | ⬜ Pendiente | No implementado; nada en `tools/` además de `analyze.sh`. |
| B — Línea base documentada | ⬜ Pendiente | Depende de B.1. |
| CI | ⬜ Pendiente | No hay pipeline (`.github/workflows/` no existe); fuera de alcance de este documento. |

---

## Restricciones de toolchain (verificadas en esta máquina)

- **Apple clang NO trae el runtime de libFuzzer** (`libclang_rt.fuzzer_osx.a` no
  existe). En macOS el target de fuzzing se compila con el **LLVM de Homebrew**,
  ya presente: `/opt/homebrew/opt/llvm/bin/clang++` (probado: compila y corre
  `-fsanitize=fuzzer,address`). En Linux basta el `clang` del sistema, siempre
  que su `compiler-rt` incluya el runtime de fuzzer (ver verificación abajo).
- **wrk no está instalado** → `brew install wrk` (macOS) / `apt-get install wrk`
  (Debian/Ubuntu). En Fedora/RHEL/CentOS y Arch no hay paquete oficial; se
  compila desde fuente. Es una herramienta externa, no una dependencia del
  framework: no entra en el árbol de `include/`+`src/`, solo en `tools/` y CI.
- Política de dependencias intacta: ni wrk ni libFuzzer se enlazan en la
  librería `oreshnek`; son andamiaje de test/CI.
- **Instalación completa por sistema operativo** (macOS, Debian/Ubuntu,
  Fedora/RHEL/CentOS Stream, Arch): [`docs/DEPENDENCIES.md`](DEPENDENCIES.md).
  Resumen mínimo para este plan:

  ```bash
  # macOS
  brew install llvm wrk

  # Debian / Ubuntu
  sudo apt-get install -y clang wrk

  # Fedora / RHEL / CentOS Stream
  sudo dnf install -y clang compiler-rt
  # wrk: sin paquete — compilar desde fuente (ver docs/DEPENDENCIES.md)

  # Arch Linux
  sudo pacman -S --needed clang compiler-rt
  # wrk: en AUR (yay -S wrk) o compilar desde fuente
  ```

  Verificación rápida de que el `clang` disponible trae runtime de libFuzzer
  (cualquier Linux): `find / -name 'libclang_rt.fuzzer*' 2>/dev/null`.

---

## Parte A — Fuzzing del `HttpParser` (libFuzzer)

**Estado: ✅ harness implementado** (commit `c271ee8`). El detalle de piezas y
cómo ejecutarlo vive ahora en [`tests/fuzz/README.md`](../tests/fuzz/README.md);
lo que sigue documenta el diseño y sirve de referencia. Pendiente: correr una
campaña larga y archivar su evidencia (ver más abajo).

### A.1 Harness

`tests/fuzz/fuzz_http_parser.cpp` con `LLVMFuzzerTestOneInput`:

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

1. ✅ `fuzz_http_parser` compila y corre bajo libFuzzer+ASan+UBSan.
2. ⬜ Campaña de ≥5 min sin crash/leak/UB partiendo del corpus semilla; si aparece
   alguno, se corrige y su reproductor queda en `regressions/` (verde en ctest).
   **No ejecutada/archivada todavía** — es lo que falta para poder marcar Parte
   A como cerrada de cara al indicador de producción.
3. ✅ `fuzz_regression_test` integrado en `ctest` y verde en build normal
   (`tests/fuzz/regressions/` existe y está vacío: cero crashes conocidos
   pendientes de corregir).

---

## Parte B — Validación de carga (wrk)

**Estado: ⬜ pendiente.** Nada de lo descrito abajo está implementado todavía
(no existe `tools/loadtest/`); esta sección sigue siendo el diseño a construir,
no una descripción de algo ya hecho.

### B.1 Andamiaje

- `tools/loadtest/` con:
  - `run.sh <url> [--soak]`: lanza el servidor de demo
    (`examples/07_config_server`) con una config de carga conocida, espera a
    `/health`, corre los escenarios wrk y vuelca resultados.
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

## Recolección y documentación de resultados

Correr una campaña o un escenario de carga sin dejar rastro no cuenta como
evidencia — un `crash-*` no capturado o un `req/s` que solo vio quien lo
ejecutó no sirve para auditar el estado de producción más tarde. Regla
general: **toda ejecución que se invoque para cerrar un criterio de
aceptación (A.5 / B.4) se archiva** con salida cruda + metadatos, no solo un
"pasó"/"falló" de palabra.

### Metadatos mínimos de cada corrida

Independientemente de si es fuzz o carga, cada evidencia archivada debe
llevar:

- **Fecha** y **commit** (`git rev-parse HEAD`) sobre el que se corrió.
- **Máquina/SO** (`uname -a`) y compilador (`clang++ --version` /
  `gcc --version`) — el rendimiento y la superficie que cubre libFuzzer
  dependen del toolchain.
- **Comando exacto** ejecutado (copiable, no parafraseado).
- **Duración** real de la corrida.
- **Salida cruda** del comando (no un resumen editado a mano).

### Fuzzing (Parte A)

```bash
# Campaña con salida archivada; -print_final_stats da el resumen final
# (execs totales, exec/s, cobertura de edges, tamaño de corpus) en stderr.
mkdir -p tests/fuzz/campaigns
./build-fuzz/fuzz_http_parser -max_total_time=300 -print_final_stats=1 \
    tests/fuzz/corpus \
    2>&1 | tee "tests/fuzz/campaigns/$(date +%Y%m%d)-$(git rev-parse --short HEAD).log"
```

- Qué extraer del log para el criterio A.5.2: líneas `stat::number_of_executed_units`,
  `stat::average_exec_per_sec`, `stat::new_units_added` (corpus creció / no
  encontró nada nuevo) y ausencia de cualquier reporte de ASan/UBSan o
  `SUMMARY: libFuzzer: deadly signal`.
- **Si aparece un crash**: el propio libFuzzer deja un fichero `crash-<hash>`
  en el directorio de trabajo. Cópialo a `tests/fuzz/regressions/`, corrige el
  parser, y confirma que `ctest -R fuzz_replay` lo cubre en verde — el log de
  la corrida que lo encontró se conserva igualmente (no se sobreescribe) como
  registro de cuándo y cómo se halló.
- `tests/fuzz/campaigns/*.log` es el archivo de evidencia para A.5.2; una vez
  exista al menos una corrida ≥5 min limpia, esa línea pasa de ⬜ a ✅ en la
  tabla de "Estado actual" (con el nombre de fichero como referencia).

### Carga (Parte B, una vez implementada `tools/loadtest/`)

```bash
# Snapshot de /metrics antes y después de cada escenario (formato Prometheus,
# texto plano — diff directo con `diff`).
curl -s http://localhost:8080/metrics > "results/<escenario>-antes.prom"

# wrk con salida completa archivada (no solo el resumen de consola).
wrk -t4 -c200 -d60s -s tools/loadtest/get_json.lua http://localhost:8080/ \
    2>&1 | tee "results/<escenario>-$(date +%Y%m%d).log"

curl -s http://localhost:8080/metrics > "results/<escenario>-despues.prom"

# RSS durante el soak: muestreo periódico a un CSV (timestamp,rss_kb).
while kill -0 "$SERVER_PID" 2>/dev/null; do
    printf '%s,%s\n' "$(date +%s)" "$(ps -o rss= -p "$SERVER_PID")" >> results/soak-rss.csv
    sleep 30
done
```

- Qué extraer del log de wrk para B.4: `Requests/sec`, la tabla de
  latencias (`50%`/`90%`/`99%`), `Socket errors` (debe ser 0 salvo 503
  deliberados) y `Non-2xx or 3xx responses`.
- El diff de `/metrics` antes/después confirma `requests_total`,
  `load_shed_total` y que `workers_in_flight` volvió a su línea base tras el
  escenario (sin fuga de handlers en vuelo).
- `results/soak-rss.csv` es la evidencia del criterio "RSS estable" — se
  espera una serie plana o con oscilación acotada, no una pendiente positiva
  sostenida; un gráfico rápido (`gnuplot`/hoja de cálculo) basta para el
  registro, no hace falta tooling adicional.
- Una vez exista una corrida completa de los 4 escenarios de B.3 con su
  evidencia, se transcribe un resumen (no el log completo) a una tabla de
  línea base en este documento (sección B.4) y/o en
  [`docs/ANALYSIS.md`](ANALYSIS.md), con enlace al log crudo correspondiente.

### Dónde vive la evidencia

- `tests/fuzz/campaigns/*.log` — corridas de fuzzing (nuevo directorio, git-
  tracked; los logs son texto plano y pequeños).
- `tools/loadtest/results/` (una vez exista Parte B) — logs de wrk, snapshots
  de `/metrics` y CSV de RSS. Si algún artefacto es demasiado grande para el
  repo, se resume aquí y el crudo se adjunta como artefacto de CI/release en
  vez de commitearse.
- Resúmenes legibles (no el log crudo) van en este documento y en
  [`docs/ANALYSIS.md`](ANALYSIS.md), que ya es el punto de referencia para
  "cómo verificar y endurecer una app Oreshnek".

---

## Qué desbloquea el indicador de producción

La evaluación original identificó **tres bloqueantes** para considerar el
framework listo para producción. Este documento cierra el diseño de dos; el
indicador solo pasa a "listo" cuando los tres tienen evidencia archivada y
reproducible, no cuando el código que los ejercita simplemente compila:

| # | Bloqueante | Qué lo desbloquea | Estado |
|---|---|---|---|
| 1 | Claim de "alto rendimiento" no medido | Parte B implementada + línea base documentada (B.4) con evidencia archivada (logs de wrk + snapshots de `/metrics` + CSV de RSS) y los 4 criterios cualitativos de B.4 cumplidos | ⬜ Parte B ni siquiera implementada |
| 2 | `HttpParser` no fuzzeado | Parte A implementada (✅) + al menos una campaña ≥5 min sin crash/leak/UB con su log archivado en `tests/fuzz/campaigns/` (A.5.2) | 🔄 harness listo, falta la campaña archivada |
| 3 | Sin CI | Pipeline (`.github/workflows/` o equivalente) que corra en cada PR: `ctest` bajo ASan/UBSan y TSan, `fuzz_replay_test`, y un job corto de fuzzing (~120 s); idealmente una corrida nightly de fuzz largo + soak de carga | ⬜ no existe |

Mientras cualquiera de las tres filas esté en ⬜/🔄, el claim de "listo para
producción" en el README y en `docs/ROADMAP.md` debe seguir matizado con "Fases
0–6 completadas" (que es exacto) en vez de "producción validada" (que no lo
es todavía).

---

## Orden de implementación propuesto

1. ✅ **Parte A (fuzz)** — harness hecho; queda ejecutar y archivar la campaña
   larga (A.5.2) para cerrar el bloqueante #2.
2. **Parte B (carga)**: andamiaje `tools/loadtest/` + línea base documentada
   — siguiente paso, cierra el bloqueante #1.
3. Integración de ambos en el pipeline de CI (job de fuzz corto por PR + soak/
   fuzz largo nightly), cerrando el bloqueante #3.

Todo el trabajo en una rama de feature; cada commit compila y mantiene verde el
gate de sanitizers y `ctest`.
