/****************************************************************************
 * sched/semaphore/sem_holder.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sched.h>
#include <assert.h>
#include <debug.h>
#include <nuttx/arch.h>

#include "sched/sched.h"
#include "semaphore/semaphore.h"

#ifdef CONFIG_PRIORITY_INHERITANCE

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Configuration ************************************************************/

#ifndef CONFIG_SEM_PREALLOCHOLDERS
#  define CONFIG_SEM_PREALLOCHOLDERS 0
#endif

/****************************************************************************
 * Private Type Declarations
 ****************************************************************************/

typedef int (*holderhandler_t)(FAR struct semholder_s *pholder,
                               FAR sem_t *sem, FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Preallocated holder structures */

#if CONFIG_SEM_PREALLOCHOLDERS > 0
static struct semholder_s g_holderalloc[CONFIG_SEM_PREALLOCHOLDERS];
static FAR struct semholder_s *g_freeholders;
#endif

/****************************************************************************
 * Name: nxsem_allocholder
 ****************************************************************************/

static inline FAR struct semholder_s *nxsem_allocholder(sem_t *sem)
{
  FAR struct semholder_s *pholder;

  /* Check if the "built-in" holder is being used.  We have this built-in
   * holder to optimize for the simplest case where semaphores are only
   * used to implement mutexes.
   */

#if CONFIG_SEM_PREALLOCHOLDERS > 0
  pholder = g_freeholders;
  if (pholder != NULL)
    {
      /* Remove the holder from the free list an put it into the semaphore's
       * holder list
       */

      g_freeholders    = pholder->flink;
      pholder->flink   = sem->hhead;
      sem->hhead       = pholder;

      /* Make sure the initial count is zero */

      pholder->counts  = 0;
    }
#else
  if (sem->holder[0].htcb == NULL)
    {
      pholder          = &sem->holder[0];
      pholder->counts  = 0;
    }
  else if (sem->holder[1].htcb == NULL)
    {
      pholder          = &sem->holder[1];
      pholder->counts  = 0;
    }
#endif
  else
    {
      serr("ERROR: Insufficient pre-allocated holders\n");
      pholder          = NULL;
    }

  DEBUGASSERT(pholder != NULL);
  return pholder;
}

/****************************************************************************
 * Name: nxsem_findholder
 *
 * NOTE: htcb may be used only as a look-up key.  It certain cases, the task
 * may have exited and htcb may refer to a stale memory.  It must not be
 * dereferenced.
 *
 ****************************************************************************/

static FAR struct semholder_s *nxsem_findholder(sem_t *sem,
                                                FAR struct tcb_s *htcb)
{
  FAR struct semholder_s *pholder;

#if CONFIG_SEM_PREALLOCHOLDERS > 0
  /* Try to find the holder in the list of holders associated with this
   * semaphore
   */

  for (pholder = sem->hhead; pholder != NULL; pholder = pholder->flink)
    {
      if (pholder->htcb == htcb)
        {
          /* Got it! */

          return pholder;
        }
    }
#else
  int i;
  pholder = NULL;

  /* We have two hard-allocated holder structures in sem_t */

  for (i = 0; i < 2; i++)
    {
      pholder = &sem->holder[i];
      if (pholder->htcb == htcb)
        {
          /* Got it! */

          return pholder;
        }
    }
#endif

  /* The holder does not appear in the list */

  return NULL;
}

/****************************************************************************
 * Name: nxsem_findorallocateholder
 ****************************************************************************/

static inline FAR struct semholder_s *
nxsem_findorallocateholder(sem_t *sem, FAR struct tcb_s *htcb)
{
  FAR struct semholder_s *pholder = nxsem_findholder(sem, htcb);
  if (!pholder)
    {
      pholder = nxsem_allocholder(sem);
    }

  return pholder;
}

/****************************************************************************
 * Name: nxsem_freeholder
 ****************************************************************************/

static inline void nxsem_freeholder(sem_t *sem,
                                    FAR struct semholder_s *pholder)
{
#if CONFIG_SEM_PREALLOCHOLDERS > 0
  FAR struct semholder_s *curr;
  FAR struct semholder_s *prev;
#endif

  /* Release the holder and counts */

  pholder->htcb   = NULL;
  pholder->counts = 0;

#if CONFIG_SEM_PREALLOCHOLDERS > 0
  /* Search the list for the matching holder */

  for (prev = NULL, curr = sem->hhead;
       curr && curr != pholder;
       prev = curr, curr = curr->flink);

  if (curr != NULL)
    {
      /* Remove the holder from the list */

      if (prev != NULL)
        {
          prev->flink = pholder->flink;
        }
      else
        {
          sem->hhead = pholder->flink;
        }

      /* And put it in the free list */

      pholder->flink = g_freeholders;
      g_freeholders  = pholder;
    }
#endif
}

/****************************************************************************
 * Name: nxsem_findandfreeholder
 ****************************************************************************/

static inline void nxsem_findandfreeholder(sem_t *sem,
                                           FAR struct tcb_s *htcb)
{
  FAR struct semholder_s *pholder = nxsem_findholder(sem, htcb);

  /* When no more counts are held, remove the holder from the list.  The
   * count was decremented in nxsem_release_holder.
   */

  if (pholder != NULL && pholder->counts <= 0)
    {
      nxsem_freeholder(sem, pholder);
    }
}

/****************************************************************************
 * Name: nxsem_foreachholder
 ****************************************************************************/

static int nxsem_foreachholder(FAR sem_t *sem, holderhandler_t handler,
                               FAR void *arg)
{
  FAR struct semholder_s *pholder;
  int ret = 0;

#if CONFIG_SEM_PREALLOCHOLDERS > 0
  FAR struct semholder_s *next;

  for (pholder = sem->hhead; pholder && ret == 0; pholder = next)
    {
      /* In case this holder gets deleted */

      next = pholder->flink;

      /* Check if there is a handler... there should always be one
       * in this configuration.
       */

      if (pholder->htcb != NULL)
        {
          /* Call the handler */

          ret = handler(pholder, sem, arg);
        }
    }
#else
  int i;

  /* We have two hard-allocated holder structures in sem_t */

  for (i = 0; i < 2; i++)
    {
      pholder = &sem->holder[i];

      /* The hard-allocated containers may hold a NULL holder */

      if (pholder->htcb != NULL)
        {
          /* Call the handler */

          ret = handler(pholder, sem, arg);
        }
    }
#endif

  return ret;
}

/****************************************************************************
 * Name: nxsem_recoverholders
 ****************************************************************************/

#if CONFIG_SEM_PREALLOCHOLDERS > 0
static int nxsem_recoverholders(FAR struct semholder_s *pholder,
                                FAR sem_t *sem, FAR void *arg)
{
  nxsem_freeholder(sem, pholder);
  return 0;
}
#endif

/****************************************************************************
 * Name: nxsem_boostholderprio
 ****************************************************************************/

static int nxsem_boostholderprio(FAR struct semholder_s *pholder,
                                 FAR sem_t *sem, FAR void *arg)
{
  FAR struct tcb_s *htcb = (FAR struct tcb_s *)pholder->htcb;
  FAR struct tcb_s *rtcb = (FAR struct tcb_s *)arg;

  /* Make sure that the holder thread is still active.  If it exited without
   * releasing its counts, then that would be a bad thing.  But we can take
   * no real action because we don't know what the program is doing.
   * Perhaps its plan is to kill a thread, then destroy the semaphore.
   */

  if (!nxsched_verify_tcb(htcb))
    {
      swarn("WARNING: TCB %p is a stale handle, counts lost\n", htcb);
      nxsem_freeholder(sem, pholder);
    }

#if CONFIG_SEM_NNESTPRIO > 0
  /* If the priority of the thread that is waiting for a count is greater
   * than the base priority of the thread holding a count, then we add this
   * priority for the semaphore to the list of boosting semaphores
   */

  else if (rtcb->sched_priority > htcb->base_priority)
    {
      if (htcb->nsem_boosts < CONFIG_SEM_NNESTPRIO)
      {
        /* Store this boost in the list of active boosts */
        struct semboost_s *boost = &htcb->sem_boosts[htcb->nsem_boosts];
        htcb->nsem_boosts++;
        boost->sem = sem;
        boost->priority = rtcb->sched_priority;
        /* If the boost we just received is a new maximum. We need to boost
         * ourselves */
        if (boost->priority > htcb->sched_priority) {
          nxsched_set_priority(htcb, boost->priority);
        }
      }
      else
      {
        serr("ERROR: TCB %p out of priority boost slots.", htcb);
      }
    }

#else
  /* If the priority of the thread that is waiting for a count is less than
   * or equal to the priority of the thread holding a count, then do nothing
   * because the thread is already running at a sufficient priority.
   */

  else if (rtcb->sched_priority > htcb->sched_priority)
    {
      /* Raise the priority of the holder of the semaphore.  This
       * cannot cause a context switch because we have preemption
       * disabled.  The task will be marked "pending" and the switch
       * will occur during up_block_task() processing.
       */

      nxsched_set_priority(htcb, rtcb->sched_priority);
    }
#endif

  return 0;
}

/****************************************************************************
 * Name: nxsem_verifyholder
 ****************************************************************************/

#ifdef CONFIG_DEBUG_ASSERTIONS
static int nxsem_verifyholder(FAR struct semholder_s *pholder,
                              FAR sem_t *sem, FAR void *arg)
{
  /* Need to revisit this, but these assumptions seem to be untrue -- OR
   * there is a bug???
   */

#if 0
  FAR struct tcb_s *htcb = (FAR struct tcb_s *)pholder->htcb;

  /* Called after a semaphore has been released (incremented), the semaphore
   * could be non-negative, and there is no thread waiting for the count.
   * In this case, the priority of the holder should not be boosted.
   */

#if CONFIG_SEM_NNESTPRIO > 0
  DEBUGASSERT(htcb->npend_reprio == 0);
#endif
  DEBUGASSERT(htcb->sched_priority == htcb->base_priority);
#endif
  return 0;
}
#endif

/****************************************************************************
 * Name: nxsem_dumpholder
 ****************************************************************************/

#if defined(CONFIG_DEBUG_INFO) && defined(CONFIG_SEM_PHDEBUG)
static int nxsem_dumpholder(FAR struct semholder_s *pholder, FAR sem_t *sem,
                            FAR void *arg)
{
#if CONFIG_SEM_PREALLOCHOLDERS > 0
  _info("  %08x: %08x %08x %04x\n",
        pholder, pholder->flink, pholder->htcb, pholder->counts);
#else
  _info("  %08x: %08x %04x\n", pholder, pholder->htcb, pholder->counts);
#endif
  return 0;
}
#endif

/****************************************************************************
 * Name: nxsem_restoreholderprio
 ****************************************************************************/

static int nxsem_restoreholderprio(FAR struct tcb_s *htcb,
                                   FAR sem_t *sem, FAR void *arg)
{
  FAR struct semholder_s *pholder = nxsem_findholder(sem, htcb);

  /* Make sure that the holder thread is still active.  If it exited without
   * releasing its counts, then that would be a bad thing.  But we can take
   * no real action because we don't know what the program is doing.
   * Perhaps its plan is to kill a thread, then destroy the semaphore.
   */

  if (!nxsched_verify_tcb(htcb))
    {
      swarn("WARNING: TCB %p is a stale handle, counts lost\n", htcb);

      if (pholder != NULL)
        {
          nxsem_freeholder(sem, pholder);
        }
    }

  /* Was the priority of the holder thread boosted? If so, then drop its
   * priority back to the correct level.  What is the correct level?
   */

  else if (htcb->sched_priority != htcb->base_priority)
    {
#if CONFIG_SEM_NNESTPRIO > 0

      if (pholder == NULL || pholder->counts == 0) {
        /* We do no longer hold a count on that variable. In that case,
         * we are no longer obliged to loiter around at a higher priority,
         * since that would not release the resource any faster anymore. */
        /* Discard any boosts for this semaphore */
        for (int i = 0; i < htcb->nsem_boosts; i++) {
          if (htcb->sem_boosts[i].sem == sem) {
            htcb->sem_boosts[i] = htcb->sem_boosts[htcb->nsem_boosts-1];
            htcb->nsem_boosts--;
            i--; /* Re check the current index */
          }
        }
      } else {
          /* We still have some of that semaphore. The highest priority waiter
          * for the specific semaphore has been satisfied. We can remove the
          * highest boost for that semaphore from our list of boosts, and
          * re-evaluate what is the now the highest priority still waiting */
          int max_boost_index = -1;
          uint8_t max_boost_priority = 0;
          for (int i = 0; i<htcb->nsem_boosts; i++) {
            if (htcb->sem_boosts[i].sem == sem) {
              if (max_boost_priority < htcb->sem_boosts[i].priority) {
                max_boost_priority = htcb->sem_boosts[i].priority;
                max_boost_index = i;
              }
            }
          }


          if (max_boost_index >= 0) {
            /* We found the maximum boost for this semaphore on this task.
            * Remove this, as this is no longer required (the semaphore was posted,
            * e.g. the highest priority waiting thread satisfied) */
            /* remove (replace max with last, decrease count)*/
            htcb->sem_boosts[max_boost_index] = htcb->sem_boosts[htcb->nsem_boosts-1];
            htcb->nsem_boosts--;
          }
      }

      /* Find new max priority by going through the boosts still present */
      uint8_t new_priority = htcb->base_priority;
      for (int i=0; i<htcb->nsem_boosts; i++) {
        new_priority = (htcb->sem_boosts[i].priority > new_priority) ?
            htcb->sem_boosts[i].priority : new_priority;
      }

      if (new_priority != htcb->sched_priority) {
        nxsched_set_priority(htcb, new_priority);
      }

#else
      /* There is no alternative restore priorities, drop the priority
       * of the holder thread all the way back to the threads "base"
       * priority.
       */

      nxsched_reprioritize(htcb, htcb->base_priority);
#endif
    }

  return 0;
}

/****************************************************************************
 * Name: nxsem_restoreholderprioall
 *
 * Description:
 *   Reprioritize all holders
 *
 ****************************************************************************/

static int nxsem_restoreholderprioall(FAR struct semholder_s *pholder,
                                      FAR sem_t *sem, FAR void *arg)
{
  return nxsem_restoreholderprio(pholder->htcb, sem, arg);
}

/****************************************************************************
 * Name: nxsem_restoreholderprio_others
 *
 * Description:
 *   Reprioritize all holders except the currently executing task
 *
 ****************************************************************************/

static int nxsem_restoreholderprio_others(FAR struct semholder_s *pholder,
                                          FAR sem_t *sem, FAR void *arg)
{
  FAR struct tcb_s *rtcb = this_task();
  if (pholder->htcb != rtcb)
    {
      return nxsem_restoreholderprio(pholder->htcb, sem, arg);
    }

  return 0;
}

/****************************************************************************
 * Name: nxsem_restoreholderprio_self
 *
 * Description:
 *   Reprioritize only the currently executing task
 *
 ****************************************************************************/

static int nxsem_restoreholderprio_self(FAR struct semholder_s *pholder,
                                        FAR sem_t *sem, FAR void *arg)
{
  FAR struct tcb_s *rtcb = this_task();

  if (pholder->htcb == rtcb)
    {
      /* The running task has given up a count on the semaphore */

#if CONFIG_SEM_PREALLOCHOLDERS == 0
      /* In the case where there are only 2 holders. This step
       * is necessary to ensure we have space. Release the holder
       * if all counts have been given up before reprioritizing
       * causes a context switch.
       */

      nxsem_findandfreeholder(sem, rtcb);
#endif
      nxsem_restoreholderprio(rtcb, sem, arg);
      return 1;
    }

  return 0;
}

/****************************************************************************
 * Name: nxsem_restore_baseprio_irq
 *
 * Description:
 *   This function is called after an interrupt handler posts a count on
 *   the semaphore.  It will check if we need to drop the priority of any
 *   threads holding a count on the semaphore.  Their priority could have
 *   been boosted while they held the count.
 *
 * Input Parameters:
 *   stcb - The TCB of the task that was just started (if any).  If the
 *     post action caused a count to be given to another thread, then stcb
 *     is the TCB that received the count.  Note, just because stcb received
 *     the count, it does not mean that it is higher priority than other
 *     threads.
 *   sem - A reference to the semaphore being posted.
 *     - If the semaphore count is <0 then there are still threads waiting
 *       for a count.  stcb should be non-null and will be higher priority
 *       than all of the other threads still waiting.
 *     - If it is ==0 then stcb refers to the thread that got the last count;
 *       no other threads are waiting.
 *     - If it is >0 then there should be no threads waiting for counts and
 *       stcb should be null.
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   The scheduler is locked.
 *
 ****************************************************************************/

static inline void nxsem_restore_baseprio_irq(FAR struct tcb_s *stcb,
                                              FAR sem_t *sem)
{
  /* Perform the following actions only if a new thread was given a count.
   * The thread that received the count should be the highest priority
   * of all threads waiting for a count from the semaphore.  So in that
   * case, the priority of all holder threads should be dropped to the
   * next highest pending priority.
   */

  if (stcb != NULL)
    {
      /* Drop the priority of all holder threads */

      nxsem_foreachholder(sem, nxsem_restoreholderprioall, stcb);
    }

  /* If there are no tasks waiting for available counts, then all holders
   * should be at their base priority.
   */

#ifdef CONFIG_DEBUG_ASSERTIONS
  else
    {
      nxsem_foreachholder(sem, nxsem_verifyholder, NULL);
    }
#endif
}

/****************************************************************************
 * Name: nxsem_restore_baseprio_task
 *
 * Description:
 *   This function is called after the current running task releases a
 *   count on the semaphore.  It will check if we need to drop the priority
 *   of any threads holding a count on the semaphore.  Their priority could
 *   have been boosted while they held the count.
 *
 * Input Parameters:
 *   stcb - The TCB of the task that was just started (if any).  If the
 *     post action caused a count to be given to another thread, then stcb
 *     is the TCB that received the count.  Note, just because stcb received
 *     the count, it does not mean that it is higher priority than other
 *     threads.
 *   sem - A reference to the semaphore being posted.
 *     - If the semaphore count is <0 then there are still threads waiting
 *       for a count.  stcb should be non-null and will be higher priority
 *       than all of the other threads still waiting.
 *     - If it is ==0 then stcb refers to the thread that got the last count;
 *       no other threads are waiting.
 *     - If it is >0 then there should be no threads waiting for counts and
 *       stcb should be null.
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   The scheduler is locked.
 *
 ****************************************************************************/

static inline void nxsem_restore_baseprio_task(FAR struct tcb_s *stcb,
                                               FAR sem_t *sem)
{
  FAR struct tcb_s *rtcb = this_task();

  /* Perform the following actions only if a new thread was given a count.
   * The thread that received the count should be the highest priority
   * of all threads waiting for a count from the semaphore.  So in that
   * case, the priority of all holder threads should be dropped to the
   * next highest pending priority.
   */

  if (stcb != NULL)
    {
      /* The currently executed thread should be the lower priority
       * thread that just posted the count and caused this action.
       * However, we cannot drop the priority of the currently running
       * thread -- because that will cause it to be suspended.
       *
       * So, do this in two passes.  First, reprioritizing all holders
       * except for the running thread.
       */

      nxsem_foreachholder(sem, nxsem_restoreholderprio_others, stcb);

      /* Now, find an reprioritize only the ready to run task */

      nxsem_foreachholder(sem, nxsem_restoreholderprio_self, stcb);
    }

  /* If there are no tasks waiting for available counts, then all holders
   * should be at their base priority.
   */

#ifdef CONFIG_DEBUG_ASSERTIONS
  else
    {
      nxsem_foreachholder(sem, nxsem_verifyholder, NULL);
    }
#endif

  /* In any case, the currently executing task should have an entry in the
   * list.  Its counts were previously decremented; if it now holds no
   * counts, then we need to remove it from the list of holders.
   */

  nxsem_findandfreeholder(sem, rtcb);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxsem_initialize_holders
 *
 * Description:
 *   Called from nxsem_initialize() to set up semaphore holder information.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

void nxsem_initialize_holders(void)
{
#if CONFIG_SEM_PREALLOCHOLDERS > 0
  int i;

  /* Put all of the pre-allocated holder structures into the free list */

  g_freeholders = g_holderalloc;
  for (i = 0; i < (CONFIG_SEM_PREALLOCHOLDERS - 1); i++)
    {
      g_holderalloc[i].flink = &g_holderalloc[i + 1];
    }

  g_holderalloc[CONFIG_SEM_PREALLOCHOLDERS - 1].flink = NULL;
#endif
}

/****************************************************************************
 * Name: nxsem_destroyholder
 *
 * Description:
 *   Called from nxsem_destroy() to handle any holders of a semaphore
 *   when it is destroyed.
 *
 * Input Parameters:
 *   sem - A reference to the semaphore being destroyed
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

void nxsem_destroyholder(FAR sem_t *sem)
{
  /* It might be an error if a semaphore is destroyed while there are any
   * holders of the semaphore (except perhaps the thread that release the
   * semaphore itself).  We actually have to assume that the caller knows
   * what it is doing because could have killed another thread that is the
   * actual holder of the semaphore.
   *
   * It is also a standard practice to destroy the semaphore while the
   * caller holds it.  Of course, the caller MUST assure that there are no
   * other holders of the semaphore in this case.  This occurs, for example,
   * when a driver is unlink'ed and the driver instance must be destroyed.
   *
   * Therefore, we cannot make any assumptions about the state of the
   * semaphore or the state of any of the holder threads.  So just recover
   * any stranded holders and hope the task knows what it is doing.
   */

#if CONFIG_SEM_PREALLOCHOLDERS > 0
  if (sem->hhead != NULL)
    {
      /* There may be an issue if there are multiple holders of
       * the semaphore.
       */

      DEBUGASSERT(sem->hhead->flink == NULL);
      nxsem_foreachholder(sem, nxsem_recoverholders, NULL);
    }

#else
  /* There may be an issue if there are multiple holders of the semaphore. */

  DEBUGASSERT(sem->holder[0].htcb == NULL || sem->holder[1].htcb == NULL);

  sem->holder[0].htcb = NULL;
  sem->holder[1].htcb = NULL;
#endif
}

/****************************************************************************
 * Name: nxsem_add_holder_tcb
 *
 * Description:
 *   Called from nxsem_wait() when the calling thread obtains the semaphore;
 *   Called from sem_post() when the waiting thread obtains the semaphore.
 *
 * Input Parameters:
 *   htcb - TCB of the thread that just obtained the semaphore
 *   sem  - A reference to the incremented semaphore
 *
 * Returned Value:
 *   0 (OK) or -1 (ERROR) if unsuccessful
 *
 * Assumptions:
 *   Interrupts are disabled.
 *
 ****************************************************************************/

void nxsem_add_holder_tcb(FAR struct tcb_s *htcb, FAR sem_t *sem)
{
  FAR struct semholder_s *pholder;

  /* If priority inheritance is disabled for this thread, then do not add
   * the holder.  If there are never holders of the semaphore, the priority
   * inheritance is effectively disabled.
   */

  if ((sem->flags & PRIOINHERIT_FLAGS_DISABLE) == 0)
    {
      /* Find or allocate a container for this new holder */

      pholder = nxsem_findorallocateholder(sem, htcb);
      if (pholder != NULL)
        {
          /* Then set the holder and increment the number of counts held by
           * this holder
           */

          pholder->htcb = htcb;
          pholder->counts++;
        }
    }
}

/****************************************************************************
 * Name: nxsem_add_holder
 *
 * Description:
 *   Called from nxsem_wait() when the calling thread obtains the semaphore
 *
 * Input Parameters:
 *   sem - A reference to the incremented semaphore
 *
 * Returned Value:
 *   0 (OK) or -1 (ERROR) if unsuccessful
 *
 * Assumptions:
 *   Interrupts are disabled.
 *
 ****************************************************************************/

void nxsem_add_holder(FAR sem_t *sem)
{
  nxsem_add_holder_tcb(this_task(), sem);
}

/****************************************************************************
 * Name: void nxsem_boost_priority(sem_t *sem)
 *
 * Description:
 *
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   0 (OK) or -1 (ERROR) if unsuccessful
 *
 * Assumptions:
 *
 ****************************************************************************/

void nxsem_boost_priority(FAR sem_t *sem)
{
  FAR struct tcb_s *rtcb = this_task();

  /* Boost the priority of every thread holding counts on this semaphore
   * that are lower in priority than the new thread that is waiting for a
   * count.
   */


  nxsem_foreachholder(sem, nxsem_boostholderprio, rtcb);
}

/****************************************************************************
 * Name: nxsem_release_holder
 *
 * Description:
 *   Called from sem_post() after a thread releases one count on the
 *   semaphore.
 *
 * Input Parameters:
 *   sem - A reference to the semaphore being posted
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

void nxsem_release_holder(FAR sem_t *sem)
{
  FAR struct tcb_s *rtcb = this_task();
  FAR struct semholder_s *pholder;

  /* Find the container for this holder */

  pholder = nxsem_findholder(sem, rtcb);
  if (pholder != NULL && pholder->counts > 0)
    {
      /* Decrement the counts on this holder -- the holder will be freed
       * later in nxsem_restore_baseprio.
       */

      pholder->counts--;
    }
}

/****************************************************************************
 * Name: nxsem_restore_baseprio
 *
 * Description:
 *   This function is called after the current running task releases a
 *   count on the semaphore or an interrupt handler posts a new count.  It
 *   will check if we need to drop the priority of any threads holding a
 *   count on the semaphore.  Their priority could have been boosted while
 *   they held the count.
 *
 * Input Parameters:
 *   stcb - The TCB of the task that was just started (if any).  If the
 *     post action caused a count to be given to another thread, then stcb
 *     is the TCB that received the count.  Note, just because stcb received
 *     the count, it does not mean that it is higher priority than other
 *     threads.
 *   sem - A reference to the semaphore being posted.
 *     - If the semaphore count is <0 then there are still threads waiting
 *       for a count.  stcb should be non-null and will be higher priority
 *       than all of the other threads still waiting.
 *     - If it is ==0 then stcb refers to the thread that got the last count;
 *       no other threads are waiting.
 *     - If it is >0 then there should be no threads waiting for counts and
 *       stcb should be null.
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   The scheduler is locked.
 *
 ****************************************************************************/

void nxsem_restore_baseprio(FAR struct tcb_s *stcb, FAR sem_t *sem)
{
#if 0  /* DSA: sometimes crashes when Telnet calls external cmd (i.e. 'i2c') */
  /* Check our assumptions */

  DEBUGASSERT((sem->semcount > 0  && stcb == NULL) ||
              (sem->semcount <= 0 && stcb != NULL));
#endif

  /* Handler semaphore counts posed from an interrupt handler differently
   * from interrupts posted from threads.  The primary difference is that
   * if the semaphore is posted from a thread, then the poster thread is
   * a player in the priority inheritance scheme.  The interrupt handler
   * externally injects the new count without otherwise participating
   * itself.
   */

  if (up_interrupt_context())
    {
      nxsem_restore_baseprio_irq(stcb, sem);
    }
  else
    {
      nxsem_restore_baseprio_task(stcb, sem);
    }
}

/****************************************************************************
 * Name: nxsem_canceled
 *
 * Description:
 *   Called from nxsem_wait_irq() after a thread that was waiting for a
 *   semaphore count was awakened because of a signal and the semaphore wait
 *   has been canceled.  This function restores the correct thread priority
 *   of each holder of the semaphore.
 *
 * Input Parameters:
 *   sem - A reference to the semaphore no longer being waited for
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

void nxsem_canceled(FAR struct tcb_s *stcb, FAR sem_t *sem)
{
  /* Check our assumptions */

  DEBUGASSERT(sem->semcount <= 0);

  /* Adjust the priority of every holder as necessary */

  nxsem_foreachholder(sem, nxsem_restoreholderprioall, stcb);
}

/****************************************************************************
 * Name: sem_enumholders
 *
 * Description:
 *   Show information about threads currently waiting on this semaphore
 *
 * Input Parameters:
 *   sem - A reference to the semaphore
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

#if defined(CONFIG_DEBUG_FEATURES) && defined(CONFIG_SEM_PHDEBUG)
void sem_enumholders(FAR sem_t *sem)
{
#ifdef CONFIG_DEBUG_INFO
  nxsem_foreachholder(sem, nxsem_dumpholder, NULL);
#endif
}
#endif

/****************************************************************************
 * Name: nxsem_nfreeholders
 *
 * Description:
 *   Return the number of available holder containers.  This is a good way
 *   to find out which threads are not calling sem_destroy.
 *
 * Input Parameters:
 *   sem - A reference to the semaphore
 *
 * Returned Value:
 *   The number of available holder containers
 *
 * Assumptions:
 *
 ****************************************************************************/

#if defined(CONFIG_DEBUG_FEATURES) && defined(CONFIG_SEM_PHDEBUG)
int nxsem_nfreeholders(void)
{
#if CONFIG_SEM_PREALLOCHOLDERS > 0
  FAR struct semholder_s *pholder;
  int n;

  for (pholder = g_freeholders, n = 0; pholder; pholder = pholder->flink)
    {
      n++;
    }

  return n;
#else
  return 0;
#endif
}
#endif

#endif /* CONFIG_PRIORITY_INHERITANCE */
