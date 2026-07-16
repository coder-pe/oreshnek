# Changelog

Formato basado en [Keep a Changelog](https://keepachangelog.com/es-ES/1.0.0/).
Este proyecto sigue [SemVer](https://semver.org/lang/es/) (`0.x` = API aún
puede cambiar entre versiones menores).

## [0.1.0] - 2026-07-16

Primer release público. Resultado de las fases de endurecimiento hacia
producción descritas en [`docs/ROADMAP.md`](docs/ROADMAP.md).

### Añadido

- **Licenciamiento**: modelo dual — [AGPL-3.0](LICENSE) para uso libre/pruebas
  (con la obligación de publicar fuentes, incluido uso por red) y
  [licencia comercial](COMMERCIAL.md) bajo solicitud explícita.
- **Servidor**: reactor de un solo hilo (`epoll`/`kqueue`) + thread pool de
  handlers, HTTP/1.1 (keep-alive, pipelining, chunked, Range/HEAD,
  `Expect: 100-continue`), streaming de ficheros con `sendfile` y caché
  condicional (`ETag`/`Last-Modified`).
- **Persistencia**: gateway SQL genérico y agnóstico del dominio
  (`query`/`exec` parametrizado, sin `virtual`, CRTP + `std::variant`) sobre
  backends **SQLite3**, **PostgreSQL** (`libpq`) y **Oracle** (OCI/Instant
  Client SDK), todos **opt-in en CMake**
  (`ORESHNEK_WITH_SQLITE`/`_POSTGRES`/`_ORACLE`) para no forzar dependencias
  que un proyecto no use.
- **Portabilidad de C++**: compila con **C++20 o C++17**
  (`ORESHNEK_CXX_STANDARD`), con *fallback* automático del contrato de
  backends (concepts → SFINAE) para toolchains sin C++20 completo.
- **Seguridad**: PBKDF2-HMAC-SHA256, JWT HS256 en tiempo constante, límites
  anti-DoS, verificado con AddressSanitizer/UndefinedBehaviorSanitizer y
  ThreadSanitizer; fuzzing del parser HTTP con libFuzzer + replay
  determinista en ctest.
- **TLS/HTTPS** opcional sobre OpenSSL, **rate limiting** por IP (token
  bucket) y **métricas Prometheus** (`/metrics`).
- **Operación**: configuración externa (JSON + entorno), logging
  estructurado con rotación, timeouts configurables, apagado graceful, load
  shedding (503) bajo saturación.
- **Compresión** de respuestas (gzip/zlib siempre disponible; brotli
  opcional, autodetectado).
- Middleware encadenable (CORS, logging, JWT, propios), subidas
  `multipart/form-data`, [ejemplos de referencia](examples/README.md) y
  documentación de [arquitectura](docs/ARCHITECTURE.md),
  [seguridad](docs/SECURITY.md) y [persistencia](docs/DATABASE.md).

[0.1.0]: https://github.com/coder-pe/oreshnek/releases/tag/v0.1.0
