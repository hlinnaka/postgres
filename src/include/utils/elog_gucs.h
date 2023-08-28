/*-------------------------------------------------------------------------
 *
 * elog_gucs.h
 *	  XXX
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/elog_gucs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ELOG_GUCS_H
#define ELOG_GUCS_H

extern PGDLLIMPORT session_guc int Log_error_verbosity;
extern PGDLLIMPORT sighup_guc char *Log_line_prefix;
extern PGDLLIMPORT sighup_guc int Log_destination;
extern PGDLLIMPORT sighup_guc char *Log_destination_string;
extern PGDLLIMPORT sighup_guc bool syslog_sequence_numbers;
extern PGDLLIMPORT sighup_guc bool syslog_split_messages;

/* Log destination bitmap */
#define LOG_DESTINATION_STDERR	 1
#define LOG_DESTINATION_SYSLOG	 2
#define LOG_DESTINATION_EVENTLOG 4
#define LOG_DESTINATION_CSVLOG	 8
#define LOG_DESTINATION_JSONLOG	16

#endif							/* ELOG_H */
