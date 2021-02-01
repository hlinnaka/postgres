-- Enumerate all valid EUC_JP byte sequences. Not all of these encode an actual character, but
-- this enumerates everything that's accepted by the verification function.
create function all_euc_jp() returns setof bytea language plpgsql as
$$
declare
  byte1 integer;
  byte2 integer;
  byte3 integer;
  byte4 integer;
begin
  -- JIS X 0201: 2-byte encoded chars starting with 0x8e (SS2)
  byte1 = hex('8e');
  for byte2 in hex('a1')..hex('df') loop
    return next b(byte1, byte2);
  end loop;

  -- JIS X 0212: 3-byte encoded chars, starting with 0x8f (SS3)
  byte1 = hex('8f');
  for byte2 in hex('a1')..hex('fe') loop
    for byte3 in hex('a1')..hex('fe') loop
      return next b(byte1, byte2, byte3);
    end loop;
  end loop;

  -- JIS X 0208?: other 2-byte chars
  for byte1 in hex('a1')..hex('fe') loop
    for byte2 in hex('a1')..hex('fe') loop
      return next b(byte1, byte2);
    end loop;
  end loop;

  -- ASCII
  for byte1 in hex('01')..hex('7f') loop
    return next b(byte1);
  end loop;
end;
$$;

select euc_jp_char, convert_roundtrip(euc_jp_char, 'euc_jp', 'utf8') from all_euc_jp() as euc_jp_char;

select euc_jp_char, convert_roundtrip(euc_jp_char, 'euc_jp', 'mule_internal') from all_euc_jp() as euc_jp_char;

select euc_jp_char, convert_roundtrip(euc_jp_char, 'euc_jp', 'sjis') from all_euc_jp() as euc_jp_char;
