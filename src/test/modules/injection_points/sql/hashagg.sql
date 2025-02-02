-- Test for hash aggregation
CREATE EXTENSION injection_points;

SELECT injection_points_set_local();

SELECT injection_points_attach('hash-aggregate-enter-spill-mode', 'notice');
SELECT injection_points_attach('hash-aggregate-process-batch', 'notice');
SELECT injection_points_attach('hash-aggregate-single-partition', 'notice');

CREATE TABLE hashagg_ij(x INTEGER);
INSERT INTO hashagg_ij SELECT g FROM generate_series(1,100000) g;

SET max_parallel_workers=0;
SET max_parallel_workers_per_gather=0;
SET enable_sort=FALSE;
SET work_mem='1MB';
SET hash_mem_multiplier=1.0;

SELECT COUNT(*) FROM (SELECT DISTINCT x FROM hashagg_ij) s;

DROP TABLE hashagg_ij;
DROP EXTENSION injection_points;
