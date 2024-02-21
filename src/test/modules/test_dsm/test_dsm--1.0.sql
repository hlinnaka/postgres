/* src/test/modules/test_dsm/test_dsm--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_dsm" to load this file. \quit

CREATE FUNCTION test_dsm_basic()
	RETURNS pg_catalog.void
	AS 'MODULE_PATHNAME' LANGUAGE C;

