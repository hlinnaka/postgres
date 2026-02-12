/*-------------------------------------------------------------------------
 *
 * procsignal.h
 *	  Routines for interprocess signaling
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/procsignal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROCSIGNAL_H
#define PROCSIGNAL_H

typedef enum
{
	PROCSIGNAL_BARRIER_SMGRRELEASE, /* ask smgr to close files */
	PROCSIGNAL_BARRIER_UPDATE_XLOG_LOGICAL_INFO,	/* ask to update
													 * XLogLogicalInfo */
} ProcSignalBarrierType;

/*
 * Length of query cancel keys generated.
 *
 * Note that the protocol allows for longer keys, or shorter, but this is the
 * length we actually generate.  Client code, and the server code that handles
 * incoming cancellation packets from clients, mustn't use this hardcoded
 * length.
 */
#define MAX_CANCEL_KEY_LENGTH  32

/*
 * prototypes for functions in procsignal.c
 */
extern Size ProcSignalShmemSize(void);
extern void ProcSignalShmemInit(void);

extern void ProcSignalInit(const uint8 *cancel_key, int cancel_key_len);
extern void SendCancelRequest(int backendPID, const uint8 *cancel_key, int cancel_key_len);

extern uint64 EmitProcSignalBarrier(ProcSignalBarrierType type);
extern void WaitForProcSignalBarrier(uint64 generation);
extern void ProcessProcSignalBarrier(void);

/* ProcSignalHeader is an opaque struct, details known only within procsignal.c */
typedef struct ProcSignalHeader ProcSignalHeader;

#ifdef EXEC_BACKEND
extern PGDLLIMPORT ProcSignalHeader *ProcSignal;
#endif

#endif							/* PROCSIGNAL_H */
