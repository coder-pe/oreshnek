# Oreshnek C++ Web Framework

[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](LICENSE)

Oreshnek es un framework web para C++ ligero y de alto rendimiento, diseñado para construir aplicaciones y APIs web rápidas y escalables. Compila con **C++20 o C++17** (`ORESHNEK_CXX_STANDARD`), para funcionar también en toolchains sin compilador C++20. Utiliza un modelo asíncrono y basado en eventos con `epoll` en Linux y `kqueue` en macOS para una gestión eficiente de las conexiones.

> **Estado:** endurecido hacia producción (Fases 0–6). Completadas:
> estabilidad/concurrencia (sin data races ni use-after-free, verificado con
> sanitizers), seguridad (JWT/PBKDF2, anti directory-traversal, límites),
> HTTP/1.1 + streaming, robustez productiva (config externa, logging, timeouts,
> shutdown graceful, middleware), abstracción de BD + **PostgreSQL/Oracle**, y
> **TLS + rate limiting + métricas Prometheus**. El plan y su progreso están en
> [`docs/ROADMAP.md`](docs/ROADMAP.md).
>
> **Recursos:** [ejemplos de referencia](examples/README.md) ·
> [análisis y apps optimizadas](docs/ANALYSIS.md) ·
> [arquitectura](docs/ARCHITECTURE.md) · [seguridad](docs/SECURITY.md) ·
> [persistencia/BD](docs/DATABASE.md) ·
> [comparativa con Drogon](docs/COMPARISON_DROGON.md) ·
> [changelog](CHANGELOG.md) ·
> [licencia](LICENSE) · [licencia comercial](COMMERCIAL.md).

## Características Principales

*   **Servidor Asíncrono:** Construido sobre `epoll` (Linux) y `kqueue` (macOS) para manejar un gran número de conexiones concurrentes con baja sobrecarga.
*   **Moderno:** Escrito en C++20 (concepts, etc.) con un *fallback* automático a C++17 cuando se compila con `ORESHNEK_CXX_STANDARD=17`, sin cambios de código para el usuario del framework.
*   **Multihilo:** Utiliza un pool de hilos para procesar las peticiones de forma concurrente y no bloqueante.
*   **Enrutador (Router):** Un sistema de enrutamiento simple pero potente para mapear rutas y métodos HTTP a funciones manejadoras (handlers).
*   **Manejo de HTTP/1.1:** Peticiones (`HttpRequest`) y respuestas (`HttpResponse`), keep-alive, pipelining, `Transfer-Encoding: chunked`, `Expect: 100-continue` y `HEAD`.
*   **Streaming de ficheros:** Servido zero-copy con `sendfile`, **Range requests** (`206 Partial Content`) y **caché condicional** (`ETag`/`Last-Modified` → `304`) para vídeo y descargas reanudables.
*   **Compresión:** `gzip` (zlib) y `brotli` opcional, negociados por `Accept-Encoding`, para texto/JSON/manifiestos (nunca ficheros/video).
*   **Procesamiento de JSON:** Usa [nlohmann/json](https://github.com/nlohmann/json) como motor JSON.
*   **Subidas multipart:** Parser `multipart/form-data` integrado (`Http::Multipart`).
*   **Subidas grandes (streaming a disco):** cuerpos con `Content-Length` por encima de un umbral se derraman directo a un fichero temporal según llegan (memoria constante), y el handler recibe su ruta vía `HttpRequest::body_file()` — sin el tope de ~1 MiB del buffer de lectura. `Server::enable_upload_streaming` / sección `upload`. App de referencia: [`apps/fileapi`](apps/fileapi/README.md).
*   **TLS/HTTPS:** Opcional sobre OpenSSL con handshake no bloqueante.
*   **Middleware:** Cadena encadenable con short-circuit (CORS, logging, JWT, propios).
*   **Bases de datos:** Gateway SQL **genérico y agnóstico del dominio** (`query`/`exec` parametrizado, filas genéricas); el framework no impone modelos. Abstracción sin `virtual` (CRTP) con backends **SQLite**, **PostgreSQL** (libpq) y **Oracle** (OCI), seleccionables por configuración. Cada backend es **opt-in en compilación** (`ORESHNEK_WITH_SQLITE` / `_POSTGRES` / `_ORACLE`): el build solo depende del cliente que el proyecto realmente use.
*   **Operación:** Configuración externa (JSON + entorno), logging estructurado con rotación, timeouts, apagado graceful, **rate limiting** por IP y **métricas Prometheus** (`/metrics`).
*   **Seguridad:** PBKDF2-HMAC-SHA256, JWT HS256 (tiempo constante), límites anti-DoS.
*   **Extensible:** Arquitectura modular; ver [puntos de personalización](examples/README.md).

## Requisitos

Para compilar y ejecutar un proyecto con Oreshnek, necesitarás:

*   Un compilador compatible con C++17 o C++20 (GCC 8+ para C++17, GCC 10+ para C++20; Clang 7+/12+ respectivamente).
*   CMake (versión 3.16 o superior).
*   OpenSSL (criptografía y TLS).
*   Al menos un backend de base de datos habilitado en CMake (ver abajo):
    SQLite3, `libpq` (PostgreSQL) o el Instant Client SDK de Oracle (OCI).
*   zlib (gzip); brotli **opcional** (Content-Encoding: br, autodetectado).
*   nlohmann/json (vendorizado en `nlohmann_json/`, o un paquete del sistema).

## Cómo Empezar

### 1. Compilación

El proyecto utiliza CMake para la compilación. Sigue estos pasos para compilar el servidor de ejemplo:

```bash
# 1. Clona el repositorio (si no lo has hecho)
# git clone ...

# 2. Crea un directorio de compilación, habilitando al menos un backend de BD
#    (ninguno está habilitado por defecto — ver "Backends de base de datos").
cmake -B build -DORESHNEK_WITH_SQLITE=ON

# 3. Compila el proyecto
cmake --build build
```

El ejecutable `oreshnek_server` se encontrará en el directorio `build/`.

#### Opciones de compilación

| Opción CMake | Por defecto | Descripción |
|--------------|-------------|-------------|
| `ORESHNEK_CXX_STANDARD` | `20` | Estándar de C++ a usar (`17` o `20`). Usa `17` si el compilador del servidor no soporta C++20. |
| `ORESHNEK_BUILD_TESTS` | `ON` | Compila la suite de tests. |
| `ORESHNEK_BUILD_EXAMPLES` | `ON` | Compila los ejemplos de `examples/`. |
| `ORESHNEK_ASAN` | `OFF` | AddressSanitizer + UndefinedBehaviorSanitizer. |
| `ORESHNEK_TSAN` | `OFF` | ThreadSanitizer (mutuamente excluyente con ASan). |
| `ORESHNEK_WITH_SQLITE` | `OFF` | Backend SQLite3. |
| `ORESHNEK_WITH_POSTGRES` | `OFF` | Backend PostgreSQL (libpq). |
| `ORESHNEK_WITH_ORACLE` | `OFF` | Backend Oracle (OCI, Instant Client SDK). |
| `ORESHNEK_ORACLE_HOME` | ver [`docs/DATABASE.md`](docs/DATABASE.md) | Ruta al Instant Client (solo si `ORESHNEK_WITH_ORACLE=ON`). |

#### Backends de base de datos

Ningún backend está habilitado por defecto: el proyecto **falla la configuración
de CMake** si no se activa al menos uno, para no arrastrar dependencias que no
vas a usar. Combínalos según necesites:

```bash
# Solo SQLite (sin dependencias externas de servidor)
cmake -B build -DORESHNEK_WITH_SQLITE=ON

# Solo PostgreSQL
cmake -B build -DORESHNEK_WITH_POSTGRES=ON

# Solo Oracle (usa el Instant Client SDK instalado en ORESHNEK_ORACLE_HOME)
cmake -B build -DORESHNEK_WITH_ORACLE=ON \
    -DORESHNEK_ORACLE_HOME=/home/miguel/oracle/instantclient_23_26

# Varios a la vez
cmake -B build -DORESHNEK_WITH_SQLITE=ON -DORESHNEK_WITH_POSTGRES=ON -DORESHNEK_WITH_ORACLE=ON

# Compilador sin C++20 (por ejemplo GCC 8/9 en un servidor antiguo)
cmake -B build -DORESHNEK_WITH_SQLITE=ON -DORESHNEK_CXX_STANDARD=17
```

Detalles de cada backend (config, límites, pool de conexiones) en
[`docs/DATABASE.md`](docs/DATABASE.md).

La compilación produce una librería estática `oreshnek` (el framework) y el
ejecutable de ejemplo `oreshnek_server`.

### 2. Uso Básico

A continuación se muestra un ejemplo de un servidor "Hola, Mundo" simple utilizando Oreshnek:

```cpp
#include "oreshnek/Oreshnek.h"
#include <iostream>

int main() {
    // Crear una instancia del servidor
    Oreshnek::Server::Server server;

    // Definir una ruta para el método GET en "/"
    server.get("/", [](const Oreshnek::HttpRequest& req, Oreshnek::HttpResponse& res) {
        // Crear un objeto JSON para la respuesta (nlohmann/json)
        nlohmann::json response_json;
        response_json["message"] = "Hola, Mundo!";

        // Enviar la respuesta JSON con un código de estado 200 OK
        res.status(Oreshnek::Http::HttpStatus::OK).json(response_json);
    });

    // Iniciar el servidor en el puerto 8080
    if (!server.listen("0.0.0.0", 8080)) {
        std::cerr << "No se pudo iniciar el servidor." << std::endl;
        return 1;
    }

    // Ejecutar el bucle principal del servidor
    server.run();

    return 0;
}
```

### 3. Ejecutar el Servidor

Para ejecutar el servidor de ejemplo compilado:

```bash
./build/oreshnek_server
```

El servidor estará escuchando en `http://localhost:8080`. Puedes probar la ruta principal con `curl`:

```bash
curl http://localhost:8080/
# Salida esperada: {"message":"Hola, Mundo!"}
```

## Pruebas

La suite incluye un test de integración a nivel de socket
(`tests/integration_test.cpp`) que ejercita keep-alive, pipelining, cuerpos
grandes (con escrituras parciales) y carga concurrente. Está pensado para
ejecutarse bajo sanitizers como puerta de regresión del modelo de concurrencia.

```bash
# Ejecución normal
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure

# Bajo ThreadSanitizer (detecta data races)
cmake -B build-tsan -DORESHNEK_TSAN=ON && cmake --build build-tsan
./build-tsan/integration_test

# Bajo AddressSanitizer + UBSan (use-after-free, leaks, UB)
cmake -B build-asan -DORESHNEK_ASAN=ON && cmake --build build-asan
ASAN_OPTIONS=detect_leaks=0 ./build-asan/integration_test   # LSan no está soportado en macOS
```

O todo de una vez con el gate de análisis (sanitizers + estático si está disponible):

```bash
tools/analyze.sh
```

Estado actual: **TSan 0 races, ASan/UBSan 0 errores, `ctest` verde (12 targets).**
Ver [`docs/ANALYSIS.md`](docs/ANALYSIS.md) para construir apps optimizadas y seguras.

El `HttpParser` se fuzzea con **libFuzzer + ASan/UBSan** (target opcional
`fuzz_http_parser`, `-DORESHNEK_FUZZ=ON`); el corpus y los reproductores de
crashes se re-ejecutan de forma determinista en `ctest` (`fuzz_replay_test`).
Detalles en [`tests/fuzz/README.md`](tests/fuzz/README.md).

## Modelo de hilos

El framework sigue un patrón reactor con una regla central: **solo el hilo del
event loop toca los objetos `Connection` y el descriptor de epoll/kqueue.** Los
hilos worker reciben una petición *propietaria* de sus bytes, ejecutan el handler
y devuelven la respuesta al event loop por una cola de finalización. Esto evita
data races y use-after-free, y preserva el orden de respuestas de HTTP/1.1.

El apagado es seguro desde manejadores de señales vía `request_stop()`. Los
detalles (ciclo de vida de la petición y contrato de apagado) están en
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Arquitectura del Framework

El framework está organizado en los siguientes módulos principales:

*   `/include/oreshnek/`
    *   `http/`: `HttpRequest`, `HttpResponse`, el parser HTTP y `Multipart`.
    *   `net/`: Red de bajo nivel: `Connection`, `SocketUtil` y `TlsContext`.
    *   `server/`: Núcleo del servidor: `Server`, `Router`, `ThreadPool`,
        `Middleware`, `RateLimiter` y `Metrics`.
    *   `platform/`: `Config`, abstracción de BD (`DatabaseBackend`/`DatabaseManager`,
        `SqliteBackend`/`PgBackend`, `SqlitePool`/`PgPool`) y `SecurityUtils`.
    *   `utils/`: `Logger` (estructurado, thread-safe) y `StringUtil`/`TimeUtil`.
*   `/src/`: Implementaciones de los ficheros de cabecera correspondientes.
*   `/examples/`: [Programas de referencia](examples/README.md) por caso de uso.
*   `/tools/`: `analyze.sh` (gate de sanitizers + análisis estático).
*   `/tests/`: Pruebas de integración del framework (12 targets ctest) y
    fuzzing del parser en [`tests/fuzz/`](tests/fuzz/README.md).
*   `/docs/`: [Arquitectura](docs/ARCHITECTURE.md), [seguridad](docs/SECURITY.md),
    [persistencia](docs/DATABASE.md), [análisis](docs/ANALYSIS.md),
    [comparativa](docs/COMPARISON_DROGON.md) y [roadmap](docs/ROADMAP.md).

## Licencia

Oreshnek se distribuye bajo **licencia dual**:

*   **[AGPL-3.0](LICENSE)** — libre para usar, modificar y redistribuir,
    incluidas pruebas y evaluación, sin costo. Su condición: si ejecutas una
    versión modificada (o una aplicación que lo integre) accesible a terceros
    por red, debes publicar el código fuente correspondiente (sección 13 de
    AGPL-3.0, la "cláusula de red/SaaS").
*   **Licencia comercial** — para quien necesite usar Oreshnek sin las
    obligaciones de AGPL-3.0 (por ejemplo, dentro de un producto cerrado). No
    es automática: debe solicitarse explícitamente. Ver
    [`COMMERCIAL.md`](COMMERCIAL.md) para el proceso y el contacto.
