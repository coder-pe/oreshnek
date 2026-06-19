# Roadmap de madurez a producción

Plan progresivo para llevar Oreshnek de "demo funcional" a "listo para
producción". Política de dependencias: minimizar y usar solo librerías muy
maduras (nlohmann/json, OpenSSL, SQLite, libpq; libsodium como opción futura). El
núcleo (parser HTTP, router, multipart, thread pool, logging y configuración) se
mantiene propio; en particular el logging es propio (no se añadió spdlog) y para
PostgreSQL se usa `libpq` (cliente C oficial), no `libpqxx`.

Leyenda: ✅ hecho · 🔄 en progreso · ⬜ pendiente

---

## Fase 0 — Red de seguridad ✅

- ✅ CMake reestructurado: librería `oreshnek` + ejecutable + tests.
- ✅ Opciones de sanitizers: `-DORESHNEK_ASAN=ON` (ASan+UBSan), `-DORESHNEK_TSAN=ON`.
- ✅ Test de integración a nivel de socket (`tests/integration_test.cpp`):
  keep-alive, pipelining, body grande con escrituras parciales, concurrencia.
- ✅ Integración con `ctest`.

## Fase 1 — Memoria y concurrencia ✅

- ✅ El event loop es el único dueño de `Connection` y del fd de epoll/kqueue.
- ✅ `HttpRequest::make_owned()` — la petición posee sus bytes al cruzar de hilo
  (elimina los `string_view` colgantes).
- ✅ `Connection::parse_next` / `consume` — separa parseo de mutación del buffer.
- ✅ Cola de finalización + self-pipe; los workers solo producen `HttpResponse`.
- ✅ `connections_` con `shared_ptr` (sin use-after-free) + guarda de reuso de fd.
- ✅ Una petición en vuelo por conexión (orden de respuestas HTTP/1.1).
- ✅ Apagado async-signal-safe (`request_stop()`); teardown del loop en su hilo.
- ✅ `Utils::Logger` thread-safe (elimina la race de `std::cout`/`std::cerr`).
- ✅ Eliminado el `memmove(data, data, n)` no-op.

**Resultado:** TSan 0 races, ASan/UBSan 0 errores, `ctest` verde
(antes: 52 races + SEGV, pipelining corrupto).

## Fase 2 — Seguridad ✅

- ✅ JWT: base64**url** sin padding, valida `exp`, rechaza `alg != HS256`
  (anti `alg:none`/confusión de algoritmo), verifica firma en **tiempo constante**
  (`CRYPTO_memcmp`). `decodeJWT` se usa solo tras `validateJWT`.
- ✅ Corregido el UB en `base64url_decode` (tabla indexada con `unsigned char`).
- ✅ Passwords: **PBKDF2-HMAC-SHA256** (200k iteraciones) con **salt aleatorio por
  usuario** embebido en formato `pbkdf2_sha256$iter$salt$hash`; verificación en
  tiempo constante. (Argon2id queda como opción futura vía libsodium.)
- ✅ Eliminado el uso de `SHA256_*` (vía `PKCS5_PBKDF2_HMAC`) → 0 warnings.
- ✅ `SIGPIPE`: `SIG_IGN` en la app + `MSG_NOSIGNAL` (Linux) + `SO_NOSIGPIPE` (macOS).
- ✅ Rutas estáticas y de vídeo canonicalizadas (`weakly_canonical` + verificación
  de que el resultado queda dentro del directorio base) → sin directory traversal.
- ✅ Límites anti-DoS en el parser: header block ≤ 64 KiB, `Content-Length` ≤ 8 MiB.
- ✅ Tests: `tests/security_test.cpp` (hash/verify de password, JWT válido, secreto
  incorrecto, firma/payload manipulados, decodificación de claims).

## Fase 3 — HTTP/1.1 completo y streaming ✅

- ✅ **nlohmann/json** como motor JSON (`JsonValue` = `nlohmann::json`); elimina la
  pérdida de precisión de enteros y el código JSON propio.
- ✅ `sendfile` zero-copy para respuestas de fichero (Linux y BSD/macOS); el
  cuerpo en memoria usa offset (sin `erase(0,n)` O(n²)).
- ✅ Range requests: `206 Partial Content` con `Content-Range` (incl. sufijo
  `-N`), `416` si no es satisfacible, `Accept-Ranges: bytes`.
- ✅ Decodificación chunked en `HttpParser` (dos pasadas, compactación in-place);
  rechaza `Content-Length` + `Transfer-Encoding` simultáneos (anti smuggling).
- ✅ `Expect: 100-continue` (envío único) y `HEAD` (enruta a GET, suprime cuerpo).
- ✅ Parser multipart robusto (`Http::Multipart`), zero-copy; reemplaza el
  placeholder roto. Tests en `tests/multipart_test.cpp`.

Nota: el cuerpo de la petición aún se almacena completo en el buffer de lectura
(1 MiB); el streaming de cuerpos grandes a disco queda para más adelante.

## Fase 4 — Robustez productiva ✅

- ✅ **Shutdown graceful**: `request_stop()` marca `stop_requested_` y el event
  loop entra en drenado (deja de aceptar, termina peticiones en vuelo y vacía sus
  respuestas, cerrando cada conexión); sale al quedar sin trabajo o al expirar
  `shutdown_grace_sec`.
- ✅ **Timeouts** de lectura (`408`), escritura e idle, configurables (cadencia de
  barrido 1 s; `0` desactiva cada uno).
- ✅ **Logging estructurado**: timestamp + nivel + thread-id, sink a fichero con
  rotación por tamaño, nivel y destino desde configuración. Implementación propia
  (sin spdlog) para minimizar dependencias.
- ✅ **Configuración externa** (`Platform::Config`): fichero JSON + overrides por
  entorno; el secreto JWT sale del código (`ORESHNEK_JWT_SECRET`).
  Ver `config/oreshnek.example.json`.
- ✅ **SQLite**: pool de conexiones (`SqlitePool`) en WAL + `synchronous=NORMAL` +
  `foreign_keys=ON` + `busy_timeout`; `DatabaseManager` sin mutex global.
- ✅ **Middleware** encadenable (`Server::use`) con short-circuit; built-ins
  `cors()`, `request_logger()`, `require_jwt()`.
- ✅ Tests: `lifecycle_test` (drenado + 408), `db_test` (pool/WAL concurrente),
  `middleware_test` (orden, short-circuit, CORS, JWT). Verde en normal, ASan/UBSan
  y TSan.

Pendiente (movido a fases posteriores): compresión gzip/brotli como middleware;
timeout de handler (`504`) — requiere cancelación cooperativa de los workers.

## Fase 5 — Capa de persistencia y PostgreSQL ✅

PostgreSQL pasa a ser la base de datos **principal**; SQLite3 se mantiene como
backend ligero (dev/tests/embebido). Diseño detallado en
[DATABASE.md](DATABASE.md).

- ✅ Abstracción **sin `virtual`**: `concept DatabaseBackend` + base **CRTP**
  `DatabaseBase<Derived>`; selección de backend en runtime con `std::variant` +
  `std::visit` en la frontera `DatabaseManager`.
- ✅ **Concreto SQLite3**: lógica movida a `SqliteBackend` (reusa `SqlitePool`),
  sin cambios de comportamiento.
- ✅ **Concreto PostgreSQL** vía **`libpq`** (cliente C oficial, sin `libpqxx`):
  `PgPool` (pool con RAII + reconexión `PQreset`), consultas parametrizadas
  (`$n`, anti-inyección), DDL en dialecto PostgreSQL (SERIAL/TIMESTAMPTZ/BOOLEAN).
- ✅ **Configuración**: sección `db` (`backend`, `sqlite`, `postgres`); secretos por
  entorno (`ORESHNEK_PG_PASSWORD`, `ORESHNEK_DATABASE_URL`); `sslmode` configurable.
- ✅ **Tests**: `pg_test` (mismas aserciones que `db_test`) que se salta si no hay
  `ORESHNEK_PG_TEST_DSN`; validado contra PostgreSQL 18 real en normal, ASan/UBSan
  y TSan.
- ✅ Extensible a futuro (Oracle, MySQL, MongoDB, ClickHouse, DB2, ...) añadiendo
  concretos al `std::variant`.

## Fase 6 — TLS y rendimiento 🔄

- ✅ **TLS con OpenSSL** (handshake no bloqueante integrado en el event loop):
  `Net::TlsContext` carga cert/key; `SSL_accept`/`SSL_read`/`SSL_write` con
  re-armado por `WANT_READ`/`WANT_WRITE`; cuerpo de fichero vía `pread`+`SSL_write`
  (sendfile no cifra). Activable por `tls.enabled`; cert/key por config o entorno
  (`ORESHNEK_TLS_CERT`/`ORESHNEK_TLS_KEY`). Test `tls_test` (verde normal/ASan/TSan).
- ✅ **Envío de body optimizado** con offset en vez de `erase(0,n)` O(n²) — ya
  resuelto en la Fase 3 (`write_body_offset_`).
- ⬜ Rate limiting (token bucket por IP).
- ⬜ Métricas Prometheus (`/metrics`).
- ⬜ Timeout de handler (`504`) con cancelación cooperativa de workers.
- ⬜ (Opcional) compresión gzip/brotli, HTTP/2 (`nghttp2`).
