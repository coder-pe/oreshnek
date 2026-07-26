-- Fuerza una cabecera Range en cada petición, para el Escenario 3 de la
-- Parte B en docs/LOAD_AND_FUZZ_PLAN.md ("Estático + Range -> valida ruta
-- sendfile/206 bajo carga"). El servidor debe responder 206 Partial Content
-- con Content-Range; si el fichero servido es menor a 1024 bytes, ajusta el
-- rango de abajo (o pasa un fichero más grande al escenario).

wrk.headers["Range"] = "bytes=0-1023"
