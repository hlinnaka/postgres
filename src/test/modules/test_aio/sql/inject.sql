SELECT count(*) FROM tbl_b WHERE ctid = '(2, 1)';

-- injected what we'd expect
SELECT inj_io_short_read_attach(8192);
SELECT invalidate_rel_block('tbl_b', 2);
SELECT count(*) FROM tbl_b WHERE ctid = '(2, 1)';
SELECT inj_io_short_read_detach();


-- injected a read shorter than a single block, expecting error
SELECT inj_io_short_read_attach(17);
SELECT invalidate_rel_block('tbl_b', 2);
SELECT redact($$
  SELECT count(*) FROM tbl_b WHERE ctid = '(2, 1)';
$$);
SELECT inj_io_short_read_detach();


-- shorten multi-block read to a single block, should retry
SELECT count(*) FROM tbl_b; -- for comparison
SELECT invalidate_rel_block('tbl_b', 0);
SELECT invalidate_rel_block('tbl_b', 1);
SELECT invalidate_rel_block('tbl_b', 2);
SELECT inj_io_short_read_attach(8192);
-- no need to redact, no messages to client
SELECT count(*) FROM tbl_b;
SELECT inj_io_short_read_detach();


-- shorten multi-block read to 1 1/2 blocks, should retry
SELECT count(*) FROM tbl_b; -- for comparison
SELECT invalidate_rel_block('tbl_b', 0);
SELECT invalidate_rel_block('tbl_b', 1);
SELECT invalidate_rel_block('tbl_b', 2);
SELECT inj_io_short_read_attach(8192 + 4096);
-- no need to redact, no messages to client
SELECT count(*) FROM tbl_b;
SELECT inj_io_short_read_detach();


-- shorten single-block read to read that block partially, we'll error out,
-- because we assume we can read at least one block at a time.
SELECT count(*) FROM tbl_b WHERE ctid = '(2, 1)'; -- for comparison
SELECT invalidate_rel_block('tbl_b', 2);
SELECT inj_io_short_read_attach(4096);
SELECT redact($$
  SELECT count(*) FROM tbl_b WHERE ctid = '(2, 1)';
$$);
SELECT inj_io_short_read_detach();


-- shorten single-block read to read 0 bytes, expect that to error out
SELECT count(*) FROM tbl_b WHERE ctid = '(2, 1)'; -- for comparison
SELECT invalidate_rel_block('tbl_b', 2);
SELECT inj_io_short_read_attach(0);
SELECT redact($$
  SELECT count(*) FROM tbl_b WHERE ctid = '(2, 1)';
$$);
SELECT inj_io_short_read_detach();


-- verify that checksum errors are detected even as part of a shortened
-- multi-block read
-- (tbl_a, block 1 is corrupted)
SELECT redact($$
  SELECT count(*) FROM tbl_a WHERE ctid < '(2, 1)';
$$);
SELECT inj_io_short_read_attach(8192);
SELECT invalidate_rel_block('tbl_a', 0);
SELECT invalidate_rel_block('tbl_a', 1);
SELECT invalidate_rel_block('tbl_a', 2);
SELECT redact($$
  SELECT count(*) FROM tbl_a WHERE ctid < '(2, 1)';
$$);
SELECT inj_io_short_read_detach();


-- trigger a hard error, should error out
SELECT inj_io_short_read_attach(-errno_from_string('EIO'));
SELECT invalidate_rel_block('tbl_b', 2);
SELECT redact($$
  SELECT count(*) FROM tbl_b WHERE ctid = '(2, 1)';
$$);
SELECT inj_io_short_read_detach();
