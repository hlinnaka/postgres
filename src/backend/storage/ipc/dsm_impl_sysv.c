/*-------------------------------------------------------------------------
 *
 * dsm_impl_sysv.c
 *	  Operating system primitives to support System V shared memory.
 *
 * System V shared memory segments are manipulated using shmget(), shmat(),
 * shmdt(), and shmctl().  As the default allocation limits for System V
 * shared memory are usually quite low, the POSIX facilities may be
 * preferable; but those are not supported everywhere.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/dsm_impl_sysv.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>

#include "common/file_perm.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "portability/mem.h"
#include "postmaster/postmaster.h"
#include "storage/dsm_impl.h"
#include "storage/fd.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#ifdef USE_DSM_SYSV

/*
 * Return the segment's shmid as a handle.
 *
 * Because 0 is a valid shmid, but not a valid dsm_impl_handle, add one to
 * avoid it.  The valid range for shmid is from 0 to INT_MAX, so it fits in
 * uint32 this way.
 */
static inline dsm_impl_handle
handle_from_shmid(int shmid)
{
	return ((uint32) shmid) + 1;
}

static inline int
shmid_from_handle(dsm_impl_handle handle)
{
	return handle - 1;
}

static dsm_impl_handle
dsm_impl_sysv_create(Size request_size,
					 dsm_impl_private *impl_private, void **mapped_address,
					 int elevel)
{
	int			ident;
	char	   *address;
	char		name[64];

	/*
	 * Create a new shared memory segment.  We let shmget() pick a unique
	 * identifier for us.
	 */
	if ((ident = shmget(IPC_PRIVATE, request_size, IPCProtection | IPC_CREAT | IPC_EXCL)) == -1)
	{
		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not create shared memory segment: %m")));
		return DSM_IMPL_HANDLE_INVALID;
	}

	/*
	 * POSIX shared memory and mmap-based shared memory identify segments with
	 * names.  To avoid needless error message variation, we use the shmid as
	 * the name.
	 */
	snprintf(name, 64, "shmid %d", ident);

	/* Map it to this process's address space. */
	address = shmat(ident, NULL, PG_SHMAT_FLAGS);
	if (address == (void *) -1)
	{
		int			save_errno;

		/* Back out what's already been done. */
		save_errno = errno;
		shmctl(ident, IPC_RMID, NULL);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not map shared memory segment \"%s\": %m",
						name)));
		return DSM_IMPL_HANDLE_INVALID;
	}
	*mapped_address = address;
	*impl_private = 0;			/* not used by this implementation */

	return handle_from_shmid(ident);
}

static bool
dsm_impl_sysv_attach(dsm_impl_handle handle,
					 dsm_impl_private *impl_private, void **mapped_address, Size *mapped_size,
					 int elevel)
{
	int			ident = shmid_from_handle(handle);
	char	   *address;
	char		name[64];
	struct shmid_ds shm;
	Size		size;

	/*
	 * POSIX shared memory and mmap-based shared memory identify segments with
	 * names.  To avoid needless error message variation, we use the shmid as
	 * the name.
	 */
	snprintf(name, 64, "shmid %d", ident);

	/* Use IPC_STAT to determine the size. */
	if (shmctl(ident, IPC_STAT, &shm) != 0)
	{
		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not stat shared memory segment \"%s\": %m",
						name)));
		return false;
	}
	size = shm.shm_segsz;

	/* Map it to this process's address space. */
	address = shmat(ident, NULL, PG_SHMAT_FLAGS);
	if (address == (void *) -1)
	{
		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not map shared memory segment \"%s\": %m",
						name)));
		return false;
	}
	*mapped_address = address;
	*mapped_size = size;
	*impl_private = 0;			/* not used by this implementation */

	return true;
}


static bool
dsm_impl_sysv_detach(dsm_impl_handle handle,
					 dsm_impl_private impl_private, void *mapped_address, Size mapped_size,
					 int elevel)
{
	int			ident = shmid_from_handle(handle);
	char		name[64];

	Assert(mapped_address != NULL);
	Assert(impl_private == 0);	/* not used by this implementation */

	/*
	 * POSIX shared memory and mmap-based shared memory identify segments with
	 * names.  To avoid needless error message variation, we use the shmid as
	 * the name.
	 */
	snprintf(name, 64, "shmid %d", ident);

	if (shmdt(mapped_address) != 0)
	{
		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not unmap shared memory segment \"%s\": %m",
						name)));
		return false;
	}
	return true;
}

static bool
dsm_impl_sysv_destroy(dsm_impl_handle handle, int elevel)
{
	int			ident = shmid_from_handle(handle);
	char		name[64];

	/*
	 * POSIX shared memory and mmap-based shared memory identify segments with
	 * names.  To avoid needless error message variation, we use the shmid as
	 * the name.
	 */
	snprintf(name, 64, "shmid %d", ident);

	/* Handle teardown cases. */
	if (shmctl(ident, IPC_RMID, NULL) < 0)
	{
		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not remove shared memory segment \"%s\": %m",
						name)));
		return false;
	}
	return true;
}

const dsm_impl_ops dsm_impl_sysv_ops = {
	.create = dsm_impl_sysv_create,
	.attach = dsm_impl_sysv_attach,
	.detach = dsm_impl_sysv_detach,
	.destroy = dsm_impl_sysv_destroy,
	.pin_segment = dsm_impl_noop_pin_segment,
	.unpin_segment = dsm_impl_noop_unpin_segment,
};

#endif
