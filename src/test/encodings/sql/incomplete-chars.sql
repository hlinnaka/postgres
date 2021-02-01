-- The test string we use are three letters: åäö
-- (The above comment is in UTF-8 encoding, but otherwise this test is not dependent on
-- client_encoding)

-- we print a lot of errors coming from the copytest() function, make them more terse.
\set SHOW_CONTEXT never

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

-- Remember the function in a psql variable so that we can easily create
-- it in the other test databases later.
select pg_get_functiondef('copytest'::regproc) as funcdef
\gset



-- Create UTF-8 test database
-- First perform tests with UTF-8 as the server encoding.
drop database if exists utf8test;
create database utf8test template=template0 encoding 'utf8' lc_ctype='C' lc_collate='C';
\c utf8test
create temp table if not exists copytest (line text) on commit delete rows;
select :'funcdef'
\gexec


-- Valid input.
select copytest('encoding ''utf8''', '\xc3a5c3a4c3b6');
select copytest('encoding ''utf8''', '\xe8b1a1');
-- incomplete multibyte char.
select copytest('encoding ''utf8''', '\xc3a5c3a4c3');
-- incomplete multibyte char, followed by newline (0A)
select copytest('encoding ''utf8''', '\xc3a5c3a4c30A');
-- invalid sequences: embedded nul-bytes
select copytest('encoding ''utf8''', '\x00c3a5c3a4c3b6');
select copytest('encoding ''utf8''', '\xc300a5c3a4c3b6');
select copytest('encoding ''utf8''', '\xc3a500c3a4c3b6');
select copytest('encoding ''utf8''', '\xc3a5c3a4c3b600');

-- Valid input
select copytest('encoding ''latin1''', '\xe5e4f6');
-- invalid sequences: embedded nul-bytes
select copytest('encoding ''latin1''', '\x00e5e4f6');
select copytest('encoding ''latin1''', '\xe5e400f6');
select copytest('encoding ''latin1''', '\xe5e4f600');

--
-- SJIS_2004 -> UTF-8
--
select copytest('encoding ''shiftjis2004''', '\x8fdb');
-- combined char
select copytest('encoding ''shiftjis2004''', '\x82f5');
-- incomplete multibyte char.
select copytest('encoding ''shiftjis2004''', '\x8f');
-- incomplete multibyte char, followed by newline (0A)
select copytest('encoding ''shiftjis2004''', '\x8f0A');
-- invalid sequence: embedded nul-bytes
select copytest('encoding ''shiftjis2004''', '\x008fdb');
select copytest('encoding ''shiftjis2004''', '\x8f00');
-- untranslatable character
select copytest('encoding ''shiftjis2004''', '\x81c0');

--
-- GB18030 -> UTF-8
--
select copytest('encoding ''gb18030''', '\xcff3');
-- this range is converted by mapping function
select copytest('encoding ''gb18030''', '\x84309C38');
-- incomplete char
select copytest('encoding ''gb18030''', '\x84309C');
-- embedded NUL
select copytest('encoding ''gb18030''', '\x84309C3800');
select copytest('encoding ''gb18030''', '\x84309C00');

-- untranslatable char
select copytest('encoding ''gb18030''', '\x8431a530');


-- Repeat the tests in a LATIN1 test database
drop database if exists latin1test;
create database latin1test template=template0 encoding 'latin1' lc_ctype='C' lc_collate='C';
\c latin1test
create temp table if not exists copytest (line text) on commit delete rows;
select :'funcdef'
\gexec


-- Valid input.
select copytest('encoding ''utf8''', '\xc3a5c3a4c3b6');
-- incomplete multibyte char.
select copytest('encoding ''utf8''', '\xc3a5c3a4c3');

-- invalid sequences: embedded nul-bytes
--
-- NOTE: These are accepted by existing PostgreSQL versions. The string is
-- truncated at the NULL.
--
select copytest('encoding ''utf8''', '\x00c3a5c3a4c3b6');
select copytest('encoding ''utf8''', '\xc300a5c3a4c3b6');
select copytest('encoding ''utf8''', '\xc3a500c3a4c3b6');
select copytest('encoding ''utf8''', '\xc3a5c3a4c3b600');

-- untranslatable char (three-byte UTF-8 character with no mapping to LATIN1)
select copytest('encoding ''utf8''', '\xe8b1a1');

-- Valid input
select copytest('encoding ''latin1''', '\xe5e4f6');
-- invalid sequences: embedded nul-bytes
select copytest('encoding ''latin1''', '\x00e5e4f6');
select copytest('encoding ''latin1''', '\xe5e400f6');
select copytest('encoding ''latin1''', '\xe5e4f600');


--
-- Test MULE_INTERNAL -> LATIN1 conversions
--

-- same LATIN1 chars in MULE_INTERNAL encoding
select copytest('encoding ''mule_internal''', '\x81e581e481f6');
-- incomplete char
select copytest('encoding ''mule_internal''', '\x81e581e481');
-- incomplete character, followed by newline (0A).
select copytest('encoding ''mule_internal''', '\x81e581e4810A');
-- invalid sequences: embedded nul-bytes
select copytest('encoding ''mule_internal''', '\x0081e581e481f6');
select copytest('encoding ''mule_internal''', '\x8100e581e481f6');
select copytest('encoding ''mule_internal''', '\x81e50081e481f6');
select copytest('encoding ''mule_internal''', '\x81e581e481f600');


--
-- Test with EUC_JIS_2004 as server encoding
--
drop database if exists euc_jis_2004_test;
create database euc_jis_2004_test template=template0 encoding 'euc_jis_2004' lc_ctype='C' lc_collate='C';
\c euc_jis_2004_test
create temp table if not exists copytest (line text) on commit delete rows;
select :'funcdef'
\gexec

-- Valid input.
select copytest('encoding ''euc_jis_2004''', '\xbedd');
select copytest('encoding ''utf8''', '\xe8b1a1');
-- combined char
select copytest('encoding ''utf8''', '\xe382abe3829a');
-- incomplete multibyte char.
select copytest('encoding ''utf8''', '\xe8b1');
select copytest('encoding ''euc_jis_2004''', '\xbeddbe');
-- embedded NUL bytes
select copytest('encoding ''utf8''', '\x00');
select copytest('encoding ''utf8''', '\xe8b100e8b1a1');
select copytest('encoding ''euc_jis_2004''', '\x00bedd');
select copytest('encoding ''euc_jis_2004''', '\xbe00dd');
select copytest('encoding ''euc_jis_2004''', '\xbedd00');
