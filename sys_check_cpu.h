/*************************************************
File name: sys_check_cpu.h
Author: liuk@fiberhome.com
Version: 0.1
Date: 20150819
Description: sys_check_cpu.c's head file

Function List: 
supply fellowing interface function
int sys_check_cpu_sched (float *load)
int sys_check_cpu_usage (float *idle)
int sys_check_cpu_process (char *name, float *usage)
*************************************************/

#ifndef _SYS_CHECK_CPU_H_
#define _SYS_CHECK_CPU_H_

/* FAST_FUNC is a qualifier which (possibly) makes function call faster
 * and/or smaller by using modified ABI. Recent versions of gcc
 * optimize statics automatically. FAST_FUNC on static is required
 * only if you need to match a function pointer's type */
#if __GNUC_PREREQ(3,0) && defined(i386) /* || defined(__x86_64__)? */
# define FAST_FUNC __attribute__((regparm(3),stdcall))
#else
# define FAST_FUNC
#endif

/* define area */
#define MAX_BUF_SIZE 3000 /* store some temp read out data */
#define MAX_PID_NUM  1024 /* define the max scan progress number while get progress pid from progress name */

/* enum area */
/*  used for store /proc/loadavg data */
enum 
{
	CPU_LOADAVG_1MINS = 0,
	CPU_LOADAVG_5MINS,
	CPU_LOADAVG_15MINS,
	CPU_LOADAVG_RESERVED1,
    CPU_LOADAVG_RESERVED2,
    CPU_LOADAVG_MAX
};

/*  used for store /proc/pid/stat data */
enum 
{
    PID = 0,
    CMD_NAME,
    TASK_STAT,
    PPID,
    PGID,
    SID,
    TTY_NR,
    TTY_PGRP,
    TASK_FLAGS,
    MIN_FLT,
    CMIN_FLT,
    MAJ_FLT,
    CMAJ_FLT,
    UTIME,
    STIME,
    CUTIME,
    CSTIME,
    PRIORITY,
    NICE,
    NUM_THREADS,
    IT_REAL_VALUE,
    START_TIME,
    VSIZE,
    RSS,
    RLIM,
    START_CODE,
    END_CODE,
    START_STACK,
    KSTKESP,
    KSTKEIP,
    PENDINGSIG,
    BLOCK_SIG,
    SIGIGN,
    SIGCATCH,
    WCHAN,
    NSWAP,
    CNSWAP,
    EXIT_SIGNAL,
    TASK_CPU,
    TASK_RT_PRIORITY,
    TASK_POLICY,
    DUMMY_ITEM1,
    DUMMY_ITEM2,
    DUMMY_ITEM3,
    DUMMY_ITEM4,
    DUMMY_ITEM5,
    PID_STAT_MAX
};

/* struct area */
/*  used for store /proc/loadaverage */
typedef struct proc_load_t
{
	float cpu_load_1min;
	float cpu_load_5min;
	float cpu_load_15min;
	float cpu_load_reserved1;
	float cpu_load_reserved2;
} proc_load_t;

/*  used for store the result of caculate /proc/stat */
typedef struct jiffy_counts_t 
{
	/* Linux 2.4.x has only first four */
	unsigned long long usr, nic, sys, idle;
	unsigned long long iowait, irq, softirq, steal;
	unsigned long long total;
	unsigned long long busy;
} jiffy_counts_t;

/*  used for store the result of caculate /proc/pid/stat */
typedef struct cpu_usage_t
{
	float cpu_us;
	float cpu_sy;
	float cpu_ni;
	float cpu_id;
	float cpu_wa;
	float cpu_hi;
	float cpu_si;
	float cpu_st;
	float cpu_total;
} cpu_usage_t;

/*function area*/
int sys_check_cpu_sched (float *load); /* check cpu's idle precent */
int sys_check_cpu_usage (float *kernel, float *user);/* check cpu's usage precent */
int sys_check_cpu_process (char *name, float *usage, int interval);/* check a progress take how many cpu's usage precent */

#endif
