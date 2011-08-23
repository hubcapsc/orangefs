/* 
 * (C) 2011 Clemson University and The University of Chicago 
 *
 * See COPYING in top-level directory.
 */

/** \file
 *  \ingroup usrint
 *
 *  PVFS2 user interface routines
 */
#ifndef USRINT_H
#define USRINT_H 1

#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define __USE_MISC 1
#define __USE_ATFILE 1
#define __USE_GNU 1

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <libgen.h>
#include <string.h>
#include <stdarg.h>
#include <memory.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
/* #include <sys/statvfs.h> */ /* struct statfs on OS X */
#include <sys/vfs.h> /* struct statfs on Linux */
#include <sys/stat.h>
#include <sys/uio.h>
#include <attr/xattr.h>
#include <linux/types.h>
/* #include <linux/dirent.h> diff source need diff versions */
#include <time.h>
#include <dlfcn.h>

//#include <mpi.h>

/* PVFS specific includes */
#include <pvfs2.h>
#include <pvfs2-hint.h>


/* magic numbers for PVFS filesystem */
#define PVFS_FS 537068840
#define LINUX_FS 61267

#define PVFS_FD_SUCCESS 0
#define PVFS_FD_FAILURE -1

/* Defines GNU's O_NOFOLLOW flag to be false if its not set */ 
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#define true   1 
#define false  0 
#define O_HINTS     02000000  /* PVFS hints are present */
#define O_NOTPVFS   04000000  /* Open non-PVFS files if possible */

/* constants for this library */
/* size of stdio default buffer - starting at 1Meg */
#define PVFS_BUFSIZE (1024*1024)
#define PVFS_PATH_MAX 1024

/* extra function prototypes */
int fseek64(FILE *stream, const off64_t offset, int whence);

off64_t ftell64(FILE *stream);

int pvfs_convert_iovec(const struct iovec *vector,
                       int count,
                       PVFS_Request *req,
                       void **buf);

/* MPI functions */ 
//int MPI_File_open(MPI_Comm comm, char *filename,
//                    int amode, MPI_Info info, MPI_File *mpi_fh); 
//int MPI_File_write(MPI_File mpi_fh, void *buf,
//                    int count, MPI_Datatype datatype, MPI_Status *status); 

/* Macros */

/* debugging */

//#define USRINT_DEBUG
#ifdef  USRINT_DEBUG
#define debug(s,v) fprintf(stderr,s,v)
#else
#define debug(s,v)
#endif

/* FD sets */

#ifdef FD_SET
#undef FD_SET
#endif
#define FD_SET(d,fdset)                 \
do {                                    \
    pvfs_descriptor *pd;                \
    pd = pvfs_find_descriptor(d);       \
    if (pd)                             \
    {                                   \
        __FD_SET(pd->true_fd,(fdset));  \
    }                                   \
} while(0)

#ifdef FD_CLR
#undef FD_CLR
#endif
#define FD_CLR(d,fdset)                 \
do {                                    \
    pvfs_descriptor *pd;                \
    pd = pvfs_find_descriptor(d);       \
    if (pd)                             \
    {                                   \
        __FD_CLR(pd->true_fd,(fdset));  \
    }                                   \
} while(0)

#ifdef FD_ISSET
#undef FD_ISSET
#endif
#define FD_ISSET(d,fdset)               \
do {                                    \
    pvfs_descriptor *pd;                \
    pd = pvfs_find_descriptor(d);       \
    if (pd)                             \
    {                                   \
        __FD_ISSET(pd->true_fd,(fdset));\
    }                                   \
} while(0)


//typedef uint64_t off64_t;

#endif

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
