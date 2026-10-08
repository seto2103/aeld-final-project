/**
 * @file thread_util.h
 * @brief Starts worker threads with SIGINT and SIGTERM blocked, so only the main thread
 *        handles them.
 */

#ifndef THREAD_UTIL_H
#define THREAD_UTIL_H

#include <pthread.h>
#include <signal.h>

/**
 * pthread_create() with SIGINT and SIGTERM blocked in the new thread. The calling thread's
 * signal mask is restored before returning.
 * @return 0 on success, or the error number from pthread_create()
 */
static inline int thread_create_signals_blocked(pthread_t *thread, void *(*fn)(void *),
                                                void *arg)
{
    sigset_t block;
    sigset_t old;
    int rc;

    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    rc = pthread_create(thread, NULL, fn, arg);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return rc;
}

#endif /* THREAD_UTIL_H */
