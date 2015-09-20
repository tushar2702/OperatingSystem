/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
*/

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Maximum level of nesting for priority donation. */
#define NESTING_DEPTH 8

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
     decrement it.

   - up or "V": increment the value (and wake up one waiting
     thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) 
{
  ASSERT (sema != NULL);

  sema->value = value;
  list_init (&sema->waiters);
}

/* Higher priority thread donates its priority if it is blocked
 * on a lock acquired by a lower priority thread. It checks for
 * possible nested priority donations upto NESTING_DEPTH. */
static void
donate_priority (void)
{
	struct thread *curr = thread_current ();
	struct lock *lock = curr->waiting_for_lock;
	int depth = 0;
	while (lock && depth < NESTING_DEPTH) {
		if(lock->holder) {
			if (lock->holder->priority < curr->priority) {
				lock->holder->priority = curr->priority;
			}
		}
		curr = lock->holder;
		lock = curr->waiting_for_lock;
		depth++;
	}
}

/* Updates priority back to original priority after lock release or explicit
 * call from thread_set_priority(). During nested donations, updates the
 * current thread priority to highest priority of element in its donor list.
 */
void
update_priority ()
{
	struct thread *cur = thread_current ();
	cur->priority = cur->original_priority;
	if (!list_empty (&cur->donor_list)) {
		struct thread *top_donor = list_entry (list_front (&cur->donor_list),
				struct thread, donor_elem);
		if (cur->priority < top_donor->priority)
			cur->priority = top_donor->priority;
	}
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. */
void
sema_down (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  while (sema->value == 0) 
    {
	  donate_priority();
//      list_push_back (&sema->waiters, &thread_current ()->elem);
      list_insert_ordered (&sema->waiters,
    		  &thread_current ()->elem,
			  (list_less_func *) &compare_priority,
			  NULL);
      thread_block ();
    }
  sema->value--;
  intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) 
{
  enum intr_level old_level;
  bool success;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (sema->value > 0) 
    {
      sema->value--;
      success = true; 
    }
  else
    success = false;
  intr_set_level (old_level);

  return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) 
{
  enum intr_level old_level;
  ASSERT (sema != NULL);
  old_level = intr_disable ();

  if (!list_empty (&sema->waiters)) {
	list_sort (&sema->waiters, (list_less_func *) &compare_priority, NULL);
    struct thread *waiter_thread = list_entry (list_pop_front (&sema->waiters),
                                struct thread, elem);
    thread_unblock (waiter_thread);
  }

  sema->value++;
  /* When a lower priority thread releases a lock on which higher priority
   * threads were blocked, so control should be immediately given to the
   * unblocked higher priority thread. */
  if(thread_current ()->priority < get_top_thread_priority ()) {
    	  thread_yield ();
  }

  intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) 
{
  struct semaphore sema[2];
  int i;

  printf ("Testing semaphores...");
  sema_init (&sema[0], 0);
  sema_init (&sema[1], 0);
  thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) 
    {
      sema_up (&sema[0]);
      sema_down (&sema[1]);
    }
  printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) 
{
  struct semaphore *sema = sema_;
  int i;

  for (i = 0; i < 10; i++) 
    {
      sema_down (&sema[0]);
      sema_up (&sema[1]);
    }
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock)
{
  ASSERT (lock != NULL);

  lock->holder = NULL;
  sema_init (&lock->semaphore, 1);
}

/* If the thread is blocked on any lock, it adds itself to
 * the lock holder's donor list. */
static void
add_to_donor_list (struct lock *lock)
{
	if (lock->holder) {
		thread_current ()->waiting_for_lock = lock;
		list_insert_ordered (&lock->holder->donor_list,
				&thread_current ()->donor_elem,
				(list_less_func *) &compare_priority,
				NULL);
	  }
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  add_to_donor_list(lock);
  sema_down (&lock->semaphore);
  lock->holder = thread_current ();
  thread_current ()->waiting_for_lock = NULL;
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock)
{
  bool success;

  ASSERT (lock != NULL);
  ASSERT (!lock_held_by_current_thread (lock));

  success = sema_try_down (&lock->semaphore);
  if (success)
    lock->holder = thread_current ();
  return success;
}

/* After a thread releases a lock, it removes all the threads
 * waiting on this lock from the lock holder's donor list. */
static void
remove_from_donor_list (struct lock *lock)
{
	struct thread *lock_holder = lock->holder;
	struct list_elem *e;
	for (e = list_begin (&lock_holder->donor_list);
			e != list_end (&lock_holder->donor_list);
			e = list_next (e)) {
		 struct thread *t = list_entry (e, struct thread, donor_elem);
		 if (t->waiting_for_lock == lock) {
			 list_remove (e);
		 }
	}
}

/* Releases LOCK, which must be owned by the current thread.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) 
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));

  /* Remove thread from donor_list waiting on this lock. */
  remove_from_donor_list (lock);
  lock->holder = NULL;
  /* Current thread's priority is updated to highest priority thread present
   * in donor_list or set to original priority. */
  update_priority ();
  sema_up (&lock->semaphore);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) 
{
  ASSERT (lock != NULL);

  return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem 
  {
    struct list_elem elem;              /* List element. */
    struct semaphore semaphore;         /* This semaphore. */
  };

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond)
{
  ASSERT (cond != NULL);

  list_init (&cond->waiters);
}

/* Comparison based on priority associated with each semaphore waiter list. */
static bool
compare_sema_priority (struct list_elem *a, struct list_elem *b,
		void *aux UNUSED)
{
	struct semaphore_elem *sema1 =  list_entry (a,
			struct semaphore_elem, elem);
	struct list *sema1_waiters = &sema1->semaphore.waiters;

	struct semaphore_elem *sema2 =  list_entry (b,
			struct semaphore_elem, elem);
	struct list *sema2_waiters = &sema2->semaphore.waiters;

	/* If the second list is empty, then the first one has higher
	 * priority so return true. */
//	if (list_empty (sema2_waiters))
//		return true;
	/* If the first list is empty, then the second one has higher
	 * priority so return false. */
//	if (list_empty (sema1_waiters))
//		return false;

//	list_sort (sema1_waiters, (list_less_func *) &compare_priority, NULL);
//	list_sort (sema2_waiters, (list_less_func *) &compare_priority, NULL);

	struct thread *thread1 = list_entry(list_begin (sema1_waiters),
			struct thread, elem);
	struct thread *thread2 = list_entry(list_begin (sema2_waiters),
			struct thread,	elem);
	return (thread1->priority > thread2->priority);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
cond_wait (struct condition *cond, struct lock *lock) 
{
  struct semaphore_elem waiter;

  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));
  
  sema_init (&waiter.semaphore, 0);
//  list_push_back (&cond->waiters, &waiter.elem);
  list_insert_ordered (&cond->waiters,
		  &waiter.elem,
		  (list_less_func *) &compare_sema_priority,
		  NULL);
  lock_release (lock);
  sema_down (&waiter.semaphore);
  lock_acquire (lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  if (!list_empty (&cond->waiters)) {
	  list_sort (&cond->waiters,
			  (list_less_func *) &compare_sema_priority, NULL);
      sema_up (&list_entry (list_pop_front (&cond->waiters),
    		  struct semaphore_elem, elem)->semaphore);
  }
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);

  while (!list_empty (&cond->waiters))
    cond_signal (cond, lock);
}
