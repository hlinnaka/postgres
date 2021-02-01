-- Enumerate all valid EUC_TW byte sequences. Not all of these encode an actual character, but
-- this enumerates everything that's accepted by the verification function.
create function all_euc_tw() returns setof bytea language plpgsql as
$$
declare
  byte1 integer;
  byte2 integer;
  byte3 integer;
  byte4 integer;
begin
  -- CNS 11643 Plane 1-7: encoded in 4 bytes, starting with 0x8e (SS2)
  byte1 = hex('8e');
  for byte2 in hex('a1')..hex('a7') loop
    for byte3 in hex('a1')..hex('fe') loop
      for byte4 in hex('a1')..hex('fe') loop
        return next b(byte1, byte2, byte3, byte4);
      end loop;
    end loop;
  end loop;

  -- 3-byte encoded values, starting with (SS3) are not accepted in EUC_TW.

  -- CNS 11643 Plane 1: encoded in 2 bytes
  for byte1 in hex('80')..hex('ff') loop
    if byte1 <> hex('8e') and byte1 <> hex('8f') then -- skip SS2 and SS3
      for byte2 in 161..254 loop -- 0xa1..0xfe
          return next b(byte1, byte2);
      end loop;
    end if;
  end loop;

  -- ASCII
  for byte1 in hex('01')..hex('7f') loop
    return next b(byte1);
  end loop;
end;
$$;

select euc_tw_char, convert_roundtrip(euc_tw_char, 'euc_tw', 'utf8') from all_euc_tw() as euc_tw_char;

select euc_tw_char, convert_roundtrip(euc_tw_char, 'euc_tw', 'mule_internal') from all_euc_tw() as euc_tw_char;

select euc_tw_char, convert_roundtrip(euc_tw_char, 'euc_tw', 'big5') from all_euc_tw() as euc_tw_char;
