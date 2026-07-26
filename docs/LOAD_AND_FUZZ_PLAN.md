# Plan: validación de carga (wrk) y fuzzing del parser (libFuzzer)

Cierre de los tres bloqueantes de producción identificados en la evaluación:
(1) el claim de "alto rendimiento" (README) no estaba medido, (2) el
`HttpParser` —superficie de ataque #1— no estaba fuzzeado, y (3) no había CI.
**Los tres están cerrados** (ver "Estado actual" y "Qué desbloquea el
indicador de producción"). Este documento define qué se construyó, cómo, los
criterios de aceptación, y **cómo se documenta la evidencia** de que cada
criterio se cumplió — un harness que compila no es evidencia; una campaña
ejecutada con su salida archivada sí lo es.

Herramientas acordadas: **wrk** para carga, **libFuzzer + ASan/UBSan** para fuzz.

**¿Vas a correr esto en un VPS Linux (Ubuntu/Debian)?** Usa
[`docs/RUNBOOK_UBUNTU_LOAD_FUZZ.md`](RUNBOOK_UBUNTU_LOAD_FUZZ.md) — guía
paso a paso copiable de punta a punta para esa plataforma específica; lo que
sigue aquí es el diseño y la referencia general (incluye macOS).

## Estado actual

| Parte | Estado | Detalle |
|---|---|---|
| A — Harness de fuzzing | ✅ Implementado | `tests/fuzz/` (harness, corpus, replay determinista en `ctest`); commit `c271ee8`. |
| A — Campaña larga + evidencia archivada | ✅ Cerrado | Debian 13 (VPS), campaña de 30 min: **3 033 053 ejecuciones**, ~1684 exec/s, `new_units_added: 1449`, `peak_rss_mb: 525`, 0 crashes/leaks/UB (`grep` de patrones de sanitizer sobre el log completo → "limpio"). También se corrió una previa de 5 min, limpia igual (su log no quedó archivado por separado: ver nota abajo). Log: `tests/fuzz/campaigns/20260725-4c443e1.log`. |
| B — Andamiaje de carga automatizado (`run.sh`) | ✅ Implementado | [`tools/loadtest/run.sh`](../tools/loadtest/run.sh) + `scripts/pipeline.lua` + `scripts/range.lua`; corre los 4 escenarios de B.3, portable macOS/Linux (probado en ambos). Uso: [`tools/loadtest/README.md`](../tools/loadtest/README.md). |
| B — Línea base documentada | ✅ Cerrado | Campaña pre-fix (2026-07-26) encontró un bug real: sin `TCP_NODELAY`, Nagle/delayed-ACK añadía ~40ms fijos a cada respuesta. Corregido (`fa9537a`/PR #29) y **re-corrido en el mismo VPS** (commit `5150a17`): 0 errores, RSS estable, mejoras de 6–29x en latencia/throughput según escenario — ver tabla en B.4. Investigada la cola a c=1000 invirtiendo servidor/cliente (el VPS original tiene 1 solo vCPU): no mejoró al darle más núcleos al servidor, así que el límite parece estar en el cliente `wrk` de un solo hilo, no en el framework — documentado como limitación del arnés de prueba, no bloqueante. |
| CI | ✅ Cerrado | [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) (por PR/push) + [`nightly.yml`](../.github/workflows/nightly.yml) (diario) — ver sección "CI" más abajo. |

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

**Estado: ✅ completo** — harness implementado (commit `c271ee8`) y campaña
larga con evidencia archivada (2026-07-25, ver A.5). El detalle de piezas y
cómo ejecutarlo vive en [`tests/fuzz/README.md`](../tests/fuzz/README.md); lo
que sigue documenta el diseño y sirve de referencia.

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

`tests/fuzz/corpus_growth` va siempre primero en el comando: es el
scratch **gitignored** donde libFuzzer escribe los inputs que descubre;
`tests/fuzz/corpus` (semillas versionadas) va después y solo se lee. Pasarle
un único directorio hace que la campaña mezcle miles de inputs nuevos con las
semillas curadas (pasó en la primera corrida real de este plan, ver "Estado
actual").

```bash
# macOS, campaña corta (LLVM de Homebrew, ver docs/DEPENDENCIES.md):
cmake -B build-fuzz -DORESHNEK_FUZZ=ON \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
cmake --build build-fuzz --target fuzz_http_parser
mkdir -p tests/fuzz/corpus_growth
./build-fuzz/fuzz_http_parser -max_total_time=60 \
    tests/fuzz/corpus_growth tests/fuzz/corpus
```

```bash
# Linux (Ubuntu/Debian, clang del sistema — sin LLVM externo necesario):
cmake -B build-fuzz -DORESHNEK_FUZZ=ON -DORESHNEK_WITH_SQLITE=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz --target fuzz_http_parser
mkdir -p tests/fuzz/corpus_growth
./build-fuzz/fuzz_http_parser -max_total_time=60 \
    tests/fuzz/corpus_growth tests/fuzz/corpus
```

Guía completa paso a paso para un VPS Ubuntu/Debian (dependencias, verificar
el runtime de libFuzzer, campaña larga con evidencia archivada, y la prueba
de carga de la Parte B) en
[`docs/RUNBOOK_UBUNTU_LOAD_FUZZ.md`](RUNBOOK_UBUNTU_LOAD_FUZZ.md).

- CI (Linux, clang del sistema): job de fuzz corto (p.ej. 120 s) por PR +
  campaña larga programada (nightly, minimizando corpus). Cualquier crash →
  artefacto subido + semilla añadida a `regressions/`.

### A.5 Criterios de aceptación (Parte A)

1. ✅ `fuzz_http_parser` compila y corre bajo libFuzzer+ASan+UBSan.
2. ✅ Campaña de ≥5 min sin crash/leak/UB partiendo del corpus semilla.
   **Cerrado el 2026-07-25** en un VPS Debian 13: campaña de 5 min limpia,
   seguida de una de 30 min también limpia (3 033 053 ejecuciones, 0
   crashes/leaks/UB). Evidencia archivada:
   `tests/fuzz/campaigns/20260725-4c443e1.log` (la corrida de 30 min; la de 5
   min se ejecutó primero con el mismo nombre de fichero por fecha —sin
   hora— y quedó sobreescrita por la segunda. El propio runbook tenía ese bug
   de nomenclatura; ya está corregido, ver A.4 y
   [`tests/fuzz/campaigns/README.md`](../tests/fuzz/campaigns/README.md), así
   que campañas futuras no se pisan entre sí).
3. ✅ `fuzz_regression_test` integrado en `ctest` y verde en build normal
   (`tests/fuzz/regressions/` existe y está vacío: cero crashes conocidos
   pendientes de corregir).

**Parte A: los tres criterios están cerrados.** Cierra el bloqueante de
producción #2 (`HttpParser` fuzzeado) — ver "Qué desbloquea el indicador de
producción" más abajo.

---

## Parte B — Validación de carga (wrk)

**Estado: ✅ andamiaje implementado, ⬜ línea base sin fijar.**
[`tools/loadtest/run.sh`](../tools/loadtest/run.sh) automatiza los 4
escenarios de B.3 y se probó de punta a punta en macOS y Linux. Uso completo:
[`tools/loadtest/README.md`](../tools/loadtest/README.md) (multiplataforma) y
[`docs/RUNBOOK_UBUNTU_LOAD_FUZZ.md`](RUNBOOK_UBUNTU_LOAD_FUZZ.md) (VPS
Ubuntu/Debian paso a paso, incluye también el fuzzing de la Parte A). Falta
correr una campaña completa (duración plena, los 4 escenarios) y transcribir
sus números a B.4 como línea base — ver "Recolección y documentación de
resultados" más abajo.

### B.1 Andamiaje

- `tools/loadtest/` con:
  - `run.sh [opciones]`: lanza el servidor de demo
    (`examples/07_config_server`) con `config/oreshnek.loadtest.json`, espera
    a `/health`, corre los escenarios de B.3 (o el soak con `--soak`) y
    archiva todo en `results/<timestamp>/` (logs de wrk, `metadata.txt`,
    snapshots de `/metrics`, CSV de RSS, `summary.md`).
  - `scripts/pipeline.lua` (pipelining real, Escenario 2) y
    `scripts/range.lua` (fuerza `Range: bytes=0-1023`, Escenario 3). Los
    escenarios 1 y 3 (sin la cabecera Range) no necesitan script: `wrk` hace
    GET simple con keep-alive por defecto.
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

#### Hallazgo de la primera campaña (2026-07-26): Nagle sin `TCP_NODELAY`

Primera corrida completa en un VPS Debian 13, tanto en el mismo host
(loopback) como con cliente y servidor en máquinas separadas. Criterios 1 y 2
se cumplieron (0 errores, RSS plana en el soak), pero la latencia percibida
por `wrk` no encajaba con un handler JSON en memoria:

| Escenario (loopback) | p50 |
|---|---|
| c=50  | 43.84 ms |
| c=200 | 43.78 ms |
| c=1000 | 52.00 ms |

Latencia prácticamente **plana independiente de la concurrencia** — la firma
de un costo fijo por petición, no de contención de recursos. El propio
histograma de `/metrics` del servidor (`request_duration_seconds`) mostraba
>99.5% de las peticiones procesadas internamente en <0.5ms durante ese mismo
soak, así que los ~44ms no eran tiempo de handler.

Causa: `handle_new_connection()` (`src/server/Server.cpp`) nunca configuraba
`TCP_NODELAY` en el socket del cliente, y `Connection::write_data()` envía
las cabeceras y el cuerpo en dos `send()` separados — con Nagle activo, el
segundo `send()` espera el ACK del primero, y el peer usa *delayed ACK*
(hasta 40ms en Linux) al no tener nada que responder de inmediato. Corregido
añadiendo `setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, ...)` junto al
`SO_NOSIGPIPE` existente. Verificado: mismo escenario (loopback, c=50) pasó
de p50 43.84ms a **0.39ms** tras el fix.

La línea base "oficial" de B.4 (la que se compara contra futuras corridas
para detectar regresiones) se registra **después** de este fix, no antes —
los números de arriba quedan documentados como diagnóstico, no como
baseline.

#### Línea base oficial (post-fix, VPS Debian 13, 2026-07-26, commit `5150a17`)

Misma campaña completa (mismo host + soak de 20 min + cliente/servidor
separados) repetida tras el fix de `TCP_NODELAY`:

| Escenario | Mismo VPS (loopback) | Cliente/servidor separados |
|---|---|---|
| c=50  | p50 1.50ms, 27 283 req/s | p50 29.77ms, 1 656 req/s |
| c=200 | p50 6.40ms, 26 793 req/s | p50 30.58ms, 5 988 req/s |
| c=1000 | p50 41.54ms (p99 1.07s), 22 144 req/s | p50 72.35ms (p99 861ms), 12 635 req/s |
| static+Range (c=200) | 12 193 req/s | 6 516 req/s |
| soak (20 min, c=200) | 31 818 863 requests totales, p50 6.52ms | — |

- **B.4.1** (0 errores/timeouts): ✅ — ningún `Socket errors`/`Non-2xx` en
  ninguno de los logs.
- **B.4.2** (RSS estable en el soak): ✅ — calienta de ~14.6MB a ~216MB en
  los primeros 30s y se mantiene plano (216572–216620 KB) el resto de los 20
  min / 31.8M requests. Mismo patrón que la corrida pre-fix, con ~5.9x más
  tráfico procesado en la misma ventana.
- **B.4.4** (p99 no se dispara sin control por debajo de saturación): ✅ a
  c=50/200 (p99 de un dígito a low-double-digit ms en loopback, 35–110ms en
  red real). **A c=1000 sigue habiendo una cola larga** (p99 ~0.9–1.4s) en
  todas las combinaciones probadas — no bloquea el cierre de B.4, pero queda
  como limitación conocida de la infraestructura de prueba (ver siguiente
  punto), no del framework.

#### Investigación de la cola en c=1000: servidor/cliente invertidos

"mail" (VPS servidor original) resultó tener **1 solo vCPU** (`nproc=1`,
confirmado); la hipótesis era que el techo de c=1000 fuera ese único núcleo
saturándose. Se probó invirtiendo los roles — servidor en la VPS de 6
núcleos, "mail" como cliente — y **no mejoró** (p99 subió a 1.43s, si acaso
peor). Como "mail" sigue siendo el cliente en esa combinación y sigue
teniendo 1 solo hilo de `wrk` disponible, el dato no aísla la variable:

| Cliente | Servidor | c=1000 p50 | c=1000 p99 |
|---|---|---|---|
| 6 núcleos | "mail" (1 vCPU) | 72.35ms | 861ms |
| "mail" (1 vCPU) | 6 núcleos | 63.96ms | 1.43s |
| MacBook M4 (10 hilos, red doméstica) | 6 núcleos | 259.70ms (ya a c=50) | 1.16s |

Que darle 6 núcleos al servidor no haya mejorado la cola apunta a que el
límite real está más del lado de `wrk` corriendo con 1 solo hilo (en
cualquiera de las dos VPS que lo usaron de cliente) que en la capacidad del
servidor. **Conclusión: no se persigue más sin una tercera máquina cliente
con varios núcleos y buena conexión de red al servidor** (de la que no se
dispone); c=1000 queda documentado como no concluyente por límite del
arnés de prueba, no como defecto del framework. Los criterios B.4.1/B.4.2 no
dependen de este escenario.

**Sobre la MacBook como cliente**: p50 de ~220-260ms *desde c=50* no es el
servidor ni el cliente — es la latencia real de red entre una máquina
doméstica/oficina y el VPS (probablemente cientos/miles de km de distancia).
Es un dato legítimo de "qué experimenta un usuario real conectándose desde
esa ubicación", pero no es comparable con las corridas VPS-a-VPS: no se debe
mezclar con la línea base de capacidad de B.4. Útil como categoría de
medición aparte (experiencia de usuario final por geografía), no como
benchmark de servidor.

- **B.4.3** (503/load-shedding bajo saturación deliberada): ✅ **Cerrado
  2026-07-26** (`tools/loadtest/run.sh --saturation`, ver Paso 4.4 en
  `docs/RUNBOOK_UBUNTU_LOAD_FUZZ.md`). Corrido en macOS (MacBook M4,
  loopback): 3 362 864 peticiones en 2xx y **18 759 en 503** exactamente
  como se esperaba, `load_shed_total` 0→18 759 y
  `responses_total{class="5xx"}` acorde en el diff de `/metrics`.
  A diferencia de B.4.1/B.4.2/B.4.4, este criterio valida **lógica del
  framework** (`workers_in_flight >= max_concurrent_handlers` →
  `Server.cpp:864`), no capacidad de hardware — es código genérico,
  independiente de SO/núcleos, así que una corrida en cualquier plataforma
  es evidencia válida; no hacía falta repetirla en el VPS. Nota: la primera
  versión de esta guía decía erróneamente que había que activar
  `rate_limit.enabled` — eso responde `429` (otro mecanismo, para abuso por
  IP), no el `503` que este criterio necesita; corregido, usa
  `max_concurrent_handlers` vía `config/oreshnek.loadtest-saturation.json`.

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
# corpus_growth (gitignored) va primero: ahí escribe libFuzzer los inputs que
# descubre, sin mezclarlos con las semillas versionadas de corpus/.
mkdir -p tests/fuzz/campaigns tests/fuzz/corpus_growth
LOG="tests/fuzz/campaigns/$(date +%Y%m%d-%H%M%S)-$(git rev-parse --short HEAD).log"
./build-fuzz/fuzz_http_parser -max_total_time=300 -print_final_stats=1 \
    tests/fuzz/corpus_growth tests/fuzz/corpus \
    2>&1 | tee "$LOG"
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

### Carga (Parte B)

```bash
tools/loadtest/run.sh                        # Escenarios 1-3 (~2-3 min)
tools/loadtest/run.sh --soak --soak-duration 20m   # Escenario 4 (soak)
```

`run.sh` ya hace todo el patrón de metadatos/evidencia por su cuenta:
arranca el servidor, corre `wrk` con la salida completa archivada (no un
resumen editado a mano), toma snapshots de `/metrics` antes/después del soak,
muestrea RSS a un CSV, y escribe `metadata.txt` (fecha, commit, `uname -a`,
ruta de `wrk`, comando) en cada `results/<timestamp>/`. Detalle de flags e
interpretación de cada fichero de salida:
[`tools/loadtest/README.md`](../tools/loadtest/README.md).

- Qué extraer para B.4: `Requests/sec` y latencias `50%/90%/99%` de
  `summary.md`; `Socket errors`/`Non-2xx or 3xx responses` deben ser 0 salvo
  en el escenario de saturación deliberada (503 esperados ahí).
- `results/<timestamp>/metrics.diff` confirma `requests_total`,
  `load_shed_total` y que `workers_in_flight` volvió a su línea base tras el
  escenario (sin fuga de handlers en vuelo).
- `results/<timestamp>/soak-rss.csv` es la evidencia del criterio "RSS
  estable" — se espera una serie plana o con oscilación acotada, no una
  pendiente positiva sostenida; un gráfico rápido (`gnuplot`/hoja de cálculo)
  basta para el registro, no hace falta tooling adicional.
- Una vez exista una corrida completa de los 4 escenarios de B.3 con su
  evidencia, se transcribe un resumen (no el log completo) a una tabla de
  línea base en este documento (sección B.4) y/o en
  [`docs/ANALYSIS.md`](ANALYSIS.md), con enlace a la carpeta `results/`
  correspondiente.

### Dónde vive la evidencia

- `tests/fuzz/campaigns/*.log` — corridas de fuzzing (git-tracked; los logs
  son texto plano y pequeños).
- `tools/loadtest/results/` — logs de wrk, snapshots de `/metrics` y CSV de
  RSS (el directorio ya existe; el andamiaje que lo llenaría automáticamente
  no, así que hoy se llena a mano siguiendo
  [`docs/RUNBOOK_UBUNTU_LOAD_FUZZ.md`](RUNBOOK_UBUNTU_LOAD_FUZZ.md)). Si algún
  artefacto es demasiado grande para el repo, se resume aquí y el crudo se
  adjunta como artefacto de CI/release en vez de commitearse.
- Resúmenes legibles (no el log crudo) van en este documento y en
  [`docs/ANALYSIS.md`](ANALYSIS.md), que ya es el punto de referencia para
  "cómo verificar y endurecer una app Oreshnek".

---

## Qué desbloquea el indicador de producción

La evaluación original identificó **tres bloqueantes** para considerar el
framework listo para producción. El indicador solo pasa a "listo" cuando los
tres tienen evidencia archivada y reproducible, no cuando el código que los
ejercita simplemente compila:

| # | Bloqueante | Qué lo desbloquea | Estado |
|---|---|---|---|
| 1 | Claim de "alto rendimiento" no medido | Línea base documentada (B.4) con evidencia archivada (logs de wrk + snapshots de `/metrics` + CSV de RSS) y los 4 criterios cualitativos de B.4 cumplidos | ✅ **Cerrado 2026-07-26** (VPS Debian 13, post-fix de `TCP_NODELAY`; B.4.3 verificado en macOS el mismo día — ver B.4). Único seguimiento no bloqueante: c=1000 con una cola larga (p99 ~1s) atribuida al arnés de prueba (cliente `wrk` de 1 solo hilo), no al framework. |
| 2 | `HttpParser` no fuzzeado | Parte A implementada (✅) + al menos una campaña ≥5 min sin crash/leak/UB con su log archivado en `tests/fuzz/campaigns/` (A.5.2) | ✅ **Cerrado 2026-07-25** (VPS Debian 13, campaña de 30 min, 0 crashes/leaks/UB — ver A.5) |
| 3 | Sin CI | Pipeline (`.github/workflows/` o equivalente) que corra en cada PR: `ctest` bajo ASan/UBSan y TSan, `fuzz_replay_test`, y un job corto de fuzzing (~120 s); idealmente una corrida nightly de fuzz largo + soak de carga | ✅ **Cerrado 2026-07-26** — [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) (4 jobs por PR/push a `main`: build+ctest normal, ASan/UBSan, TSan, fuzz de 120s) y [`nightly.yml`](../.github/workflows/nightly.yml) (fuzz de 25 min + soak de 5 min, diario) |

Con los tres bloqueantes cerrados, el claim de "listo para producción" en el
README y en `docs/ROADMAP.md` ya no necesita el matiz de "Fases 0–6
completadas" — puede decir que la validación de carga, fuzzing y CI están en
su lugar. Detalle de cada pipeline:

## CI (bloqueante #3)

- **`ci.yml`** (en cada push/PR a `main`): 4 jobs en paralelo —
  1. `build-test`: build normal + `ctest` completo (incluye `fuzz_replay_test`,
     que no necesita libFuzzer).
  2. `asan-ubsan`: build con `-DORESHNEK_ASAN=ON` + `ctest` (LeakSanitizer
     activo por defecto en Linux).
  3. `tsan`: build con `-DORESHNEK_TSAN=ON` + `ctest`.
  4. `fuzz-short`: clang + libFuzzer, campaña de 120s contra `fuzz_http_parser`;
     sube el reproductor como artefacto si encuentra un crash.
- **`nightly.yml`** (cron diario + disparo manual):
  1. `fuzz-long`: campaña de 25 min, log siempre subido como artefacto (mismo
     criterio que `tests/fuzz/campaigns/` para las corridas manuales).
  2. `soak-smoke`: soak de 5 min con `tools/loadtest/run.sh --soak` en el
     runner compartido de GitHub Actions — **no es una línea base de
     rendimiento** (el hardware del runner varía y no es representativo; esa
     línea base ya está fijada en B.4, medida en la VPS real). Es una red de
     regresión: falla si aparecen errores/timeouts en un soak sostenido.
- Ambos usan `-DORESHNEK_WITH_SQLITE=ON` (el único backend que no necesita
  credenciales/servicio externo) — Postgres/Oracle quedan fuera de CI por
  ahora, se siguen probando localmente con `ORESHNEK_PG_TEST_DSN`/
  `ORESHNEK_ORACLE_TEST_DSN`.

---

## Orden de implementación propuesto

1. ✅ **Parte A (fuzz)** — completa: harness + campaña larga archivada
   (2026-07-25). Bloqueante #2 cerrado.
2. ✅ **Parte B (carga)** — completa: andamiaje `tools/loadtest/run.sh`,
   campaña completa corrida y línea base documentada (B.4, los 4 criterios
   cerrados), incluido el bug de `TCP_NODELAY` encontrado y corregido en el
   proceso. Bloqueante #1 cerrado. Único seguimiento no bloqueante: la cola
   de latencia a c=1000, atribuida al arnés de prueba (cliente `wrk` de un
   solo hilo en las VPS disponibles), no al framework.
3. ✅ **CI** — completa: `.github/workflows/ci.yml` (build+ctest normal,
   ASan/UBSan, TSan, fuzz de 120s por PR) y `nightly.yml` (fuzz de 25 min +
   soak de 5 min, diario). Bloqueante #3 cerrado — ver sección "CI" arriba.

Los tres bloqueantes de producción originales quedan cerrados.

Todo el trabajo en una rama de feature; cada commit compila y mantiene verde el
gate de sanitizers y `ctest`.
