#pragma once

#include "catalog/catalog.h"
#include "executor/executor.h"
#include "parser/parser.h"
#include "storage/wal_manager.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>

namespace sqltiny::core {

/// 单例数据库(进程内)
class Database {
public:
    static Database& instance();

    catalog::Catalog& catalog() { return catalog_; }
    WalManager& wal() { return wal_; }

    /// 启动时调用:打开 WAL 并 replay 到 catalog
    /// data_dir 为空则持久化禁用
    bool open(const std::string& data_dir);

    /// 优雅关闭
    void shutdown();

private:
    Database() = default;
    catalog::Catalog catalog_;
    WalManager wal_;
    std::string wal_path_;  // 给 session.cpp::open 用
};

/// 会话 —— 一条 SQL 入口
class Session {
public:
    Session();

    /// 执行一条 SQL,返回 ResultSet
    executor::ResultSet execute(const std::string& sql);

    /// 当前数据库(USE 用)
    std::string current_database = "default";

    /// 用户名(占位)
    std::string user = "root";

private:
    executor::Executor exec_{ Database::instance().catalog() };
};

} // namespace sqltiny::core
