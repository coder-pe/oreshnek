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

## Fase 4 — Robustez productiva ⬜ (siguiente)

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
