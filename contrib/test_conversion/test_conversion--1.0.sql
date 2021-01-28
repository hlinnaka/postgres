/* contrib/test_conversion/test_conversion--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_conversion" to load this file. \quit

CREATE FUNCTION latin1_to_latin2(
    INTEGER,  -- source encoding id
    INTEGER,  -- destination encoding id
    CSTRING,  -- source string (null terminated C string)
    CSTRING,  -- destination string (null terminated C string)
    INTEGER   -- source string length
) returns VOID IMMUTABLE STRICT;

CREATE DEFAULT CONVERSION FOR 'latin1' TO 'latin2' FROM 'latin1_to_latin2';
