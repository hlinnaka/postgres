-- Enumerate all valid Big5 byte sequences. Not all of these encode an actual character, but
-- this enumerates everything that's accepted by the verification function.
create function all_big5() returns setof bytea language plpgsql as
$$
declare
  byte1 integer;
  byte2 integer;
begin
 -- Enumerate all 2-byte combinations
  for byte1 in hex('80')..hex('ff') loop
    for byte2 in hex('01')..hex('ff') loop
       return next b(byte1, byte2);
    end loop;
  end loop;

  -- ASCII
  for byte1 in hex('01')..hex('7f') loop
    return next b(byte1);
  end loop;
end;
$$;

select big5_char, convert_roundtrip(big5_char, 'big5', 'utf8') from all_big5() as big5_char;

select big5_char, convert_roundtrip(big5_char, 'big5', 'mule_internal') from all_big5() as big5_char;

select big5_char, convert_roundtrip(big5_char, 'big5', 'euc_tw') from all_big5() as big5_char;
