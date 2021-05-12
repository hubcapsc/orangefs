/*
 * (C) 2012 Clemson University
 *
 * See COPYING in top-level directory.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

#include "pvfs2-internal.h"
#include "gossip.h"
#include "pvfs2-debug.h"
#include "pvfs3-handle.h"
#include "sidcache.h"
#include "pvfs2-server.h"
#include "server-config.h"
#include "server-config-mgr.h"

/* Length of string representation of PVFS_SID */
/* #define SID_STR_LEN (37) in sidcache.h */

/* Used by bulk retrieve function to keep track of the sid obtained
 * from the database during bulk retrieval 
 */
DBT bulk_next_key;

/* Global Number of records (SIDs) in the cache
 */
static int sids_in_cache = 0;


/* Global database variables */
DB *SID_db = NULL;                  /* Primary database (sid cache) */
DB_ENV *SID_envp = NULL;            /* Env for sid cache and secondary dbs */
DB *SID_attr_index[SID_NUM_ATTR];   /* Array of secondary databases */
DBC *SID_attr_cursor[SID_NUM_ATTR]; /* Array of secondary database cursors */
DB_TXN *SID_txn = NULL;             /* Main transaction variable */
                                    /* (transactions are currently not used */
                                    /*     with the sid cache) */
DB  *SID_type_db = NULL;            /* secondary db for server type */
DBC *SID_type_cursor = NULL;        /* cursor for server type db */
DB  *SID_type_index = NULL;         /* index on sid for server type db */
DBC *SID_index_cursor = NULL;       /* cursor for server type sid index */

/* NOTE: SID_type_db and SID_type_index are plain DB's they are not
 * secondary indexes at all.  Originally they were going to be but
 * various restrictions in DB made that not work out.  They can be used
 * as secondary indices, but it must be done manually, in particular,
 * records inserted/removed in one should also be inserved/removed in
 * the other.
 */

/* <========================= STATIC FUNCTIONS ============================> */
static int SID_initialize_secondary_dbs(DB *secondary_dbs[]);
static int SID_initialize_database_cursors(DBC *db_cursors[]);
static int SID_cache_parse_header(FILE *inpfile,
                                  int *records_in_file,
                                  int *attrs_in_file,
                                  int **attr_positions);
static int SID_create_type_table(void);
static char *SID_type_to_string(char *buf, struct SID_type_s typeval, int n);
static int SID_type_store(PVFS_SID *sid, FILE *outpfile);
static int SID_cache_update_type(const PVFS_SID *sid_server,
                                 struct SID_type_s *new_type_val);
static int SID_cache_update_type_single(const PVFS_SID *sid_server,
                                        uint32_t new_type_val,
                                        PVFS_fs_id fsid);
static int SID_cache_update_attrs(DB *dbp,
                                  const PVFS_SID *sid_server,
                                  int new_attr[]);

/* <====================== INITIALIZATION FUNCTIONS =======================> */

/* 
 * This function initializes the indices of a secondary DB to all be equal
 * to NULL
 *
 * On success 0 is returned, otherwise the index that is not equal to NULL
 * is returned unless the index is 0, then -1 is returned
 */
static int SID_initialize_secondary_dbs(DB *secondary_dbs[])
{
    int ret =  0;
    int i = 0;

    memset(secondary_dbs, '\0', (sizeof(DB *) * SID_NUM_ATTR));

    for(i = 0; i < SID_NUM_ATTR; i++)
    {
        if(secondary_dbs[i] != NULL)
        {
            if(i == 0)
            {
                return(-1);
            }
            else
            {
                return(i);
            }
        }
    }

    return(ret);
}

/*
 * This function initializes the indices of a DBC array to all be equal to NULL
 *
 * On success 0 is returned, otherwise the index that is not equal to NULL
 * is returned unless the index is 0, then -1 is returned
 */
static int SID_initialize_database_cursors(DBC *db_cursors[])
{
    int ret = 0;
    int i = 0;

    memset(SID_attr_cursor, '\0', (sizeof(DBC *) * SID_NUM_ATTR));

    for(i = 0; i < SID_NUM_ATTR; i++)
    {
        if(SID_attr_cursor[i] != NULL)
        {
            if(i == 0)
            {
                return(-1);
            }
            else
            {
                return(i);
            }
        }
    }

    return(ret);
}
/* <=========================== HELPER FUNCTIONS =========================> */

/* These are defined in sidcacheval.h */

struct type_conv
{
    uint32_t typeval;
    char *typestring;
} type_conv_table[] =
{
    {SID_SERVER_ROOT        , "ROOT"} ,
    {SID_SERVER_PRIME       , "PRIME"} ,
    {SID_SERVER_CONFIG      , "CONFIG"} ,
    {SID_SERVER_LOCAL       , "LOCAL"} ,
    {SID_SERVER_META        , "META"} ,
    {SID_SERVER_DATA        , "DATA"} ,
    {SID_SERVER_DIRM        , "DIR"} ,
    {SID_SERVER_DIRD        , "DIRDATA"} ,
    {SID_SERVER_SECURITY    , "SECURITY"} ,
    /* these should always be last */
    {SID_SERVER_ME          , "ME"} ,
    {SID_SERVER_VALID_TYPES , "ALL"} ,
    {SID_SERVER_NULL        , "INVALID"}
};
/* This must be defined as the size of the longest typestring */
#define MAX_TYPE_STR 8
    
static char *SID_type_to_string(char *buf, struct SID_type_s typeval, int n)
{
    int i;
    for (i = 0;
         type_conv_table[i].typeval != typeval.server_type &&
         type_conv_table[i].typeval != SID_SERVER_NULL;
         i++);
    strcpy(buf, type_conv_table[i].typestring);
    if (typeval.fsid != 0)
    {
        if (n < MAX_TYPE_STR + 12)
        {
            return NULL;
        }
        sprintf(buf,
                "%s(%8d) ",
                type_conv_table[i].typestring,
                typeval.fsid);
    }
    else
    {
        if (n < MAX_TYPE_STR + 2)
        {
            return NULL;
        }
        sprintf(buf, "%s ", type_conv_table[i].typestring);
    }
    return buf;
}
 
/* This function is exported for parsing the config file 
 * Looks for predefined strings that represent types
 * returns positive numerical* representation of type
 * or 0 if type not found or negative on error
 */
int SID_string_to_type(const char *typestring)
{
    int i;
    char *mytype;
    int len;
    int server_type = 0;

    if(!typestring)
    {
        return -PVFS_EINVAL;
    }

    /* there are at most MAX_TYPE_STR non-zero chars */
    len = strnlen(typestring, MAX_TYPE_STR + 1);

    if (len > MAX_TYPE_STR)
    {
        return -PVFS_EINVAL;
    }

    mytype = (char *)malloc(len + 1);
    if (!mytype)
    {
        return -1;
    }

    for (i = 0; i < len; i++)
    {
        mytype[i] = toupper(typestring[i]);
    }
    mytype[len] = 0;

    for (i = 0;
         strncmp(type_conv_table[i].typestring, mytype, MAX_TYPE_STR) &&
         type_conv_table[i].typeval != SID_SERVER_NULL;
         i++);

    if (type_conv_table[i].typeval == SID_SERVER_NULL)
    {
        server_type = SID_SERVER_NULL;
        free(mytype);
        return -PVFS_EINVAL;
    }

    server_type = type_conv_table[i].typeval;

    free(mytype);

    return server_type;
}

/* We are expecting a string of the form "someattr=val" where val is an
 * integer.  If attributes is not created yet, we create it.  If
 * something goes wrong we pretty much just return -1 without trying to
 * diagnose it sinice this is a very limited parse
 */
int SID_set_attr(const char *attr_str, int **attributes)
{
    int i = 0;;
    int len = 0;;
    int eqflag = 0;
    int atflag = 0;
    char *myattr = NULL;
    char *myval = NULL;

    if (!attributes || !attr_str)
    {
        return -1;
    }

    len = strnlen(attr_str, MAX_ATTR_STR);

    if (len > MAX_ATTR_STR)
    {
        return -1;
    }

    myattr = (char *)malloc(len + 1);
    if (!myattr)
    {
        gossip_err("%s: failed to malloc myattr\n", __func__);
        return -1;
    }
    for (i = 0; i < len; i++)
    {
        if (attr_str[i] == '=')
        {
            myattr[i] = 0;
            if (i == len - 1)
            {
                /* badly formed string */
                free(myattr);
                return -1;
            }
            myval = &myattr[i + 1];
            eqflag = 1;
        }
        else
        {
            myattr[i] = tolower(attr_str[i]);
        }
    }
    myattr[len] = 0;
    if (!eqflag)
    {
        /* badly formed string */
        free(myattr);
        return -1;
    }

    if (!*attributes)
    {
        *attributes = (int *)malloc(SID_NUM_ATTR * sizeof(int));
        if (!myattr)
        {
            gossip_err("%s: failed to malloc attributes\n", __func__);
            free(myattr);
            return -1;
        }
        memset(*attributes, -1, SID_NUM_ATTR * sizeof(int));
        atflag = 1;
    }

    for (i = 0; i < SID_NUM_ATTR; i++)
    {
        if(!strcmp(myattr, SID_attr_map[i]))
        {
            /* found the one we want */
            /* call frees attributes */
            *attributes[i] = atoi(myval);
            free(myattr);
            return 0;
        }
    }
    /* didn't find it, just return */

    if (atflag)
    {
        /* we created this but didn't add any data so get rid of it */
        free(*attributes);
        *attributes = NULL;
    }
    free(myattr);
    return -1;
}

/* <====================== SID_cacheval_t FUNCTIONS =======================> */

/*
 * This function initializes a SID_cacheval_t struct to default values
*/
void SID_cacheval_init(SID_cacheval_t **cacheval)
{
    memset((*cacheval)->attr, -1, (sizeof(int) * SID_NUM_ATTR));
    memset(&((*cacheval)->bmi_addr), -1, sizeof(BMI_addr));
    (*cacheval)->url = (char *)&(*cacheval)[1];
    (*cacheval)->url[0] = 0;
}

/** HELPER - assigns DBT for cacheval
 *
 * This function marshalls the data for the SID_cacheval_t to store in the 
 * sidcache
*/
void SID_cacheval_pack(const SID_cacheval_t *cacheval, DBT *val)
{
    if (!cacheval)
    {
        gossip_err("%s: NULL caacheval passed in\n", __func__);
        return;
    }
    val->data = (SID_cacheval_t *)cacheval;
    /*
    val->size = (sizeof(int) * SID_NUM_ATTR) +
                 sizeof(BMI_addr) +
     */
    val->size = sizeof(SID_cacheval_t) + strlen(cacheval->url) + 1;
    val->ulen = val->size;
    val->flags = DB_DBT_USERMEM;
}

/** HELPER - LOADS cacheval
 *
 * This function creates a SID_cacheval_t struct with the
 * attributes that are passed to
 * this function by dynamically creating the SID_cacheval_t.
 * The url attribute cannot
 * be null otherwise the SID_cacheval_t is not dynamically created
 *
 * Returns 0 on success, otherwise -1 is returned
 */
int SID_cacheval_alloc(SID_cacheval_t **cacheval,
                       const int sid_attributes[],
                       const BMI_addr sid_bmi,
                       const char *sid_url)
{
    int ulen = 0;
    if(!sid_url)
    {
        gossip_err("%s: url passed in is NULL\n", __func__);
        *cacheval = NULL;
        return(-1);
    }

    ulen = strlen(sid_url) + 1;
    /* Mallocing space for the SID_cacheval_t struct */
    *cacheval = (SID_cacheval_t *)malloc(sizeof(SID_cacheval_t) + ulen);
    if (!*cacheval)
    {
        gossip_err("%s: failed to malloc cacheval\n", __func__);
        return -1;
    }
    SID_cacheval_init(cacheval); /* sets up url pointer */
    
    /* Setting the values of the SID_cacheval_t struct */    
    if (sid_attributes)
    {
        /* attributes may or may not be provided */
        memcpy((*cacheval)->attr, sid_attributes, (sizeof(int) * SID_NUM_ATTR));
    }
    memcpy(&((*cacheval)->bmi_addr), &sid_bmi, sizeof(BMI_addr));
    /* we alreaddy know the size of the string and buffer so we do a memcpy
     * and NOT a strcpy
     */
    memcpy((*cacheval)->url, sid_url, ulen);

    return(0);
}

/*
 * This function clean up a SID_cacheval_t struct by freeing the dynamically
 * created SID_cacheval_t struct
 */
void SID_cacheval_free(SID_cacheval_t **cacheval)
{
    if (!cacheval)
    {
        return;
    }
    free(*cacheval);
}

/** HELPER - copies cacheval out of DBT
 *
 * This function unpacks the data recieved from the database, mallocs the 
 * SID_cacheval_t struct, and sets the values inside of the SID_cacheval_t 
 * struct with the data retrieved from the database
 */
void SID_cacheval_unpack(SID_cacheval_t **cacheval, DBT *data)
{              
    if (!cacheval || !data)
    {
        gossip_err("%s: NULL cacheval or data passed\n", __func__);
        return;
    }
    *cacheval = malloc(data->size);
    /* no point in calling init if we are going to overwrite anyway */
    if (!*cacheval)
    {
        return;
    }
    memcpy(*cacheval, data->data, data->size); /* includes url copy */
    (*cacheval)->url = (char *)&(*cacheval)[1];
}

/* <====================== SID CACHE FUNCTIONS ============================> */

/** HELPER for SID_LOAD
 *
 * This function parses the first two lines in the input file and gets
 * the number of attributes per sid, number of sids,
 * and the string representations of the int attributes for the sids.
 * It gets the attributes that each sid has
 * parsing the strings representations of the attributes.
 * This function also sets the attr_positions array to make sure that the
 * attributes in the input file go into their correct positions in the
 * SID_cacheval_t attrs array.
 *
 * Returns 0 on success, otherwise returns an error code
 * The attr_position array is malloced inside of this function and is freed
 * inside the SID_load function which calls this one.
 *
 * fmemopen may be used to read from a message buffer.
 */
static int SID_cache_parse_header(FILE *inpfile,
                                  int *records_in_file,
                                  int *attrs_in_file,
                                  int **attr_positions)
{
    int i = 0, j = 0;             /* Loop index variables */
    char **attrs_strings;         /* Strings of the attributes in the file */
    char tmp_buff[TMP_BUFF_SIZE] = {0}; /* Temporary string buffer to read the
                                     attribute strings from the input file */

    /* Checking to make sure the input file is open, the function
     * load_sid_cache_from_file should have opened the file
     */
    if(!inpfile)
    {
        gossip_err("File is not opened. Exiting load_sid_cache_from_file\n");
        return(-1);
    }
    
    /* Getting the total number of attributes from the file */
    fscanf(inpfile, "%s", tmp_buff);
    fscanf(inpfile, "%d", attrs_in_file);
    if(*attrs_in_file > SID_NUM_ATTR || *attrs_in_file < 1)
    {
        gossip_err("The number of attributes in the input file "
                   "was not within the proper range\n");
        gossip_err("The contents of the database will not be read "
                   "from the inputfile\n");
        return(-1);
    }
    
    /* Getting the number of sids in from the file */
    fscanf(inpfile, "%s", tmp_buff);
    fscanf(inpfile, "%d", records_in_file);

    /* Checking to make sure the input file has the number of sids as the
     *  entry in the input file
     */
    if(*records_in_file == 0)
    {
        gossip_err("%s: There are no sids in the input file\n", __func__);
        return(-1);
    }
    
    /* Mallocing space to hold the name of the attributes in the file 
     * and initializing the attributes string array 
     */
    attrs_strings = (char **)malloc(sizeof(char *) * *attrs_in_file);
    if (!attrs_strings)
    {
        gossip_err("%s: malloc attr strings failed\n", __func__);
        return -1;
    }
    memset(attrs_strings, '\0', sizeof(char *) * *attrs_in_file);

    /* Mallocing space to hold the positions of the attributes in the file for
     * the cacheval_t attribute arrays and initializing the position array 
     */
    *attr_positions = (int *)malloc(sizeof(int) * *attrs_in_file);
    if (!attrs_strings)
    {
        gossip_err("%s: malloc attr positions failed\n", __func__);
        return -1;
    }
    memset(*attr_positions, 0, (sizeof(int) * *attrs_in_file));
    
    /* Getting the attribute strings from the input file */
    for(i = 0; i < *attrs_in_file; i++)
    {
        int slen = 0;
        slen = strlen(tmp_buff) + 1;
        fscanf(inpfile, "%s", tmp_buff);
        attrs_strings[i] = (char *)malloc(sizeof(char) * slen);
        if (!attrs_strings[i])
        {
            gossip_err("%s: malloc attr strings %d failed\n", __func__, i);
            return -1;
        }
        /* we already know the length of the string and buffer so
         * do a memcpy, not a strcpy
         */
        memcpy(attrs_strings[i], tmp_buff, slen);
    }

    /* Getting the correct positions from the input file for the
     * attributes in the SID_cacheval_t attrs array 
     */
    for(i = 0; i < *attrs_in_file; i++)
    {
        for(j = 0; j < SID_NUM_ATTR; j++)
        {
            if(!strcmp(attrs_strings[i], SID_attr_map[j]))
            {
                (*attr_positions)[i] = j;
                break;
            }
        }
        /* If the attribute string was not a valid option it will be ignored
         * and not added to the sid cache 
         */
        if(j == SID_NUM_ATTR)
        {
            gossip_debug(GOSSIP_SIDCACHE_DEBUG, 
                         "Attribute: %s is an invalid attribute, "
                         "and it will not be added\n",
                         attrs_strings[i]);
            (*attr_positions)[i] = -1;
        }
    }

    /* Freeing the dynamically allocated memory */
    for(i = 0; i < *attrs_in_file; i++)
    {
        free(attrs_strings[i]);
    }
    free(attrs_strings);

    return(0);
}

/** SID_type_load
 * This function reads from a file and locates type identifiers
 * Each one is added to the type_db
 * Invalid items or EOL cause the scan to stop
 */
#define TYPELINELEN 1024
static int SID_type_load(FILE *inpfile, const PVFS_SID *sid)
{
    int ret = 0;
    DBT type_key;
    DBT type_val;
    DBT index_key;
    DBT index_val;
    char linebuff[TYPELINELEN];
    char *lineptr = linebuff;
    char *saveptr = linebuff;
    char *typeword = NULL;
    PVFS_SID sid_buf;
    struct SID_type_s type_buf;

    sid_buf = *sid;

    SID_zero_dbt(&type_key, &type_val, NULL);
    SID_zero_dbt(&index_key, &index_val, NULL);

    type_key.data = &type_buf;
    type_key.size = sizeof(struct SID_type_s);
    type_key.ulen = sizeof(struct SID_type_s);
    type_key.flags = DB_DBT_USERMEM;

    type_val.data = &sid_buf;
    type_val.size = sizeof(sid_buf);
    type_val.ulen = sizeof(sid_buf);
    type_val.flags = DB_DBT_USERMEM;

    index_key.data = &sid_buf;
    index_key.size = sizeof(sid_buf);
    index_key.ulen = sizeof(sid_buf);
    index_key.flags = DB_DBT_USERMEM;

    index_val.data = &type_buf;
    index_val.size = sizeof(struct SID_type_s);
    index_val.ulen = sizeof(struct SID_type_s);
    index_val.flags = DB_DBT_USERMEM;

    /* read a line */
    memset(linebuff, 0, TYPELINELEN);
    fgets(linebuff, TYPELINELEN, inpfile);

    while(1)
    {
        /* read next word */
        memset(typeword, 0, 50);

        /* strtok is not reentrant, strtok_r is not standard (gcc
         * extension) need to add config support to select the right
         * approach - V3
         * NOTE: strtok_r is POSIX 2001
         */
# if 1
        typeword = strtok_r(lineptr, " \t\n", &saveptr);
# else
        typeword = strtok(lineptr, " \t\n");
# endif
        if (!typeword)
        {
            return 0; /* no more type tokens */
        }
        lineptr = NULL; /* subsequent calls from saved string pointer */

        /* V3 NEEDS TO BE DONE */
        /* check for an fs_id and set in buffer */
        type_buf.fsid = 0; /* applies to all fs */

        /* a zero return is an invalid typeval */
        if (0 < (ret = SID_string_to_type(typeword)))
        {
            type_buf.server_type = ret; /* numerical type value */
            /* insert into type database */
            ret = SID_type_db->put(SID_type_db,
                                   NULL,
                                   &type_key,
                                   &type_val,
                                   0);
            if (!ret)
            {
                break; /* DB error */
            }
            /* insert into type index database */
            ret = SID_type_index->put(SID_type_index,
                                      NULL,
                                      &index_key,
                                      &index_val,
                                      0);
            if (!ret)
            {
                break; /* DB error */
            }
        }
        else
        {
            break; /* invalid type string or error in string-to-type */
        }
    }
    return(ret);    
}
#undef TYPELINELEN

/** SID_LOAD
 *
 * This function loads the contents of an input file into the cache.
 * SID records in the file are added to the current contents of the
 * cache.  Duplicates are rejected with an error, only the first value
 * is kept.
 *
 * Returns 0 on success, otherwise returns an error code
 * The number of sids in the file is returned through the
 * parameter db_records.
 */
int SID_cache_load(DB *dbp, FILE *inpfile, int *num_db_records)
{
    int ret = 0;                      /* Function return value */
    int i = 0;                        /* Loop index variables */
    int *attr_positions = NULL;
    int attr_pos_index = 0;           /* Index of the attribute from the */
                                      /*   file in the SID_cacheval_t attrs */
    int attrs_in_file = 0;            /* Number of attrs in input file */
    int records_in_file = 0;          /* Total number of sids in input file */
    int sid_attributes[SID_NUM_ATTR]; /* Temporary attribute array to hold */
                                      /*   the attributes from the input file */
    int throw_away_attr = 0;          /* If the attribute is not a valid */
                                      /*   choice its value will be ignored */
    BMI_addr tmp_bmi = 0;             /* Temporary BMI_addr for the bmi */
                                      /*   addresses from input file */
    char tmp_url[TMP_BUFF_SIZE];      /* Temporary string for the url's from */
                                      /*   the input file */
    char tmp_sid_str[SID_STR_LEN];    /* Temporary string to get the PVFS_SID */
                                      /*   string representation from input */
                                      /*   file */
    PVFS_SID current_sid;             /* Temporary location to create the */
                                      /*   PVFS_SID from the tmp_SID_str */
    SID_cacheval_t *current_sid_cacheval; /* Sid's attributes from the input file */

    /* Getting the attributes from the input file */
    ret = SID_cache_parse_header(inpfile,
                                 &records_in_file,
                                 &attrs_in_file,
                                 &attr_positions);
    if(ret)
    {
        fclose(inpfile);
        return(ret);
    }

    for(i = 0; i < records_in_file; i++)
    {
        /* Read the sid's string representation from the input file */
        fscanf(inpfile, "%s", tmp_sid_str);

        /* convert to binary */
        ret = PVFS_SID_str2bin(tmp_sid_str, &current_sid);
        if(ret)
        {
            /* Skips adding sid to sid cache */
            gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                         "Error parsing PVFS_SID in "
                         "SID_load_cache_from_file function\n");
            continue;
        }

        /* Read the bmi address */
        fscanf(inpfile, SCANF_lld, &tmp_bmi);

        /* Read the url */
        fscanf(inpfile, "%s", tmp_url);

        /* Initializing the temporary attribute array */
        /* so all index's are currently -1 */
        memset(sid_attributes, -1, (sizeof(int) * SID_NUM_ATTR));

        /* Read the attributes from the input file and place them in the
         * correct position in the attrs array in the SID_cacheval_t struct 
         */
        for(attr_pos_index = 0;
            attr_pos_index < attrs_in_file;
            attr_pos_index++)
        {
            if(attr_positions[attr_pos_index] != -1)
            {
                fscanf(inpfile,
                       "%d",
                       &(sid_attributes[attr_positions[attr_pos_index]]));
            }
            else
            {
                fscanf(inpfile, "%d", &throw_away_attr);
            }
        }

        /* Read the type indicators from the file */
        SID_type_load(inpfile, &current_sid);

        /* This allocates and fills in the cacheval */
        ret = SID_cacheval_alloc(&current_sid_cacheval,
                                 sid_attributes,
                                 tmp_bmi,
                                 tmp_url);
        if(ret)
        {
            /* Presumably out of memory */
            return(ret);
        }
    
        /* Storing the current sid from the input file into the sid cache */
        ret = SID_cache_put(dbp,
                            &current_sid,
                            current_sid_cacheval,
                            num_db_records);
        if(ret)
        {
            /* Need to examine error and decide if we should continue or
             * not - for now we stop and close the input file
             */
            fclose(inpfile);
            SID_cacheval_free(&current_sid_cacheval);
            return(ret);
        }

        SID_cacheval_free(&current_sid_cacheval);
    }

    free(attr_positions);
   
    return(ret);
}

/** SID_ADD
 * 
 * This function stores a sid into the sid_cache.
 * Pass a pointer to a record counter if we are adding a new record
 * and we will get an error if there is already a record with the same key.
 * Otherwise we assume that we are updating a record, though it will
 * insert silently if the record does not exist.
 *
 * Returns 0 on success, otherwise returns error code
*/
int SID_cache_put(DB *dbp,
                  const PVFS_SID *sid_server,
                  const SID_cacheval_t *cacheval,
                  int *num_db_records)
{
    int ret = 0;        /* Function return value */
    DBT key, val;       /* BerekeleyDB k/v pair sid value(SID_cacheval_t) */
    int putflags = 0;

    if(PVFS_SID_is_null(sid_server))
    {
        gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                     "Error: PVFS_SID in SID_store_sid_into "
                     "sid_cache function is currently NULL\n");
        return(-1);
    }

    SID_zero_dbt(&key, &val, NULL);

    key.data = (PVFS_SID *)sid_server;
    key.size = sizeof(PVFS_SID);
    key.ulen = sizeof(PVFS_SID);
    key.flags = DB_DBT_USERMEM;

    /* Marshalling the data of the SID_cacheval_t struct */
    /* to store it in the primary db */
    SID_cacheval_pack(cacheval, &val);

    if (num_db_records != NULL)
    {
        /* we are adding a new record 
         * we should not overwrite an old one
         */
        putflags |= DB_NOOVERWRITE;
    }

    /* Placing the data into the database */
    ret = (dbp)->put(dbp,           /* Primary database pointer */
                     NULL,          /* Transaction pointer */
                     &key,          /* PVFS_SID */
                     &val,          /* Data is marshalled SID_cacheval_t */
                     putflags);
    /* with DB_NOOVERWRITE this returns an error on a dup */
    if(ret)
    {
        gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                     "Error inserting sid into the sid cache : %s\n",
                     db_strerror(ret));
        return(ret);
    }

    /* Updating the record count if the sid was successfully added
     * to the sid cache 
     */
    if(num_db_records != NULL)
    {
        *num_db_records += 1;
    }
    /*
     * gossip not running yet
     */
    /*
    gossip_debug(GOSSIP_SIDCACHE_DEBUG, "sidcache adds record for SID %s\n",
                 PVFS_SID_str(sid_server));
    */
    fprintf(stderr, "sidcache adds record for SID %s\n",
                 PVFS_SID_str(sid_server));

    return(ret);
}

/** SID_LOOKUP
 *
 * This function searches for a SID in the sidcache. The PVFS_SID value
 * (sid_server parameter) must be initialized before this function is used.
 * The *cacheval will be malloced and set to the values of the attibutes
 * in the database for the sid if it is found in the database
 *
 * Returns 0 on success, otherwise returns an error code
 * Caller is expected to free the cacheval via SID_cacheval_free
 */
int SID_cache_get(DB *dbp,
                  const PVFS_SID *sid_server,
                  SID_cacheval_t **cacheval)
{
    int ret = 0;
    DBT key, val;

    SID_zero_dbt(&key, &val, NULL);

    gossip_debug(GOSSIP_SIDCACHE_DEBUG, "Searching sidcache for %s\n",
                                        PVFS_SID_str(sid_server));

    key.data = (PVFS_SID *)sid_server;
    key.size = sizeof(PVFS_SID);
    key.ulen = sizeof(PVFS_SID);
    key.flags = DB_DBT_USERMEM;

    val.flags = DB_DBT_MALLOC;

    /* val is memory managed by db */
    /* size of val is hard to predict so we let DB malloc it */
   
    ret = (dbp)->get(dbp,   /* Primary database pointer */
                     NULL,  /* Transaction Handle */
                     &key,  /* Sid_sid_t sid */
                     &val,  /* Data is marshalled SID_cacheval_t struct */
                     0);    /* get flags */
    if(ret)
    {
        gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                     "Error getting sid from sid cache : %s\n",
                     db_strerror(ret));
        return(ret);
    }

    /* Unmarshalling the data from the database for the SID_cacheval_t struct */
    if (val.data)
    {
        SID_cacheval_unpack(cacheval, &val);
        clean_free(val.data);
    }
    else
    {
        gossip_err("SID_cache_get returns a null data pointere\n");
        return -1;
    }
    gossip_debug(GOSSIP_SIDCACHE_DEBUG, "Sidcache returns %s\n",
                                        (*cacheval)->url);
    
    return(ret);
}

/** HELPER - SID_LOOKUP and assign bmi_addr
 *
 * This function searches for a sid in the sid cache, retrieves the struct,
 * malloc's the char * passed in, and copies the bmi URI address of the
 * retrieved struct into that char *.
 *
 * Caller is expected to free the bmi_addr memory
 */
int SID_cache_lookup_bmi(DB *dbp, const PVFS_SID *search_sid, char **bmi_url)
{
    SID_cacheval_t *temp;
    int ret;

    /* Retrieve the corresponding value */
    ret = SID_cache_get(dbp, search_sid, &temp);

    if (ret)
    {
        gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                     "Error retrieving from sid cache\n");
        return ret;
    }

    /* Malloc the outgoing BMI address char * to be size of retrieved one */
    *bmi_url = malloc(strlen(temp->url) + 1);
    if (!*bmi_url)
    {
        gossip_err("%s: failed to malloc bmi_url\n", __func__);
        return -1;
    }

    /* Copy retrieved BMI address to outgoing one */
    strcpy(*bmi_url, temp->url);

    /* Free any malloc'ed memory other than the outgoing BMI address */
    free(temp);

    return ret;
}

/** SID_UPDATE
 *
 * This function updates a record in the sid cache to any new values 
 * (attributes, bmi address, or url) that are in the SID_cacheval_t parameter
 * to this function if a record with a SID matching the sid_server parameter
 * is found in the sidcache
 *
 * Returns 0 on success, otherwise returns error code
 */
int SID_cache_update_server(DB *dbp,
                            const PVFS_SID *sid_server,
                            SID_cacheval_t *new_attrs,
                            struct SID_type_s *sid_types)
{
    int ret = 0;                   /* Function return value */
    SID_cacheval_t *current_attrs; /* Temp SID_cacheval_t used to get current 
                                      attributes of the sid from the sid */
   
    if(new_attrs == NULL)
    {
        gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                     "The new attributes passed to "
                     "SID_cache_update_server is NULL\n");
        return(-1);
    }
 
    /* Getting the sid from the sid cache */
    ret = SID_cache_get(dbp, sid_server, &current_attrs);
    if(ret)
    {
        return(ret);
    }

    /* Updating the old attributes to the new values */
    SID_cache_copy_attrs(current_attrs, new_attrs->attr);

    /* Updating the old bmi address to the new bmi_address */
    SID_cache_copy_bmi(current_attrs, new_attrs->bmi_addr);
   
    /* Updating the old url to the new url */
    SID_cache_copy_url(&current_attrs, new_attrs->url);

/* SHOULD NOT NEED THIS */
#if 0
    /* Deleting the old record from the sid cache */
    ret = SID_cache_delete_server(dbp, sid_server, NULL);
    if(ret)
    {
        return(ret);
    }
#endif
    
    /* writing the updated SID_cacheval_t struct back to the sid cache */
    ret = SID_cache_put(dbp, sid_server, current_attrs, NULL);
    if(ret)
    {
        return(ret);
    }

    /* Update the type db with the type bitfield provided */
    ret = SID_cache_update_type(sid_server, sid_types);
    if(ret)
    {
        return(ret);
    }
        
    /* Freeing the dynamically created bmi address in the SID_cacheval_t
     * struct 
     */
    SID_cacheval_free(&current_attrs);

    return(ret);
}

/** I don't understand what this function is for
 *  seems like we already have an update function, and there doesn't
 *  seem to be a reason to delete an old record and insert a new one
 *  if we can update.  Also return code is sketch.
 *  Maybe this should be a helper to update attrs in memory?
 *
 * This function updates the attributes for a sid in the database if a sid
 * with a matching sid_server parameter is found in the database
 *
 * Returns 0 on success, otherwise returns an error code
 */
int SID_cache_update_attrs(DB *dbp,
                           const PVFS_SID *sid,
                           int new_attr[])
{
    int ret = 0;
    int i = 0;   /* Loop index variable */
    SID_cacheval_t *sid_data;

    /* If sid_server is not passed in as NULL then only the attr's 
     * will be changed for the sid 
     */
    if(!dbp || !sid)
    {
        return(-EINVAL);
    }
    /* Get the sid from the cache */
    ret = SID_cache_get(dbp, sid, &sid_data);
    if(ret)
    {
        return(ret);
    }
        
    for(i = 0; i < SID_NUM_ATTR; i++)
    {
        /* a -1 indicates the new_attr does not need an update */
        if(new_attr[i] != -1)
        {
            sid_data->attr[i] = new_attr[i];
        }
    }

/* SHOULD NOT NEED THIS */
#if 0
    /* Deleting the old record from the cache */
    ret = SID_cache_delete_server(dbp, sid_server, NULL);
    if(ret)
    {
        return(ret);
    }
#endif

    ret = SID_cache_put(dbp, sid, sid_data, NULL);
    if(ret)
    {
        SID_cacheval_free(&sid_data);
        return(ret);
    }

    SID_cacheval_free(&sid_data);
    return(ret);    
}

/* The attr's are being updated with other attributes with
 * SID_cache_update_server
 * function, so only the current_sid_attrs need to change 
 */
int SID_cache_copy_attrs(SID_cacheval_t *current_attrs,
                         int new_attr[])
{
    int ret = 0;
    int i;

    for(i = 0; i < SID_NUM_ATTR; i++)
    {
        current_attrs->attr[i] = new_attr[i];
    }
    return(ret);
}

/* Add a type to an existing server in the SIDcache
 * The DB_NODUPDATA flag suppresses an extra error message that would
 * occur if attempt to put a dup key/val pair (IOW the exact same
 * record, not just the same key) which could happen here.  We still get
 * an error code but we can test for that.
 */
int SID_cache_update_type_single(const PVFS_SID *sid_server,
                                 uint32_t new_type_val,
                                 PVFS_fs_id new_fsid)
{
    int ret = 0;
    DBT type_key;
    DBT type_val;
    DBT index_key;
    DBT index_val;
    PVFS_SID sid_buf;
    struct SID_type_s type_buf;

    sid_buf = *sid_server;

    SID_zero_dbt(&type_key, &type_val, NULL);
    SID_zero_dbt(&index_key, &index_val, NULL);

    type_key.data = &type_buf;
    type_key.size = sizeof(type_buf);
    type_key.ulen = sizeof(type_buf);
    type_key.flags = DB_DBT_USERMEM;

    type_val.data = &sid_buf;
    type_val.size = sizeof(sid_buf);
    type_val.ulen = sizeof(sid_buf);
    type_val.flags = DB_DBT_USERMEM;

    index_key.data = &sid_buf;
    index_key.size = sizeof(sid_buf);
    index_key.ulen = sizeof(sid_buf);
    index_key.flags = DB_DBT_USERMEM;

    index_val.data = &type_buf;
    index_val.size = sizeof(type_buf);
    index_val.ulen = sizeof(type_buf);
    index_val.flags = DB_DBT_USERMEM;

    if ((new_type_val & SID_SERVER_VALID_TYPES))
    {
        type_buf.server_type = new_type_val;
        type_buf.fsid = new_fsid;

        gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                     "Adding a type record SID %s Type %o\n",
                     PVFS_SID_str(sid_server), type_buf.server_type);

        ret = SID_type_db->put(SID_type_db,
                               NULL,
                               &type_key,
                               &type_val,
                               0);
        /* if KEYEXIST it was just a duplicate - keep going */
        if (ret && ret != DB_KEYEXIST)
        {
            gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                         "Error inserting a SID type record:%s\n",
                         db_strerror(ret));
            return ret;
        }
        ret = SID_type_index->put(SID_type_index,
                                  NULL,
                                  &index_key,
                                  &index_val,
                                  0);
        /* if KEYEXIST it was just a duplicate - keep going */
        if (ret && ret != DB_KEYEXIST)
        {
            gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                         "Error inserting a SID index record:%s\n",
                         db_strerror(ret));
            return ret;;
        }
    }
 
    return ret;    
}

int SID_cache_update_type(const PVFS_SID *sid_server,
                          struct SID_type_s *new_type_val)
{
    int ret = 0;
    uint32_t mask = 0;
    uint32_t type_val = new_type_val->server_type;

    for(mask = 1; mask != 0 && type_val != 0; mask <<= 1)
    {
        if (type_val & mask)
        {
            if ((mask & SID_SERVER_VALID_TYPES))
            {
                ret = SID_cache_update_type_single(sid_server,
                                                   mask,
                                                   new_type_val->fsid);
            }
            /* allows loop to exit when all present types are coded */
            type_val &= ~mask;
        }
    }
    return ret;
}

/*
 * This function updates the bmi address for a sid in the database if a sid
 * with a matching sid_server parameter is found in the database
 *
 * Returns 0 on success, otherwise returns an error code
 */
int SID_cache_update_bmi(DB *dbp,
                         const PVFS_SID *sid_server,
                         BMI_addr new_bmi_addr)
{
    int ret = 0;
    SID_cacheval_t *sid_attrs;

    /* If sid_server is not passed in as NULL then only the bmi address 
     * will be changed for the sid 
     */
    if(!dbp || !sid_server)
    {
        return(-EINVAL);
    }
    /* Getting the sid from the sid cache */
    ret = SID_cache_get(dbp, sid_server, &sid_attrs);
    if(ret)
    {
        return(ret);
    }
        
    if(sid_attrs->bmi_addr != new_bmi_addr)
    {
        sid_attrs->bmi_addr = new_bmi_addr;

/* SHOULD NOT NEED THIS */
#if 0
        /* Deleting the old record from the sid cache */
        ret = SID_cache_delete_server(dbp, sid_server, NULL);
        if(ret)
        {
            return(ret);
        }
#endif

        ret = SID_cache_put(dbp, sid_server, sid_attrs, NULL);
        if(ret)
        {
            SID_cacheval_free(&sid_attrs);
            return(ret);
        }
    }

    SID_cacheval_free(&sid_attrs);
    return(ret);    
}

/* The bmi address is being updated with other attributes with
 * SID_cache_update_server
 * function, so only the current_sid_attrs need to change 
 */
int SID_cache_copy_bmi(SID_cacheval_t *current_attrs, BMI_addr new_bmi_addr)
{
    int ret = 0;

    current_attrs->bmi_addr = new_bmi_addr;
    return(ret);
}

/*
 * This function updates the url address for a sid in the database if a sid
 * with a matching sid_server parameter is found in the database
 *
 * Returns 0 on success, otherwise returns an error code
 */
int SID_cache_update_url(DB *dbp, const PVFS_SID *sid_server, char *new_url)
{
    int ret = 0;
    int tmp_attrs[SID_NUM_ATTR];
    BMI_addr tmp_bmi;
    SID_cacheval_t *sid_attrs;

    if(!dbp || !sid_server || !new_url)
    {
        gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                     "The url passed into SID_cache_update_url "
                     "function is currently NULL\n");
        return(-EINVAL);
    }

    /* Getting the sid from the sid cache */
    ret = SID_cache_get(dbp, sid_server, &sid_attrs);
    if(ret)
    {
        return(ret);
    }
   
    if(!(strcmp(sid_attrs->url, new_url)))
    {
        /* The new url is the same as the current */
        /* url so no changes need to happen */
        SID_cacheval_free(&sid_attrs);
        return(ret);
    }
    else
    {
        memcpy(tmp_attrs, sid_attrs->attr, sizeof(int) * SID_NUM_ATTR);
        memcpy(&tmp_bmi, &(sid_attrs->bmi_addr), sizeof(BMI_addr));
            
        SID_cacheval_free(&sid_attrs);
        ret = SID_cacheval_alloc(&sid_attrs, tmp_attrs, tmp_bmi, new_url);    
        if(ret)
        {
            return(ret);
        }        

/* SHOULD NOT NEED THIS */
#if 0
        /* Deleting the old record from the sid cache */
        ret = SID_cache_delete_server(dbp, sid_server, NULL);
        if(ret)
        {
            return(ret);
        }
#endif

        /* Writing the updated SID_cacheval_t struct back to the sid cache */
        ret = SID_cache_put(dbp, sid_server, sid_attrs, NULL);
        if(ret)
        {
            SID_cacheval_free(&sid_attrs);
            return(ret);
        }
    
        SID_cacheval_free(&sid_attrs);
        return(ret);    
    }
}


/* The url is being updated with other attributes with
 * SID_cache_update_server
 * function, so only the current_sid_attrs need to change 
 */
int SID_cache_copy_url(SID_cacheval_t **current_attrs, char *new_url)
{
    int ret = 0;
    BMI_addr tmp_bmi;
    int tmp_attrs[SID_NUM_ATTR];

    /* The new new url is the same as the old url so nothing needs
     * to be updated
     */
    if((!strcmp((*current_attrs)->url, new_url)) || (!new_url))
    {
        return(ret);
    }
    else
    {
        memcpy(tmp_attrs,
               (*current_attrs)->attr,
               sizeof(int) * SID_NUM_ATTR);
        memcpy(&tmp_bmi,
               &((*current_attrs)->bmi_addr),
               sizeof(BMI_addr));
        
        /* Freeing the current SID_cacheval_t struct to change the
         * url address to the new address 
         */
        SID_cacheval_free(current_attrs);
        
        /* Setting the current bmi address to the new values */
        ret = SID_cacheval_alloc(current_attrs, tmp_attrs, tmp_bmi, new_url);
        if(ret)
        {
            return(ret);
        }
            
        return(ret);
    }
}

/** SID_REMOVE
  *
  * This function deletes a record from the sid cache if a sid with a matching
  * sid_server parameter is found in the database
  *
  * Returns 0 on success, otherwise returns an error code 
  */
int SID_cache_delete_server(DB *dbp,
                            const PVFS_SID *sid_server,
                            int *db_records)
{
    int ret = 0; /* Function return value */
    DBT key;     /* Primary sid key  */
    DBT data;    /* Primary sid key  */
    struct SID_type_s sidtype = {0, 0};

    /* Initializing the DBT key value */
    SID_zero_dbt(&key, &data, NULL);
    
    /* Setting the values of DBT key to point to the sid to 
     * delete from the sid cache 
     */
    key.data = (PVFS_SID *)sid_server;
    key.size = sizeof(PVFS_SID);
    key.ulen = sizeof(PVFS_SID);
    key.flags = DB_DBT_USERMEM;

    data.data = &sidtype;
    data.size = sizeof(struct SID_type_s);
    data.ulen = sizeof(struct SID_type_s);
    data.flags = DB_DBT_USERMEM;

    ret = (dbp)->del(dbp,  /* Primary database (sid cache) pointer */
                     NULL, /* Transaction Handle */
                     &key, /* SID key */
                     0);   /* Delete flag */
    if(ret)
    {
        gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                     "Error deleting record from sid cache : %s\n",
                     db_strerror(ret));
        return(ret);
    }

    /* remove all associated records from the type/index dbs */

    /* get first index record */
    ret = SID_index_cursor->get(SID_index_cursor,
                                &key, /* SID key */
                                &data,/* TYPE data */
                                DB_FIRST);   /* Get flag */
    if(ret)
    {
        gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                     "Error getting record from sid index : %s\n",
                     db_strerror(ret));
        return(ret);
    }

    while(ret != DB_NOTFOUND)
    {
        /* delete index record */
        ret = SID_index_cursor->del(SID_index_cursor, 0);
        if(ret)
        {
            gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                        "Error deleting record from sid index : %s\n",
                        db_strerror(ret));
            return(ret);
        }
        /* get type record */
        ret = SID_type_cursor->get(SID_type_cursor,
                                   &data, /* TYPE key - these are backwards */
                                   &key,  /* SID data - these are backwards */
                                   DB_GET_BOTH);   /* Get flag */
        if (ret != DB_NOTFOUND)
        {
            /* delete type reccord */
            ret = SID_type_cursor->del(SID_index_cursor, 0);
            gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                         "mirror record not found int type/index\n");
        }                           
        /* get next index record */
        sidtype.server_type = 0;
        sidtype.fsid = 0;
        ret = SID_index_cursor->get(SID_type_cursor,
                                    &key, /* SID key */
                                    &data,/* TYPE data */
                                    DB_NEXT_DUP);   /* Get flag */
        if(ret)
        {
            gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                         "Error getting record from sid index : %s\n",
                         db_strerror(ret));
            return(ret);
        }
    }
  
    if(db_records != NULL)
    { 
        *db_records -= 1;
    }

    return(ret);
}

/* loop over records matching sid and write type data */
int SID_type_store(PVFS_SID *sid, FILE *outpfile)
{
    int ret = 0;
    DBT type_sid_key;
    DBT type_sid_val;
    char buff[20];
    PVFS_SID kbuf;
    struct SID_type_s tbuf = {0, 0};

    kbuf = *sid;

    SID_zero_dbt(&type_sid_key, &type_sid_val, NULL);

    type_sid_key.data = &kbuf;
    type_sid_key.size = sizeof(PVFS_SID);
    type_sid_key.ulen = sizeof(PVFS_SID);
    type_sid_key.flags = DB_DBT_USERMEM;

    type_sid_val.data = &tbuf;
    type_sid_val.size = sizeof(struct SID_type_s);
    type_sid_val.ulen = sizeof(struct SID_type_s);
    type_sid_val.flags = DB_DBT_USERMEM;

    ret = SID_index_cursor->get(SID_index_cursor,
                                &type_sid_key,
                                &type_sid_val,
                                DB_SET);

    /* halt on error or DB_NOTFOUND */
    if (ret == 0 || ret == DB_NOTFOUND)
    {
        fprintf(outpfile, "\t\tType ");
    }
    else
    {
        fprintf(outpfile, "ERROR not DB_NOTFOUND finding types\n");
        return ret;
    }
    while(ret == 0)
    {
        /* write type to file */
        SID_type_to_string(buff, tbuf, 20);
        fprintf(outpfile, "%s ", buff);
        tbuf.server_type = 0;
        tbuf.fsid = 0;
        /* should have been set by previous get */
        ret = SID_index_cursor->get(SID_index_cursor,
                                    &type_sid_key,
                                    &type_sid_val,
                                    DB_NEXT_DUP);
    }

    fprintf(outpfile, "\n");
    /* Normal return should be DB_NOTFOUND, 0 is no error */
    if (ret != 0 && ret != DB_NOTFOUND)
    {
        fprintf(outpfile, "ERROR not DB_NOTFOUND saving types\n");
        return ret;
    }
    return 0;
}

/** SID_STORE
 *
 * This function writes the contents of the sid cache in ASCII to the file
 * specified through the outpfile parameter parameter
 *
 * Returns 0 on success, otherwise returns error code
 *
 * Use open_memstream to write to a message buffer
 */
int SID_cache_store(DBC *cursorp,
                    FILE *outpfile,
                    int db_records,
                    SID_server_list_t *sid_list)
{
    int ret = 0;                   /* Function return value */
    int i = 0;                     /* Loop index variable */
    DBT key, val;          	   /* Database variables */
/* V3 old version */
#if 0
    char *ATTRS = "ATTRS: ";       /* Tag for the number of attributes on */
                                   /*     the first line of dump file */
    char *SIDS = "SIDS: ";         /* Tag for the number of sids on the */
                                   /*     first line of the dump file */
#endif
    char tmp_sid_str[SID_STR_LEN]; /* Temporary string to hold the string */
                                   /*     representation of the sids */
    PVFS_SID tmp_sid;              /* Temporary SID to hold contents of */
                                   /*     the database */
    SID_cacheval_t *tmp_sid_attrs; /* Temporary SID_cacheval_t struct to */
                                   /*     hold contents of database */
    PVFS_SID sid_buffer;           /* Temporary SID */
    uint32_t db_flags;
    struct server_configuration_s *config_s = NULL;

    /* First Write SID File Header */
    fprintf(outpfile, "<ServerDefines>\n");

/* V3 old version */
#if 0
    /* Write the number of attributes in
     * the cache to the dump file's first line 
     */
    fprintf(outpfile, "%s", ATTRS);
    fprintf(outpfile, "%d\t", SID_NUM_ATTR);

    /* Write the number of sids in the cache
     * to the dump file's first line 
     */
    fprintf(outpfile, "%s", SIDS);
    fprintf(outpfile, "%d\n", db_records);
    
    /* Write the string representation of
     * the attributes in the sid cache to
     * the dump file's second line 
     */
    for(i = 0; i < SID_NUM_ATTR; i++)
    {
       fprintf(outpfile, "%s ", SID_attr_map[i]);
    }
    fprintf(outpfile, "%c", '\n');
#endif

    config_s = PINT_server_config_mgr_get_config(PVFS_FS_ID_NULL);

    /* Now Write SID Records */

    /* Initialize the database variables */
    SID_zero_dbt(&key, &val, NULL);

    /* key will be in a fixed buffer */
    key.data = &sid_buffer;
    key.size = sizeof(PVFS_SID);
    key.ulen = sizeof(PVFS_SID);
    key.flags = DB_DBT_USERMEM;
    /* val will be malloc'd and freed */
    val.flags = DB_DBT_MALLOC;

    /* this routine can either output a whole db or a list of SIDs
     */
    if (sid_list)
    {
        db_flags = DB_SET;
        SID_pop_query_list(sid_list, &sid_buffer, NULL, NULL, 0);
    }
    else
    {
        db_flags = DB_FIRST;
    }

    /* Iterate over the database to get the sids */
    while(cursorp->get(cursorp, &key, &val, db_flags) == 0)
    {
        char *alias;
        if (val.data)
        {
            SID_cacheval_unpack(&tmp_sid_attrs, &val);
            clean_free(val.data);
            val.data = NULL;
        }
        else
        {
            /* report an error */
            fprintf(outpfile,"\tERROR val.data empty after get\n");
            return -1;
        }
    
        PVFS_SID_cpy(&tmp_sid, (PVFS_SID *)key.data);
        PVFS_SID_bin2str(&tmp_sid, tmp_sid_str);            

        /* this is a sequential search - probably should make hash tbl */
        alias = PINT_config_get_host_alias_ptr(config_s, tmp_sid_attrs->url);

        fprintf(outpfile,"\t<ServerDef>\n");
        if (alias)
        {
            fprintf(outpfile,"\t\tAlias %s\n", alias);
        }
        fprintf(outpfile,"\t\tSID %s\n", tmp_sid_str);
        fprintf(outpfile,"\t\tAddress %s(%lld)\n",
                tmp_sid_attrs->url, lld(tmp_sid_attrs->bmi_addr));

/* V3 old version */
#if 0
        /* Write SID and address info */
        fprintf(outpfile, "%s ", tmp_sid_str);
        fprintf(outpfile, "%lld ", lld(tmp_sid_attrs->bmi_addr));
        fprintf(outpfile, "%s ", tmp_sid_attrs->url);
#endif

        /* Write the user attributes to the dump file */
        fprintf(outpfile,"\t\tAttributes ");
        for(i = 0; i < SID_NUM_ATTR; i++)
        {
            
            fprintf(outpfile,
                    "%s=%d ",
                    SID_attr_map[i],
                    tmp_sid_attrs->attr[i]);      
/* V3 old version */
#if 0
            fprintf(outpfile, "%d ", tmp_sid_attrs->attr[i]);      
#endif
        }
        fprintf(outpfile,"\n"); /* end of attributes */

        /* Write system attributes for the server */
        SID_type_store(&tmp_sid, outpfile);

        fprintf(outpfile, "\t</ServerDef>\n");

        SID_cacheval_free(&tmp_sid_attrs);

        if (sid_list)
        {
            if (qlist_empty(&sid_list->link))
            {
                break;
            }
            SID_pop_query_list(sid_list, &sid_buffer, NULL, NULL, 0);
        }
        else
        {
            db_flags = DB_NEXT;
        }
    }

    /* End of servers to write to file */
    fprintf(outpfile, "</ServerDefines>\n");
    return(ret);
}


/*
 * This function retrieves entries from a primary database and stores
 * them into a bulk buffer DBT. 
 *
 * The minimum amount that can be retrieved is 8 KB of entries.
 * 
 * You must specify a size larger than 8 KB by putting an amount into the
 * two size parameters.
 *
 * The output DBT is malloc'ed so take care to make sure it is freed,
 * either by using it in a bulk_insert or by manually freeing.
 *
 * If the cache is larger than the buffer, the entry that does not fit is
 * saved in bulk_next_key global variable.
*/
int SID_bulk_retrieve_from_sid_cache(int size_of_retrieve_kb,
                                     int size_of_retrieve_mb,
                                     DB  *dbp,
                                     DBC **dbcursorp,
                                     DBT *output)
{
    int ret = 0, size;
    DBT key = bulk_next_key;

    /* If the input size of the retrieve is
     * smaller than the minimum size then exit function with error 
     */
    if (BULK_MIN_SIZE > (size_of_retrieve_kb * KILOBYTE) +
                        (size_of_retrieve_mb * MEGABYTE))
    {
        gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                  "Size of bulk retrieve buffer must be greater than 8 KB\n");
        return (-1);
    }

    /* If cursor is open, close it so we can reopen it as a bulk cursor */
    if (*dbcursorp != NULL)
    {
        (*dbcursorp)->close(*dbcursorp);
    }

    /* Calculate size of buffer as size of kb + size of mb */
    size = (size_of_retrieve_kb * KILOBYTE) + (size_of_retrieve_mb * MEGABYTE);

    /* Malloc buffer DBT */
    output->data = malloc(size);
    if (output->data == NULL)
    {
        gossip_debug(GOSSIP_SIDCACHE_DEBUG, "Error sizing buffer\n");
        return (-1);
    }

    output->ulen = size;
    output->flags = DB_DBT_USERMEM;

    /* Open bulk cursor */
    if ((ret = dbp->cursor(dbp, NULL, dbcursorp, DB_CURSOR_BULK)) != 0) {
        free(output->data);
        gossip_debug(GOSSIP_SIDCACHE_DEBUG, "Error creating bulk cursor\n");
        return (ret);
    }

    /* Get items out of db as long as no error,
     * if error is not end of entries in db then save 
     * last unstored entry into global bulk_next_key
     */
    while(ret == 0)
    {
        ret = (*dbcursorp)->get(*dbcursorp,
                                &key,
                                output,
                                DB_MULTIPLE_KEY | DB_NEXT);
        if (ret != 0 && ret != DB_NOTFOUND)
        {
            bulk_next_key = key;
        }
    }

    (*dbcursorp)->close(*dbcursorp);

    return ret;
}


/*
 * This function inserts entries from the input bulk buffer DBT into a database.
 *
 * The function uses the output from SID_bulk_retrieve as its input.
 *
 * The malloc'ed bulk buffer DBT's are freed at the end of this function.
 */
int SID_bulk_insert_into_sid_cache(DB *dbp, DBT *input)
{
    int ret;
    void *opaque_p, *op_p;
    DBT output;
    size_t retklen, retdlen;
    void *retkey, *retdata;

    /* Malloc buffer DBT */
    output.data = malloc(input->size);
    if (!output.data)
    {
        gossip_err("%s: failed to malloc buffer\n", __func__);
        return -1;
    }
    output.ulen = input->size;
    output.flags = DB_DBT_USERMEM;

    /* Initialize bulk DBTs */
    DB_MULTIPLE_WRITE_INIT(opaque_p, &output);
    DB_MULTIPLE_INIT(op_p, input);

    /*
     * Read key and data from input bulk buffer into output bulk buffer
     *
     */
    while(1)
    {
        DB_MULTIPLE_KEY_NEXT(op_p,
                             input,
                             retkey,
                             retklen,
                             retdata,
                             retdlen);
        if (op_p == NULL)
        {
            break;
        }

        DB_MULTIPLE_KEY_WRITE_NEXT(opaque_p,
                                   &output,
                                   retkey,
                                   retklen,
                                   retdata,
                                   retdlen);
        if (opaque_p == NULL)
        {
            gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                           "Error cannot fit into write buffer\n");
            break;
        }
        /*gossip_debug(GOSSIP_SIDCACHE_DEBUG, "Record bulk added %s\n",
                    );
         */
    }

    /* Bulk insert of output bulk buffer */
    ret = dbp->put(dbp,
                   NULL,
                   &output,
                   NULL,
                   DB_MULTIPLE_KEY | DB_OVERWRITE_DUP);

    /* Free both bulk buffers */
    free(input->data);
    free(output.data);

    return ret;
}

/* <======================== DATABASE FUNCTIONS ==========================> */
/*
 * This function zeros out the DBT's and should be used before any
 * element is placed into the database
 */
void SID_zero_dbt(DBT *key, DBT *val, DBT *pkey)
{
    if(key != NULL)
    {
        memset(key, 0, sizeof(DBT));
    }
    if(val != NULL)
    {
        memset(val, 0, sizeof(DBT));
    }
    if(pkey != NULL)
    {
        memset(pkey, 0, sizeof(DBT));
    }
}

/*
 * This function creates and opens the environment handle
 *
 * Returns 0 on success, otherwise returns an error code.
 */
int SID_create_environment(DB_ENV **envp)
{
    int ret = 0;

    /* Setting the opening environement flags */
    u_int32_t flags =
        DB_CREATE     | /* Creating environment if it does not exist */
        DB_INIT_MPOOL | /* Initializing the memory pool (in-memory cache) */
        DB_PRIVATE;     /* Region files are not backed up by the file system
                           instead they are backed up by heap memory */

    /* Creating the environment. Returns 0 on success */
    ret = db_env_create(envp, /* Globacl environment pointer */
                        0);    /* Environment create flag (Must be set to 0) */

    if(ret)
    {
        gossip_err("Error creating environment handle : %s\n", db_strerror(ret));
        return(ret);
    }

    /* Setting the in cache memory size. Returns 0 on success */
    ret = (*envp)->set_cachesize(
                        /* Environment pointer */
                        *envp,
                        /* Size of the cache in GB. Defined in sidcache.h */
                        CACHE_SIZE_GB,
                        /* Size of the cache in MB. Defined in sidcache.h */
                        CACHE_SIZE_MB * MEGABYTE,
                        /* Number of caches to create */
                        1);

    if(ret)
    {
	gossip_err(
              "Error setting cache memory size for environment : %s\n",
              db_strerror(ret));
        return(ret);
    }

    /* Opening the environment. Returns 0 on success */
    ret = (*envp)->open(*envp, /* Environment pointer */
                        NULL,  /* Environment home directory */
                        flags, /* Environment open flags */
                        0);    /* Mmode used to create BerkeleyDB files */

    if(ret)
    {
        gossip_err("Error opening environment database handle : %s\n",
                   db_strerror(ret));
        return(ret);
    }

    return(ret);
}

/*
 * This function creates and opens the primary database handle, which
 * has DB_HASH as the access method
 *
 * Returns 0 on success. Otherwise returns error code.
 */
int SID_create_sid_cache(DB_ENV *envp, DB **dbp)
{
    int ret = 0;

    u_int32_t flags = DB_CREATE; /* Database open flags. Creates the database if
                                    it does not already exist */

    /* Setting the global bulk next key to zero */
    SID_zero_dbt(&bulk_next_key, NULL, NULL);

    ret = db_create(dbp,   /* Primary database pointer */
                    envp,  /* Environment pointer */
                    0);    /* Create flags (Must be set to 0 or DB_XA_CREATE) */
    if(ret)
    {
        gossip_err("Error creating handle for database : %s\n",
                   db_strerror(ret));
        return(ret);
    }

    ret = (*dbp)->open(*dbp,    /* Primary database pointer */
                       NULL,    /* Transaction pointer */
                       NULL,    /* On disk file that holds database */
                       NULL,    /* Optional logical database */
                       DB_HASH, /* Database access method */
                       flags,   /* Database opening flags */
                       0);      /* File mode. Default is 0 */
   if(ret)
   {
        gossip_err("Error opening handle for database : %s\n",
                   db_strerror(ret));
        return(ret);
   }

   return(ret);
}

/* 
 * This function creates, opens, and associates the secondary attribute
 * database handles and sets the database pointers in the secondary_dbs 
 * array to point at the correct database. The acccess method for the
 * secondary databases is DB_BTREE and allows for duplicate records with
 * the same key values
 *
 * Returns 0 on success, otherwise returns an error code
 */
int SID_create_secondary_dbs(
                        DB_ENV *envp,
                        DB *dbp,
                        DB *secondary_dbs[], 
                        int (* key_extractor_func[])(DB *pri,
                                                     const DBT *pkey,
                                                     const DBT *pdata,
                                                     DBT *skey))
{
    int ret = 0;
    int i = 0;
    DB *tmp_db = NULL;
    u_int32_t flags = DB_CREATE;

    ret = SID_initialize_secondary_dbs(secondary_dbs);
    if(ret)
    {
        gossip_err("Could not initialize secondary attribute db array\n");
        return(ret);
    }

    for(i = 0; i < SID_NUM_ATTR; i++)
    {
        /* Creating temp database */
        ret = db_create(&tmp_db, /* Database pointer */
                        envp,    /* Environment pointer */
                        0);      /* Create flags (Must be 0 or DB_XA_CREATE) */
        if(ret)
        {
            gossip_err("Error creating handle for database :  %s\n",
                       db_strerror(ret));
            return(ret);
        }

        /* Pointing database array pointer at secondary attribute database */
        secondary_dbs[i] = tmp_db;

        /* Setting opening flags for secondary database to allow duplicates */
        ret = tmp_db->set_flags(tmp_db, DB_DUPSORT);
        if(ret)
        {
            gossip_err("Error setting duplicate flag for database : %s\n",
                       db_strerror(ret));
            return(ret);
        }

        /* Opening secondary database and setting its type as DB_BTREE */
        ret = tmp_db->open(tmp_db,   /* Database pointer */
                           NULL,     /* Transaction pointer */
                           NULL,     /* On disk file that holds database */
                           NULL,     /* Optional logical database */
                           DB_BTREE, /* Database access method */
                           flags,    /* Open flags */
                           0);       /* File mode. 0 is the default */

        if(ret)
        {
            gossip_err("Error opening handle for database : %s\n",
                       db_strerror(ret));
            return(ret);
        }

        /* Associating the primary database to the secondary.
           Returns 0 on success */
        ret = dbp->associate(dbp,                      /* Primary db ptr */
                             NULL,                     /* TXN id */
                             tmp_db,                   /* Secondary db ptr */
                             key_extractor_func[i],    /* key extractor func */
                             0);                       /* Associate flags */

        if(ret)
        {
            gossip_err("Error associating the two databases %s\n",
                        db_strerror(ret));
            return(ret);
        }

    }

   return(0);
}

/* get the aggregate SID type for a given SID */
int SID_get_type(PVFS_SID *sid, uint32_t *typeval)
{
    int ret = 0;
    DBT type_sid_key;
    DBT type_sid_val;
    PVFS_SID kbuf;
    struct SID_type_s vbuf;

    SID_zero_dbt(&type_sid_key, &type_sid_val, NULL);

    type_sid_key.data = &kbuf;
    type_sid_key.size = sizeof(PVFS_SID);
    type_sid_key.ulen = sizeof(PVFS_SID);
    type_sid_key.flags = DB_DBT_USERMEM;

    type_sid_val.data = &vbuf;
    type_sid_val.size = sizeof(struct SID_type_s);
    type_sid_val.ulen = sizeof(struct SID_type_s);
    type_sid_val.flags = DB_DBT_USERMEM;

    /* type_sid_val is filled in by DB */

    ret = SID_index_cursor->get(SID_index_cursor,
                                &type_sid_key,
                                &type_sid_val,
                                DB_SET);
    /* don't modify typeval unless there is data present */
    if (ret == 0)
    {
        *typeval = 0;
    }
    /* halt on error or DB_NOTFOUND */
    while(ret == 0)
    {
        *typeval |= vbuf.server_type;
        ret = SID_index_cursor->get(SID_index_cursor,
                                       &type_sid_key,
                                       &type_sid_val,
                                       DB_NEXT);
    }
    if (ret != 0 && ret != DB_NOTFOUND)
    {
        return ret;
    }
    return 0;
}

/*-------------------------------------------------------------------
 * The server type tables map SID to individual types (one bit set in a
 * field).  There will often be multiple types for a given SID, and many
 * SIDs with the same type.  We need to be able to look these up by SID,
 * or by type so we have a main db (SID_type_db) and a secondary index
 * (SID_type_index) which is associated with the main db.  The main
 * db cannot be configured with dups, so we will make the key of
 * these tables type concatenated with SID, which is unique, for the
 * main db and SID concatenated with type, which is unique, for the
 * secondary index.  Neither will have anything in the value field.
 * Using this we can use a partial lookup to either find all types for a
 * SID, or all SIDs for a type.
 *
 * DB won't let us have DUPSORT on a primary table, so we're just going
 * ot using two plain tables with same data - which means we have to
 * be caseful when updating.
 * ------------------------------------------------------------------
 */

/* Don't need an extractor if we don't associate
 */
/* V3 remove this */
#if 0
/* Extractor for type database secondard index */
static int SID_type_sid_extractor(DB *pri,
                                  const DBT *pkey,
                                  const DBT *pval,
                                  DBT *skey)
{
    SID_type_index_key *sixbuf;
    SID_type_db_key *dbbuf;

    sixbuf = (SID_type_index_key *)malloc(sizeof(SID_type_index_key));
    dbbuf = (SID_type_db_key *)pkey->data;

    sixbuf->sid = dbbuf->sid;
    sixbuf->typeval = dbbuf->typeval;

    skey->data = sixbuf;
    skey->size = sizeof(SID_type_index_key);
    skey->flags = DB_DBT_APPMALLOC;
    return 0;
}
#endif

/* Creates table for storing server types.  There may be multiple types
 * for each server, but most servers are not all types.  We need to be
 * able to search for all servers of a given type.  We can't do this
 * easily with a field in the main table or by using a secondary index
 * so we record these attributes in this table.
 */
static int SID_create_type_table(void)
{
    int ret = 0;
    u_int32_t flags = DB_CREATE;

    /* Create type database */
    ret = db_create(&SID_type_db, /* Database pointer */
                    SID_envp,    /* Environment pointer */
                    0);          /* Create flags (Must be 0 or DB_XA_CREATE) */
    if(ret)
    {
        gossip_err("Error creating type database :  %s\n",
                   db_strerror(ret));
        return(ret);
    }

    /* Set open flags for type database to allow duplicates */
    ret = SID_type_db->set_flags(SID_type_db, DB_DUPSORT);
    if(ret)
    {
        gossip_err("Error setting duplicate flag for type database : %s\n",
                   db_strerror(ret));
        return(ret);
    }

    /* Open type database */
    ret = SID_type_db->open(SID_type_db,   /* Database pointer */
                            NULL,     /* Transaction pointer */
                            NULL,     /* On disk file that holds database */
                            NULL,     /* Optional logical database */
                            DB_HASH,  /* Database access method */
                            flags,    /* Open flags */
                            0);       /* File mode. 0 is the default */

    if(ret)
    {
        gossip_err("Error opening type database : %s\n",
                   db_strerror(ret));
        return(ret);
    }

    /* Create cursor for type db */
    ret = SID_type_db->cursor(SID_type_db,      /* Type db pointer */
                              NULL,             /* TXN id */
                              &SID_type_cursor, /* Cursor pointer */
                              0);               /* Cursor opening flags */
    if(ret)
    {
        gossip_err("Error creating cursor :  %s\n", db_strerror(ret));
        return(ret);
    }

    /* Now create a secondary index for looking up types by SID */
    ret = db_create(&SID_type_index, /* SID index pointer */
                    SID_envp,    /* Environment pointer */
                    0);          /* Create flags (Must be 0 or DB_XA_CREATE) */
    if(ret)
    {
        gossip_err("Error creating handle for database :  %s\n",
                   db_strerror(ret));
        return(ret);
    }

    /* Set open flags for type SID index to allow duplicates */
    ret = SID_type_index->set_flags(SID_type_index, DB_DUPSORT);
    if(ret)
    {
        gossip_err("Error setting duplicate flag for type SID index : %s\n",
                   db_strerror(ret));
        return(ret);
    }

    /* Open type SID index database */
    ret = SID_type_index->open(SID_type_index,   /* Database pointer */
                            NULL,     /* Transaction pointer */
                            NULL,     /* On disk file that holds database */
                            NULL,     /* Optional logical database */
                            DB_HASH,  /* Database access method */
                            flags,    /* Open flags */
                            0);       /* File mode. 0 is the default */
    if(ret)
    {
        gossip_err("Error opening type SID index : %s\n",
                   db_strerror(ret));
        return(ret);
    }

/* cannot use DUPSORT for a primary table, so we are getting away from
 * an associated or secondary index table - just use a reagular table
 */
#if 0
    /* Associate the type database to the type SID index */
    ret = SID_type_index->associate(SID_type_db,   /* Type db ptr */
                         NULL,                     /* TXN id */
                         SID_type_index,           /* Secondary db ptr */
                         SID_type_sid_extractor,   /* key extractor func */
                         0);                       /* Associate flags */
    if(ret)
    {
        gossip_err("Error associating the type SID index %s\n",
                    db_strerror(ret));
        return(ret);
    }
#endif

    /* Create cursor for type SID index */
    ret = SID_type_index->cursor(SID_type_index,    /* SID index pointer */
                              NULL,                 /* TXN id */
                              &SID_index_cursor,    /* Cursor pointer */
                              0);                   /* Cursor opening flags */
    if(ret)
    {
        gossip_err("Error creating type SID cursor: %s\n", db_strerror(ret));
        return(ret);
    }
    return(ret);
}

/* 
 * This function creates and opens the database cursors set to the secondary
 * attribute databases in the database cursor pointers array
 *
 * Returns 0 on success, otherwise returns an error code
 */
int SID_create_dbcs(DB *secondary_dbs[], DBC *db_cursors[])
{
    int i = 0;                /* Loop index variable */
    int  ret = 0;             /* Function return value */
    DBC *tmp_cursorp = NULL;  /* BerkeleyDB cursor used for opening
                                 cursors for all secondary databases */

    /* Initializing all the database cursors in the array to be
     * set to NULL 
     */
    ret = SID_initialize_database_cursors(db_cursors);
    if(ret)
    {
        gossip_err("Error initializing cursor array :  %s\n", db_strerror(ret));
        return(ret);
    }

    for(i = 0; i < SID_NUM_ATTR; i++)
    {
        ret = secondary_dbs[i]->cursor(
                                secondary_dbs[i], /* Secondary db pointer */
                                NULL,             /* TXN id */
                                &tmp_cursorp,     /* Cursor pointer */
                                0);               /* Cursor opening flags */
        
        /* Checking to make sure the cursor was created */
        if(ret)
        {
            gossip_err("Error creating cursor :  %s\n", db_strerror(ret));
            return(ret);
        }
        /* If the cursor was successfully created it is added to the
         * array of database cursors 
         */
        else
        {
            db_cursors[i] = tmp_cursorp;
        }
    }
    return(ret);
}

/*
 * This function closes the database cursors in the cursors pointer array
 *
 * On success 0 is returned, otherwise returns an error code
 */
int SID_close_dbcs(DBC *db_cursors[])
{
    int i = 0;
    int ret = 0;

    for(i = 0; i < SID_NUM_ATTR; i++)
    {
        /* If the cursor is opened then it is closed */
        if(db_cursors[i] != NULL)
        {
            ret = db_cursors[i]->close(db_cursors[i]);
            if(ret)
            {
                gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                             "Error closing cursor :  %s\n", db_strerror(ret));
                return(ret);
            }
        }
    }
    return(ret);
}

/*
 * This function closes the primary database, secondary attribute databases,
 * and environment handles
 *
 * Returns 0 on success, otherwisre returns an error code
 */
int SID_close_dbs_env(DB_ENV *envp, DB *dbp, DB *secondary_dbs[])
{
    int ret = 0;
    int i = 0;

    /* Closing the array of secondary attribute databases */ 
    for(i = 0; i < SID_NUM_ATTR; i++)
    {
        if(secondary_dbs[i] != NULL)
        {

            ret = secondary_dbs[i]->close(secondary_dbs[i], /*Secondary db ptr*/
                                          0);         /* Database close flags */

            /* Checking to make sure the secondary database has been closed */
            if(ret)
            {
                gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                             "Error closing attribute database : %s\n",
                             db_strerror(ret));
                return(ret);
            }
        }
    }

    /* Checking to make sure that database handle has been opened */
    if(dbp != NULL)
    {
        /* Closing the primary database. Returns 0 on success */
        ret = dbp->close(dbp, /* Primary database pointer */
                         0);  /* Database close flags */
    }

     if(ret)
    {
        gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                     "Error closing primary database : %s\n", db_strerror(ret));
        return(ret);
    }

    if (SID_type_cursor != NULL)
    {
        ret = SID_type_cursor->close(SID_type_cursor);
    }

    if (SID_type_db != NULL)
    {
        ret = SID_type_db->close(SID_type_db, 0);
    }

    if (SID_index_cursor != NULL)
    {
        ret = SID_index_cursor->close(SID_index_cursor);
    }

    if (SID_type_index != NULL)
    {
        ret = SID_type_index->close(SID_type_index, 0);
    }

    /* Checking to make sure the environment handle is open */
    if(envp != NULL)
    {
        ret  = envp->close(envp, /* Environment pointer */
                           0);   /* Environment close flags */
    }

    if(ret)
    {
        gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                     "Error closing environment :  %s\n",
                     db_strerror(ret));
        return(ret);
    }

    return(ret);
}

/* <======================== EXPORTED FUNCTIONS ==========================> */

/* called from startup routines, this gets the SID cache up and running
 * should work on server and client
 */
int SID_initialize(void)
{
    int ret = -1;
    static int initialized = 0;

    /* if already initialized bail out - we assume higher levels
     * prevent multiple threads from initializing so we won't deal
     * with that here.
     */
    if (initialized)
    {
        ret = 0;
        goto errorout;
    }

    /* create DBD env */
    ret = SID_create_environment(&SID_envp);
    if (ret < 0)
    {
        goto errorout;
    }

    /* create main DB */
    ret = SID_create_sid_cache(SID_envp, &SID_db);
    if (ret < 0)
    {
        goto errorout;
    }

    /* create secondary (attribute index) DBs */
    ret = SID_create_secondary_dbs(SID_envp,
                                   SID_db,
                                   SID_attr_index,
                                   SID_extract_key);
    if (ret < 0)
    {
        goto errorout;
    }

    /* create server type table */
    ret = SID_create_type_table();
    if (ret < 0)
    {
        goto errorout;
    }

    /* create cursors */
    ret = SID_create_dbcs(SID_attr_index, SID_attr_cursor);
    if (ret < 0)
    {
        SID_close_dbs_env(SID_envp, SID_db, SID_attr_index);
        goto errorout;
    }

    /* load entries from the configuration */

errorout:
    return ret;
}

/* called to load the contents of the SID cache from a file
 * so we do not have to discover everything
 */
int SID_load(const char *path)
{
    int ret = -1;
    char *filename = NULL;
    FILE *inpfile = NULL;
//    struct server_configuration_s *srv_conf;
//    int fnlen;
    struct stat sbuf;
    PVFS_fs_id fsid __attribute__ ((unused)) = PVFS_FS_ID_NULL;

    if (!path)
    {
#if 0
        /* figure out the path to the cached data file */
        srv_conf = PINT_server_config_mgr_get_config(fsid);
        fnlen = strlen(srv_conf->meta_path) + strlen("/SIDcache");
        filename = (char *)malloc(fnlen + 1);
        strncpy(filename, srv_conf->meta_path, fnlen + 1);
        strncat(filename, "/SIDcache", fnlen + 1);
        PINT_server_config_mgr_put_config(srv_conf);
#endif
    }
    else
    {
        filename = (char *)path;
    }

    /* check if file exists */
    ret = stat(filename, &sbuf);
    if (ret < 0)
    {
        if (errno == EEXIST)
        {
            /* file doesn't exist - not an error, but bail out */
            errno = 0;
            ret = 0;
        }
        goto errorout;
    }

    /* Opening input file */
    inpfile = fopen(filename, "r");
    if(!inpfile)
    {
        gossip_err("Could not open the file %s in "
                   "SID_load_cache_from_file\n",
                   filename);
        return(-1);
    }

    /* load cache from file */
    ret = SID_cache_load(SID_db, inpfile, &sids_in_cache);
    if (ret < 0)
    {
        /* something failed, close up the database */
        SID_close_dbcs(SID_attr_cursor);
        SID_close_dbs_env(SID_envp, SID_db, SID_attr_index);
        goto errorout;
    }

    fclose(inpfile);

errorout:
    return ret;
}

/* This loads all of the records in the given memory buffer into the
 * cache.  This is intended for servers to pass groups of SID cache
 * records around
 */
int SID_loadbuffer(const char *buffer, int size)
{
    int ret = 0;
    FILE *inpfile = NULL;

    /* Opening the buffer as a file to load the contents of the database from */
    inpfile = fmemopen((void *)buffer, size, "r");
    if(!inpfile)
    {
        gossip_err("Error opening dump buffer in SID_loadbuffer function\n");
        return(-1);
    }
    /* load buffer into the cache */
    ret = SID_cache_load(SID_db, inpfile, &sids_in_cache);
    fclose(inpfile);
    return ret;
}

/* THis writes the entire contents of the cache into a file specified by
 * path.
 *
 * called periodically to save the contents of the SID cache to a file
 * so we can reload at some future startup and not have to discover
 * everything
 */
int SID_save(const char *path)
{
    int ret = -1;
    char *filename = NULL;
    FILE *outpfile = NULL;
    DBC *cursorp = NULL;
//    struct server_configuration_s *srv_conf;
//    int fnlen;
    PVFS_fs_id fsid __attribute__ ((unused)) = PVFS_FS_ID_NULL;

    if (!path || !path[0])
    {
#if 0
        /* figure out the path to the cached data file */
        srv_conf = PINT_server_config_mgr_get_config(fsid);
        fnlen = strlen(srv_conf->meta_path) + strlen("/SIDcache");
        filename = (char *)malloc(fnlen + 1);
        strncpy(filename, srv_conf->meta_path, fnlen + 1);
        strncat(filename, "/SIDcache", fnlen + 1);
        PINT_server_config_mgr_put_config(srv_conf);
#endif
        outpfile = stderr; /* debugging goes to stderr */
    }
    else
    {
        filename = (char *)path;
        /* Opening the file to dump the contents of the database to */
        outpfile = fopen(filename, "w");
    }

    if(!outpfile)
    {
        gossip_err("Error opening dump file in SID_save function\n");
        return(-1);
    }

    /* Creating a cursor to iterate over the database contents */
    ret = (SID_db)->cursor(SID_db,   /* Primary database pointer */
                           NULL,     /* Transaction handle */
                           &cursorp, /* Database cursor that is created */
                           0);       /* Cursor create flags */

    if(ret)
    {
        gossip_err("Error occured when trying to create cursor in \
                    SID_save function: %s",
                    db_strerror(ret));
        goto errorout;
    }

    /* dump cache to the file */
    ret = SID_cache_store(cursorp, outpfile, sids_in_cache, NULL);

errorout:

    if (cursorp)
    {
        ret = cursorp->close(cursorp);
        if(ret)
        {
            gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                         "Error closing cursor in SID_save "
                         "function: %s\n",
                         db_strerror(ret));
        }
    }
    fclose(outpfile);
    return ret;
}

/* this dumps the contents of the cache corresponding to the
 * SIDs in list into a memory buffer where it can then be dealt
 * with
 */
int SID_savelist(char *buffer, int size, SID_server_list_t *slist)
{
    int ret = -1;
    FILE *outpfile = NULL;
    DBC *cursorp = NULL;

    /* Opening the buffer as a file to dump the contents of the database to */
    outpfile = fmemopen(buffer, size, "w");
    if(!outpfile)
    {
        gossip_err("Error opening dump buffer in SID_savelist function\n");
        return(-1);
    }

    /* Creating a cursor to iterate over the database contents */
    ret = (SID_db)->cursor(SID_db,   /* Primary database pointer */
                           NULL,     /* Transaction handle */
                           &cursorp, /* Database cursor that is created */
                           0);       /* Cursor create flags */

    if(ret)
    {
        gossip_err("Error occured when trying to create cursor in \
                    SID_savelist function: %s",
                    db_strerror(ret));
        goto errorout;
    }

    /* dump cache to the buffer */
    ret = SID_cache_store(cursorp, outpfile, size, slist);

errorout:

    if (cursorp)
    {
        ret = cursorp->close(cursorp);
        if(ret)
        {
            gossip_debug(GOSSIP_SIDCACHE_DEBUG,
                         "Error closing cursor in SID_savelist "
                         "function: %s\n",
                         db_strerror(ret));
            return(ret);
        }
    }
    fclose(outpfile);
    return ret;
}

/* Add one entry to the SID cache */
int SID_add(const PVFS_SID *sid,
            PVFS_BMI_addr_t bmi_addr,
            const char *url,
            int attributes[])
{
    int ret = 0;
    SID_cacheval_t *cval;

    /* This allocates and fills in the cacheval */
    ret = SID_cacheval_alloc(&cval, attributes, bmi_addr, url);

#if 0
    /* ALL of this is taken care of in cacheval_alloc */
    cval = (SID_cacheval_t *)malloc(sizeof(SID_cacheval_t) + strlen(url) + 1);
    if(!cval)
    {
        gossip_err("%s: unable to malloc cacheeval\n", __func__);
        return -1;
    }
    /* zero out the cval - WBL probably not needed */
    memset(cval, 0, sizeof(SID_cacheval_t) + strlen(url) + 1);
    /* intialize struct */
    SID_cacheval_init(cval);

    /* the url array is placed immediately after the rest of the cval */
    cval->url = (char *)&cval[1];
    strcpy(cval->url, url);
    cval->bmi_addr = bmi_addr;
#endif

    /* if bmi_addr is zero, should register with BMI */
    if (cval->bmi_addr == 0)
    {
        /* enter url into BMI to get BMI addr */
        ret = BMI_addr_lookup(&cval->bmi_addr, cval->url, NULL);
        if (ret != 0 && ret != -BMI_NOTINITIALIZED)
        {   
            free(cval);
            return ret;
        }
    }

#if 0
    if (attributes)
    {
        /* this assumes we are adding a new record so we are not
         * overwriting existing attribtes
         */
        for (i = 0; i < SID_NUM_ATTR; i++)
        {
            cval->attr[i] = attributes[i];
        }
    }
#endif

    /* if a server already exists this should fail and we
     * just move on to the next one
     */
    ret = SID_cache_put(SID_db, sid, cval, &sids_in_cache);
    if (ret == DB_KEYEXIST)
    {
        /* this means server already exists which we ignore */
        ret = 0; 
    }
    free(cval);
    return ret;
}

/* Delete one entry from the SID cache */
int SID_delete(const PVFS_SID *sid)
{
    int ret = 0;
    ret = SID_cache_delete_server(SID_db, sid, &sids_in_cache);
    return ret;
}

/* called from shutdown routines, this closes up the BDB constructs
 * and saves the contents to a file
 */
int SID_finalize(void)
{
    int ret = -1;

#if 0
    /* save cache contents to a file */
    ret = SID_save(NULL);
    if (ret < 0)
    {
        return ret;
    }
#endif

    /* close cursors */
    ret = SID_close_dbcs(SID_attr_cursor);
    if (ret < 0)
    {
        return ret;
    }

    /* close DBs and env */
    ret = SID_close_dbs_env(SID_envp, SID_db, SID_attr_index);
    if (ret < 0)
    {
        return ret;
    }

    return ret;
}

int SID_update_type(const PVFS_SID *sid, struct SID_type_s *new_server_type)
{
    int ret = 0;
    ret = SID_cache_update_type(sid, new_server_type);
    return ret;
}

int SID_update_type_single(const PVFS_SID *sid,
                           struct SID_type_s *new_server_type)
{
    int ret = 0;
    ret = SID_cache_update_type_single(sid,
                                       new_server_type->server_type,
                                       new_server_type->fsid);
    return ret;
}

int SID_update_attributes(const PVFS_SID *sid_server, int new_attr[])
{   
    int ret = 0;
    ret = SID_cache_update_attrs(SID_db, sid_server, new_attr);
    return ret;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
