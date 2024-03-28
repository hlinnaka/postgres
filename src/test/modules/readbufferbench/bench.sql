drop table foo;
create table foo (i integer) with (fillfactor=10);
insert into foo select g from generate_series(1, 100000) g;

do $$
declare
  begints timestamptz;
  endts timestamptz;
  results float[];
  i int;
begin
  for i in 1..5 loop
    begints = clock_timestamp();
    perform readbuffer_bench('foo', 10, 10000000);
    endts = clock_timestamp();
    results[i] = extract(epoch from endts) - extract(epoch from begints);
    raise notice 'run %: %', i, results[i];
  end loop;
end;
$$;
