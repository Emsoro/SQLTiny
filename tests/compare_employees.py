#!/usr/bin/env python3
"""employees 数据集在 MySQL(3306) 与 SQLTiny(3307) 间的查询结果对比验证。
对每组查询分别在两端执行,规范化(排序+数值取整)后比对。
已知缺陷(DATE 的 MIN/MAX 只返回年)用 expect_diff 标注,不计为失败。
"""
import pymysql, time
from decimal import Decimal

MY = dict(host='127.0.0.1', port=3306, user='root', password='123456',
         database='employees', read_timeout=180)
ST = dict(host='127.0.0.1', port=3307, user='root', password='',
         database='employees', read_timeout=180)

# (sql, expect_diff)  expect_diff=True 表示已知差异,只展示不计入失败
QUERIES = [
  ("SELECT COUNT(*) FROM employees", False),
  ("SELECT gender, COUNT(*) FROM employees GROUP BY gender ORDER BY gender", False),
  ("SELECT * FROM employees WHERE emp_no=10001", False),
  ("SELECT COUNT(*) FROM employees WHERE first_name='Parto'", False),
  ("SELECT dept_name, COUNT(*) AS c FROM departments d JOIN dept_emp de ON d.dept_no=de.dept_no GROUP BY d.dept_name ORDER BY d.dept_name", False),
  ("SELECT SUM(salary) FROM salaries", False),
  ("SELECT COUNT(*) FROM salaries WHERE salary > 100000", False),
  ("SELECT title, COUNT(*) FROM titles GROUP BY title ORDER BY title", False),
  ("SELECT e.first_name, e.last_name, s.salary, s.from_date FROM employees e JOIN salaries s ON e.emp_no=s.emp_no WHERE e.emp_no=10001 ORDER BY s.from_date", False),
  ("SELECT t.title, COUNT(*) FROM employees e JOIN titles t ON e.emp_no=t.emp_no GROUP BY t.title ORDER BY t.title", False),
  ("SELECT AVG(salary) FROM salaries", False),
  ("SELECT COUNT(*) FROM employees e JOIN dept_emp de ON e.emp_no=de.emp_no JOIN departments d ON de.dept_no=d.dept_no WHERE d.dept_name='Development'", False),
  ("SELECT d.dept_name, AVG(s.salary) FROM departments d JOIN dept_emp de ON d.dept_no=de.dept_no JOIN salaries s ON de.emp_no=s.emp_no GROUP BY d.dept_name ORDER BY d.dept_name", False),
  # 已知缺陷:DATE 的 MIN/MAX 只返回年
  ("SELECT MIN(hire_date), MAX(hire_date) FROM employees", True),
]

def run(conn, q):
    cur = conn.cursor()
    t0 = time.time()
    try:
        cur.execute(q)
        return ('OK', cur.fetchall(), time.time() - t0)
    except Exception as e:
        return ('ERR', str(e)[:200], time.time() - t0)
    finally:
        cur.close()

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
    my = pymysql.connect(**MY)
    st = pymysql.connect(**ST)
    print(f"{'QUERY':62} | MYSQL | ST | MYs | STs | RESULT")
    fails = 0
    for q, exp_diff in QUERIES:
        rmy, rst = run(my, q), run(st, q)
        myok, stok = rmy[0] == 'OK', rst[0] == 'OK'
        if myok and stok:
            nm, ns = norm(rmy[1]), norm(rst[1])
            if nm == ns:
                tag = "MATCH"
            elif exp_diff:
                tag = "KNOWN-DIFF"
            else:
                tag = "MISMATCH"; fails += 1
                # 抽样展示差异
                print(f"    --- diff sample: my[:2]={nm[:2]} st[:2]={ns[:2]}")
        elif myok and not stok:
            tag = "ST-ERR"; fails += 1
            print(f"    --- SQLTiny ERR: {rst[1]}")
        elif not myok and stok:
            tag = "MY-ERR"; fails += 1
        else:
            tag = "BOTH-ERR"; fails += 1
        print(f"{q[:60]:60} | {('OK' if myok else 'ERR'):5} | {('OK' if stok else 'ERR'):5} | "
              f"{rmy[2]:5.2f} | {rst[2]:5.2f} | {tag}")
    my.close(); st.close()
    print("SUMMARY: fails(非已知差异)=", fails)

if __name__ == "__main__":
    main()
