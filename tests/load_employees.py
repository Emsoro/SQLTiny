#!/usr/bin/env python3
"""把 test_db-master 的 employees 数据集灌入 SQLTiny (默认 3307, WAL 模式)。
dump 文件本身是多行 INSERT,这里按批量重写为多行 VALUES 以适配 SQLTiny。
用法: python tests/load_employees.py [--port 3307] [--batch 2000] [--tables employees,departments]
"""
import pymysql, re, sys, time, os, argparse

DUMP_DIR = r"D:/Cplusplus/test_db-master"

TABLES = [
  ("employees", """
   CREATE TABLE employees (
     emp_no INT NOT NULL,
     birth_date DATE NOT NULL,
     first_name VARCHAR(14) NOT NULL,
     last_name VARCHAR(16) NOT NULL,
     gender CHAR(1) NOT NULL,
     hire_date DATE NOT NULL,
     PRIMARY KEY (emp_no)
   )"""),
  ("departments", """
   CREATE TABLE departments (
     dept_no CHAR(4) NOT NULL,
     dept_name VARCHAR(40) NOT NULL,
     PRIMARY KEY (dept_no)
   )"""),
  ("dept_manager", """
   CREATE TABLE dept_manager (
     emp_no INT NOT NULL,
     dept_no CHAR(4) NOT NULL,
     from_date DATE NOT NULL,
     to_date DATE NOT NULL,
     PRIMARY KEY (emp_no, dept_no)
   )"""),
  ("dept_emp", """
   CREATE TABLE dept_emp (
     emp_no INT NOT NULL,
     dept_no CHAR(4) NOT NULL,
     from_date DATE NOT NULL,
     to_date DATE NOT NULL,
     PRIMARY KEY (emp_no, dept_no)
   )"""),
  ("titles", """
   CREATE TABLE titles (
     emp_no INT NOT NULL,
     title VARCHAR(50) NOT NULL,
     from_date DATE NOT NULL,
     to_date DATE,
     PRIMARY KEY (emp_no, title, from_date)
   )"""),
  ("salaries", """
   CREATE TABLE salaries (
     emp_no INT NOT NULL,
     salary INT NOT NULL,
     from_date DATE NOT NULL,
     to_date DATE NOT NULL,
     PRIMARY KEY (emp_no, from_date)
   )"""),
]

DUMPS = {
  "departments":  ["load_departments.dump"],
  "employees":    ["load_employees.dump"],
  "dept_manager": ["load_dept_manager.dump"],
  "dept_emp":     ["load_dept_emp.dump"],
  "titles":       ["load_titles.dump"],
  "salaries":     ["load_salaries1.dump", "load_salaries2.dump", "load_salaries3.dump"],
}

def tuples_from_file(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        content = f.read()
    tups = re.findall(r'\(([^)]*)\)', content)
    tups = [t.replace('\\N', 'NULL') for t in tups]
    return tups

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=3307)
    ap.add_argument('--batch', type=int, default=2000)
    ap.add_argument('--tables', default='')
    args = ap.parse_args()
    want = set(args.tables.split(',')) if args.tables else None

    c = pymysql.connect(host='127.0.0.1', port=args.port, user='root', password='', autocommit=True)
    cur = c.cursor()
    cur.execute("CREATE DATABASE IF NOT EXISTS employees")
    cur.execute("USE employees")
    for name, ddl in TABLES:
        if want and name not in want:
            continue
        try:
            cur.execute("DROP TABLE IF EXISTS " + name)
        except Exception:
            pass
        cur.execute(ddl)
        print(f"[{time.strftime('%H:%M:%S')}] created {name}", flush=True)
    for name, files in DUMPS.items():
        if want and name not in want:
            continue
        total = 0
        t0 = time.time()
        for fn in files:
            tups = tuples_from_file(os.path.join(DUMP_DIR, fn))
            for i in range(0, len(tups), args.batch):
                chunk = tups[i:i+args.batch]
                sql = "INSERT INTO " + name + " VALUES (" + "),(".join(chunk) + ")"
                cur.execute(sql)
                total += len(chunk)
            print(f"[{time.strftime('%H:%M:%S')}]   {name} <- {fn}: +{len(tups)} (cum {total}) {time.time()-t0:.1f}s", flush=True)
        cur.execute("SELECT COUNT(*) FROM " + name)
        cnt = int(cur.fetchone()[0])
        print(f"[{time.strftime('%H:%M:%S')}]   >>> {name} COUNT = {cnt} (expected ~source)", flush=True)
    c.close()
    print("LOAD_DONE", flush=True)

if __name__ == "__main__":
    main()
