/* Yash: yet another shell */
/* job.c: job control */
/* (C) 2007-2010 magicant */

/* This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.  */


#include "common.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wctype.h>
#if HAVE_GETTEXT
# include <libintl.h>
#endif
#include "builtin.h"
#include "exec.h"
#include "job.h"
#include "option.h"
#include "plist.h"
#include "redir.h"
#include "sig.h"
#include "strbuf.h"
#include "util.h"
#include "yash.h"
#if YASH_ENABLE_LINEEDIT
# include "xfnmatch.h"
# include "lineedit/complete.h"
# if !defined(FG_DONT_SAVE_TERMINAL)
#  include "lineedit/terminfo.h"
# endif
#endif


static inline job_T *get_job(size_t jobnumber)
    __attribute__((pure));
static inline void free_job(job_T *job);
static void trim_joblist(void);
static void set_current_jobnumber(size_t jobnumber);
static size_t find_next_job(size_t numlimit);
static void apply_curstop(void);
static int calc_status(int status)
    __attribute__((const));
static wchar_t *get_job_name(const job_T *job)
    __attribute__((nonnull,warn_unused_result));
static char *get_process_status_string(const process_T *p, bool *needfree)
    __attribute__((nonnull,malloc,warn_unused_result));
static char *get_job_status_string(const job_T *job, bool *needfree)
    __attribute__((nonnull,malloc,warn_unused_result));
static int print_job_status(
	size_t jobnumber, _Bool changedonly, _Bool verbose, FILE *f)
    __attribute__((nonnull));
static size_t get_jobnumber_from_name(const wchar_t *name)
    __attribute__((nonnull,pure));
static size_t get_jobnumber_from_pid(pid_t pid)
    __attribute__((pure));

static bool jobs_builtin_print_job(size_t jobnumber,
	bool verbose, bool changedonly, bool pgidonly,
	bool runningonly, bool stoppedonly);
static int continue_job(size_t jobnumber, job_T *job, bool fg)
    __attribute__((nonnull));
static bool wait_has_job(bool jobcontrol);


/* The list of jobs.
 * `joblist.contents[ACTIVE_JOBNO]' is a special job that is called "active
 * job": the job that is currently being executed. */
static plist_T joblist;

/* number of the current/previous jobs. 0 if none. */
static size_t current_jobnumber, previous_jobnumber;

/* Initializes the job list. */
void init_job(void)
{
    assert(joblist.contents == NULL);
    pl_init(&joblist);
    pl_add(&joblist, NULL);
}

/* Sets the active job. */
void set_active_job(job_T *job)
{
    assert(joblist.contents[ACTIVE_JOBNO] == NULL);
    joblist.contents[ACTIVE_JOBNO] = job;
}

/* Moves the active job into the job list.
 * If the newly added job is stopped, it becomes the current job.
 * If `current' is true or there is no current job, the newly added job becomes
 * the current job if there is no stopped job. */
void add_job(bool current)
{
    job_T *job = joblist.contents[ACTIVE_JOBNO];
    size_t jobnumber;

    assert(job != NULL);
    joblist.contents[ACTIVE_JOBNO] = NULL;

    /* if there is an empty element in the list, use it */
    for (jobnumber = 1; jobnumber < joblist.length; jobnumber++) {
	if (joblist.contents[jobnumber] == NULL) {
	    joblist.contents[jobnumber] = job;
	    goto set_current;
	}
    }

    /* if there is no empty, append at the end of the list */
    pl_add(&joblist, job);

set_current:
    assert(joblist.contents[jobnumber] == job);
    if (job->j_status == JS_STOPPED || current)
	set_current_jobnumber(jobnumber);
    else
	set_current_jobnumber(current_jobnumber);
}

/* Returns the job of the specified number or NULL if not found. */
job_T *get_job(size_t jobnumber)
{
    return (jobnumber < joblist.length) ? joblist.contents[jobnumber] : NULL;
}

/* Removes the job of the specified number.
 * If the job is the current/previous job, the current/previous job is reset
 * (another job is assigned to it). */
void remove_job(size_t jobnumber)
{
    free_job(get_job(jobnumber));
    joblist.contents[jobnumber] = NULL;
    trim_joblist();
    set_current_jobnumber(current_jobnumber);
}

/* Removes all jobs unconditionally. */
void remove_all_jobs(void)
{
    for (size_t i = 0; i < joblist.length; i++) {
	free_job(joblist.contents[i]);
	joblist.contents[i] = NULL;
    }
    trim_joblist();
    current_jobnumber = previous_jobnumber = 0;
}

/* Frees the specified job. */
void free_job(job_T *job)
{
    if (job) {
	for (size_t i = 0; i < job->j_pcount; i++)
	    free(job->j_procs[i].pr_name);
	free(job);
    }
}

/* Removes unused elements in `joblist'. */
void trim_joblist(void)
{
    size_t tail = joblist.length;

    while (tail > 0 && joblist.contents[--tail] == NULL);
    tail++;
    assert(tail > 0 && joblist.contents[tail] == NULL);
    if (joblist.maxlength > 20 && joblist.maxlength / 2 > joblist.length)
	pl_setmax(&joblist, tail);
    else
	pl_remove(&joblist, tail, SIZE_MAX);
}

/* Negate the `j_pgid' member of all jobs.
 * All the jobs are no longer taken care of by job control. */
void neglect_all_jobs(void)
{
    for (size_t i = 0; i < joblist.length; i++) {
	job_T *job = joblist.contents[i];
	if (job && job->j_pgid >= 0)
	    job->j_pgid = (job->j_pgid > 0) ? -job->j_pgid : -1;
    }
    current_jobnumber = previous_jobnumber = 0;
}

/* Current/previous job selection discipline:
 *
 * - When there is one or more stopped jobs, the current job must be one of
 *   them.
 * - When there are more than one stopped job, the previous job must be one of
 *   them but the current one.
 * - The current job becomes the previous job when another job becomes the
 *   current.
 *
 * - When a foreground job is stopped, it becomes the current job.
 * - When an asynchronous command is executed and the "curasync" option is set,
 *   it becomes the current job.
 * - When a job is continued by the "bg" command and the "curbg" option is set,
 *   it becomes the current job.
 * - When a job is stopped and the "curstop" option is set, it becomes the
 *   current job.
 *
 * - The "wait" command doesn't change the current and previous jobs. */

/* Sets the current job number to the specified one and resets the previous job
 * number. If the specified job number is not used, a job is arbitrarily chosen
 * for the current. If there is one or more stopped jobs and the one specified
 * by the argument is not stopped, the current job is not changed. */
/* This function must be called whenever a job is added to or removed from the
 * job list or any job's status has been changed. */
void set_current_jobnumber(size_t jobnumber)
{
    size_t stopcount = stopped_job_count();
    const job_T *newcurrent = get_job(jobnumber);

    if (newcurrent == NULL
	    || (stopcount > 0 && newcurrent->j_status != JS_STOPPED)) {
	jobnumber = current_jobnumber;
	newcurrent = get_job(jobnumber);
	if (newcurrent == NULL
		|| (stopcount > 0 && newcurrent->j_status != JS_STOPPED)) {
	    jobnumber = previous_jobnumber;
	    newcurrent = get_job(jobnumber);
	    if (newcurrent == NULL
		    || (stopcount > 0 && newcurrent->j_status != JS_STOPPED))
		jobnumber = find_next_job(0);
	}
    }

    if (jobnumber != current_jobnumber) {
	size_t oldcurrentnum = current_jobnumber;
	current_jobnumber = jobnumber;
	jobnumber = oldcurrentnum;
    } else {
	jobnumber = previous_jobnumber;
    }

    const job_T *newprevious = get_job(jobnumber);

    if (newprevious == NULL || jobnumber == current_jobnumber
	    || (stopcount > 1 && newprevious->j_status != JS_STOPPED)) {
	jobnumber = previous_jobnumber;
	newprevious = get_job(jobnumber);
	if (newprevious == NULL || jobnumber == current_jobnumber
		|| (stopcount > 1 && newprevious->j_status != JS_STOPPED))
	    jobnumber = find_next_job(current_jobnumber);
    }
    previous_jobnumber = jobnumber;
}

/* Returns an arbitrary job number except the specified.
 * The returned number is suitable for the current/previous jobs.
 * If there is no job to pick out, 0 is returned.
 * Stopped jobs are preferred to running/finished jobs.
 * If there are more than one stopped jobs, the previous job is preferred. */
size_t find_next_job(size_t excl)
{
    if (previous_jobnumber != excl) {
	job_T *job = get_job(previous_jobnumber);
	if (job != NULL && job->j_status == JS_STOPPED)
	    return previous_jobnumber;
    }
    size_t jobnumber = joblist.length;
    while (--jobnumber > 0) {
	if (jobnumber != excl) {
	    job_T *job = get_job(jobnumber);
	    if (job != NULL && job->j_status == JS_STOPPED)
		return jobnumber;
	}
    }
    jobnumber = joblist.length;
    while (--jobnumber > 0) {
	if (jobnumber != excl) {
	    job_T *job = get_job(jobnumber);
	    if (job != NULL)
		return jobnumber;
	}
    }
    return 0;
}

/* If the "curstop" option is set and there is a job which has been stopped and
 * whose `j_statuschanged' flag is set, make it the current job. */
void apply_curstop(void)
{
    if (shopt_curstop) {
	for (size_t i = 0; i < joblist.length; i++) {
	    job_T *job = joblist.contents[i];
	    if (job && job->j_status == JS_STOPPED && job->j_statuschanged)
		set_current_jobnumber(i);
	}
    }
    set_current_jobnumber(current_jobnumber);
}

/* Counts the number of jobs in the job list. */
size_t job_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < joblist.length; i++)
	if (joblist.contents[i])
	    count++;
    return count;
}

/* Counts the number of stopped jobs in the job list. */
size_t stopped_job_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < joblist.length; i++) {
	job_T *job = joblist.contents[i];
	if (job && job->j_status == JS_STOPPED)
	    count++;
    }
    return count;
}


/* Updates the info about the jobs in the job list.
 * This function doesn't block. */
void do_wait(void)
{
    pid_t pid;
    int status;

start:
#ifdef HAVE_WCONTINUED
    pid = waitpid(-1, &status, WUNTRACED | WCONTINUED | WNOHANG);
#else
    pid = waitpid(-1, &status, WUNTRACED | WNOHANG);
#endif
    if (pid < 0) {
	switch (errno) {
	    case EINTR:
		goto start;  /* try again */
	    case ECHILD:
		return;      /* there are no child processes */
	    default:
		xerror(errno, "waitpid");
		return;
	}
    } else if (pid == 0) {
	/* no more jobs to be updated */
	return;
    }

    size_t jobnumber, pnumber;
    job_T *job;
    process_T *pr;

    /* determine `jobnumber', `job' and `pr' from `pid' */
    for (jobnumber = 0; jobnumber < joblist.length; jobnumber++)
	if ((job = joblist.contents[jobnumber]))
	    for (pnumber = 0; pnumber < job->j_pcount; pnumber++)
		if ((pr = &job->j_procs[pnumber])->pr_pid == pid)
		    goto found;

    /* If `pid' is not found in the job list, we simply ignore it. This may
     * happen on some occasions: e.g. the job has been "disown"ed. */
    goto start;

found:
    pr->pr_statuscode = status;
    if (WIFEXITED(status) || WIFSIGNALED(status))
	pr->pr_status = JS_DONE;
    if (WIFSTOPPED(status))
	pr->pr_status = JS_STOPPED;
#ifdef HAVE_WCONTINUED
    if (WIFCONTINUED(status))
	pr->pr_status = JS_RUNNING;
    /* On FreeBSD, when WIFCONTINUED is true, WIFSIGNALED is also true. We must
     * be careful about the order of these checks. */
#endif

    /* decide the job status from the process status:
     * - JS_RUNNING if any of the processes is running.
     * - JS_STOPPED if no processes are running but some are stopped.
     * - JS_DONE if all the processes are finished. */
    jobstatus_T oldstatus = job->j_status;
    bool anyrunning = false, anystopped = false;
    /* check if there are running/stopped processes */
    for (size_t i = 0; i < job->j_pcount; i++) {
	switch (job->j_procs[i].pr_status) {
	    case JS_RUNNING:  anyrunning = true;  goto out_of_loop;
	    case JS_STOPPED:  anystopped = true;  break;
	    default:                              break;
	}
    }
out_of_loop:
    job->j_status = anyrunning ? JS_RUNNING : anystopped ? JS_STOPPED : JS_DONE;
    if (job->j_status != oldstatus)
	job->j_statuschanged = true;

    goto start;
}

/* Waits for the specified job to finish (or stop).
 * `jobnumber' must be a valid job number.
 * If `return_on_stop' is false, waits for the job to finish.
 * Otherwise, waits for the job to finish or stop.
 * If `interruptible' is true, this function can be canceled by SIGINT.
 * If `return_on_trap' is true, this function returns false immediately after
 * trap actions are performed. Otherwise, traps are not handled.
 * This function returns immediately if the job is already finished/stopped or
 * is not a child of this shell process.
 * Returns the signal number if interrupted, or zero if successful. */
/* In most cases, you should call `put_foreground' to bring the shell back to
 * foreground after calling `wait_for_job' if `doing_job_control_now' is true.
 */
int wait_for_job(size_t jobnumber, bool return_on_stop,
	bool interruptible, bool return_on_trap)
{
    int signum = 0;
    job_T *job = joblist.contents[jobnumber];

    if (job->j_pgid >= 0) {
	bool savenonotify = job->j_nonotify;
	job->j_nonotify = true;
	for (;;) {
	    if (job->j_status == JS_DONE)
		break;
	    if (return_on_stop && job->j_status == JS_STOPPED)
		break;
	    signum = wait_for_sigchld(interruptible, return_on_trap);
	    if (signum != 0)
		break;
	}
	job->j_nonotify = savenonotify;
    }
    return signum;
}

/* Waits for the specified child process to finish (or stop).
 * `cpid' is the process ID of the child process to wait for. This must not be
 * in the job list.
 * `cpgid' is the process group ID of the child. If the child's PGID is the same
 * as that of the parent, `cpgid' must be 0.
 * If `return_on_stop' is false, waits for the job to finish.
 * Otherwise, waits for the job to finish or stop.
 * Traps are not handled in this function.
 * There must be no active job when this function is called.
 * If `return_on_stop' is true and the child is stopped, this function returns
 * a pointer to a pointer to a wide string. The caller may assign a pointer to
 * a newly malloced wide string to the variable the return value points to.
 * This string is used as the name of the new stopped job.
 * If the child exited, this function returns NULL.
 * The exit status is assigned to `laststatus' anyway. */
wchar_t **wait_for_child(pid_t cpid, pid_t cpgid, bool return_on_stop)
{
    job_T *job = xmalloc(sizeof *job + sizeof *job->j_procs);
    job->j_pgid = cpgid;
    job->j_status = JS_RUNNING;
    job->j_statuschanged = false;
    job->j_nonotify = false;
    job->j_pcount = 1;
    job->j_procs[0].pr_pid = cpid;
    job->j_procs[0].pr_status = JS_RUNNING;
    job->j_procs[0].pr_statuscode = 0;
    job->j_procs[0].pr_name = NULL;
    set_active_job(job);
    wait_for_job(ACTIVE_JOBNO, return_on_stop, false, false);
    if (doing_job_control_now)
	put_foreground(shell_pgid);
    laststatus = calc_status_of_job(job);
    if (job->j_status == JS_DONE) {
	notify_signaled_job(ACTIVE_JOBNO);
	remove_job(ACTIVE_JOBNO);
	return NULL;
    } else {
	add_job(true);
	return &job->j_procs[0].pr_name;
    }
}

/* Returns the process group ID of the specified job.
 * If no valid job is found, an error message is printed and -1 is returned.
 * `jobname' may have a preceding '%' sign. */
pid_t get_job_pgid(const wchar_t *jobname)
{
    size_t jobnumber = get_jobnumber_from_name(
	    jobname[0] == L'%' ? jobname + 1 : jobname);
    const job_T *job;

    if (jobnumber >= joblist.length) {
	xerror(0, Ngt("%ls: ambiguous job specification"), jobname);
	return -1;
    } else if (jobnumber == 0
	    || (job = joblist.contents[jobnumber]) == NULL
	    || job->j_pgid < 0) {
	xerror(0, Ngt("%ls: no such job"), jobname);
	return -1;
    } else if (job->j_pgid == 0) {
	xerror(0, Ngt("%ls: not job-controlled job"), jobname);
	return -1;
    } else {
	return job->j_pgid;
    }
}

/* Puts the specified process group in the foreground.
 * `pgrp' must be a valid process group ID and `doing_job_control_now' must be
 * true. */
void put_foreground(pid_t pgrp)
{
    sigset_t blockss, savess;

    assert(doing_job_control_now);
    assert(pgrp > 0);

    sigemptyset(&blockss);
    sigaddset(&blockss, SIGTTOU);
    sigemptyset(&savess);
    sigprocmask(SIG_BLOCK, &blockss, &savess);

    tcsetpgrp(ttyfd, pgrp);

    sigprocmask(SIG_SETMASK, &savess, NULL);
}

/* Ensures the current shell process is in the foreground.
 * The shell process is stopped by SIGTTOU until it is put in the foreground.
 * This function requires `doing_job_control_now' to be true. */
/* This function prevents the job-control shell from mangling the terminal while
 * another shell is using it. */
void ensure_foreground(void)
{
    /* This function calls `tcsetpgrp' with the default SIGTTOU handler. If the
     * shell is in the background, it will receive SIGTTOU and get stopped until
     * it is continued in the foreground. */

    struct sigaction dflsa, savesa;
    sigset_t blockss, savess;

    assert(doing_job_control_now);
    assert(shell_pgid > 0);

    dflsa.sa_handler = SIG_DFL;
    dflsa.sa_flags = 0;
    sigemptyset(&dflsa.sa_mask);
    sigemptyset(&savesa.sa_mask);
    sigaction(SIGTTOU, &dflsa, &savesa);

    sigemptyset(&blockss);
    sigaddset(&blockss, SIGTTOU);
    sigemptyset(&savess);
    sigprocmask(SIG_UNBLOCK, &blockss, &savess);

    tcsetpgrp(ttyfd, shell_pgid);

    sigprocmask(SIG_SETMASK, &savess, NULL);
    sigaction(SIGTTOU, &savesa, NULL);
}

/* Computes the exit status from the status code returned by `waitpid'. */
int calc_status(int status)
{
    if (WIFEXITED(status))
	return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
	return WTERMSIG(status) + TERMSIGOFFSET;
    if (WIFSTOPPED(status))
	return WSTOPSIG(status) + TERMSIGOFFSET;
#ifdef WIFCONTINUED
    if (WIFCONTINUED(status))
	return Exit_SUCCESS;
#endif
    assert(false);
}

/* Computes the exit status of the specified job.
 * The job state must be JS_DONE or JS_STOPPED. */
int calc_status_of_job(const job_T *job)
{
    switch (job->j_status) {
    case JS_DONE:
	if (job->j_procs[job->j_pcount - 1].pr_pid)
	    return calc_status(job->j_procs[job->j_pcount - 1].pr_statuscode);
	else
	    return job->j_procs[job->j_pcount - 1].pr_statuscode;
    case JS_STOPPED:
	for (int i = job->j_pcount; --i >= 0; ) {
	    if (job->j_procs[i].pr_status == JS_STOPPED)
		return calc_status(job->j_procs[i].pr_statuscode);
	}
	/* falls thru! */
    default:
	assert(false);
    }
}

/* Returns the name of the specified job.
 * If the job has only one process, `job->j_procs[0].pr_name' is returned.
 * Otherwise, the names of all the process are concatenated and returned, which
 * must be freed by the caller. */
wchar_t *get_job_name(const job_T *job)
{
    if (job->j_pcount == 1)
	return job->j_procs[0].pr_name;

    xwcsbuf_T buf;
    wb_init(&buf);
    for (size_t i = 0; i < job->j_pcount; i++) {
	if (i > 0)
	    wb_cat(&buf, L" | ");
	wb_cat(&buf, job->j_procs[i].pr_name);
    }
    return wb_towcs(&buf);
}

/* Returns a string that describes the status of the specified process
 * such as "Running" and "Stopped(SIGTSTP)".
 * The returned string must be freed by the caller iff `*needfree' is assigned
 * true, otherwise it must not be modified or freed. */
char *get_process_status_string(const process_T *p, bool *needfree)
{
    int status, sig;

    switch (p->pr_status) {
    case JS_RUNNING:
	*needfree = false;
	return (char *) gt("Running");
    case JS_STOPPED:
	*needfree = true;
	return malloc_printf(gt("Stopped(SIG%ls)"),
		get_signal_name(WSTOPSIG(p->pr_statuscode)));
    case JS_DONE:
	status = p->pr_statuscode;
	if (p->pr_pid == 0)
	    goto exitstatus;
	if (WIFEXITED(status)) {
	    status = WEXITSTATUS(status);
exitstatus:
	    if (status == Exit_SUCCESS) {
		*needfree = false;
		return (char *) gt("Done");
	    } else {
		*needfree = true;
		return malloc_printf(gt("Done(%d)"), status);
	    }
	} else {
	    assert(WIFSIGNALED(status));
	    *needfree = true;
	    sig = WTERMSIG(status);
#ifdef WCOREDUMP
	    if (WCOREDUMP(sig))
		return malloc_printf(gt("Killed (SIG%ls: core dumped)"),
			get_signal_name(sig));
#endif
	    return malloc_printf(gt("Killed (SIG%ls)"), get_signal_name(sig));
	}
    }
    assert(false);
}

/* Returns a string that describes the status of the specified job
 * such as "Running" and "Stopped(SIGTSTP)".
 * The returned string must be freed by the caller iff `*needfree' is assigned
 * true, otherwise it must not be modified or freed. */
char *get_job_status_string(const job_T *job, bool *needfree)
{
    switch (job->j_status) {
    case JS_RUNNING:
	*needfree = false;
	return (char *) gt("Running");
    case JS_STOPPED:
	/* find a stopped process */
	for (size_t i = job->j_pcount; ; )
	    if (job->j_procs[--i].pr_status == JS_STOPPED)
		return get_process_status_string(&job->j_procs[i], needfree);
	assert(false);
    case JS_DONE:
	return get_process_status_string(
		&job->j_procs[job->j_pcount - 1], needfree);
    }
    assert(false);
}

/* Prints the status of the specified job.
 * Finished jobs are removed from the job list after the status is printed.
 * If the specified job doesn't exist, nothing is printed (it isn't an error).
 * If `changedonly' is true, the job is printed only if the `j_statuschanged'
 * flag is true.
 * If `verbose' is true, the status is printed in the process-wise format rather
 * than the usual job-wise format.
 * Returns zero if successful. Returns errno if `fprintf' failed. */
int print_job_status(size_t jobnumber, bool changedonly, bool verbose, FILE *f)
{
    int result = 0;

    job_T *job = get_job(jobnumber);
    if (!job || (changedonly && !job->j_statuschanged) || job->j_nonotify)
	return result;

    char current;
    if      (jobnumber == current_jobnumber)  current = '+';
    else if (jobnumber == previous_jobnumber) current = '-';
    else                                      current = ' ';

    if (!verbose) {
	bool needfree;
	char *status = get_job_status_string(job, &needfree);
	wchar_t *jobname = get_job_name(job);

	/* TRANSLATORS: the translated format string can be different 
	 * from the original only in the number of spaces. This is required
	 * for POSIX compliance. */
	result = fprintf(f, gt("[%zu] %c %-20s %ls\n"),
		jobnumber, current, status, jobname);
	result = (result >= 0) ? 0 : errno;

	if (needfree)
	    free(status);
	if (jobname != job->j_procs[0].pr_name)
	    free(jobname);
    } else {
	bool needfree;
	pid_t pid = job->j_procs[0].pr_pid;
	char *status = get_process_status_string(
		&job->j_procs[posixly_correct ? job->j_pcount - 1 : 0],
		&needfree);
	wchar_t *jobname = job->j_procs[0].pr_name;

	/* TRANSLATORS: the translated format string can be different 
	 * from the original only in the number of spaces. This is required
	 * for POSIX compliance. */
	result = fprintf(f, gt("[%zu] %c %5jd %-20s   %ls\n"),
		jobnumber, current, (intmax_t) pid, status, jobname);
	result = (result >= 0) ? 0 : errno;
	if (needfree)
	    free(status);

	for (size_t i = 1; result == 0 && i < job->j_pcount; i++) {
	    pid = job->j_procs[i].pr_pid;
	    status = get_process_status_string(&job->j_procs[i], &needfree);
	    jobname = job->j_procs[i].pr_name;

	    /* TRANSLATORS: the translated format string can be different 
	     * from the original only in the number of spaces. This is required
	     * for POSIX compliance. */
	    result = fprintf(f, gt("      %5jd %-20s | %ls\n"),
		    (intmax_t) pid, (posixly_correct ? "" : status), jobname);
	    result = (result >= 0) ? 0 : errno;
	    if (needfree)
		free(status);
	}
    }
    job->j_statuschanged = false;
    if (job->j_status == JS_DONE)
	remove_job(jobnumber);

    return result;
}

/* Prints the status of jobs which have been changed but not reported. */
void print_job_status_all(void)
{
    apply_curstop();
    for (size_t i = 1; i < joblist.length; i++)
	print_job_status(i, true, false, stderr);
}

/* If the shell is interactive and the specified job has been killed by a
 * signal other than SIGPIPE, prints a notification to stderr.
 * If the signal is SIGINT, only a single newline is printed to stderr and the
 * shell is flagged as interrupted. */
void notify_signaled_job(size_t jobnumber)
{
    if (!is_interactive_now)
	return;

    job_T *job = get_job(jobnumber);
    if (!job || job->j_status != JS_DONE)
	return;

    process_T *p = &job->j_procs[job->j_pcount - 1];
    assert(p->pr_status == JS_DONE);
    if (p->pr_pid == 0 || !WIFSIGNALED(p->pr_statuscode))
	return;

    int sig = WTERMSIG(p->pr_statuscode);
    switch (sig) {
	case SIGINT:
	    fputc('\n', stderr);
	    set_interrupted();
	    break;
	case SIGPIPE:
	    break;
	default:
#if HAVE_STRSIGNAL
	    fprintf(stderr, gt("Process killed by SIG%ls: %s\n"),
		    get_signal_name(sig), strsignal(sig));
#else
	    fprintf(stderr, gt("Process killed by SIG%ls\n"),
		    get_signal_name(sig));
#endif
	    break;
    }
}

/* Returns the job number from the specified job ID string.
 * If no applicable job is found, zero is returned.
 * If more than one jobs are found, `joblist.length' is returned.
 * When `name' is a number, the number is returned if it is a valid job number.
 * The string must not have the preceding '%'. */
/* "", "%", "+"  -> the current job
 * "-"           -> the previous job
 * "n" (integer) -> the job #n
 * "xxx"         -> the job whose name starts with "xxx"
 * "?xxx"        -> the job whose name contains "xxx" */
size_t get_jobnumber_from_name(const wchar_t *name)
{
    if (name[0] == L'\0' || wcscmp(name, L"%") == 0 || wcscmp(name, L"+") == 0)
	return current_jobnumber;
    if (wcscmp(name, L"-") == 0)
	return previous_jobnumber;

    if (iswdigit(name[0])) {
	unsigned long num;
	if (xwcstoul(name, 10, &num))
	    return (num <= SIZE_MAX && get_job(num)) ? num : 0;
    }

    bool contain;
    size_t n = 0;
    if (name[0] == L'?') {
	contain = true;
	name++;
    } else {
	contain = false;
    }
    for (size_t i = 1; i < joblist.length; i++) {
	job_T *job = joblist.contents[i];
	if (job) {
	    wchar_t *jobname = get_job_name(job);
	    bool match = (contain ? wcsstr : matchwcsprefix)(jobname, name);
	    if (jobname != job->j_procs[0].pr_name)
		free(jobname);
	    if (match) {
		if (n != 0)
		    return joblist.length;  /* more than one found */
		else
		    n = i;
	    }
	}
    }
    return n;
}

/* Returns the number of job that contains a process whose process ID is `pid'.
 * If not found, 0 is returned. */
size_t get_jobnumber_from_pid(pid_t pid)
{
    size_t jobnumber;
    if (pid == 0)
	return 0;
    for (jobnumber = joblist.length; --jobnumber > 0; ) {
	job_T *job = joblist.contents[jobnumber];
	if (job) {
	    for (size_t i = 0; i < job->j_pcount; i++)
		if (job->j_procs[i].pr_pid == pid)
		    goto found;
	}
    }
found:
    return jobnumber;
}

#if YASH_ENABLE_LINEEDIT

/* Generates completion candidates for job names that match the glob pattern
 * in the specified context. */
/* The prototype of this function is declared in "lineedit/complete.h". */
void generate_job_candidates(
	le_candgentype_T type, const le_context_T *context)
{
    if (!(type & CGT_JOB))
	return;

    const wchar_t *pattern = context->pattern;
    if (pattern[0] == L'%')
	pattern += 1;
    else if (pattern[0] == L'\\' && pattern[1] == L'%')
	pattern += 2;
    le_compdebug("adding jobs for pattern \"%ls\"", pattern);

    xfnmatch_T *xfnm = xfnm_compile(pattern, XFNM_HEADONLY | XFNM_TAILONLY);
    if (xfnm == NULL)
	return;

    for (size_t i = 1; i < joblist.length; i++) {
	const job_T *job = joblist.contents[i];
	if (job == NULL)
	    continue;
	switch (job->j_status) {
	    case JS_RUNNING:  if (!(type & CGT_RUNNING)) continue;  break;
	    case JS_STOPPED:  if (!(type & CGT_STOPPED)) continue;  break;
	    case JS_DONE:     if (!(type & CGT_DONE))    continue;  break;
	}

	const wchar_t *jobname = job->j_procs[0].pr_name;
	if (xfnm_wmatch(xfnm, jobname).start == (size_t) -1)
	    continue;

	wchar_t *cand;
	if (context->src[0] != L'%')
	    cand = xwcsdup(jobname);
	else
	    cand = malloc_wprintf(L"%%%ls", jobname);
	le_new_candidate(CT_JOB, cand, NULL);
    }
    xfnm_free(xfnm);
}

#endif /* YASH_ENABLE_LINEEDIT */


/********** Builtins **********/

/* The "jobs" builtin, which accepts the following options:
 *  -l: be verbose
 *  -n: print the jobs only whose status have changed
 *  -p: print the process ID only
 *  -r: print running jobs only
 *  -s: print stopped jobs only
 * In the POSIXly correct mode, only -l and -p are available. */
int jobs_builtin(int argc, void **argv)
{
    static const struct xoption long_options[] = {
	{ L"verbose",      xno_argument, L'l', },
	{ L"new",          xno_argument, L'n', },
	{ L"pgid-only",    xno_argument, L'p', },
	{ L"running-only", xno_argument, L'r', },
	{ L"stopped-only", xno_argument, L's', },
#if YASH_ENABLE_HELP
	{ L"help",         xno_argument, L'-', },
#endif
	{ NULL, 0, 0, },
    };

    bool verbose = false, changedonly = false, pgidonly = false;
    bool runningonly = false, stoppedonly = false;
    wchar_t opt;

    xoptind = 0, xopterr = true;
    while ((opt = xgetopt_long(argv,
		    posixly_correct ? L"lp" : L"lnprs",
		    long_options, NULL))) {
	switch (opt) {
	    case L'l':  verbose     = true;  break;
	    case L'n':  changedonly = true;  break;
	    case L'p':  pgidonly    = true;  break;
	    case L'r':  runningonly = true;  break;
	    case L's':  stoppedonly = true;  break;
#if YASH_ENABLE_HELP
	    case L'-':
		return print_builtin_help(ARGV(0));
#endif
	    default:
		fprintf(stderr, gt(posixly_correct
			    ? Ngt("Usage:  jobs [-lp] [job...]\n")
			    : Ngt("Usage:  jobs [-lnprs] [job...]\n")));
		return Exit_ERROR;
	}
    }

    nextforceexit = true;
    apply_curstop();

    if (xoptind < argc) {
	/* print the specified jobs */
	do {
	    const wchar_t *jobspec = ARGV(xoptind);
	    if (jobspec[0] == L'%') {
		jobspec++;
	    } else if (posixly_correct) {
		xerror(0, Ngt("%ls: invalid job specification"), ARGV(xoptind));
		continue;
	    }
	    size_t jobnumber = get_jobnumber_from_name(jobspec);
	    if (jobnumber >= joblist.length) {
		xerror(0, Ngt("%ls: ambiguous job specification"),
			ARGV(xoptind));
	    } else if (jobnumber == 0 || joblist.contents[jobnumber] == NULL) {
		xerror(0, Ngt("%ls: no such job"), ARGV(xoptind));
	    } else {
		if (!jobs_builtin_print_job(jobnumber, verbose,
			changedonly, pgidonly, runningonly, stoppedonly))
		    return Exit_FAILURE;
	    }
	} while (++xoptind < argc);
    } else {
	/* print all jobs */
	for (size_t i = 1; i < joblist.length; i++) {
	    if (!jobs_builtin_print_job(i, verbose, changedonly, pgidonly,
		    runningonly, stoppedonly))
		return Exit_FAILURE;
	}
    }

    return (yash_error_message_count == 0) ? Exit_SUCCESS : Exit_FAILURE;
}

/* Prints the job status.
 * On an I/O error, an error message is printed to the standard error and false
 * is returned. */
bool jobs_builtin_print_job(size_t jobnumber,
	bool verbose, bool changedonly, bool pgidonly,
	bool runningonly, bool stoppedonly)
{
    job_T *job = get_job(jobnumber);

    if (job == NULL)
	return true;
    if (runningonly && job->j_status != JS_RUNNING)
	return true;
    if (stoppedonly && job->j_status != JS_STOPPED)
	return true;

    int err;
    if (pgidonly) {
	if (changedonly && !job->j_statuschanged)
	    return true;
	int result = printf("%jd\n", imaxabs(job->j_pgid));
	err = (result >= 0) ? 0 : errno;
    } else {
	err = print_job_status(jobnumber, changedonly, verbose, stdout);
    }
    if (err != 0) {
	xerror(err, Ngt("cannot print to standard output"));
	return false;
    } else {
	return true;
    }
}

#if YASH_ENABLE_HELP
const char jobs_help[] = Ngt(
"jobs - print info about jobs\n"
"\tjobs [-lnprs] [job...]\n"
"Prints the status of jobs in the current shell execution environment.\n"
"If the <job> is specified, the specified job is printed.\n"
"If none is specified, all jobs are printed.\n"
"Available options:\n"
" -l --verbose\n"
"\tprint info for each process in the job, including process ID\n"
" -n --new\n"
"\tonly print jobs whose status have changed\n"
" -p --pgid-only\n"
"\tprint process group IDs only\n"
" -r --running-only\n"
"\tprint running jobs only\n"
" -s --stopped-only\n"
"\tprint stopped jobs only\n"
"In POSIXly correct mode, only the -l and -p options are available.\n"
);
#endif

/* The "fg"/"bg" builtin */
int fg_builtin(int argc, void **argv)
{
    bool fg = wcscmp(argv[0], L"fg") == 0;
    wchar_t opt;

    xoptind = 0, xopterr = true;
    while ((opt = xgetopt_long(argv, L"", help_option, NULL))) {
	switch (opt) {
#if YASH_ENABLE_HELP
	    case L'-':
		return print_builtin_help(ARGV(0));
#endif
	    default:  print_usage:
		fprintf(stderr, gt(fg ? Ngt("Usage:  fg [job]\n")
		                      : Ngt("Usage:  bg [job...]\n")));
		return Exit_ERROR;
	}
    }

    if (!doing_job_control_now) {
	xerror(0, Ngt("job control disabled"));
	return Exit_FAILURE;
    }

    int status = Exit_SUCCESS;
    job_T *job;

    if (xoptind < argc) {
	if (fg && posixly_correct && argc - xoptind > 1) {
	    xerror(0, Ngt("too many operands"));
	    goto print_usage;
	}
	do {
	    const wchar_t *jobspec = ARGV(xoptind);
	    if (jobspec[0] == L'%') {
		jobspec++;
	    } else if (posixly_correct) {
		xerror(0, Ngt("%ls: invalid job specification"), ARGV(xoptind));
		continue;
	    }
	    size_t jobnumber = get_jobnumber_from_name(jobspec);
	    if (jobnumber >= joblist.length) {
		xerror(0, Ngt("%ls: ambiguous job specification"),
			ARGV(xoptind));
	    } else if (jobnumber == 0
		    || (job = joblist.contents[jobnumber]) == NULL
		    || job->j_pgid < 0) {
		xerror(0, Ngt("%ls: no such job"), ARGV(xoptind));
	    } else if (job->j_pgid == 0) {
		xerror(0, Ngt("%ls: not job-controlled job"), ARGV(xoptind));
	    } else {
		status = continue_job(jobnumber, job, fg);
	    }
	} while (++xoptind < argc);
    } else {
	if (current_jobnumber == 0 ||
		(job = joblist.contents[current_jobnumber])->j_pgid <= 0) {
	    xerror(0, Ngt("no current job"));
	} else {
	    status = continue_job(current_jobnumber, job, fg);
	}
    }

    if (status != 0)
	return status;
    if (yash_error_message_count != 0)
	return Exit_FAILURE;
    return Exit_SUCCESS;
}

/* Continues execution of the specified job.
 * Returns the exit code of the continued job or 0 if it is still running. */
int continue_job(size_t jobnumber, job_T *job, bool fg)
{
    assert(job->j_pgid > 0);

    wchar_t *name = get_job_name(job);
    if (fg && posixly_correct)
	printf("%ls\n", name);
    else
	printf("[%zu] %ls\n", jobnumber, name);
    if (name != job->j_procs[0].pr_name)
	free(name);

#if YASH_ENABLE_LINEEDIT && !defined(FG_DONT_SAVE_TERMINAL)
    bool termsave = fg && le_save_terminal();  /* see below */
#endif

    if (job->j_status != JS_DONE) {
	if (fg)
	    put_foreground(job->j_pgid);
	if (kill(-job->j_pgid, SIGCONT) >= 0)
	    job->j_status = JS_RUNNING;
    } else {
	if (!fg)
	    xerror(0, Ngt("job %%%zu has already terminated"), jobnumber);
    }

    int status;
    if (fg) {
	wait_for_job(jobnumber, true, false, false);
	put_foreground(shell_pgid);  /* put the shell in the foreground */
#if YASH_ENABLE_LINEEDIT && !defined(FG_DONT_SAVE_TERMINAL)
	if (termsave)
	    le_restore_terminal();
#endif
	switch (job->j_status) {
	    case JS_STOPPED:
		status = calc_status_of_job(job);
		set_current_jobnumber(jobnumber);
		break;
	    case JS_DONE:
		status = calc_status_of_job(job);
		notify_signaled_job(jobnumber);
		remove_job(jobnumber);
		break;
	    default:
		assert(false);
	}
    } else {
	set_current_jobnumber(shopt_curbg ? jobnumber : current_jobnumber);
	status = (job->j_status == JS_RUNNING) ? Exit_SUCCESS : Exit_FAILURE;
    }
    return status;

    /* We save the terminal state before continuing a job in the foreground and
     * restore the state after the job has finished. This is because some
     * programs leave the terminal in the wrong state if they were at first
     * invoked in the background.
     * Programs that change the terminal state generally save the state before
     * changing it and restore it when they are finished. But, if they are
     * invoked in the background, they may save the state that is being used by
     * another program (typically the shell's line-editing), so the state that
     * they restore is not the normal state.
     * The shell tackles this problem by saving and restoring the terminal state
     * for the continued programs. */
}

#if YASH_ENABLE_HELP

const char fg_help[] = Ngt(
"fg - run jobs in the foreground\n"
"\tfg [job...]\n"
"Continues execution of the specified jobs in the foreground.\n"
"In POSIXly correct mode, you can specify at most one job. Otherwise, more\n"
"than one jobs can be specified, which are in turn continued.\n"
"If no job is specified, the current job is continued.\n"
);

const char bg_help[] = Ngt(
"bg - run jobs in the background\n"
"\tbg [job...]\n"
"Continues execution of the specified jobs in the background.\n"
"If no job is specified, the current job is continued.\n"
);

#endif /* YASH_ENABLE_HELP */

/* The "wait" builtin */
int wait_builtin(int argc, void **argv)
{
    bool jobcontrol = doing_job_control_now;
    int status = Exit_SUCCESS;
    wchar_t opt;

    xoptind = 0, xopterr = true;
    while ((opt = xgetopt_long(argv, L"", help_option, NULL))) {
	switch (opt) {
#if YASH_ENABLE_HELP
	    case L'-':
		return print_builtin_help(ARGV(0));
#endif
	    default:
		fprintf(stderr, gt("Usage:  wait [job or pid...]\n"));
		return Exit_ERROR;
	}
    }

    job_T *job;
    if (xoptind < argc) {
	/* wait for the specified jobs */
	do {
	    const wchar_t *jobspec = ARGV(xoptind);
	    size_t jobnumber;
	    if (jobspec[0] == L'%') {
		jobnumber = get_jobnumber_from_name(jobspec + 1);
	    } else {
		long pid;
		if (!xwcstol(jobspec, 10, &pid) || pid < 0) {
		    xerror(0, Ngt("%ls: invalid job specification"), jobspec);
		    continue;
		}
		jobnumber = get_jobnumber_from_pid((pid_t) pid);
		// XXX This cast might not be safe
	    }
	    if (jobnumber >= joblist.length) {
		xerror(0, Ngt("%ls: ambiguous job specification"),
			ARGV(xoptind));
	    } else if (jobnumber == 0
		    || (job = joblist.contents[jobnumber]) == NULL
		    || job->j_pgid < 0) {
		status = Exit_NOTFOUND;
	    } else {
		status = wait_for_job(jobnumber, jobcontrol, jobcontrol, true);
		if (status == 0) {
		    status = calc_status_of_job(job);
		} else {
		    assert(TERMSIGOFFSET >= 128);
		    status += TERMSIGOFFSET;
		    break;
		}
		if (job->j_status != JS_RUNNING) {
		    if (jobcontrol && is_interactive_now && !posixly_correct)
			print_job_status(jobnumber, false, false, stdout);
		    else if (job->j_status == JS_DONE)
			remove_job(jobnumber);
		}
	    }
	} while (++xoptind < argc);
    } else {
	/* wait for all jobs */
	while (wait_has_job(jobcontrol)) {
	    status = wait_for_sigchld(jobcontrol, true);
	    if (status) {
		assert(TERMSIGOFFSET >= 128);
		status += TERMSIGOFFSET;
		break;
	    }
	}
    }

    if (status != 0)
	return status;
    if (yash_error_message_count != 0)
	return Exit_FAILURE;
    return Exit_SUCCESS;
}

/* Checks if the shell has any job to wait for. */
bool wait_has_job(bool jobcontrol)
{
    /* print/remove already-finished jobs */
    if (jobcontrol && is_interactive_now && !posixly_correct) {
	for (size_t i = 1; i < joblist.length; i++)
	    print_job_status(i, true, false, stdout);
    } else {
	for (size_t i = 1; i < joblist.length; i++) {
	    job_T *job = joblist.contents[i];
	    if (job != NULL && (job->j_pgid < 0 || job->j_status == JS_DONE))
		remove_job(i);
	}
    }

    for (size_t i = 1; i < joblist.length; i++) {
	job_T *job = joblist.contents[i];
	if (job != NULL && (!jobcontrol || job->j_status == JS_RUNNING))
	    return true;
    }
    return false;
}

#if YASH_ENABLE_HELP
const char wait_help[] = Ngt(
"wait - wait for jobs to terminate\n"
"\twait [job or pid...]\n"
"Waits for the specified jobs, or all jobs if none specified, to terminate.\n"
"Jobs can be specified in the usual job specification form such as \"%2\" or\n"
"by the process ID of a process belonging to the job.\n"
);
#endif

/* The "disown" builtin, which accepts the following option:
 *  -a: disown all jobs */
int disown_builtin(int argc, void **argv)
{
    bool all = false;
    wchar_t opt;

    xoptind = 0, xopterr = true;
    while ((opt = xgetopt_long(argv, L"a", all_option, NULL))) {
	switch (opt) {
	    case L'a':  all = true;  break;
#if YASH_ENABLE_HELP
	    case L'-':
		return print_builtin_help(ARGV(0));
#endif
	    default:
		fprintf(stderr, gt("Usage:  disown [-a] [job...]\n"));
		return Exit_ERROR;
	}
    }

    if (all) {
	remove_all_jobs();
    } else if (xoptind < argc) {
	do {
	    const wchar_t *jobspec = ARGV(xoptind);
	    if (jobspec[0] == L'%') {
		jobspec++;
	    } else if (posixly_correct) {
		xerror(0, Ngt("%ls: invalid job specification"), ARGV(xoptind));
		continue;
	    }
	    size_t jobnumber = get_jobnumber_from_name(jobspec);
	    if (jobnumber >= joblist.length) {
		xerror(0, Ngt("%ls: ambiguous job specification"),
			ARGV(xoptind));
	    } else if (jobnumber == 0 || joblist.contents[jobnumber] == NULL) {
		xerror(0, Ngt("%ls: no such job"), ARGV(xoptind));
	    } else {
		remove_job(jobnumber);
	    }
	} while (++xoptind < argc);
    } else {
	if (current_jobnumber == 0 || get_job(current_jobnumber) == NULL)
	    xerror(0, Ngt("no current job"));
	else
	    remove_job(current_jobnumber);
    }

    return (yash_error_message_count == 0) ? Exit_SUCCESS : Exit_FAILURE;
}

#if YASH_ENABLE_HELP
const char disown_help[] = Ngt(
"disown - disown jobs\n"
"\tdisown [job...]\n"
"\tdisown -a\n"
"Removes the specified jobs from the job list.\n"
"The status of the disowned jobs is no longer reported and the jobs can no\n"
"longer be put back in the foreground.\n"
"If the -a (--all) option is specified, all existing jobs are disowned.\n"
"Otherwise, the specified jobs are disowned. If none is specified, the\n"
"current job is disowned.\n"
);
#endif


/* vim: set ts=8 sts=4 sw=4 noet tw=80: */
