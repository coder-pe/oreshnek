# Roadmap de madurez a producción

Plan progresivo para llevar Oreshnek de "demo funcional" a "listo para
producción". Política de dependencias: minimizar y usar solo librerías muy
maduras (nlohmann/json, OpenSSL, SQLite, libsodium, spdlog). El núcleo (parser
HTTP, router, multipart, thread pool) se mantiene propio.

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

## Fase 2 — Seguridad ⬜ (siguiente)

- ⬜ JWT: base64**url**, validar `exp`, rechazar `alg != HS256` (anti `alg:none`),
  verificar firma antes de decodificar, comparación en **tiempo constante**.
- ⬜ Corregir UB en `base64_decode` (índice con `char` con signo).
- ⬜ Passwords: Argon2id (libsodium) o PBKDF2-HMAC-SHA256; **salt por usuario**.
- ⬜ Migrar `SHA256_*`/`HMAC` a la API EVP de OpenSSL 3 (elimina 3 warnings).
- ⬜ Ignorar `SIGPIPE` (`SIG_IGN` o `MSG_NOSIGNAL`).
- ⬜ Canonicalizar rutas estáticas (`std::filesystem::weakly_canonical`).
- ⬜ Límites de tamaño de headers y body (anti-DoS).

## Fase 3 — HTTP/1.1 completo y streaming ⬜

- ⬜ Integrar **nlohmann/json** (ya vendorizada en `nlohmann_json/`).
- ⬜ `sendfile` (Linux) / equivalente macOS para respuestas de fichero.
- ⬜ Range requests (`206 Partial Content`) — vídeo.
- ⬜ Decodificación chunked en `HttpParser`.
- ⬜ `Expect: 100-continue`; HEAD sin body.
- ⬜ Parser multipart robusto (streaming a disco).

## Fase 4 — Robustez productiva ⬜

- ⬜ Shutdown graceful (drenar peticiones en vuelo con deadline).
- ⬜ Timeouts de lectura/escritura/idle (`408`/`504`).
- ⬜ Logging estructurado (spdlog) con niveles y rotación.
- ⬜ Configuración externa (fichero); secreto JWT vía env/fichero.
- ⬜ SQLite: WAL + `busy_timeout` + pool de conexiones.
- ⬜ Middleware/filtros encadenables (auth, CORS, logging, compresión).

## Fase 5 — TLS y rendimiento ⬜

- ⬜ TLS con OpenSSL (handshake no bloqueante).
- ⬜ Rate limiting (token bucket por IP).
- ⬜ Métricas Prometheus (`/metrics`).
- ⬜ Optimizar envío de body (`erase(0,n)` es O(n²) → offset).
- ⬜ (Opcional) compresión gzip/brotli, HTTP/2 (`nghttp2`).
