/*-------------------------------------------------------------------------
 *
 * dsm_impl_mmap.c
 *	  Operating system primitives to support mmap-based shared memory.
 *
 * Calling this "shared memory" is somewhat of a misnomer, because what
 * we're really doing is creating a bunch of files and mapping them into
 * our address space.  The operating system may feel obliged to
 * synchronize the contents to disk even if nothing is being paged out,
 * which will not serve us well.  The user can relocate the pg_dynshmem
 * directory to a ramdisk to avoid this problem, if available.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/dsm_impl_mmap.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>
#ifndef WIN32
#include <sys/mman.h>
#include <sys/stat.h>
#endif

#include "common/file_perm.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "portability/mem.h"
#include "storage/dsm_impl.h"
#include "storage/fd.h"

#ifdef USE_DSM_MMAP

/* Size of buffer to be used for zero-filling. */
#define ZBUFFER_SIZE				8192

static dsm_impl_handle
dsm_impl_mmap_create(Size request_size, dsm_impl_private *impl_private,
					 void **mapped_address, int elevel)
{
	char		name[64];
	int			flags;
	int			fd;
	char	   *address;
	dsm_impl_handle handle;

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

		snprintf(name, 64, PG_DYNSHMEM_DIR "/" PG_DYNSHMEM_MMAP_FILE_PREFIX "%u",
				 handle);

		/* Create new segment or open an existing one for attach. */
		flags = O_RDWR | O_CREAT | O_EXCL;
		if ((fd = OpenTransientFile(name, flags)) == -1)
		{
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
	{
		/*
		 * Allocate a buffer full of zeros.
		 *
		 * Note: palloc zbuffer, instead of just using a local char array, to
		 * ensure it is reasonably well-aligned; this may save a few cycles
		 * transferring data to the kernel.
		 */
		char	   *zbuffer = (char *) palloc0(ZBUFFER_SIZE);
		Size		remaining = request_size;
		bool		success = true;

		/*
		 * Zero-fill the file. We have to do this the hard way to ensure that
		 * all the file space has really been allocated, so that we don't
		 * later seg fault when accessing the memory mapping.  This is pretty
		 * pessimal.
		 */
		while (success && remaining > 0)
		{
			Size		goal = remaining;

			if (goal > ZBUFFER_SIZE)
				goal = ZBUFFER_SIZE;
			pgstat_report_wait_start(WAIT_EVENT_DSM_FILL_ZERO_WRITE);
			if (write(fd, zbuffer, goal) == goal)
				remaining -= goal;
			else
				success = false;
			pgstat_report_wait_end();
		}

		if (!success)
		{
			int			save_errno;

			/* Back out what's already been done. */
			save_errno = errno;
			CloseTransientFile(fd);
			unlink(name);
			errno = save_errno ? save_errno : ENOSPC;

			ereport(elevel,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not resize shared memory segment \"%s\" to %zu bytes: %m",
							name, request_size)));
			return DSM_IMPL_HANDLE_INVALID;
		}
	}

	/* Map it to this process's address space. */
	address = mmap(NULL, request_size, PROT_READ | PROT_WRITE,
				   MAP_SHARED | MAP_HASSEMAPHORE | MAP_NOSYNC, fd, 0);
	if (address == MAP_FAILED)
	{
		int			save_errno;

		/* Back out what's already been done. */
		save_errno = errno;
		CloseTransientFile(fd);
		unlink(name);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not map shared memory segment \"%s\": %m",
						name)));
		return DSM_IMPL_HANDLE_INVALID;
	}

	/* Once it's mapped, we don't need the file descriptor for it anymore. */
	if (CloseTransientFile(fd) != 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not close shared memory segment \"%s\": %m",
						name)));
		return DSM_IMPL_HANDLE_INVALID;
	}

	*mapped_address = address;
	*impl_private = 0;			/* not used by this implementation */
	return handle;
}

static bool
dsm_impl_mmap_attach(dsm_impl_handle handle,
					 dsm_impl_private *impl_private, void **mapped_address, Size *mapped_size,
					 int elevel)
{
	char		name[64];
	int			flags;
	int			fd;
	char	   *address;
	Size		size;
	struct stat st;

	snprintf(name, 64, PG_DYNSHMEM_DIR "/" PG_DYNSHMEM_MMAP_FILE_PREFIX "%u",
			 handle);

	/* Open an existing file for attach. */
	flags = O_RDWR;
	if ((fd = OpenTransientFile(name, flags)) == -1)
	{
		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not open shared memory segment \"%s\": %m",
						name)));
		return false;
	}

	/* Determine the current size. */
	if (fstat(fd, &st) != 0)
	{
		int			save_errno;

		/* Back out what's already been done. */
		save_errno = errno;
		CloseTransientFile(fd);
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
		CloseTransientFile(fd);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not map shared memory segment \"%s\": %m",
						name)));
		return false;
	}

	/* Once it's mapped, we don't need the file descriptor for it anymore */
	if (CloseTransientFile(fd) != 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not close shared memory segment \"%s\": %m",
						name)));
		return false;
	}

	*mapped_address = address;
	*mapped_size = size;
	*impl_private = 0;			/* not used by this implementation */
	return true;
}

static bool
dsm_impl_mmap_detach(dsm_impl_handle handle,
					 dsm_impl_private impl_private, void *mapped_address, Size mapped_size,
					 int elevel)
{
	char		name[64];

	Assert(mapped_address != NULL);
	Assert(impl_private == 0);	/* not used by this implementation */

	snprintf(name, 64, PG_DYNSHMEM_DIR "/" PG_DYNSHMEM_MMAP_FILE_PREFIX "%u",
			 handle);

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
dsm_impl_mmap_destroy(dsm_impl_handle handle, int elevel)
{
	char		name[64];

	snprintf(name, 64, PG_DYNSHMEM_DIR "/" PG_DYNSHMEM_MMAP_FILE_PREFIX "%u",
			 handle);

	if (unlink(name) != 0)
	{
		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not remove shared memory segment \"%s\": %m",
						name)));
		return false;
	}
	return true;
}

const dsm_impl_ops dsm_impl_mmap_ops = {
	.create = dsm_impl_mmap_create,
	.attach = dsm_impl_mmap_attach,
	.detach = dsm_impl_mmap_detach,
	.destroy = dsm_impl_mmap_destroy,
	.pin_segment = dsm_impl_noop_pin_segment,
	.unpin_segment = dsm_impl_noop_unpin_segment,
};

#endif
