# Oreshnek vs Drogon — análisis comparativo

Comparación honesta entre **Oreshnek** y [**Drogon**](https://github.com/drogonframework/drogon),
un framework web C++ maduro y de alto rendimiento. Resumen: **Drogon es mucho más
completo y probado en producción**; **Oreshnek apuesta por el minimalismo, la
transparencia, la higiene de dependencias y una robustez verificada con
sanitizers**. No compiten en la misma liga de madurez, pero sí en filosofía.

## Tabla comparativa

| Dimensión | Oreshnek | Drogon |
|-----------|----------|--------|
| Madurez | Proyecto de endurecimiento desde cero (Fases 0–6) | ~14k★, 2000+ commits, 60+ releases, top en TechEmpower |
| Estándar C++ | C++20 | C++17/20 (soporta C++14) |
| Licencia | (la del repo) | MIT |
| Dependencias | Mínimas: nlohmann/json, OpenSSL, SQLite, libpq | Más amplias: trantor, jsoncpp, OpenSSL, zlib/brotli, clientes DB, uuid |
| Arquitectura | Reactor de **un** event loop (epoll/kqueue, edge-triggered) + thread pool de handlers | Multi event-loop (uno por hilo de E/S, modelo trantor) |
| Concurrencia handlers | Síncrona en el thread pool | Asíncrona + **corrutinas** C++ |
| HTTP | 1.1: keep-alive, pipelining, chunked, Range/HEAD, Expect:100-continue, multipart | 1.0/1.1 (cliente y servidor) |
| HTTP/2 | ❌ (planeado opcional) | Parcial/según versión |
| WebSocket | ❌ | ✅ (servidor y cliente) |
| TLS | ✅ OpenSSL (handshake no bloqueante) | ✅ OpenSSL |
| Compresión | ❌ (gzip/brotli, planeado opcional) | ✅ gzip + brotli |
| Base de datos | Abstracción CRTP (sin `virtual`) + pool; SQLite y PostgreSQL (libpq) | ORM asíncrono: PostgreSQL, MySQL/MariaDB, SQLite, Redis |
| ORM / codegen | ❌ (acceso explícito, consultas parametrizadas) | ✅ ORM + `drogon_ctl` (genera controllers/models/views) |
| Plantillas/vistas | ❌ (API JSON) | ✅ CSP con compilación de vistas en runtime |
| Sesiones | ❌ (JWT stateless) | ✅ sesiones + cookies integradas |
| Middleware/filtros | ✅ cadena con short-circuit + built-ins (CORS/log/JWT) | ✅ filtros + **AOP** (joinpoints) + plugins |
| Rate limiting | ✅ token bucket por IP (en el event loop) | Vía plugin/filtro |
| Métricas | ✅ Prometheus `/metrics` integrado | No integrado de serie |
| Logging | Propio, estructurado, con rotación | Propio (trantor) |
| Robustez verificada | **Gate de ASan/UBSan + TSan en cada fase** (TSan limpio) | Tests propios; madurez de campo |
| Tamaño/comprensibilidad | Núcleo pequeño y legible de punta a punta | Base de código grande y rica |

## Dónde gana Drogon

- **Funcionalidad**: WebSocket, ORM con generación de código, plantillas, Redis,
  compresión, corrutinas, sesiones, plugins, AOP. Para una aplicación web
  completa, Drogon trae casi todo "de fábrica".
- **Madurez y rendimiento probado**: años de uso, comunidad grande, posiciones
  altas en TechEmpower. Oreshnek **no está benchmarkeado** a esa escala.
- **Productividad**: `drogon_ctl` y el ORM reducen mucho el boilerplate.
- **Multiplataforma**: incluye Windows; Oreshnek apunta a Linux/macOS (epoll/kqueue).

## Dónde gana (o se diferencia) Oreshnek

- **Dependencias mínimas**: cuatro librerías muy maduras y nada de codegen. Más
  fácil de auditar, empotrar y compilar; superficie de ataque/suministro menor.
- **Transparencia y control**: el núcleo (parser HTTP, router, thread pool,
  abstracción de BD, TLS) es pequeño y legible; sabes exactamente qué pasa en cada
  capa. Útil como base educativa o cuando necesitas controlar/ajustar todo.
- **Robustez como requisito de primera clase**: cada fase pasa un gate de
  **AddressSanitizer/UBSan + ThreadSanitizer** (modelo de propiedad por el event
  loop, sin data races). El modelo de memoria/concurrencia es explícito y
  verificado, no emergente.
- **Sin `virtual` para extender**: polimorfismo estático (CRTP + `concept` +
  `std::variant`) en la capa de BD, coste cero.
- **Observabilidad y operación integradas**: `/metrics` Prometheus, logging
  estructurado con rotación, rate limiting, timeouts (incl. handler→504) y
  shutdown graceful, todo del propio framework.

## Cuándo elegir cada uno

**Elige Drogon** si necesitas una solución web completa y probada: WebSocket,
ORM/Redis, plantillas server-side, HTTP/2, compresión, corrutinas, y un time-to-
market corto en una aplicación grande.

**Elige Oreshnek** si priorizas un binario con **pocas dependencias**, una base
**auditable y comprensible**, control total sobre la pila, y una **robustez de
memoria/concurrencia verificada con sanitizers** — por ejemplo microservicios
JSON, APIs internas, servicios empotrados, o como cimiento sobre el que construir
exactamente lo que necesitas sin arrastrar un framework grande.

## Honestidad sobre el estado

Oreshnek **no** pretende reemplazar a Drogon hoy. Le faltan piezas relevantes
(WebSocket, HTTP/2, compresión, ORM, plantillas) y carece del rodaje de campo y de
benchmarks comparables. Su propuesta de valor es distinta: hacer **poco, pero de
forma mínima, transparente y verificada**. Varias de las ausencias están en el
[ROADMAP](ROADMAP.md) como trabajo futuro (gzip/HTTP2 opcionales de Fase 6).
