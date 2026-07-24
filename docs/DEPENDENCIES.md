# Dependencias del sistema (por sistema operativo)

Este documento reúne en un solo sitio **todo lo que hay que instalar a nivel de
sistema operativo** para: (1) compilar la librería `oreshnek` y sus tests, (2)
correr la campaña de fuzzing de [`docs/LOAD_AND_FUZZ_PLAN.md`](LOAD_AND_FUZZ_PLAN.md),
(3) correr las pruebas de carga con `wrk`, y (4) usar las herramientas de
análisis opcionales de `tools/analyze.sh`. Windows no es una plataforma
objetivo por ahora (el framework usa `epoll`/`kqueue`) y queda fuera.

Nada de esto se enlaza en la librería `oreshnek` salvo lo listado en
"Compilación del framework" — `wrk`, el LLVM de fuzzing y las herramientas de
análisis son andamiaje externo de test/CI, no dependencias del árbol
`include/`+`src/` (ver [`docs/ARCHITECTURE.md`](ARCHITECTURE.md)).

## Índice rápido: qué instalar para qué

| Objetivo | Necesitas |
|---|---|
| Compilar el framework (mínimo) | compilador C++17/20, CMake ≥3.16, OpenSSL, zlib, **un** backend de BD |
| + compresión Brotli | `brotli` (opcional, autodetectado) |
| + backend SQLite | `sqlite3` (dev headers) |
| + backend PostgreSQL | `libpq` (dev headers) |
| + backend Oracle | Instant Client SDK de Oracle (descarga manual, ver [`docs/DATABASE.md`](DATABASE.md)) |
| Fuzzing (`ORESHNEK_FUZZ=ON`) | clang con runtime de **libFuzzer** (compiler-rt) |
| Carga (`wrk`) | binario `wrk` (paquete o compilado desde fuente) |
| Análisis estático/dinámico opcional | `clang-tidy`, `cppcheck`, `valgrind` (Linux only) |

---

## macOS (Homebrew)

Apple Clang compila el framework sin problemas, pero **no trae el runtime de
libFuzzer** (`libclang_rt.fuzzer_osx.a` no existe en el toolchain de Xcode).
Para fuzzing usa el LLVM de Homebrew (verificado en esta máquina: compila y
corre `-fsanitize=fuzzer,address`).

```bash
# Xcode Command Line Tools (compilador, si no están ya instaladas)
xcode-select --install

# Herramientas de build + dependencias core
brew install cmake openssl zlib

# Backends de BD (instala solo el/los que vayas a usar)
brew install sqlite            # backend SQLite
brew install libpq             # backend PostgreSQL (keg-only; el CMakeLists ya
                                # resuelve su ruta con `brew --prefix libpq`)
# Oracle: no hay paquete Homebrew: instala el Instant Client SDK manualmente
# (ver docs/DATABASE.md)

# Compresión Brotli (opcional)
brew install brotli

# Fuzzing (LLVM de Homebrew, con runtime de libFuzzer)
brew install llvm

# Carga
brew install wrk

# Análisis estático/dinámico opcional
brew install llvm              # clang-tidy viene con este mismo paquete
brew install cppcheck
# valgrind: sin soporte real en macOS moderno (esp. Apple Silicon); usa ASan
# en su lugar (tools/analyze.sh ya lo hace y omite valgrind aquí).
```

Compilar el target de fuzzing con el LLVM de Homebrew:

```bash
cmake -B build-fuzz -DORESHNEK_FUZZ=ON \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
cmake --build build-fuzz --target fuzz_http_parser
```

En Mac Intel, Homebrew instala bajo `/usr/local` en vez de `/opt/homebrew`;
ajusta las rutas de arriba (`/usr/local/opt/llvm/bin/...`) si aplica.

---

## Debian / Ubuntu (apt)

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git \
    libssl-dev zlib1g-dev

# Backends de BD (instala solo el/los que vayas a usar)
sudo apt-get install -y libsqlite3-dev   # backend SQLite
sudo apt-get install -y libpq-dev        # backend PostgreSQL
# Oracle: no está en los repos; Instant Client SDK manual (ver docs/DATABASE.md)

# Compresión Brotli (opcional)
sudo apt-get install -y libbrotli-dev

# Fuzzing: clang trae el runtime de libFuzzer integrado desde clang 6+
sudo apt-get install -y clang

# Carga: wrk está empaquetado en Debian/Ubuntu recientes
sudo apt-get install -y wrk
# Si tu versión no lo trae, compílalo desde fuente (funciona en cualquier
# distro con make + libssl-dev + git):
#   git clone https://github.com/wg/wrk.git && cd wrk && make

# Análisis estático/dinámico opcional
sudo apt-get install -y clang-tidy cppcheck valgrind
```

Si el enlace de `fuzz_http_parser` falla buscando
`libclang_rt.fuzzer-x86_64.a` (puede pasar en versiones de Ubuntu que separan
el runtime de `compiler-rt` del paquete `clang`), instala el paquete de
`compiler-rt`/`libclang-rt` correspondiente a tu versión, p.ej.:

```bash
sudo apt-get install -y libclang-rt-$(clang --version | grep -oP '(?<=version )\d+')-dev
```

---

## Fedora / RHEL / CentOS Stream (dnf)

```bash
sudo dnf groupinstall -y "Development Tools"
sudo dnf install -y cmake git \
    openssl-devel zlib-devel

# Backends de BD (instala solo el/los que vayas a usar)
sudo dnf install -y sqlite-devel         # backend SQLite
sudo dnf install -y libpq-devel          # backend PostgreSQL (RHEL/CentOS:
                                          # puede llamarse postgresql-devel)
# Oracle: no está en los repos; Instant Client SDK manual (ver docs/DATABASE.md)

# Compresión Brotli (opcional)
sudo dnf install -y brotli-devel

# Fuzzing: clang + compiler-rt (runtime de libFuzzer)
sudo dnf install -y clang compiler-rt

# Carga: wrk no está empaquetado en Fedora/RHEL/CentOS — compílalo desde fuente
sudo dnf install -y gcc make openssl-devel git
git clone https://github.com/wg/wrk.git && cd wrk && make
# copia (o enlaza) el binario resultante a algo en tu PATH, p.ej.:
#   sudo cp wrk /usr/local/bin/

# Análisis estático/dinámico opcional
sudo dnf install -y clang-tools-extra cppcheck valgrind
```

En RHEL/CentOS clásico (no Stream) puede hacer falta habilitar EPEL para
`cppcheck`/`valgrind`:

```bash
sudo dnf install -y epel-release
```

---

## Arch Linux (pacman)

```bash
sudo pacman -Syu --needed base-devel cmake git \
    openssl zlib

# Backends de BD (instala solo el/los que vayas a usar)
sudo pacman -S --needed sqlite           # backend SQLite
sudo pacman -S --needed postgresql-libs  # backend PostgreSQL (libpq)
# Oracle: no está en los repos oficiales; Instant Client SDK manual
# (ver docs/DATABASE.md)

# Compresión Brotli (opcional)
sudo pacman -S --needed brotli

# Fuzzing: el paquete clang de Arch incluye el runtime de libFuzzer (compiler-rt)
sudo pacman -S --needed clang compiler-rt

# Carga: wrk está en el AUR, no en los repos oficiales
#   yay -S wrk
# o compílalo desde fuente (ver sección Debian/Ubuntu arriba)

# Análisis estático/dinámico opcional
sudo pacman -S --needed clang cppcheck valgrind   # clang-tidy viene con clang
```

---

## Notas comunes

- **Oracle Instant Client**: en ninguna de estas distros hay paquete oficial.
  Descarga el SDK desde Oracle, apunta `-DORESHNEK_ORACLE_HOME=/ruta/al/instantclient_XX_YY`
  al configurar CMake; detalle completo en [`docs/DATABASE.md`](DATABASE.md).
- **`nlohmann/json`**: no requiere instalación de sistema — el proyecto usa el
  single-header vendorizado en `nlohmann_json/` (git-ignored, se añade aparte);
  si no está presente, CMake cae a `find_package(nlohmann_json)` y en ese caso
  sí hace falta el paquete del sistema (`nlohmann-json3-dev` en Debian/Ubuntu,
  `json-devel` en Fedora, `nlohmann-json` en Arch/Homebrew).
- **Verifica el runtime de libFuzzer** antes de una campaña larga, en
  cualquier Linux:
  ```bash
  find / -name 'libclang_rt.fuzzer*' 2>/dev/null
  ```
  Si no aparece nada, tu paquete de `clang` no trae `compiler-rt` con soporte
  de fuzzer; instala el paquete `compiler-rt` (Fedora/Arch) o
  `libclang-rt-<ver>-dev` (Debian/Ubuntu) de tu distro.
- **`valgrind`** no tiene soporte real en macOS moderno (roto en Apple
  Silicon); en macOS usa ASan (`ORESHNEK_ASAN=ON`) como sustituto, tal como ya
  hace `tools/analyze.sh`.
- Estas listas cubren compilación y testing de carga/fuzz; para desplegar en
  producción (VPS, TLS, certbot) ver la sección de despliegue en
  [`docs/DATABASE.md`](DATABASE.md) y [`apps/fileapi/README.md`](../apps/fileapi/README.md).
