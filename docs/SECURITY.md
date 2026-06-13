# Modelo de seguridad

Estado tras la **Fase 2**. Resume las primitivas de seguridad del framework y de
la aplicación de ejemplo. Ver el progreso global en [ROADMAP.md](ROADMAP.md).

## Contraseñas

- Derivación con **PBKDF2-HMAC-SHA256**, 200 000 iteraciones, vía
  `PKCS5_PBKDF2_HMAC` (OpenSSL EVP — sin dependencias nuevas).
- **Salt aleatorio de 16 bytes por usuario** (`RAND_bytes`), distinto en cada
  hash. El resultado es autocontenido:

  ```
  pbkdf2_sha256$<iteraciones>$<salt_base64url>$<hash_base64url>
  ```

- `SecurityUtils::hashPassword(password)` genera el string anterior;
  `verifyPassword(password, stored)` lo recomputa y compara en **tiempo constante**
  (`CRYPTO_memcmp`). Devuelve `false` ante cualquier formato inválido.
- Migración: los hashes SHA256+salt-fijo anteriores no son compatibles; los
  usuarios existentes deben re-registrarse o resetear su contraseña.

## JWT (HS256)

- Codificación **base64url sin padding** en las tres partes
  (`header.payload.signature`), conforme al estándar JWT.
- `validateJWT(token, secret)` aplica, en este orden:
  1. **Firma** HMAC-SHA256 sobre `header.payload`, comparada en **tiempo
     constante**.
  2. **Algoritmo**: rechaza cualquier `alg` distinto de `HS256` (protege frente a
     `alg:none` y confusión de algoritmo).
  3. **Expiración**: el claim `exp` es obligatorio y debe ser futuro.
- `decodeJWT(token)` solo decodifica el payload y **debe llamarse después** de que
  `validateJWT` haya devuelto `true`.
- El secreto JWT no debe estar hardcodeado en producción (pendiente Fase 4:
  cargarlo de entorno/fichero).

## Directory traversal

- Las rutas servidas desde disco (`/static/:file_path`, `/video/:filename`) se
  resuelven con `resolve_within_dir()`, que usa
  `std::filesystem::weakly_canonical` y verifica que el resultado quede **dentro**
  del directorio base (con separador en el límite, evitando `/staticX`). Cualquier
  intento de escape (`..`, rutas absolutas, symlinks) devuelve `403 Forbidden`.

## Límites anti-DoS (parser HTTP)

- **Bloque de headers** (línea de petición + cabeceras): ≤ 64 KiB. Si se supera
  sin completar, el parser entra en estado de error y la conexión se cierra.
- **Cuerpo** (`Content-Length`): ≤ 8 MiB; un valor mayor se rechaza de inmediato.
- Nota: el cuerpo se almacena completo en el buffer de lectura (1 MiB) por
  conexión; el streaming de cuerpos grandes llega en la Fase 3.

## Robustez de E/S

- `SIGPIPE` neutralizado en tres capas: `SIG_IGN` en la app de ejemplo,
  `MSG_NOSIGNAL` en cada `send()` (Linux) y `SO_NOSIGPIPE` en los sockets aceptados
  (macOS). Escribir a un peer cerrado produce `EPIPE` gestionable, no una señal.

## Pendiente (fases posteriores)

- TLS/HTTPS (Fase 5), rate limiting (Fase 5).
- Secreto JWT y configuración fuera del código (Fase 4).
- Cabeceras de seguridad (HSTS, X-Frame-Options) y CORS (Fase 4).
