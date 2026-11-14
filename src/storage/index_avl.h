#pragma once

#include "storage/index_key.h"
#include <vector>
#include <cstdint>
#include <functional>
#include <optional>

namespace sqltiny::storage {

using Key = sqltiny::storage::Key;  // Key 已迁至 index_key.h(v2.0.0 与 B+ 树共用)

/// AVL 节点
struct AvlNode {
    Key     key;
    int64_t row_id = -1;           /// 指向 Row 的内部 ID(或自增序号)
    int     height = 1;
    int     count  = 1;            /// 子树节点数,用于按序索引
    AvlNode* left   = nullptr;
    AvlNode* right  = nullptr;
    AvlNode* parent = nullptr;

    int bf() const {
        int l = left  ? left->height  : 0;
        int r = right ? right->height : 0;
        return l - r;
    }
};

/// AVL 内存索引
class IndexAvl {
public:
    IndexAvl() = default;
    ~IndexAvl();

    IndexAvl(const IndexAvl&) = delete;
    IndexAvl& operator=(const IndexAvl&) = delete;

    /// 插入,返回 false 表示键已存在且 unique_=true(重复键拒绝)
    /// unique_ 构造时决定,后续不可变
    IndexAvl(bool is_unique) : unique_(is_unique) {}

    /// 插入键 + row_id。成功返回 true;若 unique 且重复返回 false
    bool insert(const Key& key, int64_t row_id);

    /// 精确查找,返回 row_id(找不到返回 nullopt)
    std::optional<int64_t> find(const Key& key) const;

    /// 找到所有匹配 row_id(非唯一索引)
    std::vector<int64_t> find_all(const Key& key) const;

    /// 从索引删除(键 + row_id)。返回是否删除了节点
    bool remove(const Key& key, int64_t row_id);

    /// 范围扫描:[lo, hi) 或 [lo, hi] —— 回调 bool(const Key&, int64_t),返回 false 停止
    void range_scan(const Key& lo, const Key& hi,
                    const std::function<bool(const Key&, int64_t)>& cb,
                    bool inclusive_hi = false) const;

    /// 前缀扫描:所有以 prefix 逐列相等的键(前缀剪枝遍历,O(log n + 命中数))。
    /// 用于复合主键首列的等值探测 (JOIN eq_ref/ref)
    void prefix_scan(const Key& prefix,
                     const std::function<bool(const Key&, int64_t)>& cb) const;

    /// 全量扫描
    void for_each(const std::function<void(const Key&, int64_t)>& cb) const;

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    // 诊断
    int height() const;
    bool check_invariants() const;   /// 递归检查 AVL 平衡 + BST 有序

private:
    AvlNode* root_  = nullptr;
    size_t   size_  = 0;
    bool     unique_ = false;

    // --- 内部旋转 / 维护 ---
    void fix_height(AvlNode* n);
    void fix_count (AvlNode* n);
    AvlNode* rotate_left (AvlNode* x);
    AvlNode* rotate_right(AvlNode* y);
    AvlNode* balance(AvlNode* n);

    // 递归 insert,返回子树新根
    AvlNode* do_insert(AvlNode* n, const Key& key, int64_t row_id, bool& inserted);
    // 找到 key 最小匹配节点
    AvlNode* find_node(AvlNode* n, const Key& key) const;
    // 后继
    AvlNode* successor(AvlNode* n) const;
    AvlNode* do_remove(AvlNode* n, const Key& key, int64_t row_id, bool& removed);
    void do_range_scan(AvlNode* n, const Key& lo, const Key& hi,
                       const std::function<bool(const Key&, int64_t)>& cb,
                       bool inclusive_hi) const;
    void do_prefix_scan(AvlNode* n, const Key& prefix,
                        const std::function<bool(const Key&, int64_t)>& cb) const;
    void do_for_each(AvlNode* n, const std::function<void(const Key&, int64_t)>& cb) const;
    void clear_tree(AvlNode* n);
    int  do_height(AvlNode* n) const;
    bool check_rec(AvlNode* n, int& out_height, AvlNode*& prev) const;
};

} // namespace sqltiny::storage
