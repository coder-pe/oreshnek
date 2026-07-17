# Frontend de ejemplo — Video Tutorial Platform

Frontend estático de referencia (HTML/JS/CSS) para el dominio del ejemplo
[`06_video_platform.cpp`](../06_video_platform.cpp): la UI "Video Tutorial
Platform" (listado de vídeos, subida, reproducción).

Es **material ilustrativo de dominio**, no forma parte del framework y no está
cableado al binario del ejemplo. Vivía antes en `static/` en la raíz del repo;
se movió aquí para mantener el núcleo (`include/` + `src/`) agnóstico.

## Archivos

- `index.html` — página principal (catálogo + formulario de subida).
- `watch.html` — página de reproducción de un vídeo.
- `script.js`, `style.css` — lógica y estilos del cliente.

## Servirlo

Con cualquier servidor de estáticos, o con el servidor de demo del framework
apuntando `static_dir` a esta carpeta:

```bash
# desde la raíz del repo
./build/examples/07_config_server            # sirve GET /static/:file_path
# (configura static_dir: ./examples/06_video_platform_web en oreshnek.json)
```

Los endpoints del API (`/api/register`, `/api/login`, `/api/upload`,
`/api/videos`) los provee `06_video_platform`.
