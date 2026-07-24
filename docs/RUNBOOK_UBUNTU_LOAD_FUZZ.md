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
centra solo en Ubuntu/Debian para no mezclar rutas.

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

### 3.3 Campaña corta (smoke test, ~1 min)

Confirma que el binario corre antes de invertir 5+ minutos:

```bash
./build-fuzz/fuzz_http_parser -max_total_time=60 tests/fuzz/corpus
```

Salida esperada: termina sola tras 60 s, sin ningún reporte de
AddressSanitizer/UndefinedBehaviorSanitizer ni `SUMMARY: libFuzzer: deadly signal`.

### 3.4 Campaña larga, con evidencia archivada (cierra el criterio A.5.2)

```bash
mkdir -p tests/fuzz/campaigns
./build-fuzz/fuzz_http_parser -max_total_time=300 -print_final_stats=1 \
    tests/fuzz/corpus \
    2>&1 | tee "tests/fuzz/campaigns/$(date +%Y%m%d)-$(git rev-parse --short HEAD).log"
```

`-max_total_time=300` = 5 minutos (el mínimo del criterio A.5.2); sube el
valor para una campaña más larga (p.ej. `-max_total_time=1800` para 30 min).
Al terminar, revisa el resumen final:

```bash
tail -25 "tests/fuzz/campaigns/$(date +%Y%m%d)-$(git rev-parse --short HEAD).log"
```

Busca `stat::number_of_executed_units`, `stat::average_exec_per_sec` y
confirma que no hay ningún reporte de sanitizer en el log completo:

```bash
grep -E 'ERROR|SUMMARY: (Address|UndefinedBehavior)Sanitizer|deadly signal' \
    "tests/fuzz/campaigns/$(date +%Y%m%d)-$(git rev-parse --short HEAD).log" || echo "limpio"
```

Si imprime "limpio", la corrida cierra A.5.2; el `.log` es la evidencia
(commitéalo si quieres dejar registro en el repo — ver Paso 5).

### 3.5 Si aparece un crash

libFuzzer deja un fichero `crash-<hash>` en el directorio desde donde
corriste el binario (`tests/fuzz/corpus/` o el `cwd`). Cópialo a
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

`tools/loadtest/` todavía no tiene el andamiaje automatizado descrito en la
Parte B de `LOAD_AND_FUZZ_PLAN.md` (`run.sh`, scripts Lua). Mientras tanto,
esto es la versión manual de los mismos escenarios, con la misma convención de
dónde guardar la evidencia.

### 4.1 Compila el servidor de demo

```bash
cd oreshnek   # si no estás ya en la raíz del repo
cmake -B build -DORESHNEK_WITH_SQLITE=ON   # ya existe si hiciste el Paso 2
cmake --build build --target 07_config_server -j"$(nproc)"
```

### 4.2 Prepara los directorios y arranca el servidor

Usa la config de carga incluida en el repo
(`config/oreshnek.loadtest.json`): rate limiting deshabilitado (para medir el
techo real, no el limitador) y `/metrics` habilitado. **No es una config de
producción** — solo para esta prueba.

```bash
mkdir -p static uploads tools/loadtest/results
echo "hello from oreshnek load test" > static/sample.txt

./build/examples/07_config_server config/oreshnek.loadtest.json \
    > /tmp/oreshnek-loadtest.log 2>&1 &
SERVER_PID=$!
echo "servidor arrancado, PID=$SERVER_PID"

# Espera a que responda /health antes de lanzar wrk (hasta 30 intentos = ~30s)
for i in $(seq 1 30); do
    curl -sf http://127.0.0.1:8080/health >/dev/null && break
    sleep 1
done
curl -s http://127.0.0.1:8080/health && echo " <- servidor listo"
```

Si el `curl` final no imprime `{"status":"ok"}`, revisa
`/tmp/oreshnek-loadtest.log` antes de seguir (ver Troubleshooting).

### 4.3 Escenario 1 — throughput/latencia a concurrencia creciente

Ajusta `-t` (hilos de wrk) al número de vCPUs del VPS; no tiene sentido pasar
de `nproc`.

```bash
WRK_THREADS=$(nproc)
for c in 50 200 1000; do
    wrk -t"$WRK_THREADS" -c"$c" -d30s http://127.0.0.1:8080/ \
        2>&1 | tee "tools/loadtest/results/throughput-c${c}-$(date +%Y%m%d).log"
done
```

Qué mirar en cada log: `Requests/sec`, la tabla de latencias `50%/90%/99%`,
y que `Socket errors`/`Non-2xx or 3xx responses` sea 0 (criterio B.4.1).

### 4.4 Escenario 2 — fichero estático (ruta `sendfile`)

```bash
wrk -t"$WRK_THREADS" -c200 -d30s http://127.0.0.1:8080/static/sample.txt \
    2>&1 | tee "tools/loadtest/results/static-$(date +%Y%m%d).log"
```

### 4.5 Escenario 3 — soak (estabilidad de memoria)

Corre en paralelo: `wrk` sostenido + muestreo de RSS + snapshot de
`/metrics` antes/después. 20 minutos de ejemplo; el plan pide 10–30 min.

```bash
curl -s http://127.0.0.1:8080/metrics > tools/loadtest/results/metrics-antes.prom

( while kill -0 "$SERVER_PID" 2>/dev/null; do
      printf '%s,%s\n' "$(date +%s)" "$(ps -o rss= -p "$SERVER_PID")" \
          >> tools/loadtest/results/soak-rss.csv
      sleep 30
  done ) &
RSS_SAMPLER_PID=$!

wrk -t"$WRK_THREADS" -c200 -d20m http://127.0.0.1:8080/ \
    2>&1 | tee "tools/loadtest/results/soak-$(date +%Y%m%d).log"

kill "$RSS_SAMPLER_PID" 2>/dev/null
curl -s http://127.0.0.1:8080/metrics > tools/loadtest/results/metrics-despues.prom
diff tools/loadtest/results/metrics-antes.prom tools/loadtest/results/metrics-despues.prom
```

Qué mirar (criterio B.4.2): `tools/loadtest/results/soak-rss.csv` no debe
tener una pendiente positiva sostenida (columna 2, en KB) — oscilación
acotada sí, crecimiento monótono no. El `diff` de `/metrics` debe mostrar
`requests_total` creciendo acorde a lo que envió `wrk`, y
`workers_in_flight` de vuelta a su valor base (sin fuga de handlers).

### 4.6 (Opcional) Escenario de saturación — confirma el load shedding

Para validar el criterio B.4.3 (503 bajo sobrecarga deliberada en vez de
estancarse), reinicia el servidor con `rate_limit.enabled: true` y un
`requests_per_second` bajo en `config/oreshnek.loadtest.json` (o una copia),
y lanza concurrencia muy por encima de ese límite; deberías ver `503` en la
sección `Non-2xx or 3xx responses` de wrk (esperado y correcto aquí, a
diferencia del Escenario 1) y `load_shed_total` creciendo en `/metrics`.

### 4.7 Detén el servidor (apagado graceful)

```bash
kill -TERM "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null
echo "servidor detenido"
```

`SIGTERM` dispara `request_stop()`: el servidor deja de aceptar conexiones
nuevas, drena las peticiones en vuelo (hasta `shutdown_grace_sec=10` en la
config) y sale solo — no hace falta `kill -9`.

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
- **El `curl http://127.0.0.1:8080/health` del Paso 4.2 nunca responde**:
  revisa `/tmp/oreshnek-loadtest.log` (errores de arranque van ahí); causa
  típica es el puerto 8080 ya ocupado por una corrida anterior —
  `ss -tlnp | grep 8080` para ver quién lo tiene y `kill` ese proceso, o
  `pkill -f 07_config_server` si es un `07_config_server` colgado de antes.
- **Probar desde otra máquina, no desde el propio VPS**: por defecto este
  runbook asume que `wrk` corre en el mismo VPS contra `127.0.0.1` (sin tocar
  el firewall). Si de verdad necesitas pegarle desde fuera, abre el puerto
  con cuidado y ciérralo después: `sudo ufw allow from <IP-del-cliente> to
  any port 8080 proto tcp`, y `sudo ufw delete allow from <IP-del-cliente> to
  any port 8080 proto tcp` al terminar. No dejes el puerto abierto a
  `0.0.0.0/0` en un VPS con datos reales.
- **VPS con pocos núcleos/RAM y el soak se ve raro (RSS con picos)**: revisa
  `dmesg | tail` por si el OOM killer intervino; baja la concurrencia
  (`-c200` → `-c50`) y el `thread_pool_size` de
  `config/oreshnek.loadtest.json` antes de repetir.
