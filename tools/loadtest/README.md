# tools/loadtest/

El andamiaje automatizado descrito en la Parte B de
[`docs/LOAD_AND_FUZZ_PLAN.md`](../../docs/LOAD_AND_FUZZ_PLAN.md) (`run.sh`,
scripts Lua de wrk) **todavía no está implementado** — ver la tabla de "Estado
actual" en ese documento.

Mientras tanto, la forma de correr la prueba de carga es manual, con
comandos `wrk` directos contra `examples/07_config_server`. Paso a paso
completo (pensado para un VPS Ubuntu/Debian, pero el mismo procedimiento
aplica a cualquier Linux/macOS con `wrk` instalado):
[`docs/RUNBOOK_UBUNTU_LOAD_FUZZ.md`](../../docs/RUNBOOK_UBUNTU_LOAD_FUZZ.md).

`results/` es donde caen los logs de `wrk`, los snapshots de `/metrics` y el
CSV de RSS de cada corrida (evidencia para el criterio B.4 del plan).
