# Análisis y construcción de aplicaciones optimizadas

Cómo verificar y endurecer una aplicación Oreshnek en los cuatro ejes que
importan en un servidor: **memoria**, **hilos/concurrencia**, **liberación de
recursos** y **seguridad**. La herramienta central es un único gate:

```bash
tools/analyze.sh            # ASan+UBSan y TSan sobre toda la suite, + tidy/cppcheck/valgrind si están
tools/analyze.sh --no-tsan  # más rápido
```

Corre los analizadores dinámicos del compilador (siempre disponibles) y, si están
instalados, `clang-tidy`, `cppcheck` y `valgrind`; si no, los salta con una pista
de instalación. Devuelve código ≠ 0 si algún sanitizer falla → úsalo en CI.

---

## 1) Memoria

**Herramienta:** AddressSanitizer + (en Linux) LeakSanitizer.

```bash
cmake -S . -B build-asan -DORESHNEK_ASAN=ON && cmake --build build-asan -j
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan --output-on-failure   # Linux
# macOS: LSan no soportado -> detect_leaks=0 (ASan sigue cazando UAF/overflow)
```

Detecta use-after-free, overflows de heap/stack y (en Linux) fugas.

**Garantías del framework en las que apoyarse:**
- El event loop es el **único dueño** de los objetos `Connection`; las peticiones
  cruzan a los workers con `HttpRequest::make_owned()` (sin `string_view`
  colgantes). Un worker nunca toca memoria propiedad del loop.
- `shared_ptr<Connection>` evita el use-after-free si el loop cierra una conexión
  con trabajo en vuelo.
- Buffers de respuesta con offset (sin `erase(0,n)` O(n²)); parseo zero-copy con
  `string_view`.

**Guía para tu app:** evita copiar `req.body()`/headers (son `string_view`);
construye respuestas con `body(std::move(...))`; no guardes punteros/vistas a la
petición más allá del handler.

## 2) Hilos / concurrencia

**Herramienta:** ThreadSanitizer (excluyente con ASan).

```bash
cmake -S . -B build-tsan -DORESHNEK_TSAN=ON && cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```

**Garantías del framework:**
- Modelo de un solo hilo de event loop + thread pool de handlers; el multiplexor
  (epoll/kqueue) y el mapa de conexiones solo los toca el loop.
- Estado compartido sincronizado: cola de finalización (mutex), pools SQLite/PG
  (mutex+condvar), métricas (atómicas), rate limiter (solo event loop, sin locks).

**Guía para tu app:** un handler solo debe tocar su `HttpRequest`/`HttpResponse`
(que posee). Si compartes estado mutable entre handlers (caché, contadores),
protégelo tú (mutex o atómicos) y **vuelve a pasar TSan**. Mantén los handlers
breves; el trabajo muy largo dispara el `handler_timeout` (504).

## 3) Liberación de recursos (fds, memoria, conexiones)

Oreshnek es **RAII de principio a fin**: sockets, `SSL`, descriptores de fichero,
conexiones de pool (SQLite/PostgreSQL) y el `SSL_CTX` se cierran en destructores o
en `close_connection`. El apagado **graceful** drena las peticiones en vuelo antes
de salir, y los timeouts (`read`/`write`/`idle`/`handler`) cierran conexiones que
no progresan.

**Verificar fugas de descriptores:**
```bash
valgrind --track-fds=yes --leak-check=full build-dbg/db_test   # Linux
lsof -p <pid> | wc -l                                          # vigilar fds vivos
```
`tools/analyze.sh` ya lo hace con valgrind si está disponible.

**Guía para tu app:** usa RAII para todo recurso propio; no dupliques fds sin
cerrarlos; recuerda que los timeouts son configurables (`0` los desactiva).

## 4) Seguridad

**Herramientas:** UndefinedBehaviorSanitizer (incluido con ASan) + `clang-tidy`
con familias `cert-*`/`bugprone-*`/`concurrency-*` (perfil en `.clang-tidy`).

```bash
# UBSan: integer overflow, desreferencias inválidas, UB varios (con ASan).
# clang-tidy (estático):
tools/analyze.sh   # incluye clang-tidy si está instalado
```

**Lo que ya aporta el framework** (ver [SECURITY.md](SECURITY.md)):
- Passwords con **PBKDF2-HMAC-SHA256** + salt por usuario; verificación en tiempo
  constante. **JWT HS256** con `exp`, rechazo de `alg` no válido y firma en tiempo
  constante. **TLS** opcional. **Rate limiting** por IP (`429`). Límites anti-DoS
  del parser (headers ≤64 KiB, cuerpo ≤8 MiB). Anti directory-traversal en rutas
  de fichero.

**Guía para tu app:**
- SQL **siempre parametrizado** (los backends lo hacen; si añades consultas, usa
  `?`/`$n`, nunca concatenación).
- Secretos por **entorno** (`ORESHNEK_JWT_SECRET`, `ORESHNEK_PG_PASSWORD`,
  `ORESHNEK_DATABASE_URL`), no en el repo.
- Valida el JWT (`validateJWT`) **antes** de usar `decodeJWT`.
- Protege rutas sensibles con `require_jwt`; sanea nombres de fichero subidos.

---

## Tabla resumen

| Eje | Herramienta | Cómo |
|-----|-------------|------|
| Memoria | ASan/LSan | `-DORESHNEK_ASAN=ON` |
| Concurrencia | TSan | `-DORESHNEK_TSAN=ON` |
| Recursos (fds) | valgrind `--track-fds` / `lsof` | `tools/analyze.sh` |
| Seguridad/UB | UBSan + clang-tidy (`cert-*`) | `.clang-tidy` + `tools/analyze.sh` |
| Estático | clang-tidy / cppcheck | `tools/analyze.sh` |

Todo el desarrollo del framework mantiene este gate en verde; aplica el mismo
estándar a tu aplicación.
