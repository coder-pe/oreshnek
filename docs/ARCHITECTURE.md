# Arquitectura de Oreshnek

Este documento describe el modelo de ejecución del framework tras la **Fase 1**
de endurecimiento (modelo de concurrencia seguro). Para el plan completo de
madurez a producción, ver [ROADMAP.md](ROADMAP.md).

## Visión general

Oreshnek es un framework HTTP en C++20 con patrón **reactor**:

- Un **event loop** de un solo hilo sobre `epoll` (Linux) o `kqueue` (macOS),
  con disparo por flanco (`EPOLLET`) y re-armado explícito (`EPOLLONESHOT`).
- Un **thread pool** que ejecuta los handlers de ruta (trabajo de CPU).
- Un **router** tipo trie con segmentos estáticos y parámetros (`:id`).

## Componentes

| Componente | Responsabilidad |
|------------|-----------------|
| `Server` | Orquesta el socket de escucha, el multiplexor de eventos, el router y el thread pool. **Único dueño del event loop.** |
| `Connection` | Estado por cliente: buffer de lectura, parser HTTP, respuesta pendiente (string o stream de fichero). |
| `HttpParser` | Parser incremental sobre `string_view`. Soporta `Content-Length` y `Transfer-Encoding: chunked`. |
| `HttpRequest` | Petición parseada. Puede *poseer* sus bytes (`make_owned`) para cruzar el límite de hilos sin punteros colgantes. |
| `HttpResponse` | Construye la respuesta (`body`, `file`, `json`, `text`, `html`); lleva rango de fichero y flag HEAD. |
| `Http::Multipart` | Parser `multipart/form-data` (zero-copy sobre el cuerpo). |
| JSON | `Oreshnek::Json::JsonValue` es un alias de `nlohmann::json`. |
| `Router` | Enrutado trie segmento a segmento. |
| `ThreadPool` | Workers que consumen tareas de una cola. |
| `Utils::Logger` | Sink de logging thread-safe (`ORE_LOG(LEVEL) << ...`). |

### Respuestas de fichero

Las respuestas de fichero (`HttpResponse::file()`) se sirven con `sendfile()`
(zero-copy). El framework aplica automáticamente, tras cada handler, semánticas
dirigidas por la petición (`apply_http_semantics`): cabecera `Accept-Ranges`,
`Range` → `206 Partial Content`/`416`, y supresión del cuerpo para `HEAD`. El
descriptor del fichero lo gestiona y cierra el event loop en `Connection`.

## Ciclo de vida de una petición (modelo Fase 1)

El principio central: **solo el hilo del event loop toca los objetos
`Connection` y el descriptor de epoll/kqueue.** Los workers nunca tocan una
`Connection` ni el multiplexor.

```
                 event-loop thread                     worker thread
                 ────────────────                      ─────────────
  EPOLLIN  ──►  read_data()                                  │
               parse_next(consumed)                          │
               request = make_shared<HttpRequest>(...)       │
               request->make_owned(buffer, consumed) ───────►│  (request es dueño de sus bytes)
               consume(consumed)                             │
               conn->processing_ = true                      │
               thread_pool.enqueue(task) ──────────────────► │  router->find_route()
                                                             │  handler(request, response)
                                                             │  push {fd, conn(shared), response}
                                                             │       a la cola de finalización
               drain_wakeup()  ◄───── notify (self-pipe) ────┘
               process_completions():
                 verifica que connections_[fd] == conn
                 conn->set_response_content(response)
                 rearm(fd, write)
  EPOLLOUT ──► write_data()  (headers + body / fichero)
               si terminó y keep-alive:
                 clear_response_state(); processing_=false
                 dispatch_next()  (sirve la siguiente petición pipelined)
```

### Garantías de seguridad

- **Sin use-after-free:** `connections_` guarda `shared_ptr<Connection>`. Si el
  event loop cierra y elimina una conexión mientras un worker sigue en vuelo, el
  objeto sigue vivo gracias a la referencia del worker; el worker solo escribe en
  la cola de finalización. `process_completions` descarta la respuesta si el fd
  ya no pertenece a esa conexión (protección frente a reuso de fd).
- **Sin `string_view` colgantes:** `HttpRequest::make_owned()` copia los bytes de
  la petición a almacenamiento propio y *rebasa* todas las vistas (path, version,
  headers, query, body) a esa copia antes de entregarla al worker. El parseo
  (`parse_next`) y la mutación del buffer (`consume`) están separados, de modo que
  el buffer no se sobrescribe antes de tomar posesión.
- **Orden de respuestas HTTP/1.1:** a lo sumo **una petición en vuelo por
  conexión** (`processing_`). Las peticiones pipelined se sirven en orden, una tras
  otra, al terminar de escribir cada respuesta.

## Contrato de apagado (thread-safe)

- `request_stop()` es **async-signal-safe**: solo escribe un atómico
  (`running_ = false`) y un byte al self-pipe. Es lo único que debe invocar un
  manejador de señales.
- `run()` hace el *teardown* de sus propios recursos (mapa de conexiones, fds de
  epoll/kqueue y escucha) **en su propio hilo** al salir del bucle.
- El dueño del hilo de `run()` debe hacer `join` después de `request_stop()`.
- `stop()` (lo llama el destructor) señaliza, apaga el thread pool (hace `join` de
  los workers) y cierra el self-pipe una vez que ningún worker puede notificar.

**No llamar `stop()` desde otro hilo mientras `run()` está activo.** Uso correcto:

```cpp
Server server(4);
server.get("/", handler);
server.listen("0.0.0.0", 8080);
std::thread t([&]{ server.run(); });   // event loop
// ... en un signal handler o desde otro punto:
server.request_stop();                  // señaliza
t.join();                               // run() hace su propio teardown
// el destructor de server apaga el thread pool
```

En el `main.cpp` de ejemplo, `run()` es bloqueante en el hilo principal, así que
`run()` retorna y luego se llama `stop()` en el mismo hilo (secuencial, sin
concurrencia).

## Logging

Todo el logging del framework pasa por `Utils::Logger`, un sink protegido por
mutex, vía la macro `ORE_LOG(LEVEL) << ...` (niveles `TRACE/DEBUG/INFO/WARN/ERROR`).
Esto elimina las data races por uso concurrente de `std::cout`/`std::cerr` desde
varios hilos. Es un stop-gap hasta integrar un backend estructurado (p. ej.
spdlog) en la Fase 4.
