/*
 * Copyright (C) 2014, 2015 netblue30 (netblue30@yahoo.com)
 *
 * This file is part of firejail project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

/* default seccomp filter
	// seccomp
	struct sock_filter filter[] = {
		VALIDATE_ARCHITECTURE,
		EXAMINE_SYSCALL,
		BLACKLIST(SYS_mount),  // mount/unmount filesystems
		BLACKLIST(SYS_umount2),
		BLACKLIST(SYS_ptrace), // trace processes
		BLACKLIST(SYS_kexec_load), // loading a different kernel
		BLACKLIST(SYS_open_by_handle_at), // open by handle
		BLACKLIST(SYS_init_module), // kernel module handling
#ifdef SYS_finit_module // introduced in 2013
		BLACKLIST(SYS_finit_module),
#endif
		BLACKLIST(SYS_delete_module),
		BLACKLIST(SYS_iopl), // io permisions
#ifdef SYS_ioperm
		BLACKLIST(SYS_ioperm),
#endif
SYS_iopl
		BLACKLIST(SYS_iopl), // io permisions
#endif
#ifdef SYS_ni_syscall), // new io permisions call on arm devices
		BLACKLIST(SYS_ni_syscall),
#endif
		BLACKLIST(SYS_swapon), // swap on/off
		BLACKLIST(SYS_swapoff),
		BLACKLIST(SYS_syslog), // kernel printk control
		RETURN_ALLOW
	};
*/
#ifdef HAVE_SECCOMP
#include "firejail.h"
#include <errno.h>
#include <linux/filter.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <linux/audit.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/prctl.h>
#ifndef PR_SET_NO_NEW_PRIVS
# define PR_SET_NO_NEW_PRIVS 38
#endif

#if HAVE_SECCOMP_H
#include <linux/seccomp.h>
#else
#define SECCOMP_MODE_FILTER	2
#define SECCOMP_RET_KILL	0x00000000U
#define SECCOMP_RET_TRAP	0x00030000U
#define SECCOMP_RET_ALLOW	0x7fff0000U
#define SECCOMP_RET_ERRNO	0x00050000U
#define SECCOMP_RET_DATA        0x0000ffffU
struct seccomp_data {
    int nr;
    __u32 arch;
    __u64 instruction_pointer;
    __u64 args[6];
};
#endif

#if defined(__i386__)
# define ARCH_NR	AUDIT_ARCH_I386
#elif defined(__x86_64__)
# define ARCH_NR	AUDIT_ARCH_X86_64
#elif defined(__arm__)
# define ARCH_NR	AUDIT_ARCH_ARM
#else
# warning "Platform does not support seccomp filter yet"
# define ARCH_NR	0
#endif


#define VALIDATE_ARCHITECTURE \
     BPF_STMT(BPF_LD+BPF_W+BPF_ABS, (offsetof(struct seccomp_data, arch))), \
     BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, ARCH_NR, 1, 0), \
     BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_KILL)

#define EXAMINE_SYSCALL BPF_STMT(BPF_LD+BPF_W+BPF_ABS,	\
		 (offsetof(struct seccomp_data, nr)))

#define BLACKLIST(syscall_nr)	\
	BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, syscall_nr, 0, 1),	\
	BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_KILL)

#define WHITELIST(syscall_nr) \
	BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, syscall_nr, 0, 1), \
	BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW)

#define RETURN_ALLOW \
	BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW)

#define KILL_PROCESS \
	BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_KILL)

#define SECSIZE 128 // initial filter size
static struct sock_filter *sfilter = NULL;
static int sfilter_alloc_size = 0;
static int sfilter_index = 0;

// debug filter
void filter_debug(void) {
	// start filter
	struct sock_filter filter[] = {
		VALIDATE_ARCHITECTURE,
		EXAMINE_SYSCALL
	};

	// print sizes
	printf("SECCOMP Filter:\n");
	if (sfilter == NULL) {
		printf("SECCOMP filter not allocated\n");
		return;
	}
	if (sfilter_index < 4)
		return;
	
	// test the start of the filter
	if (memcmp(sfilter, filter, sizeof(filter)) == 0) {
		printf("  VALIDATE_ARCHITECTURE\n");
		printf("  EXAMINE_SYSCAL\n");
	}
	
	// loop trough blacklists
	int i = 4;
	while (i < sfilter_index) {
		// minimal parsing!
		unsigned char *ptr = (unsigned char *) &sfilter[i];
		int *nr = (int *) (ptr + 4);
		if (*ptr	== 0x15 && *(ptr +14) == 0xff && *(ptr + 15) == 0x7f ) {
			printf("  WHITELIST %d %s\n", *nr, syscall_find_nr(*nr));
			i += 2;
		}
		else if (*ptr	== 0x15 && *(ptr +14) == 0 && *(ptr + 15) == 0) {
			printf("  BLACKLIST %d %s\n", *nr, syscall_find_nr(*nr));
			i += 2;
		}
		else if (*ptr == 0x06 && *(ptr +6) == 0 && *(ptr + 7) == 0 ) {
			printf("  KILL_PROCESS\n");
			i++;
		}
		else if (*ptr == 0x06 && *(ptr +6) == 0xff && *(ptr + 7) == 0x7f ) {
			printf("  RETURN_ALLOW\n");
			i++;
		}
		else {
			printf("  UNKNOWN ENTRY!!!\n");
			i++;
		}
	}
}

// initialize filter
static void filter_init(void) {
	if (sfilter) {
		assert(0);
		return;
	}

	if (arg_debug)
		printf("Initialize seccomp filter\n");	
	// allocate a filter of SECSIZE
	sfilter = malloc(sizeof(struct sock_filter) * SECSIZE);
	if (!sfilter)
		errExit("malloc");
	memset(sfilter, 0, sizeof(struct sock_filter) * SECSIZE);
	sfilter_alloc_size = SECSIZE;
	
	// copy the start entries
	struct sock_filter filter[] = {
		VALIDATE_ARCHITECTURE,
		EXAMINE_SYSCALL
	};
	sfilter_index = sizeof(filter) / sizeof(struct sock_filter);	
	memcpy(sfilter, filter, sizeof(filter));
}

static void filter_realloc(void) {
	assert(sfilter);
	assert(sfilter_alloc_size);
	assert(sfilter_index);
	if (arg_debug)
		printf("Allocating more seccomp filter entries\n");
	
	// allocate the new memory
	struct sock_filter *old = sfilter;
	sfilter = malloc(sizeof(struct sock_filter) * (sfilter_alloc_size + SECSIZE));
	if (!sfilter)
		errExit("malloc");
	memset(sfilter, 0, sizeof(struct sock_filter) *  (sfilter_alloc_size + SECSIZE));
	
	// copy old filter
	memcpy(sfilter, old, sizeof(struct sock_filter) *  sfilter_alloc_size);
	sfilter_alloc_size += SECSIZE;
}

static void filter_add_whitelist(int syscall) {
	assert(sfilter);
	assert(sfilter_alloc_size);
	assert(sfilter_index);
	if (arg_debug)
		printf("Whitelisting syscall %d %s\n", syscall, syscall_find_nr(syscall));
	
	if ((sfilter_index + 2) > sfilter_alloc_size)
		filter_realloc();
	
	struct sock_filter filter[] = {
		WHITELIST(syscall)
	};
#if 0
{
	int i;
	unsigned char *ptr = (unsigned char *) &filter[0];
	for (i = 0; i < sizeof(filter); i++, ptr++)
		printf("%x, ", (*ptr) & 0xff);
	printf("\n");
}
#endif
	memcpy(&sfilter[sfilter_index], filter, sizeof(filter));
	sfilter_index += sizeof(filter) / sizeof(struct sock_filter);	
}

static void filter_add_blacklist(int syscall) {
	assert(sfilter);
	assert(sfilter_alloc_size);
	assert(sfilter_index);
	if (arg_debug)
		printf("Blacklisting syscall %d %s\n", syscall, syscall_find_nr(syscall));
	
	if ((sfilter_index + 2) > sfilter_alloc_size)
		filter_realloc();
	
	struct sock_filter filter[] = {
		BLACKLIST(syscall)
	};
#if 0
{
	int i;
	unsigned char *ptr = (unsigned char *) &filter[0];
	for (i = 0; i < sizeof(filter); i++, ptr++)
		printf("%x, ", (*ptr) & 0xff);
	printf("\n");
}
#endif
	memcpy(&sfilter[sfilter_index], filter, sizeof(filter));
	sfilter_index += sizeof(filter) / sizeof(struct sock_filter);	
}

static void filter_end_blacklist(void) {
	assert(sfilter);
	assert(sfilter_alloc_size);
	assert(sfilter_index);
	if (arg_debug)
		printf("Ending syscall filter\n");

	if ((sfilter_index + 2) > sfilter_alloc_size)
		filter_realloc();
	
	struct sock_filter filter[] = {
		RETURN_ALLOW
	};
#if 0	
{
	int i;
	unsigned char *ptr = (unsigned char *) &filter[0];
	for (i = 0; i < sizeof(filter); i++, ptr++)
		printf("%x, ", (*ptr) & 0xff);
	printf("\n");
}
#endif
	memcpy(&sfilter[sfilter_index], filter, sizeof(filter));
	sfilter_index += sizeof(filter) / sizeof(struct sock_filter);	
}

static void filter_end_whitelist(void) {
	assert(sfilter);
	assert(sfilter_alloc_size);
	assert(sfilter_index);
	if (arg_debug)
		printf("Ending syscall filter\n");

	if ((sfilter_index + 2) > sfilter_alloc_size)
		filter_realloc();
	
	struct sock_filter filter[] = {
		KILL_PROCESS
	};
#if 0	
{
	int i;
	unsigned char *ptr = (unsigned char *) &filter[0];
	for (i = 0; i < sizeof(filter); i++, ptr++)
		printf("%x, ", (*ptr) & 0xff);
	printf("\n");
}
#endif
	memcpy(&sfilter[sfilter_index], filter, sizeof(filter));
	sfilter_index += sizeof(filter) / sizeof(struct sock_filter);	
}


// save seccomp filter in  /tmp/firejail/mnt/seccomp
static void write_seccomp_file(void) {
	fs_build_mnt_dir();
	assert(sfilter);

	char *fname;
	if (asprintf(&fname, "%s/seccomp", MNT_DIR) == -1)
		errExit("asprintf");
	int fd = open(fname, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
	if (fd == -1)
		errExit("open");

	if (arg_debug)
		printf("Save seccomp filter, size %lu bytes\n", sfilter_index * sizeof(struct sock_filter));
	errno = 0;
	ssize_t sz = write(fd, sfilter, sfilter_index * sizeof(struct sock_filter));
	if (sz != (sfilter_index * sizeof(struct sock_filter))) {
		fprintf(stderr, "Error: cannot save seccomp filter\n");
		exit(1);
	}
	close(fd);
	if (chown(fname, 0, 0) < 0)
		errExit("chown");
	free(fname);
}

// read seccomp filter from /tmp/firejail/mnt/seccomp
static void read_seccomp_file(char *file_name) {
	assert(sfilter == NULL && sfilter_index == 0);

	char *fname;
	if (file_name)
		fname = file_name;
	else {
		if (asprintf(&fname, "%s/seccomp", MNT_DIR) == -1)
			errExit("asprintf");
	}
		
	// check file
	struct stat s;
	if (stat(fname, &s) == -1) {
		fprintf(stderr, "Error: seccomp file not found\n");
		exit(1);
	}
	ssize_t sz = s.st_size;
	if (sz == 0 || (sz % sizeof(struct sock_filter)) != 0) {
		fprintf(stderr, "Error: invalid seccomp file\n");
		exit(1);
	}
	sfilter = malloc(sz);
	if (!sfilter)
		errExit("malloc");
		
	// read file
	/* coverity[toctou] */
	int fd = open(fname,O_RDONLY);
	if (fd == -1)
		errExit("open");
	errno = 0;		
	ssize_t size = read(fd, sfilter, sz);
	if (size != sz) {
		fprintf(stderr, "Error: invalid seccomp file\n");
		exit(1);
	}
	sfilter_index = sz / sizeof(struct sock_filter);

	if (arg_debug)
		printf("Read seccomp filter, size %lu bytes\n", sfilter_index * sizeof(struct sock_filter));

	close(fd);
	free(fname);
	
	if (arg_debug)
		filter_debug();
}


// drop filter for seccomp option
int seccomp_filter_drop(void) {
	filter_init();
	
	// default seccomp
	if (arg_seccomp_list_drop == NULL) {
#ifdef SYS_mount		
		filter_add_blacklist(SYS_mount);
#endif
#ifdef SYS_umount2		
		filter_add_blacklist(SYS_umount2);
#endif
#ifdef SYS_ptrace 		
		filter_add_blacklist(SYS_ptrace);
#endif
#ifdef SYS_kexec_load		
		filter_add_blacklist(SYS_kexec_load);
#endif
#ifdef SYS_open_by_handle_at		
		filter_add_blacklist(SYS_open_by_handle_at);
#endif
#ifdef SYS_init_module		
		filter_add_blacklist(SYS_init_module);
#endif
#ifdef SYS_finit_module // introduced in 2013
		filter_add_blacklist(SYS_finit_module);
#endif
#ifdef SYS_delete_module		
		filter_add_blacklist(SYS_delete_module);
#endif
#ifdef SYS_iopl		
		filter_add_blacklist(SYS_iopl);
#endif
#ifdef 	SYS_ioperm	
		filter_add_blacklist(SYS_ioperm);
#endif
#ifdef SYS_ni_syscall // new io permisions call on arm devices
		filter_add_blacklist(SYS_ni_syscall);
#endif
#ifdef SYS_swapon		
		filter_add_blacklist(SYS_swapon);
#endif
#ifdef SYS_swapoff		
		filter_add_blacklist(SYS_swapoff);
#endif
#ifdef SYS_syslog		
		filter_add_blacklist(SYS_syslog);
#endif
#ifdef SYS_process_vm_readv		
		filter_add_blacklist(SYS_process_vm_readv);
#endif
#ifdef SYS_process_vm_writev		
		filter_add_blacklist(SYS_process_vm_writev);
#endif

// mknod removed in 0.9.29
//#ifdef SYS_mknod		
//		filter_add_blacklist(SYS_mknod);
//#endif
		
		// new syscalls in 0.9,23		
#ifdef SYS_sysfs		
		filter_add_blacklist(SYS_sysfs);
#endif
#ifdef SYS__sysctl	
		filter_add_blacklist(SYS__sysctl);
#endif
#ifdef SYS_adjtimex		
		filter_add_blacklist(SYS_adjtimex);
#endif
#ifdef 	SYS_clock_adjtime	
		filter_add_blacklist(SYS_clock_adjtime);
#endif
#ifdef SYS_lookup_dcookie		
		filter_add_blacklist(SYS_lookup_dcookie);
#endif
#ifdef 	SYS_perf_event_open	
		filter_add_blacklist(SYS_perf_event_open);
#endif
#ifdef	SYS_fanotify_init 	
		filter_add_blacklist(SYS_fanotify_init);
#endif
#ifdef SYS_kcmp
		filter_add_blacklist(SYS_kcmp);
#endif
	}

	// default seccomp filter with additional drop list
	if (arg_seccomp_list && arg_seccomp_list_drop == NULL) {
		if (syscall_check_list(arg_seccomp_list, filter_add_blacklist)) {
			fprintf(stderr, "Error: cannot load seccomp filter\n");
			exit(1);
		}
	}
	// drop list
	else if (arg_seccomp_list == NULL && arg_seccomp_list_drop) {
		if (syscall_check_list(arg_seccomp_list_drop, filter_add_blacklist)) {
			fprintf(stderr, "Error: cannot load seccomp filter\n");
			exit(1);
		}
	}
	
	
	filter_end_blacklist();
	if (arg_debug)
		filter_debug();

	// save seccomp filter in  /tmp/firejail/mnt/seccomp
	// in order to use it in --join operations
	write_seccomp_file();


	struct sock_fprog prog = {
		.len = sfilter_index,
		.filter = sfilter,
	};

	if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
		fprintf(stderr, "Warning: seccomp disabled, it requires a Linux kernel version 3.5 or newer.\n");
		return 1;
	}
	else if (arg_debug) {
		printf("seccomp enabled\n");
	}
	
	return 0;
}

// keep filter for seccomp option
int seccomp_filter_keep(void) {
	filter_init();

	// these 4 syscalls are used by firejail after the seccomp filter is initialized
	filter_add_whitelist(SYS_setuid);
	filter_add_whitelist(SYS_setgid);
	filter_add_whitelist(SYS_setgroups);
	filter_add_whitelist(SYS_dup);
	
	// apply keep list
	if (arg_seccomp_list_keep) {
		if (syscall_check_list(arg_seccomp_list_keep, filter_add_whitelist)) {
			fprintf(stderr, "Error: cannot load seccomp filter\n");
			exit(1);
		}
	}
	
	filter_end_whitelist();
	if (arg_debug)
		filter_debug();

	// save seccomp filter in  /tmp/firejail/mnt/seccomp
	// in order to use it in --join operations
	write_seccomp_file();


	struct sock_fprog prog = {
		.len = sfilter_index,
		.filter = sfilter,
	};

	if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
		fprintf(stderr, "Warning: seccomp disabled, it requires a Linux kernel version 3.5 or newer.\n");
		return 1;
	}
	else if (arg_debug) {
		printf("seccomp enabled\n");
	}
	
	return 0;
}



void seccomp_set(void) {
	// read seccomp filter from  /tmp/firejail/mnt/seccomp
	read_seccomp_file(NULL);
	
	// apply filter
	struct sock_fprog prog = {
		.len = sfilter_index,
		.filter = sfilter,
	};
	
	if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
		fprintf(stderr, "Warning: seccomp disabled, it requires a Linux kernel version 3.5 or newer.\n");
		return;
	}
	else if (arg_debug) {
		printf("seccomp enabled\n");
	}
}

void seccomp_print_filter_name(const char *name) {
	if (!name || strlen(name) == 0) {
		fprintf(stderr, "Error: invalid sandbox name\n");
		exit(1);
	}
	pid_t pid;
	if (name2pid(name, &pid)) {
		fprintf(stderr, "Error: cannot find sandbox %s\n", name);
		exit(1);
	}

	seccomp_print_filter(pid);
}

void seccomp_print_filter(pid_t pid) {
	// if the pid is that of a firejail  process, use the pid of the first child process
	char *comm = pid_proc_comm(pid);
	if (comm) {
		// remove \n
		char *ptr = strchr(comm, '\n');
		if (ptr)
			*ptr = '\0';
		if (strcmp(comm, "firejail") == 0) {
			pid_t child;
			if (find_child(pid, &child) == 0) {
				pid = child;
			}
		}
		free(comm);
	}

	// check privileges for non-root users
	uid_t uid = getuid();
	if (uid != 0) {
		struct stat s;
		char *dir;
		if (asprintf(&dir, "/proc/%u/ns", pid) == -1)
			errExit("asprintf");
		if (stat(dir, &s) < 0)
			errExit("stat");
		if (s.st_uid != uid) {
			printf("Error: permission denied.\n");
			exit(1);
		}
	}


	// find the seccomp filter
	char *fname;
	if (asprintf(&fname, "/proc/%d/root/tmp/firejail/mnt/seccomp", pid) == -1)
		errExit("asprintf");

	struct stat s;
	if (stat(fname, &s) == -1) {
		printf("Cannot access seccomp filter.\n");
		exit(1);
	}

	// read and print the filter
	read_seccomp_file(fname);
	drop_privs(1);
	filter_debug();

	exit(0);
}

#endif // HAVE_SECCOMP

