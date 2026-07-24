# Campañas de fuzzing archivadas

Logs crudos de corridas de `fuzz_http_parser` que cierran (o intentan cerrar)
el criterio de aceptación A.5.2 de
[`docs/LOAD_AND_FUZZ_PLAN.md`](../../../docs/LOAD_AND_FUZZ_PLAN.md): una
campaña de ≥5 min sin crash/leak/UB partiendo del corpus semilla.

Cada fichero es la salida completa (`stdout`+`stderr`) de una corrida, sin
editar, nombrado `YYYYMMDD-<commit-corto>.log`:

```bash
mkdir -p tests/fuzz/campaigns
./build-fuzz/fuzz_http_parser -max_total_time=300 -print_final_stats=1 \
    tests/fuzz/corpus \
    2>&1 | tee "tests/fuzz/campaigns/$(date +%Y%m%d)-$(git rev-parse --short HEAD).log"
```

No se edita ni se resume el contenido de estos ficheros — el resumen legible
(fecha, duración, resultado) va en la tabla de "Estado actual" de
`docs/LOAD_AND_FUZZ_PLAN.md`, con el nombre de fichero como referencia. Si una
corrida encuentra un crash, el reproductor (`crash-<hash>`) se copia a
`tests/fuzz/regressions/` — el log de la corrida que lo encontró se conserva
igual, como registro de cuándo y cómo se halló.
