#define _XOPEN_SOURCE 700
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"

static int seen_signal_cnt = 0;

static void signal_handler(int signal) {
    __atomic_add_fetch(&seen_signal_cnt, 1, __ATOMIC_RELAXED);
    printf("signal handled: %d\n", signal);
}

static void ignore_signal(int sig) {
    sigset_t newmask;
    sigemptyset(&newmask);
    if (sig) {
        sigaddset(&newmask, sig);
    }

    CHECK(sigprocmask(SIG_SETMASK, &newmask, NULL));
}

static void set_signal_handler(int sig, void* handler) {
    struct sigaction act = {
        .sa_handler = handler,
    };
    CHECK(sigaction(sig, &act, NULL));
}

static void test_sigprocmask(void) {
    sigset_t newmask;
    sigset_t oldmask;
    sigemptyset(&newmask);
    sigemptyset(&oldmask);
    sigaddset(&newmask, SIGKILL);
    sigaddset(&newmask, SIGSTOP);

    CHECK(sigprocmask(SIG_SETMASK, &newmask, NULL));

    CHECK(sigprocmask(SIG_SETMASK, NULL, &oldmask));

    if (sigismember(&oldmask, SIGKILL) || sigismember(&oldmask, SIGSTOP)) {
        printf("SIGKILL or SIGSTOP should be ignored, but is not.\n");
        exit(1);
    }
}

static void clean_mask_and_pending_signals(void) {
    /* We should not have any pending signals other than SIGALRM. */
    set_signal_handler(SIGALRM, signal_handler);
    /* This assumes that unblocking a signal will cause its immediate delivery. */
    ignore_signal(0);
    __atomic_store_n(&seen_signal_cnt, 0, __ATOMIC_RELAXED);
}

static void test_multiple_pending(void) {
    ignore_signal(SIGALRM);

    set_signal_handler(SIGALRM, signal_handler);

    CHECK(kill(getpid(), SIGALRM));
    CHECK(kill(getpid(), SIGALRM));

    if (__atomic_load_n(&seen_signal_cnt, __ATOMIC_RELAXED) != 0) {
        printf("Handled a blocked standard signal!\n");
        exit(1);
    }

    ignore_signal(0);

    if (__atomic_load_n(&seen_signal_cnt, __ATOMIC_RELAXED) != 1) {
        printf("Multiple or none instances of standard signal were queued!\n");
        exit(1);
    }

    __atomic_store_n(&seen_signal_cnt, 0, __ATOMIC_RELAXED);

    int sig = SIGRTMIN;
    ignore_signal(sig);

    CHECK(kill(getpid(), sig));
    CHECK(kill(getpid(), sig));

    set_signal_handler(sig, signal_handler);

    if (__atomic_load_n(&seen_signal_cnt, __ATOMIC_RELAXED) != 0) {
        printf("Handled a blocked real-time signal!\n");
        exit(1);
    }

    ignore_signal(0);

    if (__atomic_load_n(&seen_signal_cnt, __ATOMIC_RELAXED) != 2) {
        printf("Multiple instances of real-time signal were NOT queued!\n");
        exit(1);
    }
}

static void test_fork(void) {
    ignore_signal(SIGALRM);

    set_signal_handler(SIGALRM, signal_handler);

    CHECK(kill(getpid(), SIGALRM));

    pid_t p = CHECK(fork());
    if (p == 0) {
        ignore_signal(0);

        if (__atomic_load_n(&seen_signal_cnt, __ATOMIC_RELAXED) != 0) {
            printf("Pending signal was inherited after fork!\n");
            exit(1);
        }

        puts("Child OK");
        exit(0);
    }

    set_signal_handler(SIGALRM, SIG_DFL);

    CHECK(waitpid(p, NULL, 0) != p);
}

static void test_execve_start(char* self) {
    ignore_signal(SIGALRM);

    set_signal_handler(SIGALRM, SIG_DFL);

    CHECK(kill(getpid(), SIGALRM));

    char* argv[] = {self, (char*)"cont", NULL};

    /* Gramine behaves incorrectly if envp doesn't contain LD_LIBRARY_PATH (cannot find
     * Gramine-specific Glibc libraries), so add this single envvar for execve() */
    char* ld_library_path_value = getenv("LD_LIBRARY_PATH");
    if (!ld_library_path_value) {
        printf("No LD_LIBRARY_PATH envvar found\n");
        exit(1);
    }
    char ld_library_path_envvar[512];
    strcpy(ld_library_path_envvar, "LD_LIBRARY_PATH=");
    strcat(ld_library_path_envvar, ld_library_path_value);
    char* envp[] = {ld_library_path_envvar, NULL};

    CHECK(execve(self, argv, envp));
}

static void test_execve_continue(void) {
    set_signal_handler(SIGALRM, signal_handler);

    if (__atomic_load_n(&seen_signal_cnt, __ATOMIC_RELAXED) != 0) {
        printf("Seen an unexpected signal!\n");
        exit(1);
    }

    ignore_signal(0);

    if (__atomic_load_n(&seen_signal_cnt, __ATOMIC_RELAXED) != 1) {
        printf("Pending signal was NOT preserved across execve!\n");
        exit(1);
    }
}

int main(int argc, char* argv[]) {
    if (argc < 1) {
        return 1;
    } else if (argc > 1) {
        test_execve_continue();
        puts("All tests OK");
        return 0;
    }

    test_sigprocmask();

    clean_mask_and_pending_signals();
    test_multiple_pending();

    clean_mask_and_pending_signals();
    test_fork();

    clean_mask_and_pending_signals();
    test_execve_start(argv[0]);
    return 1;
}
