# Oreshnek C++ Web Framework

Oreshnek es un framework web para C++20 ligero y de alto rendimiento, diseñado para construir aplicaciones y APIs web rápidas y escalables. Utiliza un modelo asíncrono y basado en eventos con `epoll` en Linux y `kqueue` en macOS para una gestión eficiente de las conexiones.

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
*   CMake (versión 3.10 o superior).
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

El ejecutable se encontrará en el directorio `build/`.

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

## Arquitectura del Framework

El framework está organizado en los siguientes módulos principales:

*   `/include/oreshnek/`
    *   `http/`: Clases para `HttpRequest`, `HttpResponse` y el parser de HTTP.
    *   `json/`: Clases para `JsonValue`, `JsonParser` y `JsonBuilder`.
    *   `net/`: Lógica de red de bajo nivel, incluyendo `Connection` y `SocketUtil`.
    *   `server/`: El núcleo del servidor, incluyendo `Server`, `Router` y `ThreadPool`.
    *   `platform/`: Abstracciones de plataforma como `DatabaseManager`.
    *   `utils/`: Utilidades como `Logger` y `StringUtil`.
*   `/src/`: Implementaciones de los ficheros de cabecera correspondientes.
*   `/static/`: Ficheros estáticos para el ejemplo de la plataforma de video.
*   `/tests/`: Pruebas unitarias para los diferentes componentes del framework.
