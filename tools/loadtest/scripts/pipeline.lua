-- Pipelining real de HTTP/1.1: concatena `depth` peticiones en un solo
-- envío al socket en vez de una request-espera-response por conexión.
-- Ejercita el Escenario 2 de la Parte B en docs/LOAD_AND_FUZZ_PLAN.md
-- ("valida el orden de respuestas HTTP/1.1 bajo presión").
--
-- Uso: wrk -s pipeline.lua -- <depth> <url>
-- (depth por defecto: 16 si no se pasa)

local depth = 16

function init(args)
    if args[1] then
        depth = tonumber(args[1]) or depth
    end

    local requests = {}
    for i = 1, depth do
        -- nil en cada campo: usa method/path/headers/body por defecto de wrk
        -- (GET a la ruta del URL pasado en la línea de comandos).
        requests[i] = wrk.format(nil, nil, nil, nil)
    end
    req = table.concat(requests)
end

function request()
    return req
end
