#include "storage/index_key.h"
#include <algorithm>

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

} // namespace sqltiny::storage
