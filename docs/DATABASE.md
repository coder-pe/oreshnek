# Capa de persistencia — gateway SQL genérico y backends

Estado: **implementado**. La capa de base de datos del framework es un **gateway
SQL agnóstico del dominio**: ejecuta sentencias parametrizadas y devuelve filas
genéricas. **No conoce ningún modelo de negocio** (usuarios, vídeos, pedidos…).
Las aplicaciones definen sus propios modelos y su propio esquema, y mapean las
filas genéricas (`SqlResult`) a sus structs. PostgreSQL es la base de datos
principal; SQLite3 se mantiene como backend ligero (desarrollo, tests,
despliegues embebidos); Oracle (vía OCI) está disponible para entornos
corporativos que ya corren sobre esa base de datos.

**Cada backend es opt-in en CMake** (`ORESHNEK_WITH_SQLITE` /
`ORESHNEK_WITH_POSTGRES` / `ORESHNEK_WITH_ORACLE`, todas `OFF` por defecto): el
build solo enlaza y compila el cliente del backend que realmente se habilita, y
CMake falla si no se activa ninguno. Ver la sección
[Compilación: backends opt-in](#compilación-backends-opt-in) más abajo.

> Un ejemplo completo de cómo construir un dominio (una plataforma de vídeo:
> usuarios + vídeos + repositorio) sobre este gateway está en
> [`examples/06_video_platform.cpp`](../examples/06_video_platform.cpp). El CRUD
> mínimo con autenticación está en
> [`examples/03_rest_crud_db.cpp`](../examples/03_rest_crud_db.cpp).

## Objetivos

- **Genérico y de propósito general**: el framework no impone modelos. Expone
  `query()` / `exec()` con consultas parametrizadas y un resultado uniforme.
- **Una abstracción, tres backends concretos**: SQLite3, PostgreSQL (vía `libpq`)
  y Oracle (vía OCI). Extensible a MySQL, MongoDB, ClickHouse, DB2, etc.
- **Sin `virtual` / herencia de interfaz**. Polimorfismo estático: **CRTP** +
  **concepts** (C++20) para el contrato — con *fallback* automático a un trait
  SFINAE equivalente cuando se compila en C++17 (`ORESHNEK_CXX_STANDARD=17`) —
  y `std::variant` + `std::visit` para la selección en tiempo de ejecución, sin
  vtables.
- **Selección por configuración** (`db.backend = "postgres" | "sqlite" | "oracle"`),
  sin recompilar.
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

**Portabilidad C++17**: cuando se compila con `ORESHNEK_CXX_STANDARD=17`
(`include/oreshnek/platform/DatabaseBackend.h` detecta la ausencia de
`__cpp_concepts`), `DatabaseBackend` deja de ser un `concept` y pasa a ser un
`inline constexpr bool` calculado con un trait SFINAE (`std::void_t` +
`std::is_same` sobre el tipo de retorno de `run_impl`). La sintaxis en los
call-sites (`static_assert(DatabaseBackend<SqliteBackend>)`, etc.) no cambia en
ningún caso — solo cambia qué hay detrás del nombre según el estándar.

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

class OracleBackend : public DatabaseBase<OracleBackend> {
    OraclePool pool_;                       // pool de sesiones OCI
public:
    SqlResult run_impl(std::string_view sql, const SqlParams&);  // OCIStmtPrepare/Execute
};

static_assert(DatabaseBackend<SqliteBackend>);
static_assert(DatabaseBackend<PgBackend>);
static_assert(DatabaseBackend<OracleBackend>);
```

Cada uno de estos tres `.cpp`/`.h` (y su pool) solo se compila si el flag de
CMake correspondiente está activo — ver
[Compilación: backends opt-in](#compilación-backends-opt-in).

### 3) Frontera con selección en runtime (sin virtual)

`DatabaseManager` es la **frontera**: mantiene un `std::variant` del backend
elegido y despacha con `std::visit` (switch generado, sin vtables). Solo expone
el gateway genérico:

```cpp
class DatabaseManager {
    // std::monostate siempre es la primera alternativa (nunca se activa en
    // runtime) para que el preprocesador pueda anteponer cada backend con una
    // coma líder sin tener que distinguir "cuál va primero" según qué flags de
    // CMake estén activos.
    std::variant<std::monostate
#if defined(ORESHNEK_WITH_SQLITE)
                , std::unique_ptr<SqliteBackend>
#endif
#if defined(ORESHNEK_WITH_POSTGRES)
                , std::unique_ptr<PgBackend>
#endif
#if defined(ORESHNEK_WITH_ORACLE)
                , std::unique_ptr<OracleBackend>
#endif
                > backend_;
public:
    explicit DatabaseManager(const ServerConfig& cfg);  // construye la alternativa elegida
    SqlResult query(std::string_view sql, const SqlParams& p = {});
    SqlResult exec (std::string_view sql, const SqlParams& p = {});
};
```

(Los pools tienen mutex/condvar no movibles, por eso las alternativas viven tras
`unique_ptr`, manteniendo el `std::variant` movible.) `query()`/`exec()` usan
`std::visit` con un `if constexpr` que descarta la rama `std::monostate` en
compilación para cada alternativa real; en runtime nunca se alcanza (lanzaría
`std::logic_error` si ocurriera). `make_backend()` recorre solo los `if
defined(...)` de los backends compilados y lanza `std::runtime_error` si
`db.backend` pide uno que este build no incluye. Añadir un backend futuro (p.
ej. `MySqlBackend`) = crear el concreto que cumple el concept, añadirlo al
`std::variant` tras su propio `#if defined(...)`, y sumar su rama en
`make_backend()`. Ningún call-site cambia.

## Portabilidad del SQL: placeholders `?`

Las sentencias usan **placeholders posicionales `?`**, ligados por posición desde
`SqlParams`. El backend PostgreSQL los traduce a la forma `$1, $2, ...` de libpq
y el backend Oracle a la forma `:1, :2, ...` de OCI (respetando en ambos casos
los `?` dentro de literales de cadena), de modo que el mismo SQL corre sin
cambios en los tres backends. Lo que sí cambia entre dialectos es el **DDL** y
algunas funciones; eso es responsabilidad de la aplicación.

| Aspecto            | SQLite3                      | PostgreSQL                         | Oracle                              |
|--------------------|------------------------------|-------------------------------------|--------------------------------------|
| Placeholders (API) | `?`                          | `?` (traducido a `$n`)             | `?` (traducido a `:n`)              |
| Autoincremento     | `INTEGER PRIMARY KEY AUTOINCREMENT` | `SERIAL` / `GENERATED ... IDENTITY` | `GENERATED [BY DEFAULT] AS IDENTITY` |
| Id tras INSERT     | `last_insert_id` del resultado | `INSERT ... RETURNING id`        | no soportado por esta API genérica (ver abajo) |
| Booleanos          | `INTEGER 0/1`                | `BOOLEAN` (`boolean()` los unifica) | `NUMBER(1)` 0/1 (`boolean()` los unifica) |
| Timestamp          | `DATETIME DEFAULT CURRENT_TIMESTAMP` | `TIMESTAMPTZ DEFAULT now()` | `TIMESTAMP DEFAULT SYSTIMESTAMP`   |
| `;` final en sentencias simples | tolerado (ignorado) | tolerado (ignorado) | **rechazado** (ORA-00911) — no lo incluyas |

**Id tras INSERT en Oracle**: `RETURNING id INTO :out` de Oracle necesita un
bind de salida (OUT), que esta interfaz genérica de solo-entrada (`?`
posicionales) no expone. La forma portable es leer el `IDENTITY` recién
insertado con un `SELECT` de seguimiento (como hace
[`tests/oracle_test.cpp`](../tests/oracle_test.cpp)), o usar
`SELECT secuencia.CURRVAL FROM dual`.

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

## Pool de sesiones Oracle (`OraclePool`)

Sigue el mismo patrón que `SqlitePool`/`PgPool`, adaptado a OCI:

- Un único `OCIEnv` (entorno OCI) compartido por todo el pool, creado con
  `OCIEnvCreate(..., OCI_THREADED, ...)` para permitir uso concurrente desde
  varios hilos.
- Cada sesión del pool tiene su **propio `OCIError`**: los handles de error de
  OCI no son seguros de compartir entre conexiones que se ejecutan
  simultáneamente en distintos hilos.
- Conexión: `OCILogon2(env, err, &svc, user, pass, connect_string, OCI_DEFAULT)`.
- Checkout/checkin con RAII sobre mutex + condvar, igual que los otros pools.
- **Sin reconexión automática** de sesiones caídas (a diferencia de `PgPool`):
  una sesión Oracle perdida simplemente fallará en la siguiente ejecución; es
  una limitación conocida de esta primera versión.
- `OracleBackend::run_impl` traduce `?` → `:1, :2, ...`, enlaza siempre como
  texto (`SQLT_STR` — Oracle convierte al tipo real de la columna) y despacha
  según el tipo de sentencia (`OCI_ATTR_STMT_TYPE`): `SELECT` usa
  `OCIDefineByPos` + `OCIStmtFetch2`; el resto (DML/DDL/PL-SQL) ejecuta con
  `OCI_COMMIT_ON_SUCCESS`, replicando el autocommit por sentencia de SQLite y
  PostgreSQL.
- **Limitación conocida**: columnas `CLOB`/`BLOB` no están soportadas por esta
  ruta de fetch basada en texto (requeriría streaming vía `OCILobRead`, fuera
  del alcance de esta primera integración).

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
  },
  "oracle": {
    "connect_string": "localhost:1521/XEPDB1",
    "user": "oreshnek", "password": "", "pool_size": 8
  }
}
```

`connect_string` es un **Easy Connect** (`host:puerto/service_name`) o un alias
TNS resoluble vía `tnsnames.ora` (directorio `network/admin` del Instant
Client, o `$TNS_ADMIN`).

**Secretos fuera del fichero**:

- `ORESHNEK_PG_PASSWORD` — contraseña de PostgreSQL.
- `ORESHNEK_ORACLE_PASSWORD` — contraseña de Oracle.
- `ORESHNEK_DATABASE_URL` — si está presente, una URL `postgresql://...` completa
  tiene prioridad sobre los campos sueltos.

## Compilación: backends opt-in

Los tres backends son **opcionales en CMake** y **ninguno está activo por
defecto**: hay que habilitar explícitamente el (los) que el proyecto necesite,
o la configuración de CMake falla con un `FATAL_ERROR`.

Cada backend necesita sus headers/librería de cliente instalados a nivel de
sistema operativo antes de configurar CMake (`sqlite3`, `libpq`, o el Instant
Client de Oracle). Comandos de instalación para macOS, Debian/Ubuntu,
Fedora/RHEL/CentOS Stream y Arch Linux: [`docs/DEPENDENCIES.md`](DEPENDENCIES.md).

| Opción CMake | Por defecto | Efecto |
|--------------|-------------|--------|
| `ORESHNEK_WITH_SQLITE`   | `OFF` | `find_package(SQLite3 REQUIRED)`, compila `SqliteBackend`/`SqlitePool`, define `ORESHNEK_WITH_SQLITE`. |
| `ORESHNEK_WITH_POSTGRES` | `OFF` | `find_package(PostgreSQL REQUIRED)`, compila `PgBackend`/`PgPool`, define `ORESHNEK_WITH_POSTGRES`. |
| `ORESHNEK_WITH_ORACLE`   | `OFF` | Localiza `oci.h`/`libclntsh.so` bajo `ORESHNEK_ORACLE_HOME`, compila `OracleBackend`/`OraclePool`, define `ORESHNEK_WITH_ORACLE`. |
| `ORESHNEK_ORACLE_HOME`   | `/home/miguel/oracle/instantclient_23_26` | Ruta al Instant Client (debe contener `sdk/include/oci.h` y `libclntsh.so`). Sobreescribible con `-D` o `$ORACLE_HOME`. |

```bash
# Un solo backend
cmake -B build -DORESHNEK_WITH_SQLITE=ON

# Varios a la vez (el proyecto puede soportar múltiples BD en el mismo binario;
# ServerConfig::db.backend elige cuál se usa en runtime)
cmake -B build -DORESHNEK_WITH_POSTGRES=ON -DORESHNEK_WITH_ORACLE=ON

# Oracle con un Instant Client en otra ruta
cmake -B build -DORESHNEK_WITH_ORACLE=ON -DORESHNEK_ORACLE_HOME=/opt/oracle/instantclient_19_20
```

Mecanismo interno: `src/*.cpp` se filtra por expresión regular según los flags
(`Sqlite(Backend|Pool)\.cpp`, `Pg(Backend|Pool)\.cpp`, `Oracle(Backend|Pool)\.cpp`)
antes de compilar la librería `oreshnek`, y cada flag activo añade
`target_compile_definitions(oreshnek PUBLIC ORESHNEK_WITH_<X>)`. Como es
`PUBLIC`, todo lo que enlaza contra `oreshnek` (tests, ejemplos, el propio
`DatabaseManager.h`) ve la misma macro y solo incluye/instancia el backend
compilado — ver la sección de `std::variant` más arriba.

Los tests `db_test` (SQLite), `pg_test` (PostgreSQL) y `oracle_test` (Oracle)
también están condicionados por sus respectivos flags; `pg_test` y
`oracle_test` además se saltan en runtime si no hay una base de datos real
accesible vía variables de entorno (`ORESHNEK_PG_TEST_DSN` /
`ORESHNEK_ORACLE_TEST_CONNECT`+`_USER`+`_PASSWORD`).

## Seguridad

- Consultas **siempre** parametrizadas — sin concatenar entrada del usuario.
- `sslmode` configurable (`prefer`/`require`/`verify-full`) para cifrado en
  tránsito en PostgreSQL; recomendado `require`+ en producción. Oracle cifra
  en tránsito según la configuración de red/`sqlnet.ora` del cliente
  (fuera del control de este código).
- Contraseña por entorno/URL, nunca en el repositorio.
- Principio de mínimo privilegio para el rol de la aplicación (documentado, no
  forzado por el código).

## Extensibilidad futura

Cada nueva base de datos (MySQL, MongoDB, ClickHouse, DB2, ...) se añade como un
nuevo concreto que satisface el `concept` (implementa `run_impl`), con su propio
pool/cliente, se incorpora al `std::variant` tras su propio `#if defined(...)`,
y se le da un flag de CMake opt-in propio (siguiendo el patrón de
`ORESHNEK_WITH_ORACLE`). Para almacenes no-SQL el primitivo puede generalizarse
o complementarse con otro contrato sin introducir `virtual`.
