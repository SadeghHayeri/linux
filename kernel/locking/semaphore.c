/*
 * Copyright (c) 2008 Intel Corporation
 * Author: Matthew Wilcox <willy@linux.intel.com>
 *
 * Distributed under the terms of the GNU GPL, version 2
 *
 * This file implements counting semaphores.
 * A counting semaphore may be acquired 'n' times before sleeping.
 * See mutex.c for single-acquisition sleeping locks which enforce
 * rules which allow code to be debugged more easily.
 */

/*
 * Some notes on the implementation:
 *
 * The spinlock controls access to the other members of the semaphore.
 * down_trylock() and up() can be called from interrupt context, so we
 * have to disable interrupts when taking the lock.  It turns out various
 * parts of the kernel expect to be able to use down() on a semaphore in
 * interrupt context when they know it will succeed, so we have to use
 * irqsave variants for down(), down_interruptible() and down_killable()
 * too.
 *
 * The ->count variable represents how many more tasks can acquire this
 * semaphore.  If it's zero, there may be tasks waiting on the wait_list.
 */

#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>
#include <linux/ftrace.h>

static noinline void __down(struct semaphore *sem);
static noinline int __down_interruptible(struct semaphore *sem);
static noinline int __down_killable(struct semaphore *sem);
static noinline int __down_timeout(struct semaphore *sem, long timeout);
static noinline void __up(struct semaphore *sem);

/**
 * down - acquire the semaphore
 * @sem: the semaphore to be acquired
 *
 * Acquires the semaphore.  If no more tasks are allowed to acquire the
 * semaphore, calling this function will put the task to sleep until the
 * semaphore is released.
 *
 * Use of this function is deprecated, please use down_interruptible() or
 * down_killable() instead.
 */
void down(struct semaphore *sem)
{
    unsigned long flags;

    raw_spin_lock_irqsave(&sem->lock, flags);
    if (likely(sem->count > 0))
        sem->count--;
    else
        __down(sem);
    raw_spin_unlock_irqrestore(&sem->lock, flags);
}
EXPORT_SYMBOL(down);

/**
 * down_interruptible - acquire the semaphore unless interrupted
 * @sem: the semaphore to be acquired
 *
 * Attempts to acquire the semaphore.  If no more tasks are allowed to
 * acquire the semaphore, calling this function will put the task to sleep.
 * If the sleep is interrupted by a signal, this function will return -EINTR.
 * If the semaphore is successfully acquired, this function returns 0.
 */
int down_interruptible(struct semaphore *sem)
{
    unsigned long flags;
    int result = 0;

    raw_spin_lock_irqsave(&sem->lock, flags);
    if (likely(sem->count > 0))
        sem->count--;
    else
        result = __down_interruptible(sem);
    raw_spin_unlock_irqrestore(&sem->lock, flags);

    return result;
}
EXPORT_SYMBOL(down_interruptible);

/**
 * down_killable - acquire the semaphore unless killed
 * @sem: the semaphore to be acquired
 *
 * Attempts to acquire the semaphore.  If no more tasks are allowed to
 * acquire the semaphore, calling this function will put the task to sleep.
 * If the sleep is interrupted by a fatal signal, this function will return
 * -EINTR.  If the semaphore is successfully acquired, this function returns
 * 0.
 */
int down_killable(struct semaphore *sem)
{
    unsigned long flags;
    int result = 0;

    raw_spin_lock_irqsave(&sem->lock, flags);
    if (likely(sem->count > 0))
        sem->count--;
    else
        result = __down_killable(sem);
    raw_spin_unlock_irqrestore(&sem->lock, flags);

    return result;
}
EXPORT_SYMBOL(down_killable);

/**
 * down_trylock - try to acquire the semaphore, without waiting
 * @sem: the semaphore to be acquired
 *
 * Try to acquire the semaphore atomically.  Returns 0 if the semaphore has
 * been acquired successfully or 1 if it it cannot be acquired.
 *
 * NOTE: This return value is inverted from both spin_trylock and
 * mutex_trylock!  Be careful about this when converting code.
 *
 * Unlike mutex_trylock, this function can be used from interrupt context,
 * and the semaphore can be released by any task or interrupt.
 */
int down_trylock(struct semaphore *sem)
{
    unsigned long flags;
    int count;

    raw_spin_lock_irqsave(&sem->lock, flags);
    count = sem->count - 1;
    if (likely(count >= 0))
        sem->count = count;
    raw_spin_unlock_irqrestore(&sem->lock, flags);

    return (count < 0);
}
EXPORT_SYMBOL(down_trylock);

/**
 * down_timeout - acquire the semaphore within a specified time
 * @sem: the semaphore to be acquired
 * @timeout: how long to wait before failing
 *
 * Attempts to acquire the semaphore.  If no more tasks are allowed to
 * acquire the semaphore, calling this function will put the task to sleep.
 * If the semaphore is not released within the specified number of jiffies,
 * this function returns -ETIME.  It returns 0 if the semaphore was acquired.
 */
int down_timeout(struct semaphore *sem, long timeout)
{
    unsigned long flags;
    int result = 0;

    raw_spin_lock_irqsave(&sem->lock, flags);
    if (likely(sem->count > 0))
        sem->count--;
    else
        result = __down_timeout(sem, timeout);
    raw_spin_unlock_irqrestore(&sem->lock, flags);

    return result;
}
EXPORT_SYMBOL(down_timeout);

/**
 * up - release the semaphore
 * @sem: the semaphore to release
 *
 * Release the semaphore.  Unlike mutexes, up() may be called from any
 * context and even by tasks which have never called down().
 */
void up(struct semaphore *sem)
{
    unsigned long flags;

    raw_spin_lock_irqsave(&sem->lock, flags);
    if (likely(list_empty(&sem->wait_list)))
        sem->count++;
    else
        __up(sem);
    raw_spin_unlock_irqrestore(&sem->lock, flags);
}
EXPORT_SYMBOL(up);

/* Functions for the contended case */

struct semaphore_waiter {
    struct list_head list;
    struct task_struct *task;
    bool up;
};

/*
 * Because this function is inlined, the 'state' parameter will be
 * constant, and thus optimised away by the compiler.  Likewise the
 * 'timeout' parameter for the cases without timeouts.
 */
static inline int __sched __down_common(struct semaphore *sem, long state,
                                        long timeout)
{
    struct task_struct *task = current;
    struct semaphore_waiter waiter;

    list_add_tail(&waiter.list, &sem->wait_list);
    waiter.task = task;
    waiter.up = false;

    for (;;) {
        if (signal_pending_state(state, task))
            goto interrupted;
        if (unlikely(timeout <= 0))
            goto timed_out;
        __set_task_state(task, state);
        raw_spin_unlock_irq(&sem->lock);
        timeout = schedule_timeout(timeout);
        raw_spin_lock_irq(&sem->lock);
        if (waiter.up)
            return 0;
    }

    timed_out:
    list_del(&waiter.list);
    return -ETIME;

    interrupted:
    list_del(&waiter.list);
    return -EINTR;
}

static noinline void __sched __down(struct semaphore *sem)
{
    __down_common(sem, TASK_UNINTERRUPTIBLE, MAX_SCHEDULE_TIMEOUT);
}

static noinline int __sched __down_interruptible(struct semaphore *sem)
{
    return __down_common(sem, TASK_INTERRUPTIBLE, MAX_SCHEDULE_TIMEOUT);
}

static noinline int __sched __down_killable(struct semaphore *sem)
{
    return __down_common(sem, TASK_KILLABLE, MAX_SCHEDULE_TIMEOUT);
}

static noinline int __sched __down_timeout(struct semaphore *sem, long timeout)
{
    return __down_common(sem, TASK_UNINTERRUPTIBLE, timeout);
}

static noinline void __sched __up(struct semaphore *sem)
{
    struct semaphore_waiter *waiter = list_first_entry(&sem->wait_list,
    struct semaphore_waiter, list);
    list_del(&waiter->list);
    waiter->up = true;
    wake_up_process(waiter->task);
}



//////////////////	MY_SEM	/////////////////
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/random.h>



///////////// linked_list functions ///////////
struct my_semaphore_list_items* __find_item(struct list_head* list, struct task_struct* task) {

    struct my_semaphore_list_items *pos;
    list_for_each_entry(pos, list, list) {
        if(pos->task == task)
            return pos;
    }

    return NULL;
}

static inline void __add_to_list(struct list_head* list, struct task_struct* task, bool up) {
    if(likely(__find_item(list, task) == NULL)) {

        struct my_semaphore_list_items* item;
        item = kmalloc(sizeof(*item), GFP_KERNEL);

        item->task = task;
        item->up = up;

        list_add_tail(&item->list, list);

    }
}

static inline void __remove_from_list(struct list_head* list, struct task_struct* task) {
    struct my_semaphore_list_items* item = __find_item(list, task);

    if(likely(item != NULL))
        list_del(&item->list);
}

struct my_semaphore_list_items* __find_max_prio_waiter(struct my_semaphore *sem) {

    int max_prio = 0;
    struct my_semaphore_list_items* max_prio_item = NULL;

    struct my_semaphore_list_items* pos;
    list_for_each_entry(pos, &sem->wait_list, list) {
        if(max_prio >= pos->task->prio) {
            max_prio = pos->task->prio;
            max_prio_item = pos;
        }
    }

    return max_prio_item;

}
////////////////////

///////////// my_sem linked-list functions ////////////
static noinline void __sched __add_current_to_wait_list(struct my_semaphore *sem) {
    struct task_struct *task = current;
    __add_to_list(&sem->wait_list, task, false);
}

//static noinline void __sched __remove_current_from_wait_list(struct my_semaphore *sem) {
//    struct task_struct *task = current;
//    __remove_from_list(&sem->wait_list, task);
//}

static noinline void __sched __add_current_to_run_list(struct my_semaphore *sem) {
    struct task_struct *task = current;
    __add_to_list(&sem->run_list, task, false);
}

static noinline void __sched __remove_current_from_run_list(struct my_semaphore *sem) {
    struct task_struct *task = current;
    __remove_from_list(&sem->run_list, task);
}
///////////////////////////////////////////////////////


////////////////// booster function ///////////////////
struct my_semaphore_list_items* __get_random_runner(struct my_semaphore *sem) {
    int runner_counts = 0;
    int index, random_runner_index;
    struct my_semaphore_list_items* pos;
    list_for_each_entry(pos, &sem->run_list, list)
        runner_counts++;

    if(runner_counts == 0)
        return NULL;

    index = 0;
    random_runner_index = get_random_int() % runner_counts;
    list_for_each_entry(pos, &sem->run_list, list) {
        if(index == random_runner_index)
            return pos;
        index++;
    }

    return NULL;
}

void __print_my_sem_info(struct my_semaphore *sem) {
    unsigned long flags;
    struct my_semaphore_list_items* pos;
    raw_spin_lock_irqsave(&sem->lock, flags);
    printk(KERN_INFO "######## MY_SEM_INFO ########\n");

    printk(KERN_INFO "# run_list:\n");

    list_for_each_entry(pos, &sem->run_list, list)
        printk(KERN_INFO "#\t\t* pid = %d\t-\tprio = %d\n", pos->task->pid, pos->task->prio);

    printk(KERN_INFO "# wait_list:\n");
    list_for_each_entry(pos, &sem->wait_list, list)
        printk(KERN_INFO "#\t\t* pid = %d\t-\tprio = %d\n", pos->task->pid, pos->task->prio);

    printk(KERN_INFO "#############################\n");
    raw_spin_unlock_irqrestore(&sem->lock, flags);
}

int __booster_thread_func(void *data) {
    struct my_semaphore* sem = (struct my_semaphore*)data;
    struct my_semaphore_list_items* random_runner;
    struct my_semaphore_list_items* max_prio_waiter;
    int sleep_time = 10 * 1000;
    int boost_time = 1 * 1000;
    int before_boost_prio, max_prio;
    unsigned long flags;

    while(true) {
        printk(KERN_INFO "# booster started!\n");
        msleep(sleep_time);

        __print_my_sem_info(sem);

        raw_spin_lock_irqsave(&sem->lock, flags);
        max_prio_waiter = __find_max_prio_waiter(sem);
        random_runner = __get_random_runner(sem);
        raw_spin_unlock_irqrestore(&sem->lock, flags);

        if(max_prio_waiter == NULL || random_runner == NULL) {
            printk(KERN_INFO "# do noting. (waiter: %p) (runner: %p)\n", max_prio_waiter, random_runner);
            continue;
        }

        raw_spin_lock_irqsave(&sem->lock, flags);
        printk(KERN_INFO "# selected max_waiter pid: %d prio: %d\n",
                max_prio_waiter->task->pid,
                max_prio_waiter->task->prio);

        printk(KERN_INFO "# selected random_runner pid: %d prio: %d\n",
                random_runner->task->pid,
                random_runner->task->prio);

        before_boost_prio = random_runner->task->prio;
        max_prio = max_prio_waiter->task->prio;

        random_runner->task->prio = max_prio;

        printk(KERN_INFO "# new random_runner prio: %d\n",
                random_runner->task->prio);
        raw_spin_unlock_irqrestore(&sem->lock, flags);

        msleep(boost_time);

        raw_spin_lock_irqsave(&sem->lock, flags);

        //TODO: check task exist before change it!
        random_runner->task->prio = before_boost_prio;

        printk(KERN_INFO "# boost down random_runner to prio: %d\n",
                random_runner->task->prio);
        raw_spin_unlock_irqrestore(&sem->lock, flags);
    }
}
///////////////////////////////////////////////////////

static noinline void __my_up(struct my_semaphore *sem)
{
    struct my_semaphore_list_items *waiter = __find_max_prio_waiter(sem);
    if(waiter != NULL) {

        // move waiter from wait_list -> run_list
        __remove_from_list(&sem->wait_list, waiter->task);
        __add_to_list(&sem->run_list, waiter->task, true);

        wake_up_process(waiter->task);

    }
}

static inline int __sched __my_down(struct my_semaphore *sem)
{
    struct task_struct *task = current;
    struct my_semaphore_list_items* item;

    for (;;) {
        if (signal_pending_state(TASK_INTERRUPTIBLE, task))
            goto interrupted;
        __set_task_state(task, TASK_INTERRUPTIBLE);
        raw_spin_unlock_irq(&sem->lock);
        raw_spin_lock_irq(&sem->lock);


        item = __find_item(&sem->wait_list, task);
        if(item == NULL || item->up)
            goto ready_to_run;
    }

    interrupted:
        __remove_current_from_run_list(sem);
        __add_current_to_run_list(sem);
        return -EINTR;

    ready_to_run:
        __remove_current_from_run_list(sem);
        __add_current_to_run_list(sem);
        return -EINTR;
}


//////////////////// syscalls ///////////////////
void my_sem_init(struct my_semaphore *sem, int val)
{
    static struct lock_class_key __key;
    printk(KERN_INFO "## init start ##\n");
    *sem = (struct my_semaphore) __MY_SEMAPHORE_INITIALIZER(*sem, val);
    lockdep_init_map(&sem->lock.dep_map, "semaphore->lock", &__key, 0);	//TODO: in chiye?

    sem->booster = kthread_run(__booster_thread_func, (void *)sem, "booster_thread");

    printk(KERN_INFO "## init end ##\n");
}
EXPORT_SYMBOL(my_sem_init);

void my_sem_up(struct my_semaphore *sem) {

    unsigned long flags;
    printk(KERN_INFO "## up start ##\n");

    raw_spin_lock_irqsave(&sem->lock, flags);

    __remove_current_from_run_list(sem);
    if (likely(list_empty(&sem->wait_list)))
        sem->count++;
    else
        __my_up(sem);

    raw_spin_unlock_irqrestore(&sem->lock, flags);
    printk(KERN_INFO "## up end ##\n");
}
EXPORT_SYMBOL(my_sem_up);

void my_sem_down(struct my_semaphore *sem) {

    unsigned long flags;
    printk(KERN_INFO "## down end ##\n");

    raw_spin_lock_irqsave(&sem->lock, flags);

    __add_current_to_wait_list(sem);
    if (likely(sem->count > 0))
        sem->count--;
    else
        __my_down(sem);

    raw_spin_unlock_irqrestore(&sem->lock, flags);
    printk(KERN_INFO "## down end ##\n");
}
EXPORT_SYMBOL(my_sem_down);

extern void my_sem_destroy(struct my_semaphore *sem) {

    unsigned long flags;
    printk(KERN_INFO "## destroy start ##\n");

    raw_spin_lock_irqsave(&sem->lock, flags);

    kthread_stop(sem->booster);

    raw_spin_unlock_irqrestore(&sem->lock, flags);
    printk(KERN_INFO "## destroy end ##\n");
}
EXPORT_SYMBOL(my_sem_destroy);