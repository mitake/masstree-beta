/* Masstree
 * Eddie Kohler, Yandong Mao, Robert Morris
 * Copyright (c) 2012-2016 President and Fellows of Harvard College
 * Copyright (c) 2012-2016 Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Masstree LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Masstree LICENSE file; the license in that file
 * is legally binding.
 */
#include "kvthread.hh"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <new>
#include <sys/mman.h>
#if HAVE_SUPERPAGE && !NOSUPERPAGE
#include <sys/types.h>
#include <dirent.h>
#endif

threadinfo *threadinfo::allthreads;
#if ENABLE_ASSERTIONS
int threadinfo::no_pool_value;
#endif

inline threadinfo::threadinfo(int purpose, int index, bool enable_quiesce_stat, bool async_quiesce) {
    memset(this, 0, sizeof(*this));
    purpose_ = purpose;
    index_ = index;

    void *limbo_space = allocate(sizeof(limbo_group), memtag_limbo);
    mark(tc_limbo_slots, limbo_group::capacity);
    limbo_head_ = limbo_tail_ = new(limbo_space) limbo_group(async_quiesce_, async_quiesce_ ? globalepoch : 0);
    ts_ = 2;

    enable_quiesce_stat_ = enable_quiesce_stat;
    async_quiesce_ = async_quiesce;
    pthread_mutex_init(&quiesce_stat_mutex_, 0);

    pthread_mutex_init(&async_limbo_group_lock_, NULL);
    pthread_cond_init(&async_limbo_group_cond_, NULL);

    pthread_mutex_init(&pool_lock_, NULL);
}

static void *async_quiesce_thread(void *arg) {
  struct threadinfo *ti = static_cast<struct threadinfo *>(arg);

  while (true) {
    pthread_mutex_lock(&ti->async_limbo_group_lock_);

  retry:
    if (ti->async_limbo_queue_.empty()) {
      struct timespec wait;
      struct timeval now;

      gettimeofday(&now, NULL);
      wait.tv_sec = now.tv_sec + 1;		// TODO: configurable?
      wait.tv_nsec = 0;
      pthread_mutex_unlock(&ti->async_limbo_group_lock_);
      pthread_cond_timedwait(&ti->async_limbo_group_cond_, &ti->async_limbo_group_lock_, &wait);
      // pthread_cond_wait(&ti->async_limbo_group_cond_, &ti->async_limbo_group_lock_);

      goto retry;
    }

    std::vector<struct limbo_group *> q = ti->async_limbo_queue_;
    ti->async_limbo_queue_.clear();
    pthread_mutex_unlock(&ti->async_limbo_group_lock_);
    int nr_freed = 0;
    for (auto lg: q) {
      auto hd = lg->head_;
      auto tl = lg->tail_;
      while (hd != tl) {
	ti->free_rcu(lg->e_[hd].ptr_, lg->e_[hd].u_.tag);
	ti->mark(tc_gc);
	hd++;
	nr_freed++;
      }
      
      ti->deallocate(lg, sizeof(limbo_group), memtag_limbo);
    }
  }

  return NULL;
}

threadinfo *threadinfo::make(int purpose, int index, bool enable_quiesce_stat, bool async_quiesce) {
    static int threads_initialized;

    threadinfo* ti = new(malloc(8192)) threadinfo(purpose, index, enable_quiesce_stat, async_quiesce);
    ti->next_ = allthreads;
    allthreads = ti;

    if (!threads_initialized) {
#if ENABLE_ASSERTIONS
        const char* s = getenv("_");
        no_pool_value = s && strstr(s, "valgrind") != 0;
#endif
        threads_initialized = 1;
    }

    if (async_quiesce) {
      pthread_t async_quiesce_tid;
      int ret = pthread_create(&async_quiesce_tid, NULL, async_quiesce_thread, ti);
      if (ret != 0) {
	printf("failed to create a thread for async rcu quiesce: %m\n");
	exit(1);
      }
    }

    return ti;
}

void threadinfo::refill_rcu() {
  if (limbo_tail_) {
    if (!limbo_tail_->next_) {
      void *limbo_space = allocate(sizeof(limbo_group), memtag_limbo);
      mark(tc_limbo_slots, limbo_group::capacity);
      limbo_tail_->next_ = new(limbo_space) limbo_group(async_quiesce_, async_quiesce_ ? globalepoch : 0);
    }
    limbo_tail_ = limbo_tail_->next_;
  } else {
    assert(!limbo_head_);
    void *limbo_space = allocate(sizeof(limbo_group), memtag_limbo);
    mark(tc_limbo_slots, limbo_group::capacity);
    limbo_head_ = limbo_tail_ = new(limbo_space) limbo_group(async_quiesce_, async_quiesce_ ? globalepoch : 0);
  }
  assert(limbo_tail_->head_ == 0 && limbo_tail_->tail_ == 0);
}

inline bool limbo_group::clean_until(threadinfo& ti, mrcu_epoch_type max_epoch, unsigned int& nr_freed) {
  assert(!async_);

    while (head_ != tail_ && mrcu_signed_epoch_type(max_epoch - e_[head_].u_.epoch) > 0) {
        ++head_;
        while (head_ != tail_ && e_[head_].ptr_) {
            ti.free_rcu(e_[head_].ptr_, e_[head_].u_.tag);
            ti.mark(tc_gc);
            ++head_;
	    ++nr_freed;
        }
    }
    if (head_ == tail_) {
        head_ = tail_ = 0;
        return true;
    } else
        return false;
}

void threadinfo::hard_rcu_quiesce() {
    mrcu_epoch_type max_epoch = active_epoch;

    limbo_group* empty_head = nullptr;
    limbo_group* empty_tail = nullptr;

    double quiesce_start = now();
    unsigned int nr_freed = 0;

    if (!async_quiesce_) {
      // clean [limbo_head_, limbo_tail_]
      while (limbo_head_->clean_until(*this, max_epoch, nr_freed)) {
        if (!empty_head)
	  empty_head = limbo_head_;
        empty_tail = limbo_head_;
        if (limbo_head_ == limbo_tail_) {
	  limbo_head_ = limbo_tail_ = empty_head;
	  goto done;
        }
        limbo_head_ = limbo_head_->next_;
      }
      // hook empties after limbo_tail_
      if (empty_head) {
        empty_tail->next_ = limbo_tail_->next_;
        limbo_tail_->next_ = empty_head;
      }

    done:
      if (limbo_head_->head_ != limbo_head_->tail_)
        limbo_epoch_ = limbo_head_->e_[limbo_head_->head_].u_.epoch;
      else
        limbo_epoch_ = 0;
    } else {
      while (limbo_head_ && (mrcu_signed_epoch_type(max_epoch - limbo_head_->epoch_) > 0)) {
	struct limbo_group *next = limbo_head_->next_;

	pthread_mutex_lock(&async_limbo_group_lock_);// not lock free
	async_limbo_queue_.push_back(limbo_head_);
	pthread_mutex_unlock(&async_limbo_group_lock_);
        pthread_cond_signal(&async_limbo_group_cond_);

	limbo_head_ = next;
	if (!limbo_head_) {
	  limbo_tail_ = nullptr;
	  refill_rcu();
	}
      }

      if (limbo_head_ && (limbo_head_->head_ != limbo_head_->tail_))
        limbo_epoch_ = limbo_head_->e_[limbo_head_->head_].u_.epoch;
      else
        limbo_epoch_ = 0;
    }

    double quiesce_end = now();
    record_quiesce_stat(max_epoch, nr_freed, quiesce_start, quiesce_end);
}

void threadinfo::report_rcu(void *ptr) const
{
    for (limbo_group *lg = limbo_head_; lg; lg = lg->next_) {
        int status = 0;
        limbo_group::epoch_type e = 0;
        for (unsigned i = 0; i < lg->capacity; ++i) {
            if (i == lg->head_)
                status = 1;
            if (i == lg->tail_) {
                status = 0;
                e = 0;
            }
            if (lg->e_[i].ptr_ == ptr)
                fprintf(stderr, "thread %d: rcu %p@%d: %s as %x @%" PRIu64 "\n",
                        index_, lg, i, status ? "waiting" : "freed",
                        lg->e_[i].u_.tag, e);
            else if (!lg->e_[i].ptr_)
                e = lg->e_[i].u_.epoch;
        }
    }
}

void threadinfo::report_rcu_all(void *ptr)
{
    for (threadinfo *ti = allthreads; ti; ti = ti->next())
        ti->report_rcu(ptr);
}


#if HAVE_SUPERPAGE && !NOSUPERPAGE
static size_t read_superpage_size() {
    if (DIR* d = opendir("/sys/kernel/mm/hugepages")) {
        size_t n = (size_t) -1;
        while (struct dirent* de = readdir(d))
            if (de->d_type == DT_DIR
                && strncmp(de->d_name, "hugepages-", 10) == 0
                && de->d_name[10] >= '0' && de->d_name[10] <= '9') {
                size_t x = strtol(&de->d_name[10], 0, 10) << 10;
                n = (x < n ? x : n);
            }
        closedir(d);
        return n;
    } else
        return 2 << 20;
}

static size_t superpage_size = 0;
#endif

static void initialize_pool(void* pool, size_t sz, size_t unit) {
    char* p = reinterpret_cast<char*>(pool);
    void** nextptr = reinterpret_cast<void**>(p);
    for (size_t off = unit; off + unit <= sz; off += unit) {
        *nextptr = p + off;
        nextptr = reinterpret_cast<void**>(p + off);
    }
    *nextptr = 0;
}

void threadinfo::refill_pool(int nl) {
    assert(!pool_[nl - 1]);

    if (!use_pool()) {
        pool_[nl - 1] = malloc(nl * CACHE_LINE_SIZE);
        if (pool_[nl - 1])
            *reinterpret_cast<void**>(pool_[nl - 1]) = 0;
        return;
    }

    void* pool = 0;
    size_t pool_size = 0;
    int r;

#if HAVE_SUPERPAGE && !NOSUPERPAGE
    if (!superpage_size)
        superpage_size = read_superpage_size();
    if (superpage_size != (size_t) -1) {
        pool_size = superpage_size;
# if MADV_HUGEPAGE
        if ((r = posix_memalign(&pool, pool_size, pool_size)) != 0) {
            fprintf(stderr, "posix_memalign superpage: %s\n", strerror(r));
            pool = 0;
            superpage_size = (size_t) -1;
        } else if (madvise(pool, pool_size, MADV_HUGEPAGE) != 0) {
            perror("madvise superpage");
            superpage_size = (size_t) -1;
        }
# elif MAP_HUGETLB
        pool = mmap(0, pool_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (pool == MAP_FAILED) {
            perror("mmap superpage");
            pool = 0;
            superpage_size = (size_t) -1;
        }
# else
        superpage_size = (size_t) -1;
# endif
    }
#endif

    if (!pool) {
        pool_size = 2 << 20;
        if ((r = posix_memalign(&pool, CACHE_LINE_SIZE, pool_size)) != 0) {
            fprintf(stderr, "posix_memalign: %s\n", strerror(r));
            abort();
        }
    }

    initialize_pool(pool, pool_size, nl * CACHE_LINE_SIZE);
    pool_[nl - 1] = pool;
}
