# Runbook: fuzzing y prueba de carga en un VPS Ubuntu/Debian

Guía autocontenida, paso a paso, para compilar y ejecutar **ambas** pruebas de
[`docs/LOAD_AND_FUZZ_PLAN.md`](LOAD_AND_FUZZ_PLAN.md) —fuzzing del
`HttpParser` y carga con `wrk`— en un servidor **Ubuntu/Debian real** (probado
para Ubuntu 22.04/24.04 y Debian 12; el resto de la familia Debian debería
comportarse igual). Todos los bloques de comandos son copiables tal cual, en
orden, desde una sesión SSH al VPS. Requiere: usuario con `sudo`, git, y
salida a internet para `apt`/`git clone`.

Si necesitas los comandos equivalentes para macOS, Fedora/RHEL/CentOS Stream o
Arch, están en [`docs/DEPENDENCIES.md`](DEPENDENCIES.md); este documento se
centra solo en Ubuntu/Debian para no mezclar rutas. Si **solo** te interesa la
prueba de carga (no el fuzzing) y quieres la versión que cubre macOS y Linux
por igual en un solo lugar, ve directo a
[`tools/loadtest/README.md`](../tools/loadtest/README.md) — el Paso 4 de aquí
abajo remite ahí de todos modos.

---

## Paso 0 — Qué vas a obtener al final

- Un log archivado de una campaña de fuzzing (`tests/fuzz/campaigns/*.log`)
  que cierra el criterio A.5.2 de `LOAD_AND_FUZZ_PLAN.md`.
- Logs de `wrk` + snapshots de `/metrics` + una serie de RSS en
  `tools/loadtest/results/` que sirven de línea base para el criterio B.4.

Ninguno de los dos pasos requiere el LLVM de Homebrew ni nada específico de
Mac: en Linux, el `clang` del sistema ya trae el runtime de libFuzzer (se
verifica en el Paso 2).

---

## Paso 1 — Dependencias del sistema

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake git \
    clang \
    libssl-dev zlib1g-dev libsqlite3-dev \
    wrk
```

Verifica que todo quedó instalado:

```bash
cmake --version   # >= 3.16
clang++ --version
wrk --version || wrk 2>&1 | head -1   # wrk no tiene --version; con -h basta
```

Si `apt-get install wrk` falla (paquete no disponible en tu versión de
Ubuntu/Debian), compílalo desde fuente — funciona igual en cualquier Debian:

```bash
sudo apt-get install -y libssl-dev   # si no quedó de arriba
git clone https://github.com/wg/wrk.git /tmp/wrk-src
cd /tmp/wrk-src && make -j"$(nproc)"
sudo cp wrk /usr/local/bin/
cd - >/dev/null
wrk -h | head -1   # confirma que quedó en el PATH
```

---

## Paso 2 — Clonar y compilar (build normal, sanity check)

Si ya tienes el repo clonado en el VPS, sáltate el `git clone` y usa tu
checkout existente.

```bash
git clone https://github.com/coder-pe/oreshnek.git
cd oreshnek

cmake -B build -DORESHNEK_WITH_SQLITE=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Si `ctest` sale en verde (todos los targets `Passed`), el toolchain del VPS
compila y corre el framework correctamente; ya puedes seguir a fuzzing y
carga.

---

## Paso 3 — Fuzzing del `HttpParser` (libFuzzer)

### 3.1 Verifica el runtime de libFuzzer antes de compilar

```bash
find / -name 'libclang_rt.fuzzer*' 2>/dev/null
```

Debe listar al menos un fichero (p.ej.
`/usr/lib/llvm-18/lib/clang/18/lib/linux/libclang_rt.fuzzer-x86_64.a`). En
Ubuntu/Debian recientes viene incluido con el paquete `clang` del Paso 1; si
la búsqueda no devuelve nada, instala el `compiler-rt` de tu versión de clang:

```bash
sudo apt-get install -y "libclang-rt-$(clang --version | grep -oP '(?<=version )\d+')-dev"
```

y repite el `find` — debe aparecer algo antes de seguir.

### 3.2 Compila el target de fuzzing

```bash
cmake -B build-fuzz -DORESHNEK_FUZZ=ON -DORESHNEK_WITH_SQLITE=ON \
    -DORESHNEK_BUILD_TESTS=OFF -DORESHNEK_BUILD_EXAMPLES=OFF \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz --target fuzz_http_parser -j"$(nproc)"
```

(`-DORESHNEK_BUILD_TESTS=OFF -DORESHNEK_BUILD_EXAMPLES=OFF` porque este árbol
de build solo existe para el target de fuzzing; evita compilar de más en un
VPS con pocos núcleos.)

**Importante — dos directorios, no uno.** libFuzzer *escribe* en el primer
directorio que le pasas (cada input nuevo que aumenta cobertura queda ahí) y
solo *lee* de los siguientes. Si le pasas `tests/fuzz/corpus` como único
argumento, tras una campaña larga acabas con miles de ficheros nuevos
mezclados con las semillas curadas y versionadas — pasó exactamente eso en la
primera corrida real de este runbook (2546 ficheros nuevos, 10 MB). Para
evitarlo, usa un directorio de scratch (gitignored) como destino de escritura
y pasa el corpus semilla como entrada de solo lectura:

```bash
mkdir -p tests/fuzz/corpus_growth   # ya está en .gitignore
```

### 3.3 Campaña corta (smoke test, ~1 min)

Confirma que el binario corre antes de invertir 5+ minutos:

```bash
./build-fuzz/fuzz_http_parser -max_total_time=60 \
    tests/fuzz/corpus_growth tests/fuzz/corpus
```

Salida esperada: termina sola tras 60 s, sin ningún reporte de
AddressSanitizer/UndefinedBehaviorSanitizer ni `SUMMARY: libFuzzer: deadly signal`.

### 3.4 Campaña larga, con evidencia archivada (cierra el criterio A.5.2)

Fija el nombre del log en una variable **antes** de arrancar — si corres más
de una campaña el mismo día sobre el mismo commit (p.ej. una corta de prueba
y luego la larga), un `date +%Y%m%d` sin hora se repite y el `tee` de la
segunda corrida sobreescribe el log de la primera sin avisar. Con hora y
minuto en el nombre no colisionan:

```bash
mkdir -p tests/fuzz/campaigns
LOG="tests/fuzz/campaigns/$(date +%Y%m%d-%H%M%S)-$(git rev-parse --short HEAD).log"
./build-fuzz/fuzz_http_parser -max_total_time=300 -print_final_stats=1 \
    tests/fuzz/corpus_growth tests/fuzz/corpus \
    2>&1 | tee "$LOG"
```

`-max_total_time=300` = 5 minutos (el mínimo del criterio A.5.2); sube el
valor para una campaña más larga (p.ej. `-max_total_time=1800` para 30 min;
en ese caso usa una `$LOG` nueva para no pisar la del smoke test/la corta).
Al terminar, revisa el resumen final:

```bash
tail -25 "$LOG"
```

Busca `stat::number_of_executed_units`, `stat::average_exec_per_sec` y
confirma que no hay ningún reporte de sanitizer en el log completo:

```bash
grep -E 'ERROR|SUMMARY: (Address|UndefinedBehavior)Sanitizer|deadly signal' "$LOG" || echo "limpio"
```

Si imprime "limpio", la corrida cierra A.5.2; el `.log` es la evidencia —
commitéalo (ver Paso 5). El directorio `tests/fuzz/corpus_growth/` (los
inputs nuevos que encontró el fuzzer) es local y gitignored: revísalo si
quieres, pero no hace falta commitearlo para que la evidencia cuente — el log
es lo que se archiva.

### 3.5 Si aparece un crash

libFuzzer deja un fichero `crash-<hash>` en el directorio desde donde
corriste el binario (el `cwd`, no `tests/fuzz/corpus_growth/`). Cópialo a
`tests/fuzz/regressions/` y confirma que el replay determinista (sin
fuzzer, cualquier compilador) lo reproduce:

```bash
cp crash-<hash> tests/fuzz/regressions/
cmake --build build --target fuzz_replay_test -j"$(nproc)"
ctest --test-dir build -R fuzz_replay --output-on-failure
```

Después de corregir el parser, vuelve a correr el replay para confirmar que
queda en verde. El log de la campaña que encontró el crash **no se borra ni
se sobreescribe** — es el registro de cuándo y cómo se halló.

---

## Paso 4 — Prueba de carga (wrk)

`tools/loadtest/run.sh` automatiza los 4 escenarios de la Parte B (arranca el
servidor, espera a `/health`, corre `wrk`, apaga el servidor con `SIGTERM` y
archiva todo). Detalle completo de flags y cómo interpretar los resultados:
[`tools/loadtest/README.md`](../tools/loadtest/README.md); aquí solo lo
mínimo para correrlo en el VPS.

### 4.1 Compila el servidor de demo

```bash
cd oreshnek   # si no estás ya en la raíz del repo
cmake -B build -DORESHNEK_WITH_SQLITE=ON   # ya existe si hiciste el Paso 2
cmake --build build --target 07_config_server -j"$(nproc)"
```

### 4.2 Escenarios 1–3 (throughput, pipelining, estático+Range)

```bash
tools/loadtest/run.sh
```

Arranca `examples/07_config_server` con
`config/oreshnek.loadtest.json` (rate limiting deshabilitado a propósito,
para medir el techo real; `/metrics` habilitado — **no es una config de
producción**), corre los tres escenarios (~2–3 min en total con la duración
por defecto de 30 s cada uno) y archiva todo en
`tools/loadtest/results/<timestamp>/summary.md`. Si el servidor no levanta,
el script te apunta a `results/<timestamp>/server.log`.

### 4.3 Escenario 4 — soak (estabilidad de memoria, 10–30 min)

```bash
tools/loadtest/run.sh --soak --soak-duration 20m
```

Añade a los resultados `metrics-antes.prom`/`metrics-despues.prom` (snapshot
de `/metrics`) y `soak-rss.csv` (RSS muestreado cada 30 s). Qué mirar
(criterio B.4.2): `soak-rss.csv` no debe tener una pendiente positiva
sostenida (columna 2, en KB) — oscilación acotada sí, crecimiento monótono
no; el diff de `/metrics` debe mostrar `requests_total` creciendo acorde a lo
que envió `wrk` y `workers_in_flight` de vuelta a su valor base.

### 4.4 Escenario de saturación — confirma el load shedding

```bash
tools/loadtest/run.sh --saturation
```

Valida el criterio B.4.3 (503 bajo sobrecarga deliberada en vez de
estancarse). Usa automáticamente
[`config/oreshnek.loadtest-saturation.json`](../config/oreshnek.loadtest-saturation.json)
(`max_concurrent_handlers: 5`, deliberadamente bajo) y una concurrencia de
500 por defecto — bien por encima del tope, para forzar el shedding.
**Importante**: esto es `max_concurrent_handlers`, no `rate_limit` — el
rate limiter responde `429 Too Many Requests` (otro mecanismo, para abuso
por IP); el *load shedding* de este escenario responde `503 Service
Unavailable` con `Retry-After`. `run.sh` te dice en `summary.md` exactamente
qué mirar: `Non-2xx or 3xx responses` > 0 (son los 503 esperados aquí, a
diferencia de los Escenarios 1–3), y en el diff de `/metrics`,
`load_shed_total` creciendo junto con `responses_total{class="5xx"}` (no
`class="4xx"`, que sería rate_limit).

El servidor se detiene solo al terminar cada corrida (`SIGTERM` →
`request_stop()`: deja de aceptar conexiones nuevas, drena las peticiones en
vuelo hasta `shutdown_grace_sec=10` de la config, y sale) — no hace falta
matarlo a mano.

---

## Paso 5 — Dónde queda la evidencia

| Qué | Dónde |
|---|---|
| Logs de campañas de fuzzing | `tests/fuzz/campaigns/*.log` (git-tracked; ver [`tests/fuzz/campaigns/README.md`](../tests/fuzz/campaigns/README.md)) |
| Logs de `wrk`, snapshots de `/metrics`, CSV de RSS | `tools/loadtest/results/` |
| Resumen legible (tabla de línea base) | añádelo a la sección B.4 de [`LOAD_AND_FUZZ_PLAN.md`](LOAD_AND_FUZZ_PLAN.md) enlazando al log crudo correspondiente |

`tools/loadtest/results/` no está en `.gitignore`; decide caso por caso si
commiteas los logs (son texto plano y pequeños) o los subes como artefacto de
CI/release si prefieres no engordar el repo con corridas frecuentes.

---

## Troubleshooting

- **`ctest`/`cmake` no encuentra SQLite3**: falta `libsqlite3-dev` del Paso 1
  (`sudo apt-get install -y libsqlite3-dev`).
- **`find` no encuentra `libclang_rt.fuzzer*`**: el paquete `clang` de tu
  distro no trae `compiler-rt` con soporte de fuzzer; instala
  `libclang-rt-<versión>-dev` como en el Paso 3.1. Confirma la versión con
  `clang --version` antes de adivinar el nombre exacto del paquete
  (`apt-cache search libclang-rt` ayuda si el nombre no calza).
- **`wrk: command not found`**: el paquete no está disponible en tu
  Ubuntu/Debian; usa el fallback de compilar desde fuente del Paso 1.
- **`tools/loadtest/run.sh` sale con "el servidor no respondió /health en
  30s"**: revisa `tools/loadtest/results/<timestamp>/server.log` (errores de
  arranque van ahí); causa típica es el puerto 9090 ya ocupado por una
  corrida anterior — `ss -tlnp | grep 9090` para ver quién lo tiene y `kill`
  ese proceso, o `pkill -f 07_config_server` si es un `07_config_server`
  colgado de antes.
- **Probar desde otra máquina, no desde el propio VPS**: por defecto este
  runbook asume que `wrk` corre en el mismo VPS contra `127.0.0.1` (sin tocar
  el firewall). Si de verdad necesitas pegarle desde fuera, abre el puerto
  con cuidado y ciérralo después: `sudo ufw allow from <IP-del-cliente> to
  any port 9090 proto tcp`, y `sudo ufw delete allow from <IP-del-cliente> to
  any port 9090 proto tcp` al terminar. No dejes el puerto abierto a
  `0.0.0.0/0` en un VPS con datos reales.
- **`tools/loadtest/run.sh --skip-server --url http://<ip-vps>:9090` dice
  "`/health` no responde"**: casi siempre son **dos firewalls, no uno**. El
  `ufw allow` de arriba abre el del sistema operativo, pero la mayoría de
  proveedores VPS (DigitalOcean, Vultr, Hetzner, AWS Security Groups, GCP
  Firewall, Azure NSG...) tienen **otro firewall a nivel de red**, gestionado
  desde su panel web, que bloquea el tráfico antes de que llegue siquiera al
  `ufw` del VPS — hay que agregar la regla ahí también. `run.sh` imprime un
  checklist de diagnóstico (¿el proceso corre?, ¿responde en `127.0.0.1`
  dentro del VPS?, ¿`ss -tlnp` lo muestra escuchando?, ...) cuando esta
  comprobación falla; síguelo en orden antes de asumir que es un bug del
  servidor.
- **VPS con pocos núcleos/RAM y el soak se ve raro (RSS con picos)**: revisa
  `dmesg | tail` por si el OOM killer intervino; baja la concurrencia
  (`-c200` → `-c50`) y el `thread_pool_size` de
  `config/oreshnek.loadtest.json` antes de repetir.
- **El servidor imprime `Too many open files` bajo carga alta (c=1000), y el
  cliente reporta `Socket errors: ... timeout N` (sin `connect`/`read`/`write`,
  solo `timeout`)**: es el `ulimit -n` del proceso servidor, no un bug ni un
  problema de red. `tools/loadtest/run.sh` sube este límite automáticamente
  cuando **él mismo** arranca el servidor (Paso 4.2), pero si lo arrancaste a
  mano —típico cuando el servidor y el cliente están en máquinas distintas—
  hereda el límite por defecto de esa sesión de shell (a menudo 1024), que se
  agota alrededor de c=1000. Antes de arrancarlo a mano:
  ```bash
  ulimit -n 65536
  ./build/examples/07_config_server config/oreshnek.loadtest.json
  ```
  Para que quede permanente entre sesiones, agrega en
  `/etc/security/limits.conf`: `<usuario> soft nofile 65536` y
  `<usuario> hard nofile 65536` (o `LimitNOFILE=65536` si el servidor corre
  como servicio `systemd`).
