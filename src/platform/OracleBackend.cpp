// oreshnek/src/platform/OracleBackend.cpp
#include "oreshnek/platform/OracleBackend.h"
#include "oreshnek/utils/Logger.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace Oreshnek {
namespace Platform {

namespace {

// Translate positional `?` placeholders to Oracle's `:1, :2, ...` bind syntax.
// A `?` inside a single-quoted string literal is data, not a placeholder, so it
// is left intact (with the standard '' escape handled). This lets applications
// write portable SQL once and run it on every backend.
std::string to_oracle_placeholders(std::string_view sql) {
    std::string out;
    out.reserve(sql.size() + 8);
    int n = 0;
    bool in_string = false;
    for (std::size_t i = 0; i < sql.size(); ++i) {
        const char c = sql[i];
        if (in_string) {
            out += c;
            if (c == '\'') {
                if (i + 1 < sql.size() && sql[i + 1] == '\'') {  // escaped quote ''
                    out += '\'';
                    ++i;
                } else {
                    in_string = false;
                }
            }
            continue;
        }
        if (c == '\'') {
            in_string = true;
            out += c;
        } else if (c == '?') {
            out += ':';
            out += std::to_string(++n);
        } else {
            out += c;
        }
    }
    return out;
}

std::string oci_error_message(OCIError* err, sword status) {
    if (status == OCI_SUCCESS) return {};
    text buf[OCI_ERROR_MAXMSG_SIZE] = {};
    sb4 code = 0;
    if (err != nullptr &&
        OCIErrorGet(err, 1, nullptr, &code, buf, sizeof(buf), OCI_HTYPE_ERROR) == OCI_SUCCESS) {
        return std::string(reinterpret_cast<char*>(buf));
    }
    return "Oracle error (status=" + std::to_string(status) + ")";
}

// RAII for an OCI statement handle. Uses the classic OCIHandleAlloc +
// OCIStmtPrepare pair rather than the statement-cache API (OCIStmtPrepare2),
// keeping this on par with the simple, uncached per-call style of the SQLite
// and PostgreSQL backends.
class OciStmt {
public:
    explicit OciStmt(OCIEnv* env) {
        OCIHandleAlloc(env, reinterpret_cast<void**>(&stmt_), OCI_HTYPE_STMT, 0, nullptr);
    }
    ~OciStmt() { if (stmt_ != nullptr) OCIHandleFree(stmt_, OCI_HTYPE_STMT); }
    OciStmt(const OciStmt&) = delete;
    OciStmt& operator=(const OciStmt&) = delete;
    OCIStmt* get() const { return stmt_; }

private:
    OCIStmt* stmt_ = nullptr;
};

}  // namespace

OracleBackend::OracleBackend(const DatabaseConfig& db)
    : pool_(db.ora_connect_string, db.ora_user, db.ora_password, db.ora_pool_size) {}

SqlResult OracleBackend::run_impl(std::string_view sql, const SqlParams& params) {
    SqlResult result;
    auto conn = pool_.acquire();
    OCISvcCtx* svc = conn.svc();
    OCIError* err = conn.err();

    const std::string translated = to_oracle_placeholders(sql);

    OciStmt stmt(pool_.env());
    if (stmt.get() == nullptr) {
        result.error = "Oracle: OCIHandleAlloc(OCI_HTYPE_STMT) failed";
        ORE_LOG(ERROR) << result.error;
        return result;
    }

    sword status = OCIStmtPrepare(stmt.get(), err,
                                  reinterpret_cast<const OraText*>(translated.c_str()),
                                  static_cast<ub4>(translated.size()), OCI_NTV_SYNTAX,
                                  OCI_DEFAULT);
    if (status != OCI_SUCCESS && status != OCI_SUCCESS_WITH_INFO) {
        result.error = oci_error_message(err, status);
        ORE_LOG(ERROR) << "Oracle prepare error: " << result.error;
        return result;
    }

    // Bind parameters positionally (`:1, :2, ...`). Every value is bound as
    // text (SQLT_STR); Oracle coerces it to the column's real type, matching
    // the driver-agnostic, text-based SqlParam contract. The indicator array
    // must outlive OCIStmtExecute below, hence it is declared in this scope.
    std::vector<sb2> bind_indicators(params.size(), 0);
    std::vector<OCIBind*> binds(params.size(), nullptr);
    for (std::size_t i = 0; i < params.size(); ++i) {
        const ub4 pos = static_cast<ub4>(i) + 1;
        if (params[i].has_value()) {
            status = OCIBindByPos(stmt.get(), &binds[i], err, pos,
                                  const_cast<char*>(params[i]->c_str()),
                                  static_cast<sb4>(params[i]->size() + 1), SQLT_STR, nullptr,
                                  nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
        } else {
            bind_indicators[i] = -1;  // OCI_IND_NULL
            status = OCIBindByPos(stmt.get(), &binds[i], err, pos, nullptr, 0, SQLT_STR,
                                  &bind_indicators[i], nullptr, nullptr, 0, nullptr, OCI_DEFAULT);
        }
        if (status != OCI_SUCCESS && status != OCI_SUCCESS_WITH_INFO) {
            result.error = oci_error_message(err, status);
            ORE_LOG(ERROR) << "Oracle bind error: " << result.error;
            return result;
        }
    }

    ub2 stmt_type = 0;
    OCIAttrGet(stmt.get(), OCI_HTYPE_STMT, &stmt_type, nullptr, OCI_ATTR_STMT_TYPE, err);

    if (stmt_type == OCI_STMT_SELECT) {
        status = OCIStmtExecute(svc, stmt.get(), err, 0, 0, nullptr, nullptr, OCI_DEFAULT);
        if (status != OCI_SUCCESS && status != OCI_SUCCESS_WITH_INFO) {
            result.error = oci_error_message(err, status);
            ORE_LOG(ERROR) << "Oracle execute error: " << result.error;
            return result;
        }

        ub4 col_count = 0;
        OCIAttrGet(stmt.get(), OCI_HTYPE_STMT, &col_count, nullptr, OCI_ATTR_PARAM_COUNT, err);

        // Per-column fetch buffers. Every column is defined as SQLT_STR
        // (Oracle converts the native value to text on fetch), matching the
        // driver-agnostic text representation used by SqlResult. LOB columns
        // (CLOB/BLOB) are not supported by this simple text-define path.
        struct Column {
            std::string name;
            std::vector<char> buffer;
            sb2 indicator = 0;
            ub2 ret_len = 0;
        };
        std::vector<Column> cols(col_count);

        for (ub4 c = 0; c < col_count; ++c) {
            OCIParam* parmdp = nullptr;
            OCIParamGet(stmt.get(), OCI_HTYPE_STMT, err, reinterpret_cast<void**>(&parmdp), c + 1);

            text* name_ptr = nullptr;
            ub4 name_len = 0;
            OCIAttrGet(parmdp, OCI_DTYPE_PARAM, &name_ptr, &name_len, OCI_ATTR_NAME, err);
            cols[c].name.assign(reinterpret_cast<char*>(name_ptr), name_len);

            ub2 data_size = 0;
            OCIAttrGet(parmdp, OCI_DTYPE_PARAM, &data_size, nullptr, OCI_ATTR_DATA_SIZE, err);
            ub4 buf_len = data_size > 0 ? static_cast<ub4>(data_size) : 128;
            buf_len = std::min<ub4>(buf_len * 4 + 1, 32768);  // headroom: multi-byte charsets,
                                                              // numeric/date-to-text conversion
            cols[c].buffer.assign(buf_len, '\0');

            OCIDescriptorFree(parmdp, OCI_DTYPE_PARAM);

            OCIDefine* defnp = nullptr;
            OCIDefineByPos(stmt.get(), &defnp, err, c + 1, cols[c].buffer.data(),
                          static_cast<sb4>(cols[c].buffer.size()), SQLT_STR, &cols[c].indicator,
                          &cols[c].ret_len, nullptr, OCI_DEFAULT);
        }

        result.columns.reserve(col_count);
        for (auto& col : cols) result.columns.push_back(col.name);

        while (true) {
            status = OCIStmtFetch2(stmt.get(), err, 1, OCI_FETCH_NEXT, 0, OCI_DEFAULT);
            if (status == OCI_NO_DATA) break;
            if (status != OCI_SUCCESS && status != OCI_SUCCESS_WITH_INFO) {
                result.error = oci_error_message(err, status);
                ORE_LOG(ERROR) << "Oracle fetch error: " << result.error;
                return result;
            }

            std::vector<std::optional<std::string>> row;
            row.reserve(col_count);
            for (auto& col : cols) {
                if (col.indicator == -1) {
                    row.emplace_back(std::nullopt);
                } else {
                    row.emplace_back(std::string(col.buffer.data(), std::strlen(col.buffer.data())));
                }
            }
            result.rows.push_back(std::move(row));
        }
        result.ok = true;
    } else {
        // DML/DDL/PL-SQL: execute once and auto-commit, mirroring the implicit
        // per-statement autocommit used by the SQLite and PostgreSQL backends.
        status = OCIStmtExecute(svc, stmt.get(), err, 1, 0, nullptr, nullptr,
                                OCI_COMMIT_ON_SUCCESS);
        if (status != OCI_SUCCESS && status != OCI_SUCCESS_WITH_INFO) {
            result.error = oci_error_message(err, status);
            ORE_LOG(ERROR) << "Oracle execute error: " << result.error;
            return result;
        }

        ub4 row_count = 0;
        OCIAttrGet(stmt.get(), OCI_HTYPE_STMT, &row_count, nullptr, OCI_ATTR_ROW_COUNT, err);
        result.affected = static_cast<std::int64_t>(row_count);
        // Oracle has no implicit last-insert-id like SQLite's ROWID. Use an
        // IDENTITY/sequence column with `RETURNING id INTO :out` (an OUT bind,
        // not exposed by this generic ?-params interface) or a follow-up
        // `SELECT sequence.CURRVAL FROM dual` — last_insert_id stays 0 here.
        result.ok = true;
    }

    return result;
}

}  // namespace Platform
}  // namespace Oreshnek
