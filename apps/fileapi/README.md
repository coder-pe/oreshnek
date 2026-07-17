# fileapi — HTTPS file storage API

Primera aplicación construida sobre el framework Oreshnek: una API HTTPS para
**recibir archivos de cualquier tipo y tamaño y guardarlos en una carpeta**.
Los cuerpos por encima de un umbral se transmiten (stream) directo a disco, así
que las subidas están limitadas por el disco, no por la RAM.

## Endpoints

| Método   | Ruta            | Descripción                                       |
|----------|-----------------|---------------------------------------------------|
| `PUT`    | `/files/:name`  | Sube un archivo (cuerpo crudo); nombre sugerido.  |
| `POST`   | `/files`        | Sube; nombre desde cabecera `X-Filename` o generado. |
| `GET`    | `/files`        | Lista los archivos almacenados (JSON).            |
| `GET`    | `/files/:name`  | Descarga (sendfile + Range/resume + ETag).        |
| `DELETE` | `/files/:name`  | Borra un archivo.                                 |
| `GET`    | `/health`       | Liveness probe.                                   |

El nombre sugerido se **sanea** (solo basename, sin traversal) y se hace único
ante colisiones. Las descargas se sirven con anti directory-traversal.

## Compilar

Desde la raíz del repo (la app necesita la librería, no una BD):

```bash
cmake -B build -DORESHNEK_BUILD_APPS=ON -DORESHNEK_WITH_SQLITE=ON
cmake --build build --target fileapi
```

El binario queda en `build/apps/fileapi/fileapi`.

## Certificado autofirmado (HTTPS)

La app exige HTTPS. Genera un par autofirmado para desarrollo local:

```bash
cd apps/fileapi
./tools/gen-selfsigned-cert.sh          # escribe certs/server.{crt,key}
```

> Autofirmado ⇒ los clientes avisarán; usa `curl -k` (`--insecure`) en local.
> No usar en producción.

## Ejecutar

```bash
cd apps/fileapi
../../build/apps/fileapi/fileapi fileapi.json
```

Escucha en `https://localhost:8443`. Config en `fileapi.json`
(`upload.stream_threshold_bytes`, `upload.max_upload_bytes` = 0 sin límite,
`upload.spool_dir`, `upload_dir` = carpeta destino).

## Ejemplos con curl

```bash
# Subir un archivo grande (streaming a disco)
curl -k -T ./pelicula.mp4 https://localhost:8443/files/pelicula.mp4

# Subir por POST con nombre en cabecera
curl -k -X POST --data-binary @foto.jpg \
     -H "X-Filename: foto.jpg" https://localhost:8443/files

# Listar
curl -k https://localhost:8443/files

# Descargar (con reanudación por rangos)
curl -k -O https://localhost:8443/files/pelicula.mp4
curl -k -r 0-1023 https://localhost:8443/files/pelicula.mp4   # primeros 1 KiB (206)

# Borrar
curl -k -X DELETE https://localhost:8443/files/pelicula.mp4
```

## Notas / límites (v1)

- La ruta de streaming requiere `Content-Length` (curl `-T`, `--data-binary`,
  `fetch` con un `File` lo envían). Cuerpos `chunked` grandes no se soportan aún.
- El spool se escribe en el hilo del event loop (disco local); ver
  `docs/LOAD_AND_FUZZ_PLAN.md` y el ROADMAP para el trabajo futuro.
