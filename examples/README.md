# Ejemplos de Oreshnek

Programas de referencia, autocontenidos y compilables, para los casos de uso más
comunes y los **puntos de personalización** del framework. Se construyen con el
resto del proyecto (opción `ORESHNEK_BUILD_EXAMPLES`, ON por defecto):

```bash
cmake -S . -B build && cmake --build build
./build/examples/01_hello_json     # y el resto
```

| Ejemplo | Caso de uso | Personalización que ilustra |
|---------|-------------|-----------------------------|
| [`01_hello_json`](01_hello_json.cpp) | Servicio JSON mínimo | Rutas, parámetros de ruta/query, cuerpo JSON, respuestas JSON/texto |
| [`02_middleware`](02_middleware.cpp) | Lógica transversal | Cadena de middleware (orden, short-circuit), built-ins (CORS/logger/JWT) y middleware propio |
| [`03_rest_crud_db`](03_rest_crud_db.cpp) | API REST con BD | Gateway SQL genérico (`query`/`exec` parametrizado): modelo y DDL **propios de la app**, mapeo de filas, backend por config (SQLite↔PostgreSQL), hashing de password, login JWT |
| [`04_static_files`](04_static_files.cpp) | Servir ficheros | `HttpResponse::file()` (sendfile/Range/HEAD automáticos), resolución segura anti directory-traversal |
| [`05_production`](05_production.cpp) | Despliegue real | `Config::load`, logging, timeouts + shutdown graceful, TLS, rate limiting, `/metrics` |
| [`06_video_platform`](06_video_platform.cpp) | App de dominio completa | Construir un dominio propio (usuarios + vídeos: modelos, esquema y repositorio) **sobre** el gateway genérico — demuestra que el framework es de propósito general |
| [`07_config_server`](07_config_server.cpp) | Servidor dirigido por config | Servidor completo cableado desde `oreshnek.json`: TLS + rate limiting + `/metrics` + compresión + recurso genérico "notes" + estáticos. Era el binario `oreshnek_server` de `src/main.cpp` |

## Puntos de personalización del framework

- **Rutas** — `server.get/post/put/del/patch(path, handler)`. Soportan segmentos
  estáticos y parámetros `:nombre` (`req.param("nombre")`); además `req.query(...)`,
  `req.header(...)`, `req.body()` y `req.json()`.
- **Middleware** — `server.use(Middleware)`; se ejecutan antes del handler en orden
  de registro y pueden cortar la cadena devolviendo `false`. Built-ins en
  [`oreshnek/server/Middleware.h`](../include/oreshnek/server/Middleware.h):
  `cors()`, `request_logger()`, `require_jwt(secret, prefijos)`. Un middleware es
  cualquier `bool(const HttpRequest&, HttpResponse&)`.
- **Base de datos** — gateway SQL **genérico y agnóstico del dominio**:
  `db.query(sql, params)` / `db.exec(sql, params)` con consultas parametrizadas
  (anti-inyección) y filas genéricas (`SqlResult`). El framework no impone modelos;
  la app define su esquema y mapea las filas a sus structs (ver `06_video_platform`).
  Abstracción sin `virtual` (CRTP + `concept` + `std::variant`); el backend se elige
  por `db.backend` en la config; añadir uno nuevo (Oracle, MySQL, ...) = crear un
  concreto que cumpla el `concept` y sumarlo al `variant`. Ver
  [`docs/DATABASE.md`](../docs/DATABASE.md).
- **Configuración** — `Platform::Config::load(path)`: defaults → fichero JSON →
  variables de entorno (secretos fuera del repo). Todas las claves en
  [`config/oreshnek.example.json`](../config/oreshnek.example.json).
- **TLS** — `server.enable_tls(cert, key, min_version)` (o `tls.enabled` en config).
- **Rate limiting** — `server.enable_rate_limit(req_por_seg, burst)`.
- **Métricas** — `server.enable_metrics("/metrics")` (formato Prometheus).
- **Logging** — `Utils::Logger` + macro `ORE_LOG(NIVEL) << ...`; nivel y sink a
  fichero con rotación desde la config.
- **Seguridad** — `Platform::SecurityUtils`: `hashPassword`/`verifyPassword`
  (PBKDF2-HMAC-SHA256), `generateJWT`/`validateJWT` (HS256).
- **Ciclo de vida** — patrón canónico en [`common.h`](common.h):
  `request_stop()` (async-signal-safe) → `run()` (drena) → `stop()`.

Para construir aplicaciones optimizadas y seguras con estos puntos, ver
[`docs/ANALYSIS.md`](../docs/ANALYSIS.md).
