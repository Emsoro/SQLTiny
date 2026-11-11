/// SQLTiny — 轻量级嵌入式数据库,兼容 MySQL 客户端协议

![status](https://img.shields.io/badge/status-M1%20Milestone-yellowgreen)
![language](https://img.shields.io/badge/C%2B%2B-20-blue)
![license](https://img.shields.io/badge/license-Apache%202.0-blue)

**SQLTiny** 是用现代 C++20 从零重写的轻量级嵌入式关系型数据库,核心设计灵感来自 [HSQLDB](http://hsqldb.org/),但额外暴露了 **MySQL 客户端协议**——让标准 MySQL 客户端(`mysql` CLI、Navicat、DBeaver、MySQL Connector/J 等)直接连接。

> 项目 CodePlan 见 [`CODEPLAN.md`](./CODEPLAN.md),详细里程碑、架构映射、技术选型、风险与对策都在里面。

---

## ✨ 特点

| 特性 | 说明 |
|---|---|
| 🧠 **纯 C++20** | `std::variant` / `std::jthread` / concepts,零 Java 依赖 |
| 🗄️ **嵌入式优先** | 默认进程内调用,无外部进程 |
| 🎯 **MySQL 协议兼容** | M2.5 起 `mysql -h 127.0.0.1 -P 3306` 直接能用 |
| ⚡ **AVL 内存索引** | 自带平衡 BST,无需依赖 SQLite / RocksDB |
| 🔒 **MVCC 事务** | READ COMMITTED 隔离级别(M3) |
| 🪶 **轻量** | 单库文件,启动毫秒级 |

---

## 🏗️ 当前进度

| 里程碑 | 状态 | 说明 |
|---|---|---|
| M1 基础骨架 | ✅ 已完成 | 类型系统 + AVL 索引 + Lexer,冒烟测试全绿 |
| M2 Catalog + DDL/DML | 🚧 进行中 | Parser + AST + 执行器 |
| **M2.5 MySQL 协议** | 📋 待启动 | 最小可用 MySQL Wire Protocol 适配层 |
| M3 MVCC 事务 + 子查询 | 📋 待启动 | |
| M4 协议层补全 | 📋 待启动 | Prepared Statement + SHOW 系列 + caching_sha2 |
| M5 持久化 WAL | 📋 待启动 | 重启不丢数据 |
| M6 性能打磨 | 📋 待启动 | B+Tree 索引 + EXPLAIN + 复制 |

---

## 🛠️ 构建

### 环境要求
- **Windows**: Visual Studio 2022 + C++20 工具链(MSVC 14.40+)
- **vcpkg**: `set VCPKG_ROOT=C:\vcpkg`
- **CMake**: 3.20+
- fmtlib(已预装); gtest / spdlog 按需安装

### 一键构建
```powershell
# 配置
cmake --preset windows-vcpkg

# 构建
cmake --build build --config Debug

# 跑冒烟测试(不依赖 gtest)
.\build\Debug\smoke_m1.exe
```

> 首次构建会自动通过 vcpkg manifest 拉取依赖(gtest/spdlog 需要 GitHub 能访问)。

### vcpkg.json 依赖
```json
{
  "dependencies": ["fmt"]
}
```

---

## 📁 目录结构
```
SQLTiny/
├── CMakeLists.txt          # 顶层构建
├── CODEPLAN.md             # 完整项目规划
├── vcpkg.json              # vcpkg manifest
├── src/
│   ├── types/              # SqlType + Value 运行时值
│   ├── parser/             # Lexer(手写递归下降入口预留)
│   ├── storage/            # AVL 内存索引
│   ├── core/               # Database / Session / Error (M2)
│   ├── catalog/            # Schema / 对象注册 (M2)
│   ├── executor/           # AST 执行器 (M2)
│   ├── txn/                # MVCC (M3)
│   └── net/                # MySQL Wire Protocol (M2.5)
└── tests/                  # smoke_m1.cpp 无 gtest;后续补 gtest
```

---

## 🚀 里程碑 2.5 验证(当前不可用,M2 完成后解锁)
```bash
mysql -h 127.0.0.1 -P 3306 -u root
> CREATE TABLE users (id BIGINT PRIMARY KEY, name VARCHAR(64));
> INSERT INTO users VALUES (1,'Alice'),(2,'Bob');
> SELECT * FROM users WHERE name LIKE 'A%';
```

---

## 📜 许可证

Apache License 2.0 — 见 [`LICENSE`](./LICENSE) 文件。

---

## 🤝 致谢

- [HSQLDB](http://hsqldb.org/) — 架构和数据结构的重要参考(AVL 索引、MVCC、Session 分层)
- [MySQL Wire Protocol](https://dev.mysql.com/doc/dev/mysql-server/latest/) — M2.5 协议层参考
