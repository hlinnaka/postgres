
create table a (id int4 primary key);
create table b (id int4);
create index on b (id);

insert into a values (1);
insert into b select generate_series(1, 100000);

/*
postgres=# explain select b.id from a, b where a.id = b.id group by b.id;
Optimal plan:

                                     QUERY PLAN                                     
------------------------------------------------------------------------------------
 Group  (cost=9.34..9.35 rows=1 width=4)
   Group Key: b.id
   ->  Sort  (cost=9.34..9.35 rows=1 width=4)
         Sort Key: b.id
         ->  Nested Loop  (cost=0.29..9.33 rows=1 width=4)
               ->  Seq Scan on a  (cost=0.00..1.01 rows=1 width=4)
               ->  Index Only Scan using i_b on b  (cost=0.29..8.31 rows=1 width=4)
                     Index Cond: (id = a.id)
(8 rows)

*/

-- With different data, the other plan looks more attractive
truncate a;
insert into a select generate_series(1, 10000);
analyze a;

/*
postgres=# explain select b.id from a, b where a.id = b.id group by b.id ;
                                      QUERY PLAN                                      
--------------------------------------------------------------------------------------
 Merge Join  (cost=0.61..804.19 rows=0 width=4)
   Merge Cond: (b.id = a.id)
   ->  Group  (cost=0.29..3300.29 rows=100000 width=4)
         Group Key: b.id
         ->  Index Only Scan using i_b on b  (cost=0.29..3050.29 rows=100000 width=4)
   ->  Index Only Scan using a_pkey on a  (cost=0.29..318.29 rows=10000 width=4)
(6 rows)
*/

set enable_mergejoin=off;
set enable_hashjoin=off;

-- Test a parameterized path with

The planner could also do this:

/*
postgres=# explain select b.id from a, b where a.id = b.id group by b.id;
                                  QUERY PLAN                                  
------------------------------------------------------------------------------
 Nested Loop  (cost=0.29..1009.32 rows=0 width=4)
   ->  Seq Scan on a  (cost=0.00..1.01 rows=1 width=4)
   ->  Group  (cost=0.29..8.31 rows=100000 width=4)
         Group Key: b.id
         ->  Index Only Scan using i_b on b  (cost=0.29..8.31 rows=1 width=4)
               Index Cond: (id = a.id)
(6 rows)

XXX cost estimation problem in above: why 100000 rows in Group node?
*/


drop table a;
drop table b;

create table a (i int, unsortable_col xid);
create table b (unsortable_col xid);

insert into a select g, g::text::xid from generate_series(1, 1) as g;
insert into b select g::text::xid from generate_series(1, 100000) as g;
analyze a,b;



-- This Grouping can be applied on top of either t1 or t2.
explain select t1.a, count(*) from t1, t2 WHERE t1.a = t2.x AND t1.b = t2.y GROUP BY t1.a, t1.b;

-- This is the same, but because of avg(t1.a), this cannot be done on 't2'
explain select t1.a, avg(t1.a) from t1, t2 WHERE t1.a = t2.x AND t1.b = t2.y GROUP BY t1.a, t1.b;

-- This is the same, but because of avg(t2.x), this cannot be done on 't1'
explain select t1.a, avg(t2.x) from t1, t2 WHERE t1.a = t2.x AND t1.b = t2.y GROUP BY t1.a, t1.b;




create table a (id int4 primary key);
create table b (id int4, v numeric);
create index on b (id);

insert into a values (1);
insert into b select g / 10, g::numeric / 10.0 from generate_series(1, 100000) g;
analyze a,b;

-- This grouping can be performed on top of 'b'
explain select b.id, avg(b.v) from a, b where a.id = b.id group by b.id;

-- But this can not (Except in a nested loop join)
explain select b.id, avg(b.v) from a, b where a.id = b.id and a.id / 10 + b.v < 1.5 group by b.id;

set enable_nestloop=off;
explain select b.id, avg(b.v) from a, b where a.id = b.id and a.id / 10 + b.v < 1.5 group by b.id;

