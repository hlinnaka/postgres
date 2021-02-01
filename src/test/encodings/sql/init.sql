-- Wrapper around convert() function that catches exceptions.
create or replace function convert_nothrow(input bytea, src_encoding text, dst_encoding text, result out bytea, error out text) returns setof record language plpgsql as
$$
declare
begin
  begin
    result := convert(input, src_encoding, dst_encoding);
    error := NULL;
  exception when others then
    result := NULL;
     error := sqlerrm;
  end;
  return next;
end;
$$;

-- Test round trip converting a string
create or replace function convert_roundtrip(input bytea, src_encoding text, dst_encoding text) returns text language plpgsql as
$$
declare
  converted bytea;
  roundtripped bytea;
begin
  begin
    converted := convert(input, src_encoding, dst_encoding);
    roundtripped := convert(converted, dst_encoding, src_encoding);

    if roundtripped <> input then
      return converted::text || ', roundtripped to ' || roundtripped;
    else
      return converted::text;
    end if;

  exception when others then
    return sqlerrm;
  end;
end;
$$;

-- Convert a hexadecimal byte to int
create function hex(t text) returns int as
$$
  select ('x' || t)::bit(8)::int;
$$ immutable strict language sql;

-- Convert 1-4 integers representing bytes to bytea
create function b(int, int, int, int) returns bytea as
$$
  select ('\x' || lpad(to_hex($1), 2, '0')
               || lpad(to_hex($2), 2, '0')
               || lpad(to_hex($3), 2, '0')
               || lpad(to_hex($4), 2, '0'))::bytea;
$$ immutable strict language sql;

create function b(int, int, int) returns bytea as
$$
  select ('\x' || lpad(to_hex($1), 2, '0')
               || lpad(to_hex($2), 2, '0')
               || lpad(to_hex($3), 2, '0'))::bytea;
$$ immutable strict language sql;


create function b(int, int) returns bytea as
$$
  select ('\x' || lpad(to_hex($1), 2, '0')
               || lpad(to_hex($2), 2, '0'))::bytea;
$$ immutable strict language sql;

create function b(int) returns bytea as
$$
  select ('\x' || lpad(to_hex($1), 2, '0'))::bytea;
$$ immutable strict language sql;
