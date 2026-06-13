# Oreshnek C++ Web Framework

Oreshnek es un framework web para C++20 ligero y de alto rendimiento, diseñado para construir aplicaciones y APIs web rápidas y escalables. Utiliza un modelo asíncrono y basado en eventos con `epoll` en Linux y `kqueue` en macOS para una gestión eficiente de las conexiones.

> **Estado:** en endurecimiento hacia producción. El modelo de concurrencia ya
> es seguro (sin data races ni use-after-free, verificado con sanitizers — ver
> Fase 1). El plan de madurez y su progreso están en
> [`docs/ROADMAP.md`](docs/ROADMAP.md), y el modelo de ejecución en
> [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Características Principales

*   **Servidor Asíncrono:** Construido sobre `epoll` (Linux) y `kqueue` (macOS) para manejar un gran número de conexiones concurrentes con baja sobrecarga.
*   **Moderno:** Escrito completamente en C++20, aprovechando las últimas características del lenguaje.
*   **Multihilo:** Utiliza un pool de hilos para procesar las peticiones de forma concurrente y no bloqueante.
*   **Enrutador (Router):** Un sistema de enrutamiento simple pero potente para mapear rutas y métodos HTTP a funciones manejadoras (handlers).
*   **Manejo de HTTP:** Clases para gestionar peticiones (`HttpRequest`) y respuestas (`HttpResponse`) de forma sencilla.
*   **Procesamiento de JSON:** Incluye un parser y constructor de JSON nativo para trabajar fácilmente con APIs.
*   **Extensible:** Diseñado con una arquitectura modular que facilita la adición de nueva funcionalidad.

## Requisitos

Para compilar y ejecutar un proyecto con Oreshnek, necesitarás:

*   Un compilador compatible con C++20 (GCC 10+, Clang 12+).
*   CMake (versión 3.16 o superior).
*   OpenSSL (para funcionalidades criptográficas).
*   SQLite3 (para la gestión de bases de datos).

## Cómo Empezar

### 1. Compilación

El proyecto utiliza CMake para la compilación. Sigue estos pasos para compilar el servidor de ejemplo:

```bash
# 1. Clona el repositorio (si no lo has hecho)
# git clone ...

# 2. Crea un directorio de compilación
cmake -B build

# 3. Compila el proyecto
cmake --build build
```

El ejecutable `oreshnek_server` se encontrará en el directorio `build/`.

#### Opciones de compilación

| Opción CMake | Por defecto | Descripción |
|--------------|-------------|-------------|
| `ORESHNEK_BUILD_TESTS` | `ON` | Compila la suite de tests. |
| `ORESHNEK_ASAN` | `OFF` | AddressSanitizer + UndefinedBehaviorSanitizer. |
| `ORESHNEK_TSAN` | `OFF` | ThreadSanitizer (mutuamente excluyente con ASan). |

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
        // Crear un objeto JSON para la respuesta
        Oreshnek::JsonValue response_json = Oreshnek::JsonValue::object();
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

Estado actual: **TSan 0 races, ASan/UBSan 0 errores, `ctest` verde.**

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
    *   `http/`: Clases para `HttpRequest`, `HttpResponse` y el parser de HTTP.
    *   `json/`: Clases para `JsonValue`, `JsonParser` y `JsonBuilder`.
    *   `net/`: Lógica de red de bajo nivel, incluyendo `Connection` y `SocketUtil`.
    *   `server/`: El núcleo del servidor, incluyendo `Server`, `Router` y `ThreadPool`.
    *   `platform/`: Abstracciones de plataforma como `DatabaseManager`.
    *   `utils/`: Utilidades como `Logger` (sink de logging thread-safe) y `StringUtil`.
*   `/src/`: Implementaciones de los ficheros de cabecera correspondientes.
*   `/static/`: Ficheros estáticos para el ejemplo de la plataforma de video.
*   `/tests/`: Pruebas de integración del framework.
*   `/docs/`: Documentación de arquitectura ([`ARCHITECTURE.md`](docs/ARCHITECTURE.md)) y hoja de ruta ([`ROADMAP.md`](docs/ROADMAP.md)).
