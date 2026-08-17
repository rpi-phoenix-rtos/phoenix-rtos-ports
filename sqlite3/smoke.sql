CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, score REAL);
INSERT INTO t VALUES(1,'alice',3.5),(2,'bob',7.25),(3,'carol',1.0);
SELECT 'row|'||id||'|'||name||'|'||score FROM t ORDER BY id;
SELECT 'agg count='||COUNT(*)||' sum='||SUM(id)||' avg='||printf('%.3f',AVG(score)) FROM t;
SELECT 'like '||name FROM t WHERE name LIKE 'b%';
WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<8) SELECT 'cte '||group_concat(x) FROM c;
.exit
