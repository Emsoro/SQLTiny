#!/usr/bin/env python3
"""employees 数据集在 MySQL(3306) 与 SQLTiny(3307) 间的查询结果对比验证。
对每组查询分别在两端执行,规范化(排序+数值取整)后比对。
覆盖: 基础查询 / 嵌套子查询 / WITH(CTE) / 其他常见复杂查询。
已知缺陷用 expect_diff 标注,不计为失败。
"""
import sys, pymysql, time
from decimal import Decimal

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

MY = dict(host='127.0.0.1', port=3306, user='root', password='123456',
         database='employees', connect_timeout=5, read_timeout=100)
ST = dict(host='127.0.0.1', port=3307, user='root', password='',
         database='employees', connect_timeout=5, read_timeout=100)

# (类别, sql, expect_diff)  expect_diff=True 表示已知差异,只展示不计入失败
# 注:SQLTiny 的 IN 子查询按外层逐行重执行,大内表用例的外层均用 emp_no 范围收敛。
QUERIES = [
  # ---------- 基础 ----------
  ("基础", "SELECT COUNT(*) FROM employees", False),
  ("基础", "SELECT gender, COUNT(*) FROM employees GROUP BY gender ORDER BY gender", False),
  ("基础", "SELECT * FROM employees WHERE emp_no=10001", False),
  ("基础", "SELECT COUNT(*) FROM employees WHERE first_name='Parto'", False),
  ("基础", "SELECT dept_name, COUNT(*) AS c FROM departments d JOIN dept_emp de ON d.dept_no=de.dept_no GROUP BY d.dept_name ORDER BY d.dept_name", False),
  ("基础", "SELECT SUM(salary) FROM salaries", False),
  ("基础", "SELECT COUNT(*) FROM salaries WHERE salary > 100000", False),
  ("基础", "SELECT title, COUNT(*) FROM titles GROUP BY title ORDER BY title", False),
  ("基础", "SELECT e.first_name, e.last_name, s.salary, s.from_date FROM employees e JOIN salaries s ON e.emp_no=s.emp_no WHERE e.emp_no=10001 ORDER BY s.from_date", False),
  ("基础", "SELECT t.title, COUNT(*) FROM employees e JOIN titles t ON e.emp_no=t.emp_no GROUP BY t.title ORDER BY t.title", False),
  ("基础", "SELECT AVG(salary) FROM salaries", False),
  ("基础", "SELECT COUNT(*) FROM employees e JOIN dept_emp de ON e.emp_no=de.emp_no JOIN departments d ON de.dept_no=d.dept_no WHERE d.dept_name='Development'", False),
  ("基础", "SELECT d.dept_name, AVG(s.salary) FROM departments d JOIN dept_emp de ON d.dept_no=de.dept_no JOIN salaries s ON de.emp_no=s.emp_no GROUP BY d.dept_name ORDER BY d.dept_name", False),
  # 已知缺陷:DATE 的 MIN/MAX 只返回年
  ("基础", "SELECT MIN(hire_date), MAX(hire_date) FROM employees", True),

  # ---------- 嵌套子查询 ----------
  ("嵌套", "SELECT COUNT(*) FROM employees WHERE emp_no IN (SELECT emp_no FROM dept_manager)", False),
  ("嵌套", "SELECT COUNT(*) FROM employees WHERE emp_no NOT IN (SELECT emp_no FROM dept_manager)", False),
  ("嵌套", "SELECT emp_no, first_name, last_name FROM employees WHERE emp_no IN (SELECT emp_no FROM dept_manager WHERE dept_no='d005') ORDER BY emp_no", False),
  ("嵌套", "SELECT COUNT(*) FROM employees WHERE emp_no <= 10050 AND emp_no IN (SELECT emp_no FROM dept_emp WHERE dept_no='d001')", False),
  ("嵌套", "SELECT COUNT(*) FROM employees WHERE emp_no IN (SELECT emp_no FROM dept_manager WHERE dept_no IN (SELECT dept_no FROM departments WHERE dept_name='Development'))", False),
  ("嵌套", "SELECT COUNT(*) FROM salaries WHERE salary > (SELECT AVG(salary) FROM salaries)", False),
  ("嵌套", "SELECT e.emp_no, e.first_name, (SELECT MAX(s.salary) FROM salaries s WHERE s.emp_no=e.emp_no) AS max_sal FROM employees e WHERE e.emp_no <= 10010 ORDER BY e.emp_no", False),
  ("嵌套", "SELECT COUNT(*) FROM employees e WHERE EXISTS (SELECT 1 FROM titles t WHERE t.emp_no=e.emp_no AND t.title='Senior Engineer')", False),
  ("嵌套", "SELECT e.emp_no FROM employees e WHERE e.emp_no <= 10050 AND e.emp_no IN (SELECT de.emp_no FROM dept_emp de WHERE de.emp_no=e.emp_no AND de.dept_no='d005') ORDER BY e.emp_no", False),
  ("嵌套", "SELECT x.dept_no, x.c FROM (SELECT dept_no, COUNT(*) AS c FROM dept_emp GROUP BY dept_no) x ORDER BY x.dept_no", False),

  # ---------- WITH / CTE ----------
  ("WITH", "WITH dc AS (SELECT dept_no, COUNT(*) AS c FROM dept_emp GROUP BY dept_no) SELECT d.dept_name, dc.c FROM dc JOIN departments d ON d.dept_no=dc.dept_no ORDER BY d.dept_name", False),
  ("WITH", "WITH hs AS (SELECT emp_no, MAX(salary) AS ms FROM salaries GROUP BY emp_no) SELECT COUNT(*) FROM hs WHERE ms > 140000", False),
  ("WITH", "WITH a AS (SELECT dept_no, COUNT(*) AS c FROM dept_emp GROUP BY dept_no), b AS (SELECT dept_no, dept_name FROM departments) SELECT b.dept_name, a.c FROM a JOIN b ON a.dept_no=b.dept_no ORDER BY b.dept_name", False),
  ("WITH", "WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM seq WHERE n < 100) SELECT COUNT(*) AS c, SUM(n) AS s FROM seq", False),

  # ---------- 其他常见复杂查询 ----------
  ("复杂", "SELECT de.dept_no, COUNT(DISTINCT de.emp_no) AS c FROM dept_emp de GROUP BY de.dept_no HAVING COUNT(DISTINCT de.emp_no) > 20000 ORDER BY de.dept_no", False),
  ("复杂", "SELECT CASE WHEN salary < 50000 THEN '1_low' WHEN salary < 90000 THEN '2_mid' ELSE '3_high' END AS band, COUNT(*) AS c FROM salaries GROUP BY band ORDER BY band", False),
  ("复杂", "SELECT d.dept_name, COUNT(*) AS c, ROUND(AVG(s.salary),2) AS avg_sal FROM departments d JOIN dept_emp de ON d.dept_no=de.dept_no JOIN salaries s ON de.emp_no=s.emp_no GROUP BY d.dept_name HAVING AVG(s.salary) > 60000 ORDER BY avg_sal DESC LIMIT 5", False),
  ("复杂", "SELECT COUNT(*) FROM employees e LEFT JOIN dept_manager dm ON e.emp_no=dm.emp_no WHERE dm.emp_no IS NULL", False),
  ("复杂", "SELECT dept_no FROM departments WHERE dept_no <= 'd004' UNION SELECT dept_no FROM departments WHERE dept_no >= 'd006' ORDER BY dept_no", False),
  ("复杂", "SELECT 'employees' AS tbl, COUNT(*) AS c FROM employees UNION ALL SELECT 'salaries', COUNT(*) FROM salaries UNION ALL SELECT 'titles', COUNT(*) FROM titles", False),
  ("复杂", "SELECT DISTINCT title FROM titles ORDER BY title LIMIT 3 OFFSET 2", False),
  ("复杂", "SELECT a.emp_no, a.dept_no AS d1, b.dept_no AS d2 FROM dept_emp a JOIN dept_emp b ON a.emp_no=b.emp_no AND a.dept_no < b.dept_no WHERE a.emp_no <= 10100 ORDER BY a.emp_no LIMIT 20", False),
  ("复杂", "SELECT COUNT(*) FROM salaries WHERE salary BETWEEN 60000 AND 70000", False),
  ("复杂", "SELECT emp_no, from_date, salary, ROW_NUMBER() OVER (PARTITION BY emp_no ORDER BY from_date DESC) AS rn FROM salaries WHERE emp_no <= 10010 ORDER BY emp_no, rn", False),
  ("复杂", "SELECT YEAR(hire_date) AS y, COUNT(*) AS c FROM employees GROUP BY y ORDER BY y", False),
]

def run(cfg, q):
    """每条查询用独立连接:某条超时/断连不会污染后续查询(PyMySQL 连接失效后
    会连锁返回 (0, ''),旧实现因此把后续正常查询误报成 ST-ERR)。"""
    try:
        conn = pymysql.connect(**cfg)
    except Exception as e:
        return ('ERR', f'connect: {e}', 0.0)
    cur = conn.cursor()
    t0 = time.time()
    try:
        cur.execute(q)
        return ('OK', cur.fetchall(), time.time() - t0)
    except Exception as e:
        return ('ERR', str(e)[:200], time.time() - t0)
    finally:
        try:
            cur.close()
            conn.close()
        except Exception:
            pass

def norm(rows):
    out = []
    for r in rows:
        nr = []
        for v in r:
            if isinstance(v, bool):
                nr.append(v)
            elif isinstance(v, (int, float)):
                nr.append(round(float(v), 4))
            elif isinstance(v, Decimal):
                nr.append(round(float(v), 4))
            elif isinstance(v, str):
                s = v.strip()
                try:
                    nr.append(round(float(s), 4))   # SQLTiny 常把 SUM/聚合以字符串返回
                except ValueError:
                    nr.append(s)
            else:
                nr.append(str(v))
        out.append(tuple(nr))
    return sorted(out)

def main():
    print(f"{'CAT':4} | {'QUERY':58} | MYSQL | ST    | MYs   | STs   | RESULT")
    stats = {}
    fails = 0
    for cat, q, exp_diff in QUERIES:
        rmy, rst = run(MY, q), run(ST, q)
        myok, stok = rmy[0] == 'OK', rst[0] == 'OK'
        s = stats.setdefault(cat, dict(total=0, match=0, known=0, mismatch=0,
                                       st_err=0, other_err=0, my_t=0.0, st_t=0.0))
        s['total'] += 1
        s['my_t'] += rmy[2]; s['st_t'] += rst[2]
        if myok and stok:
            nm, ns = norm(rmy[1]), norm(rst[1])
            if nm == ns:
                tag = "MATCH"; s['match'] += 1
            elif exp_diff:
                tag = "KNOWN-DIFF"; s['known'] += 1
            else:
                tag = "MISMATCH"; fails += 1; s['mismatch'] += 1
                # 抽样展示差异
                print(f"    --- diff sample: my[:2]={nm[:2]} st[:2]={ns[:2]}")
        elif myok and not stok:
            tag = "ST-ERR"; fails += 1; s['st_err'] += 1
            print(f"    --- SQLTiny ERR: {rst[1]}")
        elif not myok and stok:
            tag = "MY-ERR"; fails += 1; s['other_err'] += 1
        else:
            tag = "BOTH-ERR"; fails += 1; s['other_err'] += 1
        print(f"{cat:4} | {q[:58]:58} | {('OK' if myok else 'ERR'):5} | {('OK' if stok else 'ERR'):5} | "
              f"{rmy[2]:5.2f} | {rst[2]:5.2f} | {tag}")
    print()
    print("==== 分类汇总(准确性 / 性能) ====")
    for cat, s in stats.items():
        print(f"{cat}: 共{s['total']}条 | MATCH {s['match']} | KNOWN-DIFF {s['known']} | "
              f"MISMATCH {s['mismatch']} | ST-ERR {s['st_err']} | 其他ERR {s['other_err']} | "
              f"MySQL总耗时 {s['my_t']:.2f}s | SQLTiny总耗时 {s['st_t']:.2f}s")
    print("SUMMARY: fails(非已知差异)=", fails)

if __name__ == "__main__":
    main()