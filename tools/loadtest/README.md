# tools/loadtest/ — prueba de carga (wrk)

Automatiza los 4 escenarios de la Parte B de
[`docs/LOAD_AND_FUZZ_PLAN.md`](../../docs/LOAD_AND_FUZZ_PLAN.md) contra
`examples/07_config_server`, y archiva la evidencia (logs de `wrk`, snapshots
de `/metrics`, CSV de RSS) en `tools/loadtest/results/<timestamp>/`.

**El mismo `run.sh` funciona igual en macOS y en cualquier distro Linux** —
detecta núcleos disponibles (`nproc` en Linux, `sysctl -n hw.ncpu` en macOS),
usa solo `bash`+`curl`+`wrk`+utilidades POSIX, y se probó tal cual en ambos.
Lo único distinto entre sistemas operativos es cómo instalas `wrk` y compilas
el servidor — una vez hecho eso, el comando es idéntico.

## 1. Preparar el sistema (una vez por máquina)

### macOS

```bash
brew install cmake openssl sqlite wrk
cmake -B build -DORESHNEK_WITH_SQLITE=ON
cmake --build build --target 07_config_server -j"$(sysctl -n hw.ncpu)"
```

### Linux (cualquier distro)

```bash
# Debian/Ubuntu
sudo apt-get install -y build-essential cmake libssl-dev libsqlite3-dev wrk
# Fedora/RHEL/CentOS Stream: sudo dnf groupinstall -y "Development Tools" && \
#   sudo dnf install -y cmake openssl-devel sqlite-devel   (wrk: compilar desde fuente, ver abajo)
# Arch: sudo pacman -S --needed base-devel cmake openssl sqlite   (wrk: AUR o fuente)

cmake -B build -DORESHNEK_WITH_SQLITE=ON
cmake --build build --target 07_config_server -j"$(nproc)"
```

Si tu distro no empaqueta `wrk` (Fedora/RHEL/CentOS/Arch), compílalo desde
fuente — es el mismo procedimiento en cualquier Linux:

```bash
git clone https://github.com/wg/wrk.git /tmp/wrk-src
cd /tmp/wrk-src && make -j"$(nproc)" && sudo cp wrk /usr/local/bin/ && cd -
```

Lista completa de dependencias (todas las distros, todos los backends de BD,
fuzzing incluido): [`docs/DEPENDENCIES.md`](../../docs/DEPENDENCIES.md). Guía
aún más detallada, paso a paso, para un VPS Ubuntu/Debian (incluye también el
fuzzing de la Parte A): [`docs/RUNBOOK_UBUNTU_LOAD_FUZZ.md`](../../docs/RUNBOOK_UBUNTU_LOAD_FUZZ.md).

## 2. Correr la prueba de carga

Con el binario ya compilado (Paso 1), desde la raíz del repo, en **cualquiera**
de los dos sistemas operativos:

```bash
tools/loadtest/run.sh
```

Esto arranca `examples/07_config_server` con
[`config/oreshnek.loadtest.json`](../../config/oreshnek.loadtest.json)
(rate limiting deshabilitado a propósito, para medir el techo real; `/metrics`
habilitado), espera a que `/health` responda, corre los Escenarios 1–3 de la
Parte B (throughput a concurrencia creciente, pipelining, estático+Range) y
apaga el servidor con `SIGTERM` (graceful) al terminar. Salida:

```
tools/loadtest/results/20260726-153000/
├── metadata.txt          # fecha, commit, uname, wrk, comando
├── server.log            # stdout/stderr del servidor
├── throughput-c50.log    # salida completa de wrk por concurrencia
├── throughput-c200.log
├── throughput-c1000.log
├── pipeline.log
├── static-range.log
└── summary.md            # Requests/sec, latencias y errores, todo junto
```

Para el escenario de soak (10–30 min, estabilidad de memoria — Escenario 4),
en vez de los tres anteriores:

```bash
tools/loadtest/run.sh --soak --soak-duration 20m
```

Produce además `metrics-antes.prom`/`metrics-despues.prom` (snapshot de
`/metrics`), `soak-rss.csv` (RSS muestreado cada 30s) y `metrics.diff`.

### Flags más usadas

| Flag | Para qué |
|---|---|
| `--url URL` | Servidor ya corriendo en otra IP/puerto (junto con `--skip-server`) |
| `--skip-server` | No arrancar el servidor — útil si `wrk` corre en una máquina cliente separada del servidor bajo prueba |
| `--concurrencies "50 200 1000"` | Niveles de concurrencia del Escenario 1 |
| `--duration 30s` | Duración de cada corrida corta (Escenarios 1–3) |
| `--binary PATH` / `--config PATH` | Si tu build/config no están en las rutas por defecto |

`tools/loadtest/run.sh --help` lista todas.

### ¿Corriendo esto en tu laptop cuenta como línea base?

Como sanity check del harness y del propio servidor, sí — confirma que
compila, levanta, sirve tráfico real y no tira errores. **Como línea base de
producción para la sección B.4 del plan, no**, por dos motivos:

1. **El objetivo de despliegue es Linux** (VPS/servidor; `epoll`, no
   `kqueue`), no macOS — el número que importa para producción es el que sale
   en el mismo tipo de máquina donde correrá el servicio real.
2. **`wrk` y el servidor comparten los mismos núcleos** cuando ambos corren
   en la misma laptop: compiten por CPU entre sí, lo que puede tanto
   deprimir el throughput medido como enmascarar dónde está el techo real del
   servidor (¿el cuello de botella es el servidor o es que `wrk` también
   necesita CPU?). Correr `wrk` desde una máquina separada (o al menos en un
   VPS con más núcleos que hilos de `wrk`) aísla esa variable.

Recomendado: usa una corrida en laptop para iterar rápido mientras ajustas
algo, y una corrida en el VPS (idealmente con `wrk` en una máquina cliente
aparte, ver abajo) para el número que efectivamente se transcribe a la tabla
de línea base de `LOAD_AND_FUZZ_PLAN.md`.

### Probar desde una máquina cliente separada del servidor

Si el servidor bajo prueba es el VPS y quieres generar la carga desde otra
máquina (para no compartir CPU entre servidor y `wrk`, más representativo de
producción):

```bash
# En el VPS (servidor) — el fichero de static/ hay que crearlo a mano, ver
# el aviso más abajo:
mkdir -p static && yes 'oreshnek load test line' | head -n 200 > static/sample.txt
./build/examples/07_config_server config/oreshnek.loadtest.json

# En la máquina cliente (con wrk instalado, ver Paso 1):
tools/loadtest/run.sh --skip-server --url http://<ip-del-vps>:8080
```

Abre el puerto solo para la IP del cliente y ciérralo al terminar (ver
Troubleshooting en `docs/RUNBOOK_UBUNTU_LOAD_FUZZ.md`); no lo dejes expuesto
a `0.0.0.0/0`.

**El Escenario 3 (estático+Range) necesita `static/sample.txt` en la máquina
que sirve** — `run.sh` solo lo crea automáticamente cuando arranca el
servidor él mismo (sin `--skip-server`). Si arrancaste el servidor a mano
(como arriba) y te olvidas de crear ese fichero, `run.sh` lo detecta antes de
correr `wrk` (`curl` a `/static/sample.txt`) y **omite el escenario con un
aviso** en vez de dejar un log de "100% Non-2xx" que parece evidencia válida
sin serlo.

**Si `--skip-server` te da "`/health` no responde" contra un VPS remoto**,
la causa más común no es el servidor sino que **hay dos firewalls, no uno**:
el del sistema operativo (`ufw`/`iptables`, cubierto arriba) y, aparte, el
**firewall a nivel de red del proveedor cloud** (Security Group en AWS,
Firewall en GCP/DigitalOcean/Hetzner/Vultr, NSG en Azure, etc.) — este último
bloquea el tráfico *antes* de que llegue siquiera al SO, así que un `ufw
allow` correcto no basta si el proveedor lo sigue descartando. Revisa el
panel de control del proveedor y agrega una regla de entrada para TCP/8080
(o el puerto que uses) además de la de `ufw`. `run.sh` con `--skip-server`
te muestra un checklist de diagnóstico paso a paso cuando falla esta
comprobación.

## 3. Interpretar los resultados (criterios B.4 del plan)

- **`summary.md`**: `Requests/sec` y la tabla de latencias `50%/75%/90%/99%`
  de cada escenario (`run.sh` pasa `--latency` a `wrk` para que esa tabla
  aparezca); `Socket errors`/`Non-2xx or 3xx responses` deben ser 0 (criterio
  B.4.1) — `wrk` solo imprime esas líneas cuando hay algo que reportar, así
  que su ausencia en el log ya es la señal de "sin errores".
- **Escenario `pipeline`, caveat conocido**: con pipelining real, la tabla de
  percentiles de `wrk` puede salir con `75%/90%/99%` en `0.00us` — es una
  limitación documentada de cómo `wrk` contabiliza latencia por petición
  cuando varias respuestas llegan por una sola lectura del socket, no un bug
  del servidor ni del script. Para ese escenario confía solo en
  `Requests/sec` y en que no haya `Socket errors`/`Non-2xx`; no uses sus
  percentiles para el criterio B.4.4.
- **`server.log` con muchas líneas `[ERROR] Error reading from socket N:
  Connection reset by peer` al final de cada escenario**: también esperado —
  es `wrk` cerrando sus conexiones abruptamente (RST) cuando termina cada
  corrida, no un fallo del servidor. El número de líneas debería rondar la
  concurrencia (`-c`) de ese escenario; si ves errores de otro tipo, o muchos
  más de los esperados, ahí sí investiga.
- **`soak-rss.csv`** (columna 2, KB): no debe tener pendiente positiva
  sostenida — oscilación acotada sí, crecimiento monótono no (B.4.2).
- **`metrics.diff`**: confirma que `requests_total`/`responses_total{class="2xx"}`
  crecieron acorde a lo que envió `wrk`, y que `workers_in_flight` volvió a su
  valor base (sin fuga de handlers).
- Para el criterio B.4.3 (503/load-shedding bajo saturación deliberada), ver
  la sección correspondiente en `docs/RUNBOOK_UBUNTU_LOAD_FUZZ.md` (Paso 4.6)
  — requiere `rate_limit.enabled: true` en la config, a propósito distinto de
  la config de línea base.

Una vez tengas una corrida completa y limpia, transcribe un resumen (no el
log completo) a la tabla de línea base en la sección B.4 de
[`docs/LOAD_AND_FUZZ_PLAN.md`](../../docs/LOAD_AND_FUZZ_PLAN.md), enlazando a
la carpeta de `results/` correspondiente como evidencia — mismo criterio que
ya se usa para las campañas de fuzzing archivadas en `tests/fuzz/campaigns/`.

## Piezas

- `run.sh` — orquestador (bash portable, sin dependencias GNU-only).
- `scripts/pipeline.lua` — pipelining real de HTTP/1.1 (Escenario 2): concatena
  varias peticiones en una sola escritura al socket.
- `scripts/range.lua` — fuerza `Range: bytes=0-1023` en cada petición
  (Escenario 3), para ejercitar la ruta `206 Partial Content`.
- `results/` — evidencia archivada, un subdirectorio por corrida.
