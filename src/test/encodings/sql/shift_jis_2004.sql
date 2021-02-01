-- Enumerate all valid SHIFT_JIS_2004 byte sequences. Not all of these encode an actual character, but
-- this enumerates everything that's accepted by the verification function.
create function all_shift_jis_2004() returns setof bytea language plpgsql as
$$
declare
  byte1 integer;
  byte2 integer;
begin
  -- chars encoded in 2 bytes.
  for byte1 in hex('81') .. hex('9f') loop
    for byte2 in hex('40') .. hex('7e') loop
      return next b(byte1, byte2);
    end loop;

    for byte2 in hex('80') .. hex('fc') loop
      return next b(byte1, byte2);
    end loop;
  end loop;

  for byte1 in hex('e0') .. hex('fc') loop
    for byte2 in hex('40') .. hex('7e') loop
      return next b(byte1, byte2);
    end loop;

    for byte2 in hex('80') .. hex('fc') loop
      return next b(byte1, byte2);
    end loop;
  end loop;

  -- ASCII
  for byte1 in hex('01')..hex('7f') loop
    return next b(byte1);
  end loop;
end;
$$;

select shift_jis_2004_char, convert_roundtrip(shift_jis_2004_char, 'shift_jis_2004', 'utf8') from all_shift_jis_2004() as shift_jis_2004_char;

select shift_jis_2004_char, convert_roundtrip(shift_jis_2004_char, 'shift_jis_2004', 'euc_jis_2004') from all_shift_jis_2004() as shift_jis_2004_char;
