#!/usr/bin/env bash
#
# tools/loadtest/run.sh — corre los escenarios de carga (wrk) de la Parte B
# de docs/LOAD_AND_FUZZ_PLAN.md contra examples/07_config_server, y archiva
# la evidencia (logs de wrk, snapshots de /metrics, CSV de RSS) en un
# directorio nuevo bajo tools/loadtest/results/.
#
# Funciona igual en macOS y en cualquier distro Linux: solo usa bash, curl,
# wrk y utilidades POSIX (ps, ulimit, date). Dependencias e instalación por
# sistema operativo: ver docs/DEPENDENCIES.md. Uso detallado: README.md en
# este mismo directorio.
#
# Uso:
#   tools/loadtest/run.sh                          # escenarios 1-3, rapido
#   tools/loadtest/run.sh --soak                    # escenario 4 (soak), largo
#   tools/loadtest/run.sh --url http://otra-ip:8080 --skip-server
#   tools/loadtest/run.sh --help
#
# Salida de --help para el detalle de todas las flags.

set -eu

# --- Directorio de invocación y raíz del repo (antes de cualquier cd) -------
INVOKE_DIR="$PWD"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# --- Valores por defecto ------------------------------------------------------
BASE_URL="http://127.0.0.1:8080"
BINARY="$REPO_ROOT/build/examples/07_config_server"
CONFIG="$REPO_ROOT/config/oreshnek.loadtest.json"
RESULTS_ROOT="$REPO_ROOT/tools/loadtest/results"
CONCURRENCIES="50 200 1000"
DURATION="30s"
PIPELINE_DEPTH="16"
SOAK=0
SOAK_DURATION="20m"
SKIP_SERVER=0
THREADS=""

usage() {
    cat <<'EOF'
Uso: tools/loadtest/run.sh [opciones]

Corre los escenarios de carga de la Parte B de docs/LOAD_AND_FUZZ_PLAN.md
contra examples/07_config_server y archiva evidencia en
tools/loadtest/results/<timestamp>/.

Opciones:
  --url URL             Base URL del servidor (default: http://127.0.0.1:8080)
  --binary PATH         Ruta al binario del servidor
                         (default: build/examples/07_config_server)
  --config PATH         Config a pasarle al servidor
                         (default: config/oreshnek.loadtest.json)
  --skip-server         No arrancar el servidor: asume que --url ya responde
                         (útil si lo arrancaste a mano o corre en otra máquina)
  --concurrencies "L"   Lista de concurrencias para el escenario 1, entre
                         comillas (default: "50 200 1000")
  --duration DUR        Duración de cada corrida corta, formato wrk
                         (default: 30s)
  --pipeline-depth N    Peticiones por escritura en el escenario 2
                         (default: 16)
  --soak                Corre solo el escenario 4 (soak) en vez de 1-3
  --soak-duration DUR   Duración del soak, formato wrk (default: 20m)
  --threads N           Hilos de wrk (default: min(núcleos, concurrencia))
  --results-dir DIR     Dónde crear el subdirectorio de esta corrida
                         (default: tools/loadtest/results)
  -h, --help            Esta ayuda

Requiere que `wrk` esté instalado (ver docs/DEPENDENCIES.md) y, salvo con
--skip-server, que el binario del servidor ya esté compilado (ver
docs/RUNBOOK_UBUNTU_LOAD_FUZZ.md o docs/DEPENDENCIES.md para macOS).
EOF
}

log() { printf '[loadtest] %s\n' "$*"; }
die() { printf '[loadtest] ERROR: %s\n' "$*" >&2; exit 1; }

# Resuelve una ruta potencialmente relativa contra el directorio desde el que
# se invocó el script (no contra REPO_ROOT), para que --binary/--config
# relativos se comporten como cabría esperar.
resolve_path() {
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *)  printf '%s\n' "$INVOKE_DIR/$1" ;;
    esac
}

# --- Parseo de argumentos ------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --url) BASE_URL="$2"; shift 2 ;;
        --binary) BINARY="$(resolve_path "$2")"; shift 2 ;;
        --config) CONFIG="$(resolve_path "$2")"; shift 2 ;;
        --skip-server) SKIP_SERVER=1; shift ;;
        --concurrencies) CONCURRENCIES="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --pipeline-depth) PIPELINE_DEPTH="$2"; shift 2 ;;
        --soak) SOAK=1; shift ;;
        --soak-duration) SOAK_DURATION="$2"; shift 2 ;;
        --threads) THREADS="$2"; shift 2 ;;
        --results-dir) RESULTS_ROOT="$(resolve_path "$2")"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "opción desconocida: $1 (usa --help)" ;;
    esac
done

command -v wrk >/dev/null 2>&1 || die "wrk no está instalado — ver docs/DEPENDENCIES.md"
command -v curl >/dev/null 2>&1 || die "curl no está instalado"

# --- Núcleos disponibles (Linux: nproc: macOS: sysctl) ------------------------
if [ -z "$THREADS" ]; then
    if command -v nproc >/dev/null 2>&1; then
        THREADS="$(nproc)"
    elif command -v sysctl >/dev/null 2>&1; then
        THREADS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    else
        THREADS=4
    fi
fi

# Sube el límite de descriptores de fichero para este proceso y sus hijos
# (servidor + wrk): con concurrencias altas (p.ej. 1000) el límite por
# defecto de macOS (256) o de algunas distros agota los fds antes de llegar
# a la concurrencia pedida. Best-effort: si el límite duro es menor, sigue
# sin abortar (wrk/el servidor reportarán errores de conexión igual, pero de
# forma explícita en su log).
ulimit -n 4096 2>/dev/null || true

RUN_TS="$(date +%Y%m%d-%H%M%S)"
RESULTS_DIR="$RESULTS_ROOT/$RUN_TS"
mkdir -p "$RESULTS_DIR"

{
    echo "fecha: $(date)"
    echo "commit: $(cd "$REPO_ROOT" && git rev-parse HEAD 2>/dev/null || echo desconocido)"
    echo "uname: $(uname -a)"
    echo "wrk: $(wrk --version 2>&1 | head -1) ($(command -v wrk))"
    echo "url: $BASE_URL"
    echo "threads: $THREADS"
    echo "concurrencies: $CONCURRENCIES"
    echo "duration: $DURATION"
    echo "soak: $SOAK (duration=$SOAK_DURATION)"
} > "$RESULTS_DIR/metadata.txt"
log "resultados en $RESULTS_DIR"

# --- Arranque del servidor -----------------------------------------------------
SERVER_PID=""
cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        log "deteniendo servidor (PID $SERVER_PID, SIGTERM, apagado graceful)"
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

if [ "$SKIP_SERVER" -eq 0 ]; then
    [ -x "$BINARY" ] || die "no existe (o no es ejecutable) el binario '$BINARY'. Compílalo primero:
  cmake -B build -DORESHNEK_WITH_SQLITE=ON && cmake --build build --target 07_config_server
o pasa --binary/--skip-server. Detalle: docs/DEPENDENCIES.md / docs/RUNBOOK_UBUNTU_LOAD_FUZZ.md"
    [ -f "$CONFIG" ] || die "no existe el fichero de config '$CONFIG'"

    # El fichero estático del Escenario 3 vive bajo static_dir de la config
    # (relativo a la raíz del repo, igual que la BD sqlite y static_dir que
    # el propio servidor crea al arrancar).
    mkdir -p "$REPO_ROOT/static"
    if [ ! -f "$REPO_ROOT/static/sample.txt" ]; then
        yes "oreshnek load test line" | head -n 200 > "$REPO_ROOT/static/sample.txt"
    fi

    log "arrancando servidor: $BINARY $CONFIG (cwd=$REPO_ROOT)"
    ( cd "$REPO_ROOT" && exec "$BINARY" "$CONFIG" ) > "$RESULTS_DIR/server.log" 2>&1 &
    SERVER_PID=$!

    ok=0
    for _ in $(seq 1 30); do
        if curl -sf "$BASE_URL/health" >/dev/null 2>&1; then
            ok=1
            break
        fi
        sleep 1
    done
    [ "$ok" -eq 1 ] || die "el servidor no respondió /health en 30s — revisa $RESULTS_DIR/server.log"
    log "servidor listo (PID $SERVER_PID)"
else
    log "--skip-server: asumiendo que $BASE_URL ya está sirviendo"
    curl -sf "$BASE_URL/health" >/dev/null 2>&1 || die "$BASE_URL/health no responde"
fi

# Hilos de wrk para una concurrencia dada: nunca más que la concurrencia
# misma (wrk exige -t <= -c).
threads_for() {
    c="$1"
    if [ "$THREADS" -gt "$c" ]; then printf '%s\n' "$c"; else printf '%s\n' "$THREADS"; fi
}

run_wrk() {
    # run_wrk <nombre-log> <concurrencia> <duracion> [args extra de wrk...]
    name="$1"; c="$2"; dur="$3"; shift 3
    t="$(threads_for "$c")"
    log "escenario: $name (c=$c, d=$dur, t=$t)"
    wrk -t"$t" -c"$c" -d"$dur" --latency "$@" 2>&1 | tee "$RESULTS_DIR/$name.log"
}

if [ "$SOAK" -eq 0 ]; then
    # --- Escenario 1: JSON caliente, concurrencia creciente -------------------
    for c in $CONCURRENCIES; do
        run_wrk "throughput-c${c}" "$c" "$DURATION" "$BASE_URL/"
    done

    # --- Escenario 2: pipelining ------------------------------------------------
    run_wrk "pipeline" 50 "$DURATION" \
        -s "$SCRIPT_DIR/scripts/pipeline.lua" "$BASE_URL/" -- "$PIPELINE_DEPTH"

    # --- Escenario 3: estático + Range ------------------------------------------
    run_wrk "static-range" 200 "$DURATION" \
        -s "$SCRIPT_DIR/scripts/range.lua" "$BASE_URL/static/sample.txt"

    {
        echo "# Resumen ($RUN_TS)"
        echo
        for f in "$RESULTS_DIR"/throughput-c*.log "$RESULTS_DIR"/pipeline.log "$RESULTS_DIR"/static-range.log; do
            [ -f "$f" ] || continue
            echo "## $(basename "$f")"
            grep -E 'Requests/sec|Latency|Socket errors|Non-2xx|^ +[0-9]+%' "$f" || true
            echo
        done
    } > "$RESULTS_DIR/summary.md"
else
    # --- Escenario 4: soak -------------------------------------------------------
    log "escenario: soak (d=$SOAK_DURATION) — RSS + /metrics antes/después"
    curl -s "$BASE_URL/metrics" > "$RESULTS_DIR/metrics-antes.prom" || true

    RSS_CSV="$RESULTS_DIR/soak-rss.csv"
    : > "$RSS_CSV"
    if [ -n "$SERVER_PID" ]; then
        ( while kill -0 "$SERVER_PID" 2>/dev/null; do
              printf '%s,%s\n' "$(date +%s)" "$(ps -o rss= -p "$SERVER_PID" 2>/dev/null || echo 0)" >> "$RSS_CSV"
              sleep 30
          done ) &
        RSS_SAMPLER_PID=$!
    else
        log "--skip-server activo: no hay PID local que muestrear, RSS_CSV quedará vacío"
        RSS_SAMPLER_PID=""
    fi

    run_wrk "soak" 200 "$SOAK_DURATION" "$BASE_URL/"

    [ -n "${RSS_SAMPLER_PID:-}" ] && kill "$RSS_SAMPLER_PID" 2>/dev/null || true
    curl -s "$BASE_URL/metrics" > "$RESULTS_DIR/metrics-despues.prom" || true
    diff "$RESULTS_DIR/metrics-antes.prom" "$RESULTS_DIR/metrics-despues.prom" \
        > "$RESULTS_DIR/metrics.diff" || true

    {
        echo "# Resumen soak ($RUN_TS)"
        echo
        echo "## soak.log"
        grep -E 'Requests/sec|Latency|Socket errors|Non-2xx|^ +[0-9]+%' "$RESULTS_DIR/soak.log" || true
        echo
        echo "## RSS (primera/última muestra, KB)"
        head -1 "$RSS_CSV" 2>/dev/null || echo "(sin muestras)"
        tail -1 "$RSS_CSV" 2>/dev/null || true
        echo
        echo "## diff de /metrics (antes -> después)"
        cat "$RESULTS_DIR/metrics.diff" 2>/dev/null || echo "(sin diferencias o /metrics no disponible)"
    } > "$RESULTS_DIR/summary.md"
fi

log "listo. Resumen: $RESULTS_DIR/summary.md"
