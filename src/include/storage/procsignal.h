/*-------------------------------------------------------------------------
 *
 * procsignal.h
 *	  Routines for interprocess signaling
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
} ProcSignalBarrierType;

/*
 * prototypes for functions in procsignal.c
 */
extern Size ProcSignalShmemSize(void);
extern void ProcSignalShmemInit(void);

extern void ProcSignalInit(bool cancel_key_valid, int32 cancel_key);
extern void SendCancelRequest(int backendPID, int32 cancelAuthCode);

extern uint64 EmitProcSignalBarrier(ProcSignalBarrierType type);
extern void WaitForProcSignalBarrier(uint64 generation);
extern void ProcessProcSignalBarrier(void);

/* ProcSignalHeader is an opaque struct, details known only within procsignal.c */
typedef struct ProcSignalHeader ProcSignalHeader;

#ifdef EXEC_BACKEND
extern PGDLLIMPORT ProcSignalHeader *ProcSignal;
#endif

#endif							/* PROCSIGNAL_H */
