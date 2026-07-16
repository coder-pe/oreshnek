// oreshnek/src/platform/OraclePool.cpp
#include "oreshnek/platform/OraclePool.h"
#include "oreshnek/utils/Logger.h"

#include <stdexcept>

namespace Oreshnek {
namespace Platform {

namespace {

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

}  // namespace

OraclePool::OraclePool(const std::string& connect_string, const std::string& user,
                       const std::string& password, int size) {
    if (size < 1) size = 1;

    // OCI_THREADED: the environment (and handles derived from it) may be used
    // concurrently from multiple threads, as long as each thread/session uses
    // its own error and statement handles — which is exactly what each pooled
    // Session and OracleBackend::run_impl provide.
    if (OCIEnvCreate(&env_, OCI_THREADED, nullptr, nullptr, nullptr, nullptr, 0, nullptr) !=
        OCI_SUCCESS) {
        throw std::runtime_error("Oracle: OCIEnvCreate failed");
    }

    sessions_.reserve(static_cast<std::size_t>(size));
    try {
        for (int i = 0; i < size; ++i) {
            Session s;
            if (OCIHandleAlloc(env_, reinterpret_cast<void**>(&s.err), OCI_HTYPE_ERROR, 0,
                               nullptr) != OCI_SUCCESS) {
                throw std::runtime_error("Oracle: OCIHandleAlloc(OCI_HTYPE_ERROR) failed");
            }

            const sword status = OCILogon2(
                env_, s.err, &s.svc,
                reinterpret_cast<const OraText*>(user.c_str()), static_cast<ub4>(user.size()),
                reinterpret_cast<const OraText*>(password.c_str()),
                static_cast<ub4>(password.size()),
                reinterpret_cast<const OraText*>(connect_string.c_str()),
                static_cast<ub4>(connect_string.size()), OCI_DEFAULT);
            if (status != OCI_SUCCESS && status != OCI_SUCCESS_WITH_INFO) {
                std::string message = oci_error_message(s.err, status);
                OCIHandleFree(s.err, OCI_HTYPE_ERROR);
                throw std::runtime_error("Oracle logon failed: " + message);
            }

            // reserve()'d above, so this never reallocates: pointers taken
            // into `sessions_` below stay valid for the pool's lifetime.
            sessions_.push_back(s);
            available_.push(&sessions_.back());
        }
    } catch (...) {
        for (auto& s : sessions_) {
            if (s.svc != nullptr) OCILogoff(s.svc, s.err);
            if (s.err != nullptr) OCIHandleFree(s.err, OCI_HTYPE_ERROR);
        }
        sessions_.clear();
        OCIHandleFree(env_, OCI_HTYPE_ENV);
        throw;
    }

    ORE_LOG(INFO) << "Oracle pool ready: " << size << " session(s) to " << connect_string;
}

OraclePool::~OraclePool() {
    // All Handles are expected to be released before the pool is destroyed.
    for (auto& s : sessions_) {
        OCILogoff(s.svc, s.err);
        OCIHandleFree(s.err, OCI_HTYPE_ERROR);
    }
    if (env_ != nullptr) OCIHandleFree(env_, OCI_HTYPE_ENV);
}

OraclePool::Handle OraclePool::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !available_.empty(); });
    Session* s = available_.front();
    available_.pop();
    return Handle(this, s);
}

void OraclePool::release(Session* session) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        available_.push(session);
    }
    cv_.notify_one();
}

}  // namespace Platform
}  // namespace Oreshnek
