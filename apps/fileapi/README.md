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

## Despliegue en un VPS (Debian/Ubuntu) con certificados reales

Requisitos: un **dominio** (p.ej. `files.tudominio.com`) cuyo registro DNS `A`/`AAAA`
apunte a la IP pública del VPS, y los puertos abiertos en el firewall.

TLS es **opcional** en la app (`tls.enabled`). Hay dos formas recomendadas:

### Opción A — Reverse proxy (nginx) que termina TLS  ·  *recomendada*

nginx gestiona el certificado y su **renovación automática**; fileapi corre en
**HTTP plano** en localhost. Es lo estándar en producción.

```bash
sudo apt update && sudo apt install -y nginx certbot python3-certbot-nginx
```

`/etc/nginx/sites-available/fileapi` (habilítalo con un symlink a `sites-enabled/`):

```nginx
server {
    listen 80;
    server_name files.tudominio.com;

    client_max_body_size 0;          # no limitar el tamaño de subida en nginx
    proxy_request_buffering off;      # streaming: pasa el cuerpo sin bufferizarlo

    location / {
        proxy_pass http://127.0.0.1:8443;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_read_timeout 300s;      # subidas grandes
    }
}
```

```bash
sudo ln -s /etc/nginx/sites-available/fileapi /etc/nginx/sites-enabled/
sudo nginx -t && sudo systemctl reload nginx
# Obtiene el certificado y reconfigura nginx a HTTPS (con redirección 80→443):
sudo certbot --nginx -d files.tudominio.com
# Renovación: certbot instala un timer systemd; pruébalo con:
sudo certbot renew --dry-run
```

fileapi entonces con `tls.enabled=false` y `host` en localhost:

```json
{ "host": "127.0.0.1", "port": 8443, "tls": { "enabled": false }, ... }
```

### Opción B — fileapi termina TLS con el certificado de Let's Encrypt

La app lee directamente los PEM que emite certbot.

```bash
sudo apt update && sudo apt install -y certbot
# 'standalone' levanta un servidor temporal en el puerto 80 para validar el dominio
# (párate cualquier cosa que use el 80 durante la emisión):
sudo certbot certonly --standalone -d files.tudominio.com
```

Certbot escribe (symlinks que apuntan al certificado vigente):
`/etc/letsencrypt/live/files.tudominio.com/fullchain.pem` y `.../privkey.pem`.

Apunta la config a esos ficheros:

```json
"tls": {
  "enabled": true,
  "cert_file": "/etc/letsencrypt/live/files.tudominio.com/fullchain.pem",
  "key_file":  "/etc/letsencrypt/live/files.tudominio.com/privkey.pem",
  "min_version": "1.2"
}
```

Dos detalles operativos importantes:

1. **Permisos**: `privkey.pem` es solo-root por defecto. Corre fileapi como root
   (no ideal) o da acceso de lectura al usuario del servicio, p.ej. un grupo
   `ssl-cert` con ACL sobre `/etc/letsencrypt/{live,archive}`.
2. **Renovación**: fileapi carga el certificado **solo al arrancar**. Cuando
   certbot renueva (cada ~60 días), hay que **reiniciar el servicio** para que
   tome el nuevo cert. Añade un *deploy hook*:

   ```bash
   sudo certbot renew --deploy-hook "systemctl restart fileapi"
   ```

> Por esto la **Opción A es más cómoda**: nginx recarga el certificado sin
> reiniciar tu app. (Recarga en caliente del cert en el propio framework es una
> mejora futura anotada.)

### Servicio systemd (para cualquiera de las dos opciones)

`/etc/systemd/system/fileapi.service`:

```ini
[Unit]
Description=fileapi (Oreshnek)
After=network.target

[Service]
WorkingDirectory=/opt/fileapi
ExecStart=/opt/fileapi/fileapi /opt/fileapi/fileapi.json
Environment=ORESHNEK_JWT_SECRET=cambia-esto-por-un-secreto-largo
Restart=on-failure
User=fileapi
Group=fileapi

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload && sudo systemctl enable --now fileapi
```

## Sobre los avisos `TLS handshake failed` / `SSL_read error`

Son **ruido a nivel de conexión, no fallos del servidor**: aparecen cuando un
cliente habla **HTTP plano contra el puerto HTTPS**, **rechaza el certificado
autofirmado** (navegador sin `-k`), o **corta la conexión** a media transferencia
(un `Ctrl+C`, un escáner de puertos, un health-check TCP). El servidor solo cierra
esa conexión; las subidas por un cliente HTTPS correcto (`curl -k` / cert válido)
no se ven afectadas. En local, usa siempre `https://…` y `curl -k`.

## Notas / límites (v1)

- TLS es opcional (`tls.enabled`): actívalo para que fileapi termine TLS, o
  desactívalo y pon delante un reverse proxy (ver arriba).
- La ruta de streaming requiere `Content-Length` (curl `-T`, `--data-binary`,
  `fetch` con un `File` lo envían). Cuerpos `chunked` grandes no se soportan aún.
- El spool se escribe en el hilo del event loop (disco local); ver
  `docs/LOAD_AND_FUZZ_PLAN.md` y el ROADMAP para el trabajo futuro.
