# Capa de persistencia — gateway SQL genérico y backends

Estado: **implementado**. La capa de base de datos del framework es un **gateway
SQL agnóstico del dominio**: ejecuta sentencias parametrizadas y devuelve filas
genéricas. **No conoce ningún modelo de negocio** (usuarios, vídeos, pedidos…).
Las aplicaciones definen sus propios modelos y su propio esquema, y mapean las
filas genéricas (`SqlResult`) a sus structs. PostgreSQL es la base de datos
principal; SQLite3 se mantiene como backend ligero (desarrollo, tests,
despliegues embebidos).

> Un ejemplo completo de cómo construir un dominio (una plataforma de vídeo:
> usuarios + vídeos + repositorio) sobre este gateway está en
> [`examples/06_video_platform.cpp`](../examples/06_video_platform.cpp). El CRUD
> mínimo con autenticación está en
> [`examples/03_rest_crud_db.cpp`](../examples/03_rest_crud_db.cpp).

## Objetivos

- **Genérico y de propósito general**: el framework no impone modelos. Expone
  `query()` / `exec()` con consultas parametrizadas y un resultado uniforme.
- **Una abstracción, dos backends concretos**: SQLite3 y PostgreSQL (vía `libpq`).
  Extensible a Oracle, MySQL, MongoDB, ClickHouse, DB2, etc.
- **Sin `virtual` / herencia de interfaz**. Polimorfismo estático: **CRTP** +
  **concepts** (C++20) para el contrato, y `std::variant` + `std::visit` para la
  selección en tiempo de ejecución, sin vtables.
- **Selección por configuración** (`db.backend = "postgres" | "sqlite"`), sin
  recompilar.
- **Seguridad por defecto**: siempre consultas parametrizadas (nunca concatenación)
  → anti SQL injection.

## Tipos genéricos

```cpp
// oreshnek/platform/SqlResult.h
using SqlParam  = std::optional<std::string>;  // std::nullopt => SQL NULL
using SqlParams = std::vector<SqlParam>;

class SqlResult {
public:
    bool ok = false;            // false ante un error del driver (mensaje en error)
    std::string error;
    std::int64_t affected = 0;        // filas afectadas (INSERT/UPDATE/DELETE)
    std::int64_t last_insert_id = 0;  // rowid nuevo (SQLite; PG solo con RETURNING)
    std::vector<std::string> columns;
    std::vector<std::vector<std::optional<std::string>>> rows;  // texto; nullopt = NULL

    std::size_t row_count() const;  bool empty() const;
    int  column(std::string_view name) const;        // índice por nombre, -1 si no existe
    bool is_null(std::size_t row, std::size_t col) const;
    std::string_view text  (std::size_t row, std::size_t col) const;
    std::int64_t     integer(std::size_t row, std::size_t col, std::int64_t def = 0) const;
    double           real   (std::size_t row, std::size_t col, double def = 0.0) const;
    bool             boolean(std::size_t row, std::size_t col) const;  // 0/1 o t/f
};
```

Los valores se guardan como **texto** tal cual los devuelve el driver; los
accesores tipados parsean bajo demanda, de modo que el código de aplicación
nunca toca tipos de columna específicos del backend ni el manejo de NULL.

## Diseño

### 1) Contrato (concept) + base CRTP

El contrato se reduce a **un único primitivo** que ejecuta una sentencia
parametrizada y devuelve un `SqlResult`. La base CRTP reenvía la API pública al
método `run_impl` del concreto (resuelto en compilación, sin despacho dinámico):

```cpp
template <typename T>
concept DatabaseBackend = requires(T b, std::string_view sql, const SqlParams& p) {
    { b.run_impl(sql, p) } -> std::same_as<SqlResult>;
};

template <typename Derived>
class DatabaseBase {
public:
    // query() y exec() son alias semánticos del mismo primitivo.
    SqlResult query(std::string_view sql, const SqlParams& p = {}) { return self().run_impl(sql, p); }
    SqlResult exec (std::string_view sql, const SqlParams& p = {}) { return self().run_impl(sql, p); }
protected:
    Derived&       self()       { return static_cast<Derived&>(*this); }
    const Derived& self() const { return static_cast<const Derived&>(*this); }
};
```

### 2) Backends concretos

```cpp
class SqliteBackend : public DatabaseBase<SqliteBackend> {
    SqlitePool pool_;                       // pool WAL
public:
    SqlResult run_impl(std::string_view sql, const SqlParams&);  // prepare/bind/step
};

class PgBackend : public DatabaseBase<PgBackend> {
    PgPool pool_;                           // pool libpq
public:
    SqlResult run_impl(std::string_view sql, const SqlParams&);  // PQexecParams
};

static_assert(DatabaseBackend<SqliteBackend>);
static_assert(DatabaseBackend<PgBackend>);
```

### 3) Frontera con selección en runtime (sin virtual)

`DatabaseManager` es la **frontera**: mantiene un `std::variant` del backend
elegido y despacha con `std::visit` (switch generado, sin vtables). Solo expone
el gateway genérico:

```cpp
class DatabaseManager {
    std::variant<std::unique_ptr<SqliteBackend>, std::unique_ptr<PgBackend>> backend_;
public:
    explicit DatabaseManager(const ServerConfig& cfg);  // construye la alternativa elegida
    SqlResult query(std::string_view sql, const SqlParams& p = {});
    SqlResult exec (std::string_view sql, const SqlParams& p = {});
};
```

(Los pools tienen mutex/condvar no movibles, por eso las alternativas viven tras
`unique_ptr`, manteniendo el `std::variant` movible.) Añadir un backend futuro
(p. ej. `MySqlBackend`) = crear el concreto que cumple el concept y añadirlo al
`std::variant`. Ningún call-site cambia.

## Portabilidad del SQL: placeholders `?`

Las sentencias usan **placeholders posicionales `?`**, ligados por posición desde
`SqlParams`. El backend PostgreSQL los traduce a la forma `$1, $2, ...` de libpq
(respetando los `?` dentro de literales de cadena), de modo que el mismo SQL corre
sin cambios en ambos backends. Lo que sí cambia entre dialectos es el **DDL** y
algunas funciones; eso es responsabilidad de la aplicación.

| Aspecto            | SQLite3                      | PostgreSQL                         |
|--------------------|------------------------------|------------------------------------|
| Placeholders (API) | `?`                          | `?` (traducido a `$n`)             |
| Autoincremento     | `INTEGER PRIMARY KEY AUTOINCREMENT` | `SERIAL` / `GENERATED ... IDENTITY` |
| Id tras INSERT     | `last_insert_id` del resultado | `INSERT ... RETURNING id`        |
| Booleanos          | `INTEGER 0/1`                | `BOOLEAN` (`boolean()` los unifica) |
| Timestamp          | `DATETIME DEFAULT CURRENT_TIMESTAMP` | `TIMESTAMPTZ DEFAULT now()` |

## Uso (patrón de repositorio en la aplicación)

```cpp
Platform::DatabaseManager db(config);

// La aplicación corre su propio DDL.
db.exec("CREATE TABLE IF NOT EXISTS notes ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL);");

// Escritura parametrizada.
auto ins = db.exec("INSERT INTO notes (title) VALUES (?);", {"hola"});
long long id = ins.last_insert_id;

// Lectura + mapeo a un modelo propio.
auto r = db.query("SELECT id, title FROM notes WHERE id = ?;", {std::to_string(id)});
if (r.ok && !r.empty()) {
    long long got_id      = r.integer(0, 0);
    std::string got_title = std::string(r.text(0, 1));
}
```

## Pool de conexiones PostgreSQL (`PgPool`)

Análogo a `SqlitePool`: N conexiones `PGconn*` abiertas con una cadena de conexión,
checkout/checkin con RAII sobre mutex + condvar.

- Conexión: `PQconnectdb(conninfo)`; se valida `PQstatus(c) == CONNECTION_OK`.
- **Reconexión**: al devolver una conexión caída (`CONNECTION_BAD`) se intenta
  `PQreset()`; si falla, se reabre.
- **Consultas parametrizadas siempre** con `PQexecParams` — nunca concatenación.
- Tipos transferidos como texto (formato 0); el `SqlResult` los expone como texto
  con accesores tipados.
- Wrappers RAII propios para `PGconn*` y `PGresult*` (`PQfinish` / `PQclear`).

## Configuración

`ServerConfig` tiene una sección de base de datos (cargada por `Platform::Config`):

```json
"db": {
  "backend": "postgres",
  "sqlite":   { "path": "./database.db", "pool_size": 4, "busy_timeout_ms": 5000 },
  "postgres": {
    "host": "127.0.0.1", "port": 5432, "dbname": "oreshnek",
    "user": "oreshnek", "password": "", "sslmode": "prefer",
    "pool_size": 8, "connect_timeout_sec": 5
  }
}
```

**Secretos fuera del fichero**:

- `ORESHNEK_PG_PASSWORD` — contraseña de PostgreSQL.
- `ORESHNEK_DATABASE_URL` — si está presente, una URL `postgresql://...` completa
  tiene prioridad sobre los campos sueltos.

## Seguridad

- Consultas **siempre** parametrizadas — sin concatenar entrada del usuario.
- `sslmode` configurable (`prefer`/`require`/`verify-full`) para cifrado en
  tránsito; recomendado `require`+ en producción.
- Contraseña por entorno/URL, nunca en el repositorio.
- Principio de mínimo privilegio para el rol de la aplicación (documentado, no
  forzado por el código).

## Extensibilidad futura

Cada nueva base de datos (Oracle, MySQL, MongoDB, ClickHouse, DB2, ...) se añade
como un nuevo concreto que satisface el `concept` (implementa `run_impl`), con su
propio pool/cliente, y se incorpora al `std::variant`. Para almacenes no-SQL el
primitivo puede generalizarse o complementarse con otro contrato sin introducir
`virtual`.
