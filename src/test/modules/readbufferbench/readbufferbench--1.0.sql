/* src/test/modules/readbufferbench/readbufferbench--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION readbufferbench" to load this file. \quit


CREATE FUNCTION readbuffer_bench(regclass, int4, int4)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT PARALLEL SAFE;
