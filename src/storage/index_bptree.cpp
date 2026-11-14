#include "storage/index_bptree.h"
#include <algorithm>
#include <fmt/format.h>

namespace sqltiny::storage {

// ---------- 生命周期 ----------

IndexBPlusTree::~IndexBPlusTree() { clear_all(); }

void IndexBPlusTree::clear_all() {
    if (!root_) return;
    std::vector<Node*> level{ root_ };
    while (!level.empty()) {
        std::vector<Node*> next;
        for (Node* n : level) {
            if (!n->leaf)
                for (Node* c : n->children) next.push_back(c);
            delete n;
        }
        level.swap(next);
    }
    root_ = nullptr;
    size_ = 0;
}

// ---------- 基础定位 ----------

size_t IndexBPlusTree::route(const Node* n, const Key& key) {
    // 第一个满足 key < seps[i] 的子下标;key == sep 时落入右侧
    // (sep 是右子树首键副本,等键条目在右子树,路由保持一致)
    size_t i = 0;
    while (i < n->seps.size() && key.compare(n->seps[i]) >= 0) ++i;
    return i;
}

IndexBPlusTree::Node* IndexBPlusTree::find_leaf(
        const Key& key, std::vector<std::pair<Node*, size_t>>* path) const {
    Node* n = root_;
    if (!n) return nullptr;
    while (!n->leaf) {
        size_t idx = route(n, key);
        if (path) path->push_back({ n, idx });
        n = n->children[idx];
    }
    return n;
}

IndexBPlusTree::Node* IndexBPlusTree::leftmost_leaf() const {
    Node* n = root_;
    if (!n) return nullptr;
    while (!n->leaf) n = n->children.front();
    return n;
}

size_t IndexBPlusTree::lower_bound_in(const Node* leaf, const Key& key) {
    size_t lo = 0, hi = leaf->entries.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (leaf->entries[mid].first.compare(key) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

bool IndexBPlusTree::prefix_match(const Key& key, const Key& prefix) {
    size_t m = prefix.size();
    if (key.size() < m) return false;
    for (size_t i = 0; i < m; ++i)
        if (key.parts()[i].compare(prefix.parts()[i]) != 0) return false;
    return true;
}

// ---------- 插入 ----------

bool IndexBPlusTree::insert(const Key& key, int64_t row_id) {
    if (!root_) {
        root_ = new Node();
        root_->leaf = true;
    }
    std::vector<std::pair<Node*, size_t>> path;
    Node* leaf = find_leaf(key, &path);
    size_t pos = lower_bound_in(leaf, key);
    if (unique_ && pos < leaf->entries.size() &&
        leaf->entries[pos].first.compare(key) == 0) {
        return false;
    }
    leaf->entries.insert(leaf->entries.begin() + pos, { key, row_id });
    ++size_;

    // 自底向上处理分裂:叶子分裂上推右叶首键,内部节点分裂继续上推中位分隔键
    Node*  new_right = nullptr;
    Key    up_sep;
    bool   have_sep = false;
    if (leaf->entries.size() >= kLeafMax) {
        new_right = new Node();
        new_right->leaf = true;
        size_t mid = leaf->entries.size() / 2;
        new_right->entries.assign(
            std::make_move_iterator(leaf->entries.begin() + mid),
            std::make_move_iterator(leaf->entries.end()));
        leaf->entries.resize(mid);
        new_right->next = leaf->next;
        leaf->next = new_right;
        up_sep = new_right->entries.front().first;
        have_sep = true;
    }
    for (int i = static_cast<int>(path.size()) - 1; i >= 0; --i) {
        Node*  parent    = path[i].first;
        size_t child_idx = path[i].second;
        if (!have_sep) break;
        parent->seps.insert(parent->seps.begin() + child_idx, up_sep);
        parent->children.insert(parent->children.begin() + child_idx + 1, new_right);
        new_right = nullptr; have_sep = false;
        if (parent->children.size() > kInternalMax) {
            Node* rn = new Node();
            rn->leaf = false;
            size_t mid = parent->seps.size() / 2;
            up_sep = parent->seps[mid];
            have_sep = true;
            rn->seps.assign(
                std::make_move_iterator(parent->seps.begin() + mid + 1),
                std::make_move_iterator(parent->seps.end()));
            parent->seps.resize(mid);
            rn->children.assign(parent->children.begin() + mid + 1,
                                parent->children.end());
            parent->children.resize(mid + 1);
            new_right = rn;
        }
    }
    if (have_sep) {
        Node* nr = new Node();
        nr->leaf = false;
        nr->seps.push_back(std::move(up_sep));
        nr->children.push_back(root_);
        nr->children.push_back(new_right);
        root_ = nr;
    }
    return true;
}

// ---------- 查找 / 删除 ----------

std::optional<int64_t> IndexBPlusTree::find(const Key& key) const {
    Node* leaf = find_leaf(key, nullptr);
    if (!leaf) return std::nullopt;
    size_t pos = lower_bound_in(leaf, key);
    if (pos < leaf->entries.size() &&
        leaf->entries[pos].first.compare(key) == 0)
        return leaf->entries[pos].second;
    return std::nullopt;
}

void IndexBPlusTree::collect_equal(const Node* start_leaf, size_t start_pos,
                                   const Key& key, std::vector<int64_t>& out) const {
    const Node* n = start_leaf;
    size_t pos = start_pos;
    while (n) {
        while (pos < n->entries.size() &&
               n->entries[pos].first.compare(key) == 0) {
            out.push_back(n->entries[pos].second);
            ++pos;
        }
        if (pos < n->entries.size()) break;          // 已越过 key
        // 本叶耗尽:仅当最后条目 == key 时等键可能延续到下一叶
        if (n->entries.empty() || n->entries.back().first.compare(key) != 0) break;
        n = n->next;
        if (!n) break;
        pos = lower_bound_in(n, key);
        if (pos >= n->entries.size() || n->entries[pos].first.compare(key) != 0) break;
    }
}

std::vector<int64_t> IndexBPlusTree::find_all(const Key& key) const {
    std::vector<int64_t> out;
    std::vector<std::pair<Node*, size_t>> path;
    Node* leaf = find_leaf(key, &path);
    if (!leaf) return out;
    Node*  start     = leaf;
    size_t start_pos = lower_bound_in(leaf, key);
    bool   found     = start_pos < leaf->entries.size() &&
                       leaf->entries[start_pos].first.compare(key) == 0;
    if (!found && !path.empty()) {
        // 等键条目可能全部落在路由目标的左兄弟叶(跨叶分裂且 sep == key)
        auto [parent, idx] = path.back();
        if (idx > 0) {
            Node* left_leaf = parent->children[idx - 1];
            while (!left_leaf->leaf) left_leaf = left_leaf->children.back();
            size_t p = lower_bound_in(left_leaf, key);
            if (p < left_leaf->entries.size() &&
                left_leaf->entries[p].first.compare(key) == 0) {
                start = left_leaf;
                start_pos = p;
                found = true;
            }
        }
    }
    if (found) collect_equal(start, start_pos, key, out);
    // 等键条目也可能延伸到路由目标叶的左兄弟(跨叶分裂)
    if (found && !path.empty()) {
        auto [parent, idx] = path.back();
        if (idx > 0) {
            Node* left_leaf = parent->children[idx - 1];
            while (!left_leaf->leaf) left_leaf = left_leaf->children.back();
            if (!left_leaf->entries.empty() &&
                left_leaf->entries.back().first.compare(key) == 0) {
                size_t p = lower_bound_in(left_leaf, key);
                collect_equal(left_leaf, p, key, out);
            }
        }
    }
    return out;
}

bool IndexBPlusTree::remove(const Key& key, int64_t row_id) {
    std::vector<std::pair<Node*, size_t>> path;
    Node* leaf = find_leaf(key, &path);
    if (!leaf) return false;
    auto try_leaf = [&](Node* l) -> bool {
        size_t pos = lower_bound_in(l, key);
        while (pos < l->entries.size() &&
               l->entries[pos].first.compare(key) == 0) {
            if (l->entries[pos].second == row_id) {
                l->entries.erase(l->entries.begin() + pos);
                --size_;
                return true;
            }
            ++pos;
        }
        return false;
    };
    if (try_leaf(leaf)) return true;
    if (!path.empty()) {
        auto [parent, idx] = path.back();
        if (idx > 0) {
            Node* left_leaf = parent->children[idx - 1];
            while (!left_leaf->leaf) left_leaf = left_leaf->children.back();
            if (try_leaf(left_leaf)) return true;
        }
    }
    Node* nxt = leaf->next;
    if (nxt && !nxt->entries.empty() &&
        nxt->entries.front().first.compare(key) == 0)
        return try_leaf(nxt);
    return false;
}

// ---------- 范围 / 前缀 / 全量扫描 ----------

void IndexBPlusTree::range_scan(const Key& lo, const Key& hi,
                                const std::function<bool(const Key&, int64_t)>& cb,
                                bool inclusive_hi) const {
    Node* leaf = find_leaf(lo, nullptr);
    if (!leaf) return;
    size_t pos = lower_bound_in(leaf, lo);
    while (leaf) {
        while (pos < leaf->entries.size()) {
            auto& [k, rid] = leaf->entries[pos];
            int c = k.compare(hi);
            if (inclusive_hi ? c > 0 : c >= 0) return;
            if (!cb(k, rid)) return;
            ++pos;
        }
        leaf = leaf->next;
        pos = 0;
    }
}

void IndexBPlusTree::prefix_scan(const Key& prefix,
                                 const std::function<bool(const Key&, int64_t)>& cb) const {
    Node* leaf = find_leaf(prefix, nullptr);
    if (!leaf) return;
    size_t pos = lower_bound_in(leaf, prefix);
    while (leaf) {
        while (pos < leaf->entries.size()) {
            auto& [k, rid] = leaf->entries[pos];
            if (!prefix_match(k, prefix)) return;   // 键全局有序,越过前缀即全部越过
            if (!cb(k, rid)) return;
            ++pos;
        }
        leaf = leaf->next;
        pos = 0;
    }
}

void IndexBPlusTree::for_each(const std::function<void(const Key&, int64_t)>& cb) const {
    for (Node* leaf = leftmost_leaf(); leaf; leaf = leaf->next)
        for (auto& [k, rid] : leaf->entries) cb(k, rid);
}

// ---------- 诊断 ----------

int IndexBPlusTree::height() const {
    int h = 0;
    for (Node* n = root_; n; n = n->leaf ? nullptr : n->children.front()) ++h;
    return h;
}

bool IndexBPlusTree::check_invariants() const {
    if (!root_) return size_ == 0;
    // 1) 内部节点:children == seps + 1,且不超过扇出上限
    std::vector<Node*> level{ root_ };
    std::vector<Node*> leaves;
    while (!level.empty()) {
        std::vector<Node*> next;
        for (Node* n : level) {
            if (n->leaf) { leaves.push_back(n); continue; }
            if (n->children.size() != n->seps.size() + 1) return false;
            if (n->children.size() > kInternalMax) return false;
            for (Node* c : n->children) next.push_back(c);
        }
        level.swap(next);
    }
    // 2) 分隔键下界:每个 sep <= 右子树最左叶首键(允许偏高,不允许偏低)
    std::function<bool(const Node*)> check_lb = [&](const Node* n) -> bool {
        if (n->leaf) return true;
        for (size_t i = 0; i < n->seps.size(); ++i) {
            const Node* c = n->children[i + 1];
            while (!c->leaf) c = c->children.front();
            if (c->entries.empty()) continue;   // 空叶(惰性删除)
            if (n->seps[i].compare(c->entries.front().first) > 0) return false;
        }
        for (Node* c : n->children)
            if (!check_lb(c)) return false;
        return true;
    };
    if (!check_lb(root_)) return false;
    // 3) 叶子链:全局有序且条目总数 == size_
    size_t chain_entries = 0;
    const Key* prev = nullptr;
    for (Node* leaf = leftmost_leaf(); leaf; leaf = leaf->next) {
        for (auto& [k, rid] : leaf->entries) {
            if (prev && prev->compare(k) > 0) return false;
            prev = &k;
            ++chain_entries;
        }
    }
    return chain_entries == size_;
}

} // namespace sqltiny::storage
