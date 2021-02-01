-- Enumerate all valid GB18030 byte sequences. Not all of these encode an actual character, but
-- this enumerates everything that's accepted by the verification function.

create function most_gb18030() returns setof bytea language plpgsql as
$$
declare
  byte1 integer;
  byte2 integer;
  byte3 integer;
  byte4 integer;
begin
  -- 4-byte encoded values have:
  -- byte1 in range 0x81-0xfe
  -- byte2 in range 0x30-0x39
  -- byte3 in range 0x81-0xfe
  -- byte2 in range 0x30-0x39
  for byte1 in hex('81') .. hex('fe') loop

     -- Skip some values that are not interesting, to keep the table
     -- manageable.
     if (byte1 >= hex('85') and byte1 <= hex('8e')) or
        (byte1 >= hex('e4') and byte1 <= hex('fd')) then
       continue;
     end if;

     -- skip more values
     if (byte1 >= hex('a1') and byte1 <= hex('d0')) then
       continue;
     end if;

     for byte2 in hex('30') .. hex('39') loop
      for byte3 in hex('81') .. hex('fe') loop
          for byte4 in hex('30') .. hex('39') loop
              return next b(byte1, byte2, byte3, byte4);
          end loop;
	  --
      end loop;
    end loop;
  end loop;

  -- 2-byte encoded values have:
  -- byte1 in range 0x81-0xfe
  -- byte2 in range 0x40-0x7e or 0x80-0xfe
  for byte1 in hex('81') .. hex('fe') loop
    for byte2 in hex('40') .. hex('7e') loop
      return next b(byte1, byte2);
    end loop;

    for byte2 in hex('80') .. hex('fe') loop
      return next b(byte1, byte2);
    end loop;
  end loop;

  -- ASCII
  for byte1 in hex('01')..hex('7f') loop
    return next b(byte1);
  end loop;
end;
$$;

select gb18030_char, convert_roundtrip(gb18030_char, 'gb18030', 'utf8') from most_gb18030() as gb18030_char;
