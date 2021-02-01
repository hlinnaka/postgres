--
-- Test COPY FROM, with 'content' as the input file.
--
create or replace function copytest(copyoptions text, content bytea)
returns setof text language plpgsql as
$func$
declare
  copycmd text;
begin

  -- COPY FROM a one-line perl script that prints out the 'content'. To pass
  -- the content down to the perl script, so that it can regurgitate it, we
  -- encode it as hex. The perl script unpacks the bytes from the hex
  -- representation.
  copycmd = format(
    $$copy copytest from program $prog$ perl -e 'printf "%%s", pack ("H*", "%s")' $prog$ %s$$,
    encode(content, 'hex'), copyoptions);

  execute copycmd;

  return query (select * from copytest);
end;
$func$;
