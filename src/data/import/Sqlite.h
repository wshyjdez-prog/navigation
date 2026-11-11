#pragma once
#include <sqlite3.h>
#include <QByteArray>
#include <QString>
#include <stdexcept>

namespace p1::data::detail {
inline void error(sqlite3* db, int rc) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(db));
}
struct Db {
    sqlite3* value = nullptr;
    explicit Db(const QString& path, bool readOnly = false) {
        const auto bytes = path.toUtf8();
        const int rc = sqlite3_open_v2(bytes.constData(), &value,
            readOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        if (rc != SQLITE_OK) {
            const std::string message = value ? sqlite3_errmsg(value) : "Cannot allocate SQLite connection";
            if (value) sqlite3_close(value);
            throw std::runtime_error(message);
        }
        sqlite3_busy_timeout(value, 500);
    }
    ~Db() { sqlite3_close(value); }
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    void exec(const char* sql) {
        char* message = nullptr;
        const int rc = sqlite3_exec(value, sql, nullptr, nullptr, &message);
        const std::string copy = message ? message : "SQLite error";
        sqlite3_free(message);
        if (rc != SQLITE_OK) throw std::runtime_error(copy);
    }
};
struct Statement {
    sqlite3* db;
    sqlite3_stmt* value = nullptr;
    Statement(Db& connection, const char* sql) : db(connection.value) {
        error(db, sqlite3_prepare_v2(db, sql, -1, &value, nullptr));
    }
    ~Statement() { sqlite3_finalize(value); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void integer(int n, qint64 v) { error(db, sqlite3_bind_int64(value, n, v)); }
    void real(int n, double v) { error(db, sqlite3_bind_double(value, n, v)); }
    void text(int n, const QString& v) {
        const auto bytes = v.toUtf8();
        error(db, sqlite3_bind_text(value, n, bytes.constData(), bytes.size(), SQLITE_TRANSIENT));
    }
    void blob(int n, const QByteArray& v) {
        error(db, sqlite3_bind_blob(value, n, v.constData(), int(v.size()), SQLITE_TRANSIENT));
    }
    bool row() { const int rc = sqlite3_step(value); error(db, rc); return rc == SQLITE_ROW; }
    void insert() {
        const int rc = sqlite3_step(value);
        if (rc != SQLITE_DONE) error(db, rc);
        error(db, sqlite3_reset(value));
        error(db, sqlite3_clear_bindings(value));
    }
    qint64 integer(int n) const { return sqlite3_column_int64(value, n); }
    double real(int n) const { return sqlite3_column_double(value, n); }
    bool isNull(int n) const { return sqlite3_column_type(value, n) == SQLITE_NULL; }
    QByteArray blob(int n) const {
        return QByteArray(static_cast<const char*>(sqlite3_column_blob(value, n)), sqlite3_column_bytes(value, n));
    }
    QString text(int n) const {
        return QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(value, n)),
                                 sqlite3_column_bytes(value, n));
    }
};
}
