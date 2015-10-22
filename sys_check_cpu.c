/*************************************************
File name: sys_check_cpu.c
Author: liuk@fiberhome.com
Version: 0.1
Date: 20150819
Description: Provide CPU related detection interface

Function List: 
supply fellowing interface function
int sys_check_cpu_sched (float *load)
int sys_check_cpu_usage (float *idle)
int sys_check_cpu_process (char *name, float *usage)
*************************************************/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <sys/time.h>

#include "sys_check_cpu.h"

int g_num_cpus = 0; /* save how many cpu exist at this system */
proc_load_t g_cur_cpuload; // read /proc/loadavg and store cpu's loadaverage @g_cur_cpuload
jiffy_counts_t *g_cpu_jif; // read /proc/stat and store cpu's jiffies
jiffy_counts_t g_cur_jif;// read /proc/stat and store cpu's jiffies
jiffy_counts_t g_prev_jif;// read /proc/stat and store cpu's jiffies
cpu_usage_t g_cur_cpu_usage;// caculate cpu's usage accord cpu's jiffies and store here
char g_line_buf[MAX_BUF_SIZE];//store some read out data here
unsigned long long g_prev_pid_cpu_stat[PID_STAT_MAX];//read /proc/pid/stat and store here
unsigned long long g_cur_pid_cpu_stat[PID_STAT_MAX];//read /proc/pid/stat and store here

FILE* FAST_FUNC xfopen_for_read(const char *path)
{
	FILE *fp = fopen(path,"r");
	if (fp == NULL)
		printf("can't open '%s'", path);
	return fp;
}

/* Resize (grow) malloced vector.
 *
 *  #define magic packed two parameters into one:
 *  sizeof = sizeof_and_shift >> 8
 *  shift  = (sizeof_and_shift) & 0xff
 *
 * Lets say shift = 4. 1 << 4 == 0x10.
 * If idx == 0, 0x10, 0x20 etc, vector[] is resized to next higher
 * idx step, plus one: if idx == 0x20, vector[] is resized to 0x31,
 * thus last usable element is vector[0x30].
 *
 * In other words: after xrealloc_vector(v, 4, idx), with any idx,
 * it's ok to use at least v[idx] and v[idx+1].
 * v[idx+2] etc generally are not ok.
 *
 * New elements are zeroed out, but only if realloc was done
 * (not on every call). You can depend on v[idx] and v[idx+1] being
 * zeroed out if you use it like this:
 *  v = xrealloc_vector(v, 4, idx);
 *  v[idx].some_fields = ...; - the rest stays 0/NULL
 *  idx++;
 * If you do not advance idx like above, you should be more careful.
 * Next call to xrealloc_vector(v, 4, idx) may or may not zero out v[idx].
 */
void* FAST_FUNC xrealloc_vector_helper(void *vector, unsigned sizeof_and_shift, int idx)
{
	int mask = 1 << (uint8_t)sizeof_and_shift;

	if (!(idx & (mask - 1))) 
	{
		sizeof_and_shift >>= 8; /* sizeof(vector[0]) */
		vector = realloc(vector, sizeof_and_shift * (idx + mask + 1));
		memset((char*)vector + (sizeof_and_shift * idx), 0, sizeof_and_shift * (mask + 1));
	}
	return vector;
}

/* After v = xrealloc_vector(v, SHIFT, idx) it's ok to use
 * at least v[idx] and v[idx+1], for all idx values.
 * SHIFT specifies how many new elements are added (1:2, 2:4, ..., 8:256...)
 * when all elements are used up. New elements are zeroed out.
 * xrealloc_vector(v, SHIFT, idx) *MUST* be called with consecutive IDXs -
 * skipping an index is a bad bug - it may miss a realloc!
 */
#define xrealloc_vector(vector, shift, idx) \
	xrealloc_vector_helper((vector), (sizeof((vector)[0]) << 8) + (shift), (idx))

/*************************************************
Function: parse_loadavg
Description: parse /proc/loadavg file and put data into float cpuloadavg[CPU_LOADAVG_MAX]
Calls: 
	FILE* FAST_FUNC xfopen_for_read(const char *path)
Input: float cpuloadavg[CPU_LOADAVG_MAX]  used to save current cpu's loadaverage data
Output: current cpu's loadaverage data
*************************************************/
static int parse_loadavg(float cpuloadavg[CPU_LOADAVG_MAX])
{
	char buf[60];
	FILE *f;
	int i = 0;

	memset(cpuloadavg, 0, sizeof(cpuloadavg[0]) * CPU_LOADAVG_MAX);
	if (NULL == (f = fopen("/proc/loadavg","r")))
	{
		printf("can't open /proc/loadavg because:%s\n", strerror(errno));
		return -1;
	}

	while (fgets(buf, sizeof(buf), f) != NULL) 
	{
		//printf("buf=%s\n",buf);//for debug
		char *tmp = buf;
		char *p[CPU_LOADAVG_MAX];
		while ((p[i] = strtok(tmp, " ")) != NULL) 
		{
			//printf("p[%d]=%s\n", i, p[i]); //for debug
			cpuloadavg[i] = atof(p[i]);
			
			//printf("cpuloadavg[%d]=%.2f\n", i, cpuloadavg[i]);//for debug
			i++;
			tmp = NULL;
		}		
	}
	memset(&g_cur_cpuload, 0, sizeof(g_cur_cpuload));
	g_cur_cpuload.cpu_load_1min = cpuloadavg[CPU_LOADAVG_1MINS];
	g_cur_cpuload.cpu_load_5min = cpuloadavg[CPU_LOADAVG_5MINS];
	g_cur_cpuload.cpu_load_15min = cpuloadavg[CPU_LOADAVG_15MINS];
	fclose(f);
	return 0;
}

/*************************************************
Function: read_cpu_jiffy
Description: parse /proc/stat file and put data into jiffy_counts_t struct
Input: jiffy_counts_t *p_jif used to save current cpu's jiffies data
Output: current cpu's jiffies data
*************************************************/
static int read_cpu_jiffy(FILE *fp, jiffy_counts_t *p_jif)
{
	static const char s_fmt[] = "cpu %llu %llu %llu %llu %llu %llu %llu %llu";

	int ret;

	if (!fgets(g_line_buf, MAX_BUF_SIZE, fp) || g_line_buf[0] != 'c' /* not "cpu" */)
		return 0;
	ret = sscanf(g_line_buf, s_fmt,
			&p_jif->usr, &p_jif->nic, &p_jif->sys, &p_jif->idle,
			&p_jif->iowait, &p_jif->irq, &p_jif->softirq,
			&p_jif->steal);
	if (ret >= 4) 
	{
		p_jif->total = p_jif->usr + p_jif->nic + p_jif->sys + p_jif->idle
			+ p_jif->iowait + p_jif->irq + p_jif->softirq + p_jif->steal;
		p_jif->busy = p_jif->total - p_jif->idle - p_jif->iowait;
	}
	
/*printf("usr(%llu) nic(%llu) sys(%llu) idle(%llu) iowait(%llu) irq(%llu) 
softirq(%llu) steal(%llu) total(%llu) busy(%llu)\n",
p_jif->usr,p_jif->nic,p_jif->sys,p_jif->idle,p_jif->iowait,p_jif->irq,
p_jif->softirq,p_jif->steal, p_jif->total, p_jif->busy);*/
	return ret;
}

/*************************************************
Function: get_jiffy_counts
Description: get /proc/stat information and store @jiffy_counts_t
Calls: 
	FILE* FAST_FUNC xfopen_for_read(const char *path)
Input: jiffy_counts_t *jif used to save current /proc/stat data
Output: current cpu's jiffies data
*************************************************/
static int get_jiffy_counts(jiffy_counts_t *jif)
{
	FILE *fp = xfopen_for_read("/proc/stat");
	if (fp == NULL)
		return -1;

	if (read_cpu_jiffy(fp, jif) < 4)
	{
		printf("can't read '%s'", "/proc/stat");
		fclose(fp);
		return -1;
	}
		

	fclose(fp);
	return 0;
}

/*************************************************
Function: get_num_cpus
Description: get current system's cpu number
Calls: 
Input: 
Output: 
Return: 
*************************************************/
static int get_num_cpus()
{
	FILE *fp;
	if (NULL == (fp = fopen("/proc/stat","r")))
	{
		printf("can't open /proc/stat because:%s\n", strerror(errno));
		return -1;
	}
		
	g_prev_jif = g_cur_jif;
	if (read_cpu_jiffy(fp, &g_cur_jif) < 4)
		printf("can't read '%s'", "/proc/stat");

	if (0 == g_num_cpus) 
	{
		while (1) 
		{
			g_cpu_jif = xrealloc_vector(g_cpu_jif, 1, g_num_cpus);
			if (read_cpu_jiffy(fp, &g_cpu_jif[g_num_cpus]) <= 4)
				break;
			g_num_cpus++;
		}	
	}
	fclose(fp);
	return 0;
}

/*************************************************
Function: display_cpus
Description: accord /proc/stat to calculate cpu's usage
Calls: 
Input: 
Output: 
*************************************************/
static void display_cpus()
{
	unsigned total_diff;
	jiffy_counts_t *local_pjif = &g_cur_jif;
	jiffy_counts_t *local_prev_pjif = &g_prev_jif;

# define  CALC_TOTAL_DIFF do { \
	total_diff = (unsigned)(local_pjif->total - local_prev_pjif->total); \
	if (total_diff == 0) total_diff = 1; \
} while (0)

#  define CALC_STAT(xxx) unsigned xxx = 100 * (unsigned)(local_pjif->xxx - local_prev_pjif->xxx) / total_diff
#  define SHOW_STAT(xxx) xxx
#  define FMT "%4u%% "

	{
		CALC_TOTAL_DIFF;

		//printf("total_diff(%u)\n", total_diff);
		{
			CALC_STAT(usr);
			CALC_STAT(sys);
			CALC_STAT(nic);
			CALC_STAT(idle);
			CALC_STAT(iowait);
			CALC_STAT(irq);
			CALC_STAT(softirq);
			
			g_cur_cpu_usage.cpu_us = usr;
			g_cur_cpu_usage.cpu_sy = sys;
			g_cur_cpu_usage.cpu_ni = nic;
			g_cur_cpu_usage.cpu_id = idle;
			g_cur_cpu_usage.cpu_wa = iowait;
			g_cur_cpu_usage.cpu_hi = irq;
			g_cur_cpu_usage.cpu_si = softirq;

			/*printf(
				"CPU:"FMT"usr"FMT"sys"FMT"nic"FMT"idle"FMT"io"FMT"irq"FMT"sirq\n",
				SHOW_STAT(usr), SHOW_STAT(sys), SHOW_STAT(nic), SHOW_STAT(idle),
				SHOW_STAT(iowait), SHOW_STAT(irq), SHOW_STAT(softirq)
			);*/
		}
	}
# undef SHOW_STAT
# undef CALC_STAT
# undef FMT
}

/*************************************************
Function: get_basename
Description: filter string to get a base name
Input: progress's abs path
Output: progress's base name
*************************************************/
char *get_basename(const char *path)
{
    register const char *s;
    register const char *pp;

    pp = s = path;

    while (*s) 
    {
        if (*s++ == '/') 
        {
            pp = s;
        }
    }

    return (char *) pp;
}

/*************************************************
Function: get_pid_by_name
Description: accord progress's name to get its pid
Calls: 
	char *get_basename(const char *path)
Input: progress's name
Output: progress's  pid
*************************************************/
int get_pid_by_name(const char *process_name, pid_t pid_list[], int list_size)
{
    DIR *dir;
    struct dirent *next;
    int count = 0;
    pid_t pid;
    FILE *fp;
    char *base_pname = NULL;
    char base_fname[MAX_BUF_SIZE];
    char cmdline[MAX_BUF_SIZE];
    char path[MAX_BUF_SIZE];

    if(process_name == NULL || pid_list == NULL)
        return -EINVAL;

    base_pname = get_basename(process_name);
    if(strlen(base_pname) <= 0)
        return -EINVAL;

    dir = opendir("/proc");
    if (NULL == dir)//(!dir)
    {
        return -EIO;
    }
    while ((next = readdir(dir)) != NULL) 
    {
        /* skip non-number */
        if (!isdigit(*next->d_name))
            continue;

        pid = strtol(next->d_name, NULL, 0);
        sprintf(path, "/proc/%u/status", pid);//change from cmdline
		fp = fopen(path, "r");
        if(fp == NULL)
            continue;

        memset(cmdline, 0, sizeof(cmdline));
		if(fgets(cmdline, sizeof(cmdline), fp) > 0)//get message from the file opend before whose file pointer is fp
		{
			cmdline[strlen(cmdline) - 1] = '\0';
			if (strstr(cmdline, "Name:") != NULL)//find Name: from line
			{
				strcpy(base_fname, (char *)cmdline + 6);
			}
		}        
        fclose(fp);
        
        if (strcmp(base_fname, base_pname) == 0 )
        {
            if(count >= list_size)
            {
                break;
            }
            else
            {
                pid_list[count] = pid;
                count++;
            }
        }
    }
    closedir(dir) ;
    return count;
}

/*************************************************
Function: is_process_exist
Description: check whether progress exist
Calls: 
	int get_pid_by_name(const char *process_name, pid_t pid_list[], int list_size)
Input: const char *process_name---progress's name
Output:
return:
	0---progress is exist
	-1---progress is not exist
*************************************************/
int is_process_exist(const char *process_name)
{
    pid_t pid;

    return (get_pid_by_name(process_name, &pid, 1) > 0);
}

/*************************************************
Function: parse_pidstat
Description: open /proc/pid/stat, parse the content and store in pid_cpu_stat[PID_STAT_MAX]
Input: progress pid
Output: unsigned long long pid_cpu_stat[PID_STAT_MAX]
Return:
	0   function run success
	-1  function run error
*************************************************/
static int parse_pidstat(pid_t pid, unsigned long long pid_cpu_stat[PID_STAT_MAX])
{
    char buf[600];
    char path[200];
    FILE *f;
    int i = 0;

    memset(pid_cpu_stat, 0, sizeof(pid_cpu_stat[0]) * PID_STAT_MAX);
    snprintf(path, sizeof(path), "%s%u%s","/proc/", pid, "/stat");
    f = xfopen_for_read(path);
	if (f == NULL)
		return -1;
    while (fgets(buf, sizeof(buf), f) != NULL) 
    {
        //printf("buf=%s\n",buf);//for debug
        char *tmp = buf;
        char *p[PID_STAT_MAX];
        while ((p[i] = strtok(tmp, " ")) != NULL) 
        {
            //printf("p[%d]=%s\n", i, p[i]); //for debug
            pid_cpu_stat[i] = atof(p[i]);
            
            //printf("pid_cpu_stat[%d]=%llu\n", i, pid_cpu_stat[i]);
            i++;
            tmp = NULL;
        }
    }
    fclose(f);
    return 0;
}

/*************************************************
Function: sys_check_cpu_sched
Description: check cpu's idle precent
Calls: static void parse_loadavg(float cpuloadavg[CPU_LOADAVG_MAX])
Input: float *load  used to save current cpu's idle precent
Output: current cpu's idle precent
Return:
	0   function run success
	-1  function run error
*************************************************/
int sys_check_cpu_sched (float *load)
{
	int ret;
	if (load == NULL)
        return -EINVAL;

	float cpuloadavg[CPU_LOADAVG_MAX];
	ret = parse_loadavg(cpuloadavg);
	if (ret < 0 )
		return -1;
	*load = g_cur_cpuload.cpu_load_15min;
	
	//printf("cpuloadavg[CPU_LOADAVG_1MINS]=%lu cpuloadavg[CPU_LOADAVG_5MINS]=%lu cpuloadavg[CPU_LOADAVG_15MINS]=%lu\n",
			//cpuloadavg[CPU_LOADAVG_1MINS],cpuloadavg[CPU_LOADAVG_5MINS],cpuloadavg[CPU_LOADAVG_15MINS]);
	return 0;
}

/*************************************************
Function: sys_check_cpu_usage
Description: check cpu's usage precent
Calls: 
	static int get_jiffy_counts(jiffy_counts_t *p_jif)
	static void display_cpus()
Input: float *idle  used to save current cpu's usage precent
Output: current cpu's usage precent
Return:
	0   function run success
	-1  function run error
*************************************************/
int sys_check_cpu_usage (float *kernel, float *user)
{
	int ret;
	
	if (kernel == NULL || user == NULL)
        return -EINVAL;

	ret = get_jiffy_counts(&g_prev_jif);
	if (ret < 0)
		return -1;
	usleep(300000);
	ret = get_jiffy_counts(&g_cur_jif);
	if (ret < 0)
		return -1;
	display_cpus();
	*kernel = g_cur_cpu_usage.cpu_sy;
	*user = g_cur_cpu_usage.cpu_us;
	return 0;
}

/*************************************************
Function: sys_check_cpu_process
Description: check a progress take how many cpu's usage precent
Calls: 
	int is_process_exist(const char *process_name)
	int get_pid_by_name(const char *process_name, pid_t pid_list[], int list_size)
	static void parse_pidstat(pid_t pid, unsigned long long pid_cpu_stat[PID_STAT_MAX])
	static int get_jiffy_counts(jiffy_counts_t *p_jif)
	static void display_cpus()
Input: 
	char *name---process抯 name	
	int interval---time interval between 2 take sample(unit:a millisecond),
	 default is 1200000(1.2 second) if you set interval = 0, you can change it as you wish,
	 but not bigger than 5000000.
Unit for millisecond
Output: float *usage---cpu usage precent of individual process
Return: 
	0   function run success
	-1  function run error
*************************************************/
int sys_check_cpu_process (char *name, float *usage, int interval)
{
	int ret = 0;  
	int n;  
	pid_t pid[MAX_PID_NUM];  

	if (name == NULL || usage == NULL)
        return -EINVAL;

	if ((interval < 0) || (interval > 5000001))
	{
		printf("sample interval time argument is illegal\n");
		return -1;
	}

	ret = is_process_exist(name); 
	if (ret < 1)
	{
		printf("process '%s' is not exist!\n",name);
		return -1;
	}

	ret = get_pid_by_name(name, pid, MAX_PID_NUM);  
	
	//printf("process '%s' is existed? (%d): %c\n", name, ret, (ret > 0) ? 'y' : 'n');
	for(n = 0; n < ret; n++)
	{   //maybe there are servel same progress running, but function only return one usage precent, so lazy here...
		//printf("pid:%u\n", pid[n]);
	}

	ret = get_num_cpus();
	if (ret < 0)
		return -1;

	ret = parse_pidstat(pid[0], g_prev_pid_cpu_stat);
	if (ret < 0)
		return -1;
	ret = get_jiffy_counts(&g_prev_jif);
	if (ret < 0)
		return -1;

	if (0 == interval)
		usleep(1200000);
	else
		usleep(interval);

	ret = parse_pidstat(pid[0], g_cur_pid_cpu_stat);
	if (ret < 0)
		return -1;
	ret = get_jiffy_counts(&g_cur_jif);
	if (ret < 0)
		return -1;

{
	unsigned total_pid_diff;
	unsigned pid_jif_total;
	unsigned prev_pid_jif_total;
	# define  CALC_TOTAL_PID_DIFF do { \
		total_pid_diff = (unsigned)(pid_jif_total - prev_pid_jif_total); \
		if (total_pid_diff == 0) total_pid_diff = 1; \
	} while (0)

	{
		prev_pid_jif_total = g_prev_pid_cpu_stat[UTIME] + g_prev_pid_cpu_stat[STIME];
							 
							 //+ g_prev_pid_cpu_stat[CUTIME] + g_prev_pid_cpu_stat[CSTIME];

		/*printf("prev_pid[UTIME](%llu) prev_pid[STIME](%llu) prev_pid[CUTIME](%llu) prev_pid_[CSTIME](%llu) \n", 
				g_prev_pid_cpu_stat[UTIME], g_prev_pid_cpu_stat[STIME], g_prev_pid_cpu_stat[CUTIME], g_prev_pid_cpu_stat[CSTIME]);*/
		pid_jif_total = g_cur_pid_cpu_stat[UTIME] + g_cur_pid_cpu_stat[STIME];
						
						//+ g_cur_pid_cpu_stat[CUTIME] + g_cur_pid_cpu_stat[CSTIME];

		/*printf("cur_pid[UTIME](%llu) cur_pid[STIME](%llu) cur_pid[CUTIME](%llu) cur_pid[CSTIME](%llu) \n", 
				g_cur_pid_cpu_stat[UTIME], g_cur_pid_cpu_stat[STIME], g_cur_pid_cpu_stat[CUTIME], g_cur_pid_cpu_stat[CSTIME]);*/
		CALC_TOTAL_PID_DIFF;
	}

	*usage = 100 * (float)(total_pid_diff) / (g_cur_jif.total - g_prev_jif.total) * g_num_cpus;

/*	printf("process time prev(%u) cur(%u) total_diff(%u) cpu_total(%llu)\n", 
	prev_pid_jif_total, pid_jif_total, total_pid_diff, (g_cur_jif.total - g_prev_jif.total));*/
# undef CALC_TOTAL_PID_DIFF
}
	return ret;  
}


#define TEST_COUNT 100

int main(int argc, char *argv[]) 
{
	int ret = -1;
	int i;
	float load;

	struct  timeval t1;
	struct  timeval t2;
	struct  timeval t3;
	struct  timeval t4;
	float v1,v2,v3;
	
	float kernel,usr;
	char* process;
    //float usage=0.0;
	float usage=0;

    if(argc < 2)  
	    process = argv[0];  
	else  
	    process = argv[1];  
	

/*	ret = get_num_cpus();
	if (ret == 0)
		printf("cpu number is %d\n",g_num_cpus);*/
/*	gettimeofday(&t1, NULL);
	for (i = 0; i < TEST_COUNT; i++)
	{
		ret = sys_check_cpu_sched (&load);
	   	if (ret == 0)
			printf("cpu 15 mins load=%f\n", load);
	}
	gettimeofday(&t2, NULL);
*/
/*	for (i = 0; i < TEST_COUNT; i++)
	{
		ret = sys_check_cpu_usage (&kernel, &usr);
    	if (ret == 0)
			printf("cpu USR[%f] SYS[%f] \n", usr, kernel);
	}
    gettimeofday(&t3, NULL);
*/
	for (i = 0; i < TEST_COUNT; i++)
	{
		ret = sys_check_cpu_process (process, &usage, 5000000);
	    if (ret == 0)
			printf("pid cpu usage = %f\n", usage);
	}
	gettimeofday(&t4, NULL);

/*	v1 = (float)((t2.tv_sec * 1000000 + t2.tv_usec) - (t1.tv_sec * 1000000 + t1.tv_usec)) / TEST_COUNT;
	printf("v1=%f\n", v1);*/
/*	v2 = (float)((t3.tv_sec * 1000000 + t3.tv_usec) - (t2.tv_sec * 1000000 + t2.tv_usec)) / TEST_COUNT;
	printf("v2=%f\n", v2);*/
	v3 = (float)((t4.tv_sec * 1000000 + t4.tv_usec) - (t3.tv_sec * 1000000 + t3.tv_usec)) / TEST_COUNT;
	printf("v3=%f\n", v3);
    return 0;
}


#if 0
int main(int argc, char *argv[]) 
{
	int ret = -1;
	//get_num_cpus();
	//printf("g_num_cpus=%d\n",g_num_cpus);

	float load;
    ret = sys_check_cpu_sched (&load);
    if (ret == 0)
		printf("cpu 15 mins load=%f\n", load);

	float kernel,usr;
    ret = sys_check_cpu_usage (&kernel, &usr);
    if (ret == 0)
		printf("cpu sys=%f usr=%f\n", kernel, usr);

	char* process;
    float usage=0.0;

    if(argc < 2)  
            process = argv[0];  
    else  
            process = argv[1];  

    ret = sys_check_cpu_process (process, &usage);
    if (ret == 0)
		printf("pid cpu usage = %f\n", usage);
    return 0;
}
#endif
