#!/usr/bin/env python3
"""JOIN 性能基准:PK 前缀索引探测 + WHERE 谓词下推 前后对比。"""
import pymysql, time

c = pymysql.connect(host='127.0.0.1', port=3307, user='root', password='', database='employees')
cur = c.cursor()

QUERIES = [
    ("3表JOIN求各部门均薪",
     "SELECT d.dept_name, AVG(s.salary) FROM departments d "
     "JOIN dept_emp de ON d.dept_no=de.dept_no "
     "JOIN salaries s ON de.emp_no=s.emp_no GROUP BY d.dept_name ORDER BY d.dept_name"),
    ("员工薪酬历史 WHERE emp_no=10001",
     "SELECT e.first_name, e.last_name, s.salary, s.from_date FROM employees e "
     "JOIN salaries s ON e.emp_no=s.emp_no WHERE e.emp_no=10001 ORDER BY s.from_date"),
    ("单员工行数",
     "SELECT COUNT(*) FROM employees e JOIN salaries s ON e.emp_no=s.emp_no WHERE e.emp_no=10001"),
    ("高薪员工数(下推+混合谓词)",
     "SELECT COUNT(*) FROM employees e JOIN salaries s ON e.emp_no=s.emp_no "
     "WHERE e.hire_date < '1990-01-01' AND s.salary > 90000"),
    ("2表JOIN计行(哈希路径回归)",
     "SELECT d.dept_name, COUNT(*) FROM departments d "
     "JOIN dept_emp de ON d.dept_no=de.dept_no GROUP BY d.dept_name ORDER BY d.dept_name"),
]

print("=== EXPLAIN ===")
cur.execute("EXPLAIN SELECT d.dept_name, AVG(s.salary) FROM departments d "
            "JOIN dept_emp de ON d.dept_no=de.dept_no "
            "JOIN salaries s ON de.emp_no=s.emp_no GROUP BY d.dept_name")
for row in cur.fetchall():
    print("  ", row[2], "|", row[3], "|", row[5], "|", row[6], "|", row[7])

print("=== 计时 ===")
for name, q in QUERIES:
    t0 = time.time()
    cur.execute(q)
    rows = cur.fetchall()
    dt = time.time() - t0
    print(f"{name}: {dt:.2f}s rows={len(rows)} first={rows[0] if rows else None}")

c.close()
print("BENCH_DONE")
