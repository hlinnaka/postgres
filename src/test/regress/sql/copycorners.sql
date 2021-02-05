--\set SHOW_CONTEXT never
\set VERBOSITY terse

create temp table copytest_tbl (
  a text,
  b text,
  c text) on commit delete rows;

create or replace function copytest_bin(copyoptions text, content bytea)
returns table (a text, b text, c text) language plpgsql as
$func$
declare
  copycmd text;
begin

  -- COPY FROM a one-line perl script that prints out the 'content'. To pass
  -- the content down to the perl script, so that it can regurgitate it, we
  -- encode it as hex. The perl script unpacks the bytes from the hex
  -- representation.
  copycmd = format(
    $$copy copytest_tbl from program $prog$ perl -e 'printf "%%s", pack ("H*", "%s")' $prog$ %s$$,
    encode(content, 'hex'), copyoptions);

  execute copycmd;

  return query select * from copytest_tbl;
end;
$func$;

create or replace function copytest(copyoptions text, content text)
returns table (a text, b text, c text) language plpgsql as
$func$
begin
  return query select * from copytest_bin(copyoptions, convert_to(content, 'sql_ascii'));
end;
$func$;


-- Basic tests. Not very interesting but see that write_test_file() works.
select * from copytest('', E'a	b	c');

select * from copytest('', E'a	b	c');

--
-- Test EOL detection
--
select * from copytest('', E'a	b	c\nd	e	f\n'); -- ok
select * from copytest('', E'a	b	c\rd	e	f\r'); -- ok
select * from copytest('', E'a	b	c\r\nd	e	f\r\n'); -- ok
select * from copytest('', E'a	b	c\nd	e	f\r'); -- mismatch
select * from copytest('', E'a	b	c\rd	e	f\n'); -- mismatch
select * from copytest('', E'a	b	c\r\nd	e	f\n'); -- mismatch
select * from copytest('', E'a	b	c\r\nd	e	f\r'); -- mismatch

--
-- Test end-of-copy markers at different locations.
--

select * from copytest('', E'a	b	c\\.');
select * from copytest('', E'a	b	c\\.\n');
select * from copytest('', E'a	b	c\n\n\\.');
select * from copytest('', E'a	b	c\n\n\\.\n');

-- \. on a line of its own, with garbage after it
select * from copytest('', E'a	b	c\n\\.\ngarbage');

-- \. at beginning of line, with garbage after it
select * from copytest('', E'a	b	c\n\\.garbage');

-- \. in the middle of file, and garbage after it.
select * from copytest('', E'a	b\\.garbage');


--
-- Test end-of-copy markers with different EOLs
--
select * from copytest('', E'a	b	c\nd	e	f\\.\n');
select * from copytest('', E'a	b	c\rd	e	f\\.\r');
select * from copytest('', E'a	b	c\r\nd	e	f\\.\r\n');

-- mismatch between EOL style and EOL after \.
select * from copytest('', E'a	b	c\na	b	c\\.\r');
select * from copytest('', E'a	b	c\ra	b	c\\.\n');
select * from copytest('', E'a	b	c\r\na	b	c\\.\n');
select * from copytest('', E'a	b	c\na	b	c\\.\r\n');

-- end-of-copy marker on first line, with different EOL styles
select * from copytest('', E'a	b	c\\.');
select * from copytest('', E'a	b	c\\.\n');
select * from copytest('', E'a	b	c\\.\r');
select * from copytest('', E'a	b	c\\.\r\n');
