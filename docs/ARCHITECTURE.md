# Arquitectura de Oreshnek

Este documento describe el modelo de ejecución del framework tras la **Fase 4**
de endurecimiento (concurrencia segura + robustez productiva). Para el plan
completo de madurez a producción, ver [ROADMAP.md](ROADMAP.md).

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
| JSON | `nlohmann::json` directo (sin capa de alias propia). |
| `Router` | Enrutado trie segmento a segmento. |
| `Middleware` | Filtros encadenables ejecutados antes del handler (`Server::use`). |
| `ThreadPool` | Workers que consumen tareas de una cola. |
| `Platform::Config` | Carga `ServerConfig` desde fichero JSON + overrides por entorno. |
| `DatabaseManager` | Frontera sobre los backends (`std::variant` + `std::visit`, sin `virtual`). |
| `SqliteBackend` / `PgBackend` | Concretos CRTP: SQLite3 (`SqlitePool`/WAL) y PostgreSQL (`PgPool`/libpq). |
| `Net::TlsContext` | Envuelve un `SSL_CTX` de servidor (cert/key); crea el `SSL` por conexión. |
| `Utils::Logger` | Logging estructurado thread-safe con sink a fichero y rotación (`ORE_LOG(LEVEL) << ...`). |

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
               thread_pool.enqueue(task) ──────────────────► │  middlewares (Server::use)
                                                             │  router->find_route()
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

## Contrato de apagado graceful (thread-safe)

- `request_stop()` es **async-signal-safe**: solo escribe un atómico
  (`stop_requested_ = true`) y un byte al self-pipe. Es lo único que debe invocar
  un manejador de señales.
- Al observar `stop_requested_`, el event loop entra en **fase de drenado**: deja
  de aceptar conexiones (cierra el socket de escucha), termina las peticiones en
  vuelo y vacía sus respuestas, cerrando cada conexión tras enviarla (no se
  reutilizan keep-alive). Sale cuando no queda trabajo pendiente o cuando expira
  `shutdown_grace_sec` (entonces descarta lo que quede).
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

## Timeouts de conexión

En cada barrido (cadencia 1 s) `enforce_timeouts()` recorre las conexiones que no
tienen un worker en vuelo y aplica, según su estado:

- **read_timeout** — una petición a medio recibir que no se completa: se responde
  `408 Request Timeout` y se cierra.
- **write_timeout** — una respuesta que el peer no termina de drenar: se cierra.
- **idle_timeout** — una conexión keep-alive ociosa a la espera de otra petición:
  se cierra.
- **handler_timeout** — un worker que excede su deadline ejecutando el handler:
  se responde `504` y se cierra la conexión. El worker **no se cancela** (no es
  seguro interrumpir código arbitrario); sigue corriendo y su resultado tardío se
  descarta por el guard de liveness de `process_completions`.

Todos son configurables (`Server::Settings`, poblados desde `ServerConfig`); un
valor de `0` desactiva el timeout correspondiente.

## Métricas

Con `metrics.enabled`, `Server::enable_metrics(path)` registra un `GET` que expone
`Metrics::render()` en formato de texto Prometheus: contadores (`requests_total`,
respuestas por clase 2xx–5xx, `connections_accepted_total`, `rate_limited_total`,
`handler_timeouts_total`), un gauge de conexiones activas y un histograma de
duración de petición. Todos los contadores son atómicos: el event loop los
actualiza (accept/close/rate-limit/handler-timeout) y los workers registran la
clase de status y la latencia por respuesta, sin locks.

## Rate limiting

Con `rate_limit.enabled`, un **token bucket por IP** (`TokenBucketLimiter`) se
consulta en `dispatch_next` —en el hilo del event loop, antes de copiar la
petición o lanzar un worker—; si la IP excede su cuota se responde `429 Too Many
Requests` (con `Retry-After`) directamente, sin gastar un worker. El bucket
refilla a `requests_per_second` hasta una capacidad `burst`. Al vivir solo en el
event loop no necesita locks; los buckets ociosos se descartan en el barrido
periódico para acotar memoria. La IP se captura en el `accept`
(`Connection::client_ip_`).

## Middleware

`Server::use(Middleware)` registra filtros `bool(const HttpRequest&, HttpResponse&)`
que se ejecutan **en el worker, antes del handler**, en orden de registro. Devolver
`false` corta la cadena: la respuesta ya construida se envía tal cual y el handler
no se invoca (rechazo de auth, preflight CORS...). `Middleware.h` incluye factorías
listas: `cors()`, `request_logger()` y `require_jwt(secret, prefijos)`. La cadena
se llena antes de `run()` y los workers solo la leen (sin mutación concurrente).

## Configuración

`Platform::Config::load(path)` construye un `ServerConfig` combinando, en orden de
prioridad: defaults → fichero JSON (todas las claves opcionales) → variables de
entorno (para secretos: `ORESHNEK_JWT_SECRET`, `ORESHNEK_PORT`, etc.). Un fichero
ausente usa defaults; uno malformado lanza excepción. Ver
[`config/oreshnek.example.json`](../config/oreshnek.example.json).

## TLS / HTTPS

Cuando `tls.enabled` está activo, el socket de escucha habla HTTPS. `Net::TlsContext`
(compartido, solo lectura tras construirse) carga el certificado y la clave y crea
un `SSL` por conexión en el `accept`. El **handshake es no bloqueante** y lo conduce
el event loop: en cada evento, si el handshake no está completo, se llama a
`SSL_accept` y se re-arma para lectura o escritura según `WANT_READ`/`WANT_WRITE`;
solo al completarse empieza el I/O HTTP. Las lecturas drenan `SSL_read` en bucle
(necesario con disparo por flanco + el buffer interno de OpenSSL) y las escrituras
usan `SSL_write`. Como `sendfile()` no puede cifrar, el cuerpo de fichero se sirve
con `pread`+`SSL_write`. El cierre hace `SSL_shutdown`/`SSL_free` (el `fd` lo cierra
`Connection`, ya que `SSL_set_fd` usa `BIO_NOCLOSE`).

> Es un único puerto TLS (HTTPS-only cuando se activa); HTTP+HTTPS simultáneos en
> puertos distintos queda como trabajo futuro.

## Persistencia (abstracción de backend)

`DatabaseManager` es una **frontera** sobre los backends concretos, con
**polimorfismo estático** (sin `virtual`): un `concept DatabaseBackend` + base
**CRTP** `DatabaseBase<Derived>` definen el contrato, y la selección en runtime se
hace con `std::variant` + `std::visit`. Hay dos concretos:

- **`SqliteBackend`** — `Platform::SqlitePool`: N conexiones al mismo fichero en
  modo **WAL** (lectores concurrentes + un escritor), `synchronous=NORMAL`,
  `foreign_keys=ON`, `busy_timeout`.
- **`PgBackend`** (principal) — `Platform::PgPool`: pool de conexiones **libpq**
  con RAII + reconexión (`PQreset`); consultas siempre parametrizadas (`$n`,
  anti-inyección).

El backend se elige por configuración (`db.backend`). Cada operación toma una
conexión del pool (RAII), en lugar de serializar en un mutex global, permitiendo
consultas en paralelo desde los workers. Diseño y extensibilidad (Oracle, MySQL,
MongoDB, ...) en [DATABASE.md](DATABASE.md).

## Logging

Todo el logging del framework pasa por `Utils::Logger`, un sink protegido por
mutex, vía la macro `ORE_LOG(LEVEL) << ...` (niveles `TRACE/DEBUG/INFO/WARN/ERROR`).
Cada línea lleva timestamp, nivel y thread-id. Por defecto escribe a `std::clog`;
con `set_file()` escribe a fichero con **rotación por tamaño** (`<path>.1`, `.2`,
... hasta `log_max_files`). El nivel y el destino se toman de la configuración. Se
mantiene una implementación propia (sin añadir spdlog) para minimizar dependencias.
