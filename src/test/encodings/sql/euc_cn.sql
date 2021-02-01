-- Enumerate all valid EUC_CN byte sequences. Not all of these encode an actual character, but
-- this enumerates everything that's accepted by the verification function.
create function all_euc_cn() returns setof bytea language plpgsql as
$$
declare
  byte1 integer;
  byte2 integer;
begin
  -- 2-byte chars
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


select euc_cn_char, convert_roundtrip(euc_cn_char, 'euc_cn', 'utf8') from all_euc_cn() as euc_cn_char;

select euc_cn_char, convert_roundtrip(euc_cn_char, 'euc_cn', 'mule_internal') from all_euc_cn() as euc_cn_char;
