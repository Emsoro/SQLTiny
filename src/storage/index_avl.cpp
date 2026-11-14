#include "storage/index_avl.h"
#include <algorithm>
#include <fmt/format.h>

namespace sqltiny::storage {

std::string Key::to_string() const {
    std::string s = "(";
    for (size_t i = 0; i < parts_.size(); ++i) {
        if (i) s += ", ";
        s += parts_[i].to_string();
    }
    s += ")";
    return s;
}

int Key::compare(const Key& o) const {
    size_t n = std::min(parts_.size(), o.parts_.size());
    for (size_t i = 0; i < n; ++i) {
        int c = parts_[i].compare(o.parts_[i]);
        if (c != 0) return c;
    }
    if (parts_.size() != o.parts_.size()) {
        return parts_.size() < o.parts_.size() ? -1 : 1;
    }
    return 0;
}

// ---------- IndexAvl ----------
IndexAvl::~IndexAvl() { clear_tree(root_); }

void IndexAvl::clear_tree(AvlNode* n) {
    if (!n) return;
    clear_tree(n->left);
    clear_tree(n->right);
    delete n;
}

void IndexAvl::fix_height(AvlNode* n) {
    int l = n->left  ? n->left->height  : 0;
    int r = n->right ? n->right->height : 0;
    n->height = 1 + std::max(l, r);
}
void IndexAvl::fix_count(AvlNode* n) {
    int l = n->left  ? n->left->count  : 0;
    int r = n->right ? n->right->count : 0;
    n->count = 1 + l + r;
}

AvlNode* IndexAvl::rotate_left(AvlNode* x) {
    AvlNode* y = x->right;
    x->right = y->left;
    if (y->left) y->left->parent = x;
    y->parent = x->parent;
    if (x->parent) {
        if (x->parent->left == x) x->parent->left = y;
        else                      x->parent->right = y;
    }
    y->left = x;
    x->parent = y;
    fix_height(x); fix_count(x);
    fix_height(y); fix_count(y);
    return y;
}

AvlNode* IndexAvl::rotate_right(AvlNode* y) {
    AvlNode* x = y->left;
    y->left = x->right;
    if (x->right) x->right->parent = y;
    x->parent = y->parent;
    if (y->parent) {
        if (y->parent->left == y) y->parent->left = x;
        else                      y->parent->right = x;
    }
    x->right = y;
    y->parent = x;
    fix_height(y); fix_count(y);
    fix_height(x); fix_count(x);
    return x;
}

AvlNode* IndexAvl::balance(AvlNode* n) {
    fix_height(n); fix_count(n);
    int b = n->bf();
    if (b > 1) {
        if (n->left && n->left->bf() < 0) {
            n->left = rotate_left(n->left);
        }
        return rotate_right(n);
    }
    if (b < -1) {
        if (n->right && n->right->bf() > 0) {
            n->right = rotate_right(n->right);
        }
        return rotate_left(n);
    }
    return n;
}

bool IndexAvl::insert(const Key& key, int64_t row_id) {
    bool inserted = false;
    root_ = do_insert(root_, key, row_id, inserted);
    if (root_) root_->parent = nullptr;
    if (inserted) ++size_;
    return inserted;
}

AvlNode* IndexAvl::do_insert(AvlNode* n, const Key& key, int64_t row_id, bool& inserted) {
    if (!n) {
        inserted = true;
        return new AvlNode{ key, row_id, 1, 1, nullptr, nullptr, nullptr };
    }
    int c = key.compare(n->key);
    if (c < 0) {
        n->left = do_insert(n->left, key, row_id, inserted);
        if (n->left) n->left->parent = n;
    } else if (c > 0) {
        n->right = do_insert(n->right, key, row_id, inserted);
        if (n->right) n->right->parent = n;
    } else {
        // key 相等:唯一索引拒绝;非唯一时当作更大(放到右侧,保持稳定)
        if (unique_) {
            inserted = false;
            return n;
        }
        n->right = do_insert(n->right, key, row_id, inserted);
        if (n->right) n->right->parent = n;
    }
    return balance(n);
}

AvlNode* IndexAvl::find_node(AvlNode* n, const Key& key) const {
    while (n) {
        int c = key.compare(n->key);
        if (c < 0) n = n->left;
        else if (c > 0) n = n->right;
        else return n;
    }
    return nullptr;
}

std::optional<int64_t> IndexAvl::find(const Key& key) const {
    AvlNode* n = find_node(root_, key);
    if (!n) return std::nullopt;
    return n->row_id;
}

std::vector<int64_t> IndexAvl::find_all(const Key& key) const {
    std::vector<int64_t> out;
    // 先找一个,然后沿着相等链向左扩展
    auto* n = find_node(root_, key);
    while (n && key.compare(n->key) == 0) {
        // 先一路左
        auto* l = n;
        while (l->left && key.compare(l->left->key) == 0) l = l->left;
        // 把以 l 为根的子树所有相等节点都收集
        std::function<void(AvlNode*)> collect_eq = [&](AvlNode* cur) {
            if (!cur) return;
            collect_eq(cur->left);
            if (key.compare(cur->key) == 0) out.push_back(cur->row_id);
            collect_eq(cur->right);
        };
        collect_eq(l);
        break;
    }
    return out;
}

AvlNode* IndexAvl::successor(AvlNode* n) const {
    if (!n) return nullptr;
    if (n->right) {
        n = n->right;
        while (n->left) n = n->left;
        return n;
    }
    while (n->parent && n->parent->right == n) n = n->parent;
    return n->parent;
}

bool IndexAvl::remove(const Key& key, int64_t row_id) {
    bool removed = false;
    root_ = do_remove(root_, key, row_id, removed);
    if (root_) root_->parent = nullptr;
    if (removed) --size_;
    return removed;
}

AvlNode* IndexAvl::do_remove(AvlNode* n, const Key& key, int64_t row_id, bool& removed) {
    if (!n) return nullptr;
    int c = key.compare(n->key);
    if (c < 0) {
        n->left = do_remove(n->left, key, row_id, removed);
        if (n->left) n->left->parent = n;
    } else if (c > 0) {
        n->right = do_remove(n->right, key, row_id, removed);
        if (n->right) n->right->parent = n;
    } else {
        // 找到匹配键,还要比 row_id(非唯一索引下多个同键节点,删第一个 row_id 对的)
        if (row_id != n->row_id && !unique_) {
            // 往右边子树继续找(row_id 匹配的)
            n->right = do_remove(n->right, key, row_id, removed);
            if (n->right) n->right->parent = n;
            // 这里虽然没删 n,但 removed 可能在递归里设过了
        } else {
            // 删 n
            removed = true;
            if (!n->left || !n->right) {
                AvlNode* child = n->left ? n->left : n->right;
                delete n;
                return child;
            }
            // 找后继,把 n 的内容替换成后继内容,然后删除后继
            auto* succ = successor(n);
            // succ 一定存在(有右子树)
            auto succ_key = succ->key;
            auto succ_rid = succ->row_id;
            // 删除 succ(它的 key 正好是自己)
            bool _ = false;
            n->right = do_remove(n->right, succ_key, succ_rid, _);
            if (n->right) n->right->parent = n;
            n->key    = std::move(succ_key);
            n->row_id = succ_rid;
        }
    }
    return balance(n);
}

void IndexAvl::range_scan(const Key& lo, const Key& hi,
                          const std::function<bool(const Key&, int64_t)>& cb,
                          bool inclusive_hi) const {
    do_range_scan(root_, lo, hi, cb, inclusive_hi);
}

// ---------- 前缀扫描 ----------
// 节点 key 截断到 prefix 长度与 prefix 逐列比较,决定剪枝方向:
//   截断 < prefix → 只走右子树;截断 > prefix → 只走左子树;
//   截断 == prefix → 节点本身命中(键长 >= 前缀长),左右子树都可能还有命中,两边都递归。
// 总成本 O(log n + 命中数)。
void IndexAvl::do_prefix_scan(AvlNode* n, const Key& prefix,
                              const std::function<bool(const Key&, int64_t)>& cb) const {
    if (!n) return;
    size_t m = prefix.size();
    int cmp = 0;
    for (size_t i = 0; i < m; ++i) {
        if (n->key.size() <= i) { cmp = -1; break; }   // 节点 key 更短 → 字典序更小
        cmp = n->key.parts()[i].compare(prefix.parts()[i]);
        if (cmp != 0) break;
    }
    if (cmp < 0) { do_prefix_scan(n->right, prefix, cb); return; }
    if (cmp > 0) { do_prefix_scan(n->left,  prefix, cb); return; }
    if (n->key.size() >= m) {
        if (!cb(n->key, n->row_id)) return;
    }
    do_prefix_scan(n->left,  prefix, cb);
    do_prefix_scan(n->right, prefix, cb);
}

void IndexAvl::prefix_scan(const Key& prefix,
                           const std::function<bool(const Key&, int64_t)>& cb) const {
    do_prefix_scan(root_, prefix, cb);
}

void IndexAvl::do_range_scan(AvlNode* n, const Key& lo, const Key& hi,
                             const std::function<bool(const Key&, int64_t)>& cb,
                             bool inclusive_hi) const {
    if (!n) return;
    int c_lo = lo.compare(n->key);   // <= n ?
    int c_hi = hi.compare(n->key);   // n < hi ?
    bool go_left  = (c_lo < 0);       // lo < n->key
    bool go_right = inclusive_hi ? (c_hi > 0) : (c_hi > 0); // n->key < hi (或 <=)

    // 其实 inclusive_hi 只影响当前节点是否输出,不影响遍历
    if (go_left)  do_range_scan(n->left,  lo, hi, cb, inclusive_hi);

    bool in_range = (c_lo <= 0) && (inclusive_hi ? c_hi >= 0 : c_hi > 0);
    if (in_range) {
        if (!cb(n->key, n->row_id)) return;  // 调用方要求停止
    }

    if (go_right) do_range_scan(n->right, lo, hi, cb, inclusive_hi);
}

void IndexAvl::for_each(const std::function<void(const Key&, int64_t)>& cb) const {
    do_for_each(root_, cb);
}
void IndexAvl::do_for_each(AvlNode* n, const std::function<void(const Key&, int64_t)>& cb) const {
    if (!n) return;
    do_for_each(n->left, cb);
    cb(n->key, n->row_id);
    do_for_each(n->right, cb);
}

int IndexAvl::height() const { return do_height(root_); }
int IndexAvl::do_height(AvlNode* n) const { return n ? n->height : 0; }

bool IndexAvl::check_invariants() const {
    int h; AvlNode* prev = nullptr;
    return check_rec(root_, h, prev);
}

bool IndexAvl::check_rec(AvlNode* n, int& out_height, AvlNode*& prev) const {
    if (!n) { out_height = 0; return true; }
    int lh, rh;
    if (!check_rec(n->left,  lh, prev)) return false;

    // BST 有序
    if (prev && prev->key.compare(n->key) > 0) return false;
    prev = n;

    if (!check_rec(n->right, rh, prev)) return false;
    int h = 1 + std::max(lh, rh);
    if (h != n->height) return false;
    if (std::abs(lh - rh) > 1) return false;
    out_height = h;
    return true;
}

} // namespace sqltiny::storage
