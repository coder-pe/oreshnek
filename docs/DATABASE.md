# Capa de persistencia — abstracción y PostgreSQL

Estado: **implementado** (Fase 5 del [ROADMAP](ROADMAP.md)). PostgreSQL es la base
de datos principal; SQLite3 se mantiene como backend ligero (desarrollo, tests,
despliegues embebidos). Este documento describe el diseño efectivamente
implementado.

## Objetivos

- **Una abstracción, dos backends concretos**: SQLite3 (ya existente) y PostgreSQL
  (nuevo, vía `libpq`). Extensible en el futuro a Oracle, MySQL, MongoDB,
  ClickHouse, DB2, etc.
- **Sin `virtual` / herencia de interfaz**. Se usa **polimorfismo estático**:
  **CRTP** + **concepts** (C++20) para el contrato, y `std::variant` + `std::visit`
  para la selección en tiempo de ejecución sin vtables.
- **Selección por configuración** (`db.backend = "postgres" | "sqlite"`), sin
  recompilar.
- **Misma política de dependencias**: `libpq` es el cliente C **oficial** de
  PostgreSQL, extremadamente maduro y sin dependencias extra (encaja con la
  política de "solo librerías muy maduras"). No se añade `libpqxx`.

## Diseño

### 1) Contrato (concept) + base CRTP

El contrato se expresa con un `concept` de C++20 y la base CRTP reenvía cada
operación pública a un método `*_impl` del concreto (resuelto en compilación, sin
despacho dinámico):

```cpp
// Modelos compartidos (User, Video, Comment) en oreshnek/platform/Models.h
template <typename T>
concept DatabaseBackend = requires(T b, const User& u, const std::string& s,
                                   int limit, int offset, const std::string& cat) {
    { b.create_user_impl(u) }            -> std::same_as<bool>;
    { b.user_by_username_impl(s) }       -> std::same_as<User>;
    { b.create_video_impl(std::declval<const Video&>()) } -> std::same_as<bool>;
    { b.videos_impl(limit, offset, cat) }-> std::same_as<std::vector<Video>>;
    { b.increment_views_impl(limit) }    -> std::same_as<bool>;
    // ... se amplía con cada operación nueva
};

template <typename Derived>
class DatabaseBase {
public:
    bool createUser(const User& u)              { return self().create_user_impl(u); }
    User getUserByUsername(const std::string& n){ return self().user_by_username_impl(n); }
    bool createVideo(const Video& v)            { return self().create_video_impl(v); }
    std::vector<Video> getVideos(int l, int o, const std::string& c)
                                                { return self().videos_impl(l, o, c); }
    bool incrementViews(int id)                 { return self().increment_views_impl(id); }
protected:
    Derived&       self()       { return static_cast<Derived&>(*this); }
    const Derived& self() const { return static_cast<const Derived&>(*this); }
};
```

### 2) Backends concretos

```cpp
class SqliteBackend : public DatabaseBase<SqliteBackend> {
    friend class DatabaseBase<SqliteBackend>;
    SqlitePool pool_;                       // el pool WAL actual (Fase 4)
    bool create_user_impl(const User&);
    User user_by_username_impl(const std::string&);
    // ... DDL y SQL en dialecto SQLite (placeholders '?', AUTOINCREMENT)
};

class PgBackend : public DatabaseBase<PgBackend> {
    friend class DatabaseBase<PgBackend>;
    PgPool pool_;                           // pool de conexiones libpq (nuevo)
    bool create_user_impl(const User&);
    User user_by_username_impl(const std::string&);
    // ... DDL y SQL en dialecto PostgreSQL (placeholders $1, SERIAL/IDENTITY,
    //     RETURNING id)
};

static_assert(DatabaseBackend<SqliteBackend>);
static_assert(DatabaseBackend<PgBackend>);
```

### 3) Frontera con selección en runtime (sin virtual)

`DatabaseManager` deja de ser un backend concreto y pasa a ser la **frontera**:
mantiene un `std::variant` del backend elegido y despacha con `std::visit`
(switch generado, sin vtables ni herencia dinámica).

```cpp
class DatabaseManager {
    std::variant<SqliteBackend, PgBackend> backend_;
public:
    explicit DatabaseManager(const ServerConfig& cfg); // construye la alternativa elegida

    bool createUser(const User& u) {
        return std::visit([&](auto& b){ return b.createUser(u); }, backend_);
    }
    User getUserByUsername(const std::string& n) {
        return std::visit([&](auto& b){ return b.getUserByUsername(n); }, backend_);
    }
    // ... resto de operaciones, todas vía std::visit
};
```

Añadir un backend futuro (p. ej. `MySqlBackend`) = crear el concreto que cumple el
concept y añadirlo al `std::variant`. Ningún call-site cambia.

> Alternativa considerada: parametrizar toda la app sobre el backend (CRTP puro,
> selección en **compilación**, coste cero, sin `std::variant`). Se descarta como
> opción por defecto porque impide elegir la base de datos por configuración. Queda
> disponible si en algún despliegue se prefiere fijar el backend en compilación.

## Pool de conexiones PostgreSQL (`PgPool`)

Análogo a `SqlitePool` (Fase 4): N conexiones `PGconn*` abiertas con una cadena de
conexión, checkout/checkin con RAII sobre mutex + condvar.

- Conexión: `PQconnectdb(conninfo)`; se valida `PQstatus(c) == CONNECTION_OK`.
- **Reconexión**: al devolver una conexión caída (`CONNECTION_BAD`) se intenta
  `PQreset()`; si falla, se reabre.
- **Consultas parametrizadas siempre** con `PQexecParams` / `PQexecPrepared`
  (placeholders `$1, $2, ...`) — nunca concatenación de strings → anti SQL
  injection.
- Tipos: se transfieren como texto (formato 0) y se convierten en C++ (los modelos
  ya son `std::string`/`int`); `INSERT ... RETURNING id` para recuperar ids.
- Wrappers RAII propios para `PGconn*` y `PGresult*` (cierre con `PQfinish` /
  `PQclear`), en línea con la preferencia de no usar `virtual`.

## Esquema por dialecto

Los modelos (`User`, `Video`, `Comment`) se comparten; el **DDL y el SQL son por
backend**. Diferencias principales:

| Aspecto            | SQLite3                      | PostgreSQL                         |
|--------------------|------------------------------|------------------------------------|
| Placeholders       | `?`                          | `$1, $2, ...`                      |
| Autoincremento     | `INTEGER PRIMARY KEY AUTOINCREMENT` | `GENERATED ALWAYS AS IDENTITY` / `SERIAL` |
| Id tras INSERT     | `sqlite3_last_insert_rowid`  | `INSERT ... RETURNING id`          |
| Booleanos          | `INTEGER 0/1`                | `BOOLEAN`                          |
| Timestamp          | `DATETIME DEFAULT CURRENT_TIMESTAMP` | `TIMESTAMPTZ DEFAULT now()` |

Cada backend posee su propio `initialize_tables_impl()` con el DDL de su dialecto.

## Configuración

`ServerConfig` gana una sección de base de datos (cargada por `Platform::Config`):

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

**Secretos fuera del fichero** (igual que el JWT en Fase 4):

- `ORESHNEK_PG_PASSWORD` — contraseña de PostgreSQL.
- `ORESHNEK_DATABASE_URL` — si está presente, una URL `postgresql://...` completa
  tiene prioridad sobre los campos sueltos.

## Seguridad

- Consultas **siempre** parametrizadas (`$n`) — sin concatenar entrada del usuario.
- `sslmode` configurable (`prefer`/`require`/`verify-full`) para cifrado en
  tránsito; recomendado `require`+ en producción.
- Contraseña por entorno/URL, nunca en el repositorio.
- Principio de mínimo privilegio para el rol de la aplicación (documentado, no
  forzado por el código).

## Plan de implementación (commits previstos)

Cada commit compila y mantiene la suite verde (incl. ASan/UBSan + TSan):

1. **Modelos + contrato**: extraer `User`/`Video`/`Comment` a `Models.h`; añadir el
   `concept DatabaseBackend` y la base CRTP `DatabaseBase`.
2. **SqliteBackend**: mover la lógica SQLite actual de `DatabaseManager` a
   `SqliteBackend` (reutiliza `SqlitePool`); `DatabaseManager` pasa a `std::variant`
   con solo la alternativa SQLite. `db_test` sigue verde sin cambios de
   comportamiento.
3. **PgPool**: pool de conexiones `libpq` con RAII y reconexión; `find_package(PostgreSQL)`
   en CMake.
4. **PgBackend**: implementación de las operaciones con `PQexecParams` y DDL
   PostgreSQL; añadir al `std::variant`; selección por `db.backend`.
5. **Configuración**: sección `db` en `ServerConfig`/`Config` + overrides por
   entorno; `config/oreshnek.example.json` actualizado.
6. **Tests**: `pg_test` (mismas aserciones que `db_test`) que se **salta** si no hay
   PostgreSQL disponible (`ORESHNEK_PG_TEST_DSN` sin definir), para no romper CI sin
   servidor. Opcional: servicio Postgres en el contenedor de CI.
7. **Docs**: actualizar `ARCHITECTURE.md` y `ROADMAP.md` (este documento ya fija el
   diseño).

## Extensibilidad futura

Cada nueva base de datos (Oracle, MySQL, MongoDB, ClickHouse, DB2, ...) se añade
como un nuevo concreto que satisface el `concept`, con su propio pool/cliente y
dialecto, y se incorpora al `std::variant`. Para almacenes no-SQL (MongoDB) el
concept puede dividirse en sub-contratos (p. ej. operaciones documentales) sin
introducir `virtual`.
