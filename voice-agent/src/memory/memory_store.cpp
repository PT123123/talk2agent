// src/memory/memory_store.cpp
#include "memory_store.hpp"
#include "text_features.hpp"
#include "util/log.hpp"
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace voice_agent {

namespace {

int64_t now_epoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

MemoryStore::MemoryStore(std::string db_path) : path_(std::move(db_path)) {}

MemoryStore::~MemoryStore() {
    if (db_) sqlite3_close(db_);
}

bool MemoryStore::exec_(const std::string& sql) {
    if (!db_) return false;
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        LOG_ERROR("SQLite exec error: {} | SQL={}", err ? err : "?", sql);
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

bool MemoryStore::open() {
    if (db_) return true;
    // 非内存库：确保父目录存在
    if (path_ != ":memory:") {
        auto slash = path_.find_last_of("/\\");
        if (slash != std::string::npos) {
            std::string dir = path_.substr(0, slash);
            if (!dir.empty()) {
#ifdef _WIN32
                std::string cmd = "if not exist \"" + dir + "\" mkdir \"" + dir + "\"";
                std::system(cmd.c_str());
#else
                std::string cmd = "mkdir -p '" + dir + "'";
                std::system(cmd.c_str());
#endif
            }
        }
    }

    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path_.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        LOG_ERROR("open {} failed: {}", path_, db_ ? sqlite3_errmsg(db_) : "?");
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return false;
    }

    exec_("PRAGMA journal_mode=WAL;");
    exec_("PRAGMA foreign_keys=ON;");
    exec_("PRAGMA synchronous=NORMAL;");

    // 主表
    static const char* kSchema = R"(CREATE TABLE IF NOT EXISTS memories(
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        type TEXT NOT NULL DEFAULT 'semantic',
        subject TEXT NOT NULL DEFAULT 'user',
        content TEXT NOT NULL,
        tags TEXT NOT NULL DEFAULT '',
        salience REAL NOT NULL DEFAULT 1.0,
        sensitivity INTEGER NOT NULL DEFAULT 0,
        valid_from INTEGER NOT NULL DEFAULT 0,
        valid_to INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        access_count INTEGER NOT NULL DEFAULT 0,
        last_access INTEGER NOT NULL DEFAULT 0,
        deleted INTEGER NOT NULL DEFAULT 0
    );)";
    exec_(kSchema);

    // FTS5 全文索引（拉丁语 + 短语；中文短词由 LIKE 兜底）
    exec_("CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts USING fts5("
          "subject, content, tags, tokenize='unicode61');");
    // 同步触发器
    exec_("CREATE TRIGGER IF NOT EXISTS memories_ai AFTER INSERT ON memories BEGIN "
          "INSERT INTO memories_fts(rowid, subject, content, tags) "
          "VALUES(new.id, new.subject, new.content, new.tags); END;");
    exec_("CREATE TRIGGER IF NOT EXISTS memories_au AFTER UPDATE ON memories BEGIN "
          "DELETE FROM memories_fts WHERE rowid=old.id; "
          "INSERT INTO memories_fts(rowid, subject, content, tags) "
          "VALUES(new.id, new.subject, new.content, new.tags); END;");
    exec_("CREATE TRIGGER IF NOT EXISTS memories_ad AFTER DELETE ON memories BEGIN "
          "DELETE FROM memories_fts WHERE rowid=old.id; END;");
    exec_("CREATE INDEX IF NOT EXISTS idx_m_updated ON memories(updated_at);");

    LOG_INFO("Memory store opened at '{}'", path_);
    return true;
}

std::vector<MemoryItem> MemoryStore::select_rows_(const std::string& sql,
                                                  const std::vector<std::string>& params) {
    std::vector<MemoryItem> out;
    if (!db_) return out;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_ERROR("prepare failed: {}", sqlite3_errmsg(db_));
        return out;
    }
    for (size_t i = 0; i < params.size(); ++i) {
        const std::string& v = params[i];
        if (sqlite3_bind_text(stmt, static_cast<int>(i + 1), v.c_str(),
                              static_cast<int>(v.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
            LOG_WARN("bind failed for param {}", i);
            sqlite3_finalize(stmt);
            return out;
        }
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MemoryItem m;
        m.id            = sqlite3_column_int64(stmt, 0);
        m.type          = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        m.subject       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        m.content       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        m.salience      = sqlite3_column_double(stmt, 4);
        m.sensitivity   = sqlite3_column_int(stmt, 5);
        m.valid_from    = sqlite3_column_int64(stmt, 6);
        m.valid_to      = sqlite3_column_int64(stmt, 7);
        m.created_at    = sqlite3_column_int64(stmt, 8);
        m.access_count  = sqlite3_column_int(stmt, 9);
        m.last_access   = sqlite3_column_int64(stmt, 10);
        m.superseded_by = 0;
        out.push_back(std::move(m));
    }
    sqlite3_finalize(stmt);
    return out;
}

int64_t MemoryStore::save(const std::string& subject, const std::string& content,
                          const std::vector<std::string>& tags, int64_t ttl_days) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || content.empty()) return -1;

    int64_t now = now_epoch();
    // tags 拼接
    std::string tags_str;
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i) tags_str += ",";
        tags_str += tags[i];
    }

    // 去重合并：内容完全相同时更新，避免重复堆积
    auto existing = select_rows_(
        "SELECT id,type,subject,content,salience,sensitivity,valid_from,valid_to,"
        "created_at,access_count,last_access FROM memories WHERE content=? AND deleted=0 LIMIT 1",
        {content});
    if (!existing.empty()) {
        auto& m = existing[0];
        sqlite3_stmt* s = nullptr;
        const char* u = "UPDATE memories SET subject=?, tags=?, updated_at=?, deleted=0 "
                        "WHERE id=?";
        if (sqlite3_prepare_v2(db_, u, -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, subject.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, tags_str.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 3, now);
            sqlite3_bind_int64(s, 4, m.id);
            sqlite3_step(s);
        }
        if (s) sqlite3_finalize(s);
        return m.id;
    }

    sqlite3_stmt* s = nullptr;
    const char* ins =
        "INSERT INTO memories(type,subject,content,tags,salience,sensitivity,"
        "valid_to,created_at,updated_at) VALUES('semantic',?,?,?,1.0,0,?,?,?)";
    int64_t valid_to = ttl_days > 0 ? now + ttl_days * 86400 : 0;
    int64_t id = -1;
    if (sqlite3_prepare_v2(db_, ins, -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, subject.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, content.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, tags_str.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 4, valid_to);
        sqlite3_bind_int64(s, 5, now);
        sqlite3_bind_int64(s, 6, now);
        if (sqlite3_step(s) == SQLITE_DONE) id = sqlite3_last_insert_rowid(db_);
    }
    if (s) sqlite3_finalize(s);
    return id;
}

std::vector<MemoryItem> MemoryStore::query(const std::string& text, int k) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MemoryItem> out;
    if (!db_ || k <= 0) return out;

    auto units = tokenize_units(text);

    // 收集候选 id（FTS5 拉丁 OR + 整句 LIKE 兜底）
    std::set<int64_t> cand;
    {
        // FTS5 只对拉丁词做 OR 召回，避免中文单字 OR 的空命中噪音
        std::string fts;
        for (auto& u : units) {
            bool latin = true;
            for (unsigned char c : u) {
                if (c >= 0x80) { latin = false; break; }
            }
            if (!latin) continue;
            std::string esc;
            for (char c : u) {
                if (c == '"') esc += "\"\"";
                else esc += c;
            }
            if (!esc.empty()) {
                if (!fts.empty()) fts += " OR ";
                fts += "\"" + esc + "\"";
            }
        }
        if (!fts.empty()) {
            auto rows = select_rows_(
                "SELECT m.id,m.type,m.subject,m.content,m.salience,m.sensitivity,"
                "m.valid_from,m.valid_to,m.created_at,m.access_count,m.last_access "
                "FROM memories_fts f JOIN memories m ON m.id=f.rowid WHERE m.deleted=0 "
                "AND f MATCH ? LIMIT 300",
                {fts});
            for (auto& m : rows) cand.insert(m.id);
        }
    }

    // LIKE 兜底：整句在 content/subject/tags 中出现
    {
        std::string like = "%" + text + "%";
        auto rows = select_rows_(
            "SELECT id,type,subject,content,salience,sensitivity,valid_from,valid_to,"
            "created_at,access_count,last_access FROM memories WHERE deleted=0 "
            "AND (content LIKE ? OR subject LIKE ? OR tags LIKE ?) LIMIT 300",
            {like, like, like});
        for (auto& m : rows) cand.insert(m.id);
    }

    if (cand.empty()) return out;

    // 统一打分：unit 覆盖率 + 时间衰减 + 可访问性
    std::map<int64_t, MemoryItem> by_id;
    for (auto id : cand) {
        auto rows = select_rows_(
            "SELECT id,type,subject,content,salience,sensitivity,valid_from,valid_to,"
            "created_at,access_count,last_access FROM memories WHERE id=?",
            {std::to_string(id)});
        if (!rows.empty()) by_id[id] = rows[0];
    }

    int64_t now = now_epoch();
    for (auto& kv : by_id) {
        auto& m = kv.second;
        // 内容 token 集合
        auto content_units = tokenize_units(m.subject + " " + m.content);
        double overlap = 0;
        for (auto& u : units) {
            auto it = std::find(content_units.begin(), content_units.end(), u);
            if (it != content_units.end()) overlap += 1.0;
        }
        double cover = units.empty() ? 0.0 : overlap / static_cast<double>(units.size());
        // 时间衰减（7 天半衰）
        double age_days = (now - m.created_at) / 86400.0;
        double recency = std::pow(0.5, age_days / 7.0);
        double score = cover * 2.0 + 0.25 * recency * m.salience + 0.05;
        m.salience = score;
    }

    out.reserve(by_id.size());
    for (auto& kv : by_id) out.push_back(kv.second);
    std::sort(out.begin(), out.end(),
              [](const MemoryItem& a, const MemoryItem& b) { return a.salience > b.salience; });
    if (static_cast<int>(out.size()) > k) out.resize(k);
    return out;
}

std::vector<MemoryItem> MemoryStore::list(int limit, int offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return {};
    return select_rows_(
        "SELECT id,type,subject,content,salience,sensitivity,valid_from,valid_to,"
        "created_at,access_count,last_access FROM memories WHERE deleted=0 "
        "ORDER BY updated_at DESC LIMIT ? OFFSET ?",
        {std::to_string(limit), std::to_string(offset)});
}

bool MemoryStore::remove(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;
    bool ok = false;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM memories WHERE id=?", -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, id);
        if (sqlite3_step(s) == SQLITE_DONE) ok = sqlite3_changes(db_) > 0;
    }
    if (s) sqlite3_finalize(s);
    return ok;
}

void MemoryStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) exec_("DELETE FROM memories;");
}

int64_t MemoryStore::count() const {
    if (!db_) return 0;
    int64_t c = 0;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM memories WHERE deleted=0", -1, &s, nullptr) ==
        SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) c = sqlite3_column_int64(s, 0);
    }
    if (s) sqlite3_finalize(s);
    return c;
}

}  // namespace voice_agent