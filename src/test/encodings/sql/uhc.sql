-- Enumerate all valid UHC byte sequences. Not all of these encode an actual character, but
-- this enumerates everything that's accepted by the verification function.
create function all_uhc() returns setof bytea language plpgsql as
$$
declare
  byte1 integer;
  byte2 integer;
begin
  -- chars encoded in 2 bytes.
  for byte1 in hex('80') .. hex('ff') loop
    for byte2 in hex('01') .. hex('ff') loop
      return next b(byte1, byte2);
    end loop;
  end loop;

  -- ASCII
  for byte1 in hex('01')..hex('7f') loop
    return next b(byte1);
  end loop;
end;
$$;

select uhc_char, convert_roundtrip(uhc_char, 'uhc', 'utf8') from all_uhc() as uhc_char;
