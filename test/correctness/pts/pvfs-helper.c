#include "pint-sysint.h"
#include "pvfs-helper.h"

pvfs_helper_t pvfs_helper;

int initialize_sysint(void)
{
    int ret = -1;

    memset(&pvfs_helper,0,sizeof(pvfs_helper));

    ret = parse_pvfstab(NULL,&pvfs_helper.mnt);
    if (ret > -1)
    {
/*         gossip_disable(); */

        /* init the system interface */
        ret = PVFS_sys_initialize(pvfs_helper.mnt,
                                  &pvfs_helper.resp_init);
        if(ret > -1)
        {
            pvfs_helper.initialized = 1;
            pvfs_helper.num_test_files = NUM_TEST_FILES;
            ret = 0;
        }
        else
        {
            fprintf(stderr, "Error: PVFS_sys_initialize() "
                    "failure. = %d\n", ret);
        }
    }
    else
    {
        fprintf(stderr, "Error: parse_pvfstab() failure.\n");
    }
    return ret;
}

/*
 * helper function to fill in the root pinode_refn
 * fs_id:   fsid of our file system
 *
 * returns:  0 on success; 
 *      -1 if a problem
 */
int get_root(PVFS_fs_id fs_id, PVFS_pinode_reference *pinode_refn)
{
    int ret = -1;
    PVFS_credentials credentials;
    PVFS_sysresp_lookup resp_look;

    if (pinode_refn)
    {
        memset(&resp_look, 0, sizeof(resp_look));

        credentials.uid = 100;
        credentials.gid = 100;
        credentials.perms = 1877;

        printf("looking up the root handle for fsid = %d\n", fs_id);
        ret = PVFS_sys_lookup(fs_id, "/", credentials, &resp_look);
        if (ret < 0)
        {
            printf("Lookup failed with errcode = %d\n", ret);
        }
        memcpy(pinode_refn, &resp_look.pinode_refn,
               sizeof(PVFS_pinode_reference));
    }
    return ret;
}

int create_dir(PVFS_pinode_reference parent_refn, char *name,
               PVFS_pinode_reference *out_refn)
{
    int ret = -1;
    uint32_t attrmask;
    PVFS_object_attr attr;
    PVFS_credentials credentials;
    PVFS_sysresp_mkdir resp_mkdir;

    memset(&resp_mkdir, 0, sizeof(resp_mkdir));

    attrmask = PVFS_ATTR_SYS_ALL_NOSIZE;
    attr.owner = 100;
    attr.group = 100;
    attr.perms = 1877;
    attr.objtype = PVFS_TYPE_DIRECTORY;

    credentials.uid = 100;
    credentials.gid = 100;
    credentials.perms = 1877;

    ret = PVFS_sys_mkdir(name, parent_refn, attrmask,
                         attr, credentials, &resp_mkdir);
    if (ret < 0)
    {
        printf("mkdir failed\n");
        return (-1);
    }
    if (out_refn)
    {
        memcpy(out_refn, &resp_mkdir.pinode_refn,
               sizeof(PVFS_pinode_reference));
    }
    return 0;
}

/*
 * simple helper to remove a pvfs2 file
 *
 * returns 0 on success.
 *          -1 if some error happened.
 */
int remove_file(PVFS_pinode_reference parent_refn, char *name)
{
    int ret = -1;
    PVFS_credentials credentials;

    credentials.perms = 1877;
    credentials.uid = 100;
    credentials.gid = 100;

    ret = PVFS_sys_remove(name, parent_refn, credentials);
    if (ret < 0)
    {
        printf("remove failed\n");
        return ret;
    }
    return 0;
}

/*
 * simple helper to remove a pvfs2 dir
 *
 * returns 0 on success.
 *          -1 if some error happened.
 */
int remove_dir(PVFS_pinode_reference parent_refn, char *name)
{
    return remove_file(parent_refn, name);
}

/*
 * simple helper to lookup a handle given a filename
 *
 * returns a handle to the new directory
 *          -1 if some error happened
 */
int lookup_name(PVFS_pinode_reference pinode_refn, char *name,
                PVFS_pinode_reference *out_refn)
{
    int ret = -1;
    PVFS_credentials credentials;
    PVFS_sysresp_lookup resp_lookup;

    memset(&resp_lookup, 0, sizeof(resp_lookup));

    credentials.uid = 100;
    credentials.gid = 100;
    credentials.perms = (PVFS_U_WRITE | PVFS_U_READ);

    ret = PVFS_sys_lookup(pinode_refn.fs_id, name,
                          credentials, &resp_lookup);
    if (ret < 0)
    {
       printf("Lookup failed with errcode = %d\n", ret);
       return(-1);
    }
    if (out_refn)
    {
        memcpy(out_refn, &resp_lookup.pinode_refn,
               sizeof(PVFS_pinode_reference));
    }
    return 0;
}
