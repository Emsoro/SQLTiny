#pragma once

#include "storage/index_key.h"
#include <vector>
#include <cstdint>
#include <functional>
#include <optional>

namespace sqltiny::storage {

/// B+ 树内存索引(v2.0.0,替换主键 AVL 索引)
///
/// 结构:内部节点只存分隔键用于路由,全部 (Key, row_id) 存在叶子;
/// 叶子间单向链表相连,范围/前缀扫描沿链顺序推进,缓存局部性远优于
/// AVL 的递归中序遍历;批量顺序插入只追加最右叶子,O(1) 摊销(WAL 回放友好)。
///
/// 删除采用惰性策略:只从叶子移除条目、不回收空叶、不收缩高度 ——
/// 分隔键允许"偏高"(指向键已不存在),路由与扫描语义不受影响;
/// 本引擎的删除主要来自 MVCC 逻辑删,惰性策略以最小复杂度保证正确性。
class IndexBPlusTree {
public:
    IndexBPlusTree() = default;
    explicit IndexBPlusTree(bool is_unique) : unique_(is_unique) {}
    ~IndexBPlusTree();

    IndexBPlusTree(const IndexBPlusTree&) = delete;
    IndexBPlusTree& operator=(const IndexBPlusTree&) = delete;

    /// 插入键 + row_id。unique_ 时键已存在返回 false
    bool insert(const Key& key, int64_t row_id);

    /// 精确查找(取第一个匹配的 row_id)
    std::optional<int64_t> find(const Key& key) const;

    /// 所有匹配 row_id(非唯一索引)
    std::vector<int64_t> find_all(const Key& key) const;

    /// 删除指定 (key, row_id)。返回是否删除了条目
    bool remove(const Key& key, int64_t row_id);

    /// 范围扫描:[lo, hi) 或含 hi —— 回调返回 false 停止
    void range_scan(const Key& lo, const Key& hi,
                    const std::function<bool(const Key&, int64_t)>& cb,
                    bool inclusive_hi = false) const;

    /// 前缀扫描:所有以 prefix 逐列相等的键,沿叶子链顺序推进,停止即剪枝
    void prefix_scan(const Key& prefix,
                     const std::function<bool(const Key&, int64_t)>& cb) const;

    /// 全量扫描(叶子链顺序)
    void for_each(const std::function<void(const Key&, int64_t)>& cb) const;

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    int  height() const;
    bool check_invariants() const;

private:
    struct Node {
        bool leaf = true;
        // 内部节点
        std::vector<Key>     seps;       // children.size() - 1
        std::vector<Node*>   children;
        // 叶子
        std::vector<std::pair<Key, int64_t>> entries;
        Node* next = nullptr;            // 叶子链
    };

    static constexpr size_t kLeafMax      = 128;  // 叶子分裂阈值
    static constexpr size_t kInternalMax  = 64;   // 内部节点最大子节点数

    Node*  root_   = nullptr;
    size_t size_   = 0;
    bool   unique_ = false;

    Node*  leftmost_leaf() const;
    /// 路由下降:返回目标叶子,path 记录 (父节点, 落到的子下标)
    Node*  find_leaf(const Key& key, std::vector<std::pair<Node*, size_t>>* path) const;
    /// 节点内路由:key 应落入的子下标(第一个 > key 的分隔键左侧)
    static size_t route(const Node* n, const Key& key);
    /// 叶子内第一个 >= key 的条目下标
    static size_t lower_bound_in(const Node* leaf, const Key& key);
    /// 前缀是否逐列匹配(m = prefix.size())
    static bool prefix_match(const Key& key, const Key& prefix);
    /// 从某叶子(含)开始沿链收集所有 == key 的 row_id
    void collect_equal(const Node* start_leaf, size_t start_pos,
                       const Key& key, std::vector<int64_t>& out) const;
    void clear_all();
};

} // namespace sqltiny::storage
