/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

#include "acache.h"
#include "ncache.h"
#include "pint-bucket.h"
#include "pvfs2-sysint.h"
#include "pint-sysint-utils.h"
#include "gen-locks.h"
#include "pint-servreq.h"
#include "PINT-reqproto-encode.h"
#include "dotconf.h"
#include "trove.h"
#include "server-config.h"
#include "client-state-machine.h"

gen_mutex_t mt_config = GEN_MUTEX_INITIALIZER;

/* PVFS_sys_fs_add()
 *
 * tells the system interface to dynamically "mount" a new file system
 *
 * returns 0 on success, -PVFS_error on failure
 */
int PVFS_sys_fs_add(struct PVFS_sys_mntent* mntent)
{
    int ret = -1;
    struct filesystem_configuration_s* cur_fs = NULL;

    gen_mutex_lock(&mt_config);

    /* Get configuration parameters from server */
    ret = PINT_server_get_config(PINT_get_server_config_struct(),
                                 mntent);
    if (ret < 0)
    {
	gen_mutex_unlock(&mt_config);
	return(ret);
    }

    cur_fs = PINT_config_find_fs_name(PINT_get_server_config_struct(), 
	mntent->pvfs_fs_name);
    /* it should not be possible for this to fail after a successful call to
     * PINT_server_get_config()
     */
    assert(cur_fs);

    /* load the mapping of handles to servers */
    ret = PINT_handle_load_mapping(PINT_get_server_config_struct(), cur_fs);
    if(ret < 0)
    {
	gen_mutex_unlock(&mt_config);
	return(ret);
    }

    gen_mutex_unlock(&mt_config);
    return(0);
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 noexpandtab
 */
