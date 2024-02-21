/*-------------------------------------------------------------------------
 *
 * dsm_impl_windows.c
 *	  Operating system primitives to support Windows shared memory.
 *
 * Windows shared memory implementation is done using file mapping
 * which can be backed by either physical file or system paging file.
 * Current implementation uses system paging file as other effects
 * like performance are not clear for physical file and it is used in
 * similar way for main shared memory in windows.
 *
 * A memory mapping object is a kernel object - they always get deleted
 * when the last reference to them goes away, either explicitly via a
 * CloseHandle or when the process containing the reference exits.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/dsm_impl_windows.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/file_perm.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "portability/mem.h"
#include "postmaster/postmaster.h"
#include "storage/dsm_impl.h"
#include "storage/fd.h"
#include "utils/guc.h"

#define SEGMENT_NAME_PREFIX			"Global/PostgreSQL"

static dsm_impl_handle
dsm_impl_windows_create(Size request_size,
						dsm_impl_private *impl_private, void **mapped_address,
						int elevel)
{
	char	   *address;
	HANDLE		hmap;
	char		name[64];
	DWORD		size_high;
	DWORD		size_low;
	DWORD		errcode;
	dsm_impl_handle handle;

	/* Shifts >= the width of the type are undefined. */
#ifdef _WIN64
	size_high = request_size >> 32;
#else
	size_high = 0;
#endif
	size_low = (DWORD) request_size;

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

		/*
		 * Storing the shared memory segment in the Global\ namespace can
		 * allow any process running in any session to access that file
		 * mapping object provided that the caller has the required access
		 * rights.  But to avoid issues faced in main shared memory, we are
		 * using the naming convention similar to main shared memory.  We can
		 * change here once issue mentioned in GetSharedMemName is resolved.
		 */
		snprintf(name, 64, "%s.%u", SEGMENT_NAME_PREFIX, handle);

		/* CreateFileMapping might not clear the error code on success */
		SetLastError(0);

		hmap = CreateFileMapping(INVALID_HANDLE_VALUE,	/* Use the pagefile */
								 NULL,	/* Default security attrs */
								 PAGE_READWRITE,	/* Memory is read/write */
								 size_high, /* Upper 32 bits of size */
								 size_low,	/* Lower 32 bits of size */
								 name);

		errcode = GetLastError();
		if (errcode == ERROR_ALREADY_EXISTS || errcode == ERROR_ACCESS_DENIED)
		{
			/*
			 * On Windows, when the segment already exists, a handle for the
			 * existing segment is returned.  We must close it before
			 * retrying.  However, if the existing segment is created by a
			 * service, then it returns ERROR_ACCESS_DENIED. We don't do
			 * _dosmaperr here, so errno won't be modified.
			 */
			if (hmap)
				CloseHandle(hmap);
			continue;
		}
		break;
	}

	if (!hmap)
	{
		_dosmaperr(errcode);
		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not create shared memory segment \"%s\": %m",
						name)));
		return DSM_IMPL_HANDLE_INVALID;
	}

	/* Map it. */
	address = MapViewOfFile(hmap, FILE_MAP_WRITE | FILE_MAP_READ,
							0, 0, 0);
	if (!address)
	{
		int			save_errno;

		_dosmaperr(GetLastError());
		/* Back out what's already been done. */
		save_errno = errno;
		CloseHandle(hmap);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not map shared memory segment \"%s\": %m",
						name)));
		return DSM_IMPL_HANDLE_INVALID;
	}

	*mapped_address = address;
	*impl_private = (uintptr_t) hmap;

	return handle;
}

static bool
dsm_impl_windows_attach(dsm_impl_handle handle,
						dsm_impl_private *impl_private, void **mapped_address,
						Size *mapped_size, int elevel)
{
	char	   *address;
	HANDLE		hmap;
	char		name[64];
	MEMORY_BASIC_INFORMATION info;

	snprintf(name, 64, "%s.%u", SEGMENT_NAME_PREFIX, handle);

	/* Open the existing mapping object. */
	hmap = OpenFileMapping(FILE_MAP_WRITE | FILE_MAP_READ,
						   FALSE,	/* do not inherit the name */
						   name);	/* name of mapping object */
	if (!hmap)
	{
		_dosmaperr(GetLastError());
		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not open shared memory segment \"%s\": %m",
						name)));
		return false;
	}

	/* Map it to this process's address space. */
	address = MapViewOfFile(hmap, FILE_MAP_WRITE | FILE_MAP_READ,
							0, 0, 0);
	if (!address)
	{
		int			save_errno;

		_dosmaperr(GetLastError());
		/* Back out what's already been done. */
		save_errno = errno;
		CloseHandle(hmap);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not map shared memory segment \"%s\": %m",
						name)));
		return false;
	}

	/*
	 * Determine the size of the mapping.
	 *
	 * Note: Windows rounds up the mapping size to the 4K page size, so this
	 * can be larger than what was requested when the segment was created.
	 */
	if (VirtualQuery(address, &info, sizeof(info)) == 0)
	{
		int			save_errno;

		_dosmaperr(GetLastError());
		/* Back out what's already been done. */
		save_errno = errno;
		UnmapViewOfFile(address);
		CloseHandle(hmap);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not stat shared memory segment \"%s\": %m",
						name)));
		return false;
	}

	*mapped_address = address;
	*mapped_size = info.RegionSize;
	*impl_private = (uintptr_t) hmap;

	return true;
}

static bool
dsm_impl_windows_detach(dsm_impl_handle handle,
						dsm_impl_private impl_private, void *mapped_address,
						Size mapped_size, int elevel)
{
	HANDLE		hmap;
	char		name[64];

	Assert(mapped_address != NULL);
	hmap = (HANDLE) impl_private;

	snprintf(name, 64, "%s.%u", SEGMENT_NAME_PREFIX, handle);

	/*
	 * Handle teardown cases.
	 */
	if (UnmapViewOfFile(mapped_address) == 0)
	{
		_dosmaperr(GetLastError());
		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not unmap shared memory segment \"%s\": %m",
						name)));
		return false;
	}
	if (CloseHandle(hmap) == 0)
	{
		_dosmaperr(GetLastError());
		ereport(elevel,
				(errcode_for_dynamic_shared_memory(),
				 errmsg("could not remove shared memory segment \"%s\": %m",
						name)));
		return false;
	}

	return true;
}

/*
 * Since Windows automatically destroys the object when no references remain,
 * this is a no-op.
 */
static bool
dsm_impl_windows_destroy(dsm_impl_handle handle, int elevel)
{
	return true;
}

/*
 * Windows cleans up segments automatically when no references remain. That's
 * handy, but if a segment needs to be preserved even when no backend has it
 * attached, we need to take measures to prevent it from being cleaned up.  To
 * prevent it, we duplicate the segment handle into the postmaster process.
 * The postmaster needn't do anything to receive the handle; Windows transfers
 * it automatically.
 */
static void
dsm_impl_windows_pin_segment(dsm_impl_handle handle, dsm_impl_private impl_private,
							 dsm_impl_private_pm_handle *pm_handle)
{
	if (IsUnderPostmaster)
	{
		HANDLE		hmap = (HANDLE) impl_private;
		HANDLE		pm_hmap;

		if (!DuplicateHandle(GetCurrentProcess(), hmap,
							 PostmasterHandle, &pm_hmap, 0, FALSE,
							 DUPLICATE_SAME_ACCESS))
		{
			char		name[64];

			snprintf(name, 64, "%s.%u", SEGMENT_NAME_PREFIX, handle);
			_dosmaperr(GetLastError());
			ereport(ERROR,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not duplicate handle for \"%s\": %m",
							name)));
		}

		/*
		 * Here, we remember the handle that we created in the postmaster
		 * process.  This handle isn't actually usable in any process other
		 * than the postmaster, but that doesn't matter.  We're just holding
		 * onto it so that, if the segment is unpinned, dsm_unpin_segment can
		 * close it.
		 */
		*pm_handle = (uintptr_t) pm_hmap;
	}
}

/*
 * Close the extra handle that pin_segment created in the postmaster's process
 * space.
 */
static void
dsm_impl_windows_unpin_segment(dsm_impl_handle handle, dsm_impl_private_pm_handle pm_handle)
{
	if (IsUnderPostmaster)
	{
		HANDLE		pm_hmap = (HANDLE) pm_handle;

		if (!DuplicateHandle(PostmasterHandle, pm_hmap,
							 NULL, NULL, 0, FALSE,
							 DUPLICATE_CLOSE_SOURCE))
		{
			char		name[64];

			snprintf(name, 64, "%s.%u", SEGMENT_NAME_PREFIX, handle);
			_dosmaperr(GetLastError());
			ereport(ERROR,
					(errcode_for_dynamic_shared_memory(),
					 errmsg("could not duplicate handle for \"%s\": %m",
							name)));
		}
	}
}

const dsm_impl_ops dsm_impl_windows_ops = {
	.create = dsm_impl_windows_create,
	.attach = dsm_impl_windows_attach,
	.detach = dsm_impl_windows_detach,
	.destroy = dsm_impl_windows_destroy,
	.pin_segment = dsm_impl_windows_pin_segment,
	.unpin_segment = dsm_impl_windows_unpin_segment,
};
