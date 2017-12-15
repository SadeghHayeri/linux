/*
 * Copyright (c) 2008 Intel Corporation
 * Author: Matthew Wilcox <willy@linux.intel.com>
 *
 * Distributed under the terms of the GNU GPL, version 2
 *
 * Please see kernel/semaphore.c for documentation of these functions
 */
#ifndef __LINUX_SEMAPHORE_H
#define __LINUX_SEMAPHORE_H

#include <linux/list.h>
#include <linux/spinlock.h>

/* Please don't access any members of this structure directly */
struct semaphore {
	raw_spinlock_t		lock;
	unsigned int		count;
	struct list_head	wait_list;
};

#define __SEMAPHORE_INITIALIZER(name, n)				\
{									\
	.lock		= __RAW_SPIN_LOCK_UNLOCKED((name).lock),	\
	.count		= n,						\
	.wait_list	= LIST_HEAD_INIT((name).wait_list),		\
}

#define DEFINE_SEMAPHORE(name)	\
	struct semaphore name = __SEMAPHORE_INITIALIZER(name, 1)

static inline void sema_init(struct semaphore *sem, int val)
{
	static struct lock_class_key __key;
	*sem = (struct semaphore) __SEMAPHORE_INITIALIZER(*sem, val);
	lockdep_init_map(&sem->lock.dep_map, "semaphore->lock", &__key, 0);
}

extern void down(struct semaphore *sem);
extern int __must_check down_interruptible(struct semaphore *sem);
extern int __must_check down_killable(struct semaphore *sem);
extern int __must_check down_trylock(struct semaphore *sem);
extern int __must_check down_timeout(struct semaphore *sem, long jiffies);
extern void up(struct semaphore *sem);




//////////////////	MY_SEM	//////////////////

struct my_semaphore {
	raw_spinlock_t		lock;
	unsigned int		count;

	struct list_head	wait_list;
	struct list_head	run_list;

	struct task_struct	*booster;
};

#define MY_SEM_MAX_SIZE 100
struct my_semaphore MY_SEMS[MY_SEM_MAX_SIZE];
bool MY_SEM_INUSE[MY_SEM_MAX_SIZE];

struct my_semaphore_list_items {
	struct list_head list;
	struct task_struct *task;
	bool up;
};

#define __MY_SEMAPHORE_INITIALIZER(name, n)				\
{									\
	.lock		= __RAW_SPIN_LOCK_UNLOCKED((name).lock),	\
	.count		= n,						\
	.wait_list	= LIST_HEAD_INIT((name).wait_list),		\
	.run_list	= LIST_HEAD_INIT((name).run_list),		\
}

extern int my_sem_init(int val);
extern void my_sem_down(int sem_index);
extern void my_sem_up(int sem_index);
extern void my_sem_destroy(int sem_index);

#endif /* __LINUX_SEMAPHORE_H */
