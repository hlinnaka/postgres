Interrupts
==========

"Interrupts" are a set of flags that represent conditions that should
be handled at a later time.  They are roughly analogous to Unix
signals, except that they are handled cooperatively by checking for
them at many points in the code. The CHECK_FOR_INTERRUPTS() macro is
called at strategically located spots where it is normally safe to
process interrupts.

Well behaved backend code calls CHECK_FOR_INTERRUPTS() periodically in
any long-running loops or other long computations, and never sleeps
using mechanisms other than the WaitEventSet mechanism or the more
convenient WaitInterrupt / WaitSockerOrInterrupt functions (except for
bounded short periods, eg LWLock waits). Following these rules ensures
that the backend doesn't become uncancellable for too long.

Each interrupt type can have a handler function associated with it,
which will be called by CHECK_FOR_INTERRUPTS() if the interrupt is
pending.  A standard set of interrupt handlers are installed at
process startup, but they can be overridden by
SetInterruptHandler(). A handler can also be temporarily disabled by
calling DisableInterrupt and re-enabled by EnableInterrupt.

If an interrupt is raised that doesn't have a handler function or it
is not enabled, the interrupt will remain pending until it is cleared,
or the handler is re-enabled.

If an interrupt is sent that was already pending for the target
process, it is coalesced, ie. the target process will process it only
once. Attempting to set an interrupt bit that is already set is very
fast, amounting to merely an atomic op in shared memory.

See src/include/ipc/interrupt.h for a list of all the interrupts used
in core code. Extensions can define their own interrupts.

HOLD/RESUME_INTERRUPTS
----------------------

Processing an interrupt may throw an error with ereport() (for
example, query cancellation) or terminate the process gracefully. In
some cases, we invoke CHECK_FOR_INTERRUPTS() inside low-level
subroutines that might sometimes be called in contexts that do *not*
want to allow interrupt processing.  The HOLD_INTERRUPTS() and
RESUME_INTERRUPTS() macros allow code to ensure that no interrupt
handlers are called, even if CHECK_FOR_INTERRUPTS() gets called in a
subroutine.  The interrupt will be held off until
CHECK_FOR_INTERRUPTS() is done outside any HOLD_INTERRUPTS() ...
RESUME_INTERRUPTS() section.

HOLD_INTERRUPTS() blocks can be nested. If HOLD_INTERRUPTS() is called
multiple times, interrupts are held until every HOLD_INTERRUPTS() call
has been balanced with a RESUME_INTERRUPTS() call.

Critical sections
-----------------

A related, but conceptually distinct, mechanism is the "critical
section" mechanism.  A critical section not only holds off cancel/die
interrupts, but causes any ereport(ERROR) or ereport(FATAL) to become
ereport(PANIC) --- that is, a system-wide reset is forced.  Needless
to say, only really *critical* code should be marked as a critical
section! This mechanism is mostly used for XLOG-related code.

Sending interrupts
------------------

Interrupt flags can be "raised" in different ways:
- synchronously by code that wants to defer an action, by calling
  RaiseInterrupt,
- asynchronously by timers or other signal handlers, also by calling
  RaiseInterrupt, or
- "sent" by other backends with SendInterrupt

Processing interrupts
---------------------

Interrupts are usually handled by handler functions, which are called
from CHECK_FOR_INTERRUPTS() when the corresponding interrupt is
pending. Use SetInterruptHandler() to install a handler function.

Sometimes you don't want an interrupt to be processed at any random
CHECK_FOR_INTERRUPTS() invocation. In that case, you can leave the
interrupt handler disabled, and check for the interrupt explicitly at
suitable spots with InterruptPending().

INTERRUPTS_PENDING_CONDITION() can be checked to see whether an
interrupt needs to be serviced, without trying to do so immediately.
Some callers are also interested in INTERRUPTS_CAN_BE_PROCESSED(),
which tells whether CHECK_FOR_INTERRUPTS() is sure to clear the
interrupt.


Waiting for an interrupt
------------------------
The correct pattern to wait for an event(s) that raises an interrupt
is:

```
for (;;)
{
	/* Handle any other interrupts received while we're waiting */
	CHECK_FOR_INTERRUPTS();

	/* Check if our interrupt was raised (clearing it if it was) */
	if (ConsumeInterrupt(INTERRUPT_PRINTER_ON_FIRE))
		break;

	/*
	 * Sleep until our interrupt or one of the standard interrupts is
	 * received.
	 */
	WaitInterrupt(INTERRUPT_CFI_MASK |
				  INTERRUPT_PRINTER_ON_FIRE,
				  ...);
}
```

Often an interrupt is used to wake up a process that is known to be
waiting and not doing anything else. The INTERRUPT_WAIT_WAKEUP
interrupt is reserved for such use cases where you don't need a
dedicated interrupt flag. When using INTERRUPT_WAIT_WAKEUP, you need
some other signaling, e.g. a flag in shared memory, to indicate that
the desired event has happen, as you may sometimes receive false
wakeups. The pattern using INTERRUPT_WAIT_WAKEUP looks like this:

```
for (;;)
{
	/* Handle any other interrupts received while we're waiting */
	CHECK_FOR_INTERRUPTS();

	ClearInterrupt(INTERRUPT_WAIT_WAKEUP);
	if (work to do)
	{
		/* in particular, exit the loop if some condition satisfied */
		Do Stuff();
	}
	WaitInterrupt(INTERRUPT_WAIT_WAKEUP, ...);
}
```

It's important to clear the interrupt *before* checking if there's
work to do. Otherwise, if someone sets the interrupt between the check
and the ClearInterrupt() call, you will miss it and Wait will
incorrectly block. Another valid coding pattern looks like:

```
for (;;)
{
	if (work to do)
	{
		/* in particular, exit the loop if some condition satisfied */
		Do Stuff();
	}
	WaitInterrupt(INTERRUPT_WAIT_WAKEUP, ...);
	ClearInterrupt(INTERRUPT_WAIT_WAKEUP);
}
```

This is useful to reduce interrupt traffic if it's expected that the
loop's termination condition will often be satisfied in the first
iteration; the cost is an extra loop iteration before blocking when it
is not.  What must be avoided is placing any checks for asynchronous
events after WaitInterrupt and before ClearInterrupt, as that creates
a race condition.

Custom interrupts in extensions
-------------------------------

An extension can allocate interrupt bits for its own purposes by
calling RequestAddinInterrupt(). Note that interrupts are a somewhat
scarce resource, so consider using INTERRUPT_WAIT_WAKEUP if all you
need is a simple wakeup in some loop.


Unix Signals
============

Postmaster and most child processes also respond to a few standard
Unix signals:

SIGHUP -> Raises INTERRUPT_CONFIG_RELOAD
SIGINT -> Raises INTERRUPT_QUERY_CANCEL
SIGTERM -> Raises INTERRUPT_TERMINATE
SIGQUIT -> immediate shutdown, abort the process

pg_ctl uses these Unix signals to tell postmaster to reload config,
stop, etc. These are also mentioned in the user documentation.

Postmaster cannot send interrupts to processes without PGPROC entries
(just syslogger nowadays), and it doesn't know the PGPROC entries of
other child processes anyway. Hence, it still uses the above Unix
signals for postmaster -> child signaling. The only exception is when
postmaster notifies a backend that a bgworker it launched has
exited. Postmaster sends that interrupt directly. The backend
registers explicitly for that notification, and supplies the
ProcNumber to postmaster when registering.

There are a few more exceptions:

- A few processes like the checkpointer, archiver, and IO workers
  don't react to SIGTERM, because Unix system shutdown sends a SIGTERM
  to all processes, and we want them to exit only only after all other
  process. Instead, the postmaster sends them SIGUSR2 after all the
  other processes have exited.

- Child processes use SIGUSR1 to request the postmaster to do various
  actions or notify that some event has occurred. See pmsignal.c for
  details on that mechanism


TODO: Open questions
====================

TODO: Put pendingInterrupts in PMChildSlot or PGPROC?
----------------------------------------------------

Postmaster cannot send interrupts to processes without PGPROC
entries. Hence, it still uses plain Unix signals. Would be nice if
postmaster could send interrupts directly. If we moved the interrupt
mask to PMChildSlot, it could.  However, PGPROC seems like a more
natural location otherwise.

Options:

a) Move pendingInterrupts to PMChildSlot. That way, you can send signals
   to processes also to processes that don't have a PGPROC entry.

b) Add a pendingInterrupts mask to PMChildSlot, but also have it in PGPROC.
   When postmaster sens an interrupt, it can check if it has a PGPROC entry,
   and if not, use the pendingInterrupts field in PMChildSlot instead.

c) Assign a PGPROC entry for every child process in postmaster already.
   (Except dead-end backends).

d) Keep it as it is, continue to use signals for postmaster -> child
   signaling


TODO: Unique session id
----------------------

In some places, we read the pid of a process (from PGPROC or
elsewhere), and send signal to it, accepting that the process may have
already exited.  If we directly replace those uses of 'pid' with a
ProcNumber, the ProcNumber might get reused much faster than the pid
would. Solutions:

a) Introduce the concept of a unique session ID that is never recycled
   (or not for a long time, anyway). When reading the ProcNumber to
   send an interrupt to, also read the session ID. When sending the
   interrupt, check that the session ID matches.


TODO: Other
-----------

- Check performance of CHECK_FOR_INTERRUPTS(). It's very cheap, but
  it's nevertheless more instructions than it used to be.
