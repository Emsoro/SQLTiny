// 索引微基准:IndexAvl vs IndexBPlusTree(A/B 对比)
// 工作负载模拟 employees.salaries 主键 (emp_no int, salary int):
//   1. 顺序插入(模拟 WAL 回放/批量装载)
//   2. 乱序插入(最坏插入路径)
//   3. 随机点查 find(模拟 eq_ref 探测)
//   4. 复合键前缀扫描 prefix_scan(模拟 JOIN ref 探测,每员工 ~17 条薪酬)
//   5. 全量 for_each 遍历
// 用法: bench_index [N](默认 2,000,000)
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <random>
#include <algorithm>
#include <vector>

#include "storage/index_avl.h"
#include "storage/index_bptree.h"

using namespace sqltiny;
using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct Entry { int emp; int sal; };

int main(int argc, char** argv) {
    size_t N = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2000000;
    const int EMP_COUNT = 300024;              // employees 行数
    // salaries: 每员工多条薪酬记录(总 N 条,emp 均匀分布)
    std::vector<Entry> seq; seq.reserve(N);
    for (size_t i = 0; i < N; ++i) {
        int emp = static_cast<int>(i % EMP_COUNT);
        int sal = static_cast<int>(40000 + (i / EMP_COUNT) * 500 + (i % 97));
        seq.push_back({ emp, sal });
    }
    std::vector<Entry> shuf = seq;
    std::mt19937 rng(7);
    std::shuffle(shuf.begin(), shuf.end(), rng);

    auto mk_key = [](const Entry& e) {
        return storage::Key(std::vector<types::Value>{
            types::Value::make_int(e.emp), types::Value::make_int(e.sal) });
    };

    for (int round = 0; round < 2; ++round) {
        const char* name = round == 0 ? "AVL   " : "B+Tree";
        printf("--- %s ---\n", name);

        // 1. 顺序插入(批量装载路径)
        {
            auto t0 = Clock::now();
            std::unique_ptr<storage::IndexAvl> a;
            std::unique_ptr<storage::IndexBPlusTree> b;
            for (auto& e : seq) {
                if (round == 0) { if (!a) a = std::make_unique<storage::IndexAvl>(true); a->insert(mk_key(e), 0); }
                else            { if (!b) b = std::make_unique<storage::IndexBPlusTree>(true); b->insert(mk_key(e), 0); }
            }
            printf("  seq insert %zu     : %10.1f ms\n", N, ms_since(t0));
            if (round == 0) printf("    (avl height=%d)\n", a->height());
            else            printf("    (bptree height=%d, bytes-levels ok)\n", b->height());
        }
        // 2. 乱序插入
        {
            auto t0 = Clock::now();
            std::unique_ptr<storage::IndexAvl> a;
            std::unique_ptr<storage::IndexBPlusTree> b;
            for (auto& e : shuf) {
                if (round == 0) { if (!a) a = std::make_unique<storage::IndexAvl>(true); a->insert(mk_key(e), 0); }
                else            { if (!b) b = std::make_unique<storage::IndexBPlusTree>(true); b->insert(mk_key(e), 0); }
            }
            printf("  shuffle insert %zu : %10.1f ms\n", N, ms_since(t0));

            // 3. 随机点查(取每个员工的"首条"键 —— 不一定存在,测真实点查含 miss)
            {
                std::uniform_int_distribution<int> dist(0, EMP_COUNT - 1);
                volatile int64_t sink = 0;
                auto t1 = Clock::now();
                const int PROBES = 200000;
                for (int p = 0; p < PROBES; ++p) {
                    storage::Key k(types::Value::make_int(dist(rng)));
                    if (round == 0) { auto r = a->find(k); if (r) sink += *r; }
                    else            { auto r = b->find(k); if (r) sink += *r; }
                }
                printf("  point find x%d     : %10.1f ms\n", PROBES, ms_since(t1));
            }
            // 4. 复合键前缀扫描(JOIN ref 探测路径)
            {
                std::uniform_int_distribution<int> dist(0, EMP_COUNT - 1);
                volatile int64_t sink = 0;
                auto t1 = Clock::now();
                const int PROBES = 200000;
                for (int p = 0; p < PROBES; ++p) {
                    storage::Key k(types::Value::make_int(dist(rng)));
                    auto cb = [&](const storage::Key&, int64_t) { ++sink; return true; };
                    if (round == 0) a->prefix_scan(k, cb);
                    else            b->prefix_scan(k, cb);
                }
                printf("  prefix_scan x%d    : %10.1f ms\n", PROBES, ms_since(t1));
            }
            // 5. 全量遍历
            {
                volatile int64_t sink = 0;
                auto t1 = Clock::now();
                if (round == 0) a->for_each([&](const storage::Key&, int64_t) { ++sink; });
                else            b->for_each([&](const storage::Key&, int64_t) { ++sink; });
                printf("  for_each %zu       : %10.1f ms\n", N, ms_since(t1));
            }
            printf("  invariants         : %s\n",
                   round == 0 ? (a->check_invariants() ? "ok" : "FAIL")
                              : (b->check_invariants() ? "ok" : "FAIL"));
        }
    }
    printf("BENCH_INDEX_DONE\n");
    return 0;
}
