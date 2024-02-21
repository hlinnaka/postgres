/*-------------------------------------------------------------------------
 *
 * dsm_impl_posix.c
 *	  Operating system primitives to support POSIX shared memory.
 *
 * POSIX shared memory segments are created and attached using shm_open()
 * and shm_unlink(); other operations, such as sizing or mapping the
 * segment, are performed as if the shared memory segments were files.
 *
 * Indeed, on some platforms, they may be implemented that way.  While
 * POSIX shared memory segments seem intended to exist in a flat namespace,
 * some operating systems may implement them as files, even going so far
 * to treat a request for /xyz as a request to create a file by that name
 * in the root directory.  Users of such broken platforms should select
 * a different shared memory implementation.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/dsm_impl_posix.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#ifndef WIN32
#include <sys/mman.h>
#include <sys/stat.h>
#endif

#include "common/file_perm.h"
#include "common/pg_prng.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "portability/mem.h"
#include "storage/dsm_impl.h"
#include "storage/fd.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#ifdef USE_DSM_POSIX

static int	dsm_impl_posix_resize(int fd, off_t size);

static dsm_impl_handle
dsm_impl_posix_create(Size request_size, dsm_impl_private *impl_private,
					  void **mapped_address, int elevel)
{
	char		name[64];
	int			flags;
	int			fd;
	char	   *address;
	dsm_impl_handle handle;

	/*
	 * Even though we will close the FD before returning, it seems desirable
	 * to use Reserve/ReleaseExternalFD, to reduce the probability of EMFILE
	 * failure.  The fact that we won't hold the FD open long justifies using
	 * ReserveExternalFD rather than AcquireExternalFD, though.
	 */
	ReserveExternalFD();

	/*
	 * Create new segment, with a random name.  If the name is already in use,
	 * retry until we find an unused name.
	 */
	for (;;)
	{
		do
		{
			handle = pg_prng_uint32(&pg_global_prng_state);
		} while (handle == DSM_IMPL_HANDLE_INVALID);

		snprintf(name, 64, "/PostgreSQL.%u", handle);

		flags = O_RDWR | O_CREAT | O_EXCL;
		if ((fd = shm_open(name, flags, PG_FILE_MODE_OWNER)) == -1)
		{
			ReleaseExternalFD();
			if (errno == EEXIST)
				continue;
			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not open shared memory segment \"%s\": %m",
							name)));
			return DSM_IMPL_HANDLE_INVALID;
		}
		break;
	}

	/* Enlarge it to the requested size. */
	if (dsm_impl_posix_resize(fd, request_size) != 0)
	{
		int			save_errno;

		/* Back out what's already been done. */
		save_errno = errno;
		close(fd);
		ReleaseExternalFD();
		shm_unlink(name);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not resize shared memory segment \"%s\" to %zu bytes: %m",
						name, request_size)));
		return DSM_IMPL_HANDLE_INVALID;
	}

	/* Map it to this process's address space. */
	address = mmap(NULL, request_size, PROT_READ | PROT_WRITE,
				   MAP_SHARED | MAP_HASSEMAPHORE | MAP_NOSYNC, fd, 0);
	if (address == MAP_FAILED)
	{
		int			save_errno;

		/* Back out what's already been done. */
		save_errno = errno;
		close(fd);
		ReleaseExternalFD();
		shm_unlink(name);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not map shared memory segment \"%s\": %m",
						name)));
		return DSM_IMPL_HANDLE_INVALID;
	}

	/* Once it's mapped, we don't need the file descriptor for it anymore. */
	close(fd);
	ReleaseExternalFD();

	*mapped_address = address;
	*impl_private = 0;			/* not used by this implementation */
	return handle;
}

static bool
dsm_impl_posix_attach(dsm_impl_handle handle, dsm_impl_private *impl_private,
					  void **mapped_address, Size *mapped_size,
					  int elevel)
{
	char		name[64];
	int			flags;
	int			fd;
	char	   *address;
	Size		size;
	struct stat st;

	snprintf(name, 64, "/PostgreSQL.%u", handle);

	/* Like in dsm_impl_posix_attach, make sure an FD is available */
	ReserveExternalFD();

	flags = O_RDWR;
	if ((fd = shm_open(name, flags, PG_FILE_MODE_OWNER)) == -1)
	{
		ReleaseExternalFD();
		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not open shared memory segment \"%s\": %m",
						name)));
		return false;
	}

	/* Determine the current size */
	if (fstat(fd, &st) != 0)
	{
		int			save_errno;

		/* Back out what's already been done. */
		save_errno = errno;
		close(fd);
		ReleaseExternalFD();
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not stat shared memory segment \"%s\": %m",
						name)));
		return false;
	}
	size = st.st_size;

	/* Map it to this process's address space. */
	address = mmap(NULL, size, PROT_READ | PROT_WRITE,
				   MAP_SHARED | MAP_HASSEMAPHORE | MAP_NOSYNC, fd, 0);
	if (address == MAP_FAILED)
	{
		int			save_errno;

		/* Back out what's already been done. */
		save_errno = errno;
		close(fd);
		ReleaseExternalFD();
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not map shared memory segment \"%s\": %m",
						name)));
		return false;
	}

	/* Once it's mapped, we don't need the file descriptor for it anymore */
	close(fd);
	ReleaseExternalFD();

	*mapped_address = address;
	*mapped_size = size;
	*impl_private = 0;			/* not used by this implementation */
	return true;
}

static bool
dsm_impl_posix_detach(dsm_impl_handle handle, dsm_impl_private impl_private,
					  void *mapped_address, Size mapped_size,
					  int elevel)
{
	char		name[64];

	Assert(mapped_address != NULL);
	Assert(impl_private == 0);	/* not used by this implementation */

	snprintf(name, 64, "/PostgreSQL.%u", handle);

	if (munmap(mapped_address, mapped_size) != 0)
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
dsm_impl_posix_destroy(dsm_impl_handle handle, int elevel)
{
	char		name[64];

	snprintf(name, 64, "/PostgreSQL.%u", handle);

	if (shm_unlink(name) != 0)
	{
		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not remove shared memory segment \"%s\": %m",
						name)));
		return false;
	}
	return true;
}

/*
 * Set the size of a virtual memory region associated with a file descriptor.
 * If necessary, also ensure that virtual memory is actually allocated by the
 * operating system, to avoid nasty surprises later.
 *
 * Returns non-zero if either truncation or allocation fails, and sets errno.
 */
static int
dsm_impl_posix_resize(int fd, off_t size)
{
	int			rc;
	int			save_errno;
	sigset_t	save_sigmask;

	/*
	 * Block all blockable signals, except SIGQUIT.  posix_fallocate() can run
	 * for quite a long time, and is an all-or-nothing operation.  If we
	 * allowed SIGUSR1 to interrupt us repeatedly (for example, due to
	 * recovery conflicts), the retry loop might never succeed.
	 */
	if (IsUnderPostmaster)
		sigprocmask(SIG_SETMASK, &BlockSig, &save_sigmask);

	pgstat_report_wait_start(WAIT_EVENT_DSM_ALLOCATE);
#if defined(HAVE_POSIX_FALLOCATE) && defined(__linux__)

	/*
	 * On Linux, a shm_open fd is backed by a tmpfs file.  If we were to use
	 * ftruncate, the file would contain a hole.  Accessing memory backed by a
	 * hole causes tmpfs to allocate pages, which fails with SIGBUS if there
	 * is no more tmpfs space available.  So we ask tmpfs to allocate pages
	 * here, so we can fail gracefully with ENOSPC now rather than risking
	 * SIGBUS later.
	 *
	 * We still use a traditional EINTR retry loop to handle SIGCONT.
	 * posix_fallocate() doesn't restart automatically, and we don't want this
	 * to fail if you attach a debugger.
	 */
	do
	{
		rc = posix_fallocate(fd, 0, size);
	} while (rc == EINTR);

	/*
	 * The caller expects errno to be set, but posix_fallocate() doesn't set
	 * it.  Instead it returns error numbers directly.  So set errno, even
	 * though we'll also return rc to indicate success or failure.
	 */
	errno = rc;
#else
	/* Extend the file to the requested size. */
	do
	{
		rc = ftruncate(fd, size);
	} while (rc < 0 && errno == EINTR);
#endif
	pgstat_report_wait_end();

	if (IsUnderPostmaster)
	{
		save_errno = errno;
		sigprocmask(SIG_SETMASK, &save_sigmask, NULL);
		errno = save_errno;
	}

	return rc;
}

const dsm_impl_ops dsm_impl_posix_ops = {
	.create = dsm_impl_posix_create,
	.attach = dsm_impl_posix_attach,
	.detach = dsm_impl_posix_detach,
	.destroy = dsm_impl_posix_destroy,
	.pin_segment = dsm_impl_noop_pin_segment,
	.unpin_segment = dsm_impl_noop_unpin_segment,
};

#endif							/* USE_DSM_POSIX */
