/* 
 * (C) 2008 Clemson University and The University of Chicago 
 *
 * See COPYING in top-level directory.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include "pvfs2.h"
#include "pvfs2-types.h"
#include "pint-eattr.h"
#include "pvfs2-req-proto.h"
#include "pvfs2-internal.h"
#include "gossip.h"
#include "gen-locks.h"
#include "server-config.h"
#include "pint-cached-config.h"
#include "pint-util.h"

#include "pint-security.h"
#include "security-hash.h"


static gen_mutex_t security_init_mutex = GEN_MUTEX_INITIALIZER;
static int security_init_status = 0;

#ifndef SECURITY_ENCRYPTION_NONE

/* the private key used for signing */
static EVP_PKEY *security_privkey = NULL;


static int load_private_key(const char*);
static int load_public_keys(const char*);

#endif /* SECURITY_ENCRYPTION_NONE */


/*  PINT_security_initialize	
 *
 *  Initializes the security module
 *	
 *  returns PVFS_EALREADY if already initialized
 *  returns PVFS_EIO if key file is missing or invalid
 *  returns 0 on sucess
 */
int PINT_security_initialize(void)
{
#ifndef SECURITY_ENCRYPTION_NONE
    const struct server_configuration_s *conf;
#endif /* SECURITY_ENCRYPTION_NONE */
    int ret;

    gen_mutex_lock(&security_init_mutex);
    if (security_init_status)
    {
        gen_mutex_unlock(&security_init_mutex);
        return -PVFS_EALREADY;
    }

    ERR_load_crypto_strings();
    OpenSSL_add_all_algorithms();

    ret = SECURITY_hash_initialize();
    if (ret < 0)
    {
        EVP_cleanup();
        ERR_free_strings();
        gen_mutex_unlock(&security_init_mutex);
        return ret;
    }

#ifndef SECURITY_ENCRYPTION_NONE

    conf = PINT_get_server_config();

    assert(conf->serverkey_path);
    ret = load_private_key(conf->serverkey_path);
    if (ret < 0)
    {
        SECURITY_hash_finalize();
        EVP_cleanup();
        ERR_free_strings();
        gen_mutex_unlock(&security_init_mutex);
        return -PVFS_EIO;
    }

    assert(conf->keystore_path);
    ret = load_public_keys(conf->keystore_path);
    if (ret < 0)
    {
        EVP_PKEY_free(security_privkey);
        SECURITY_hash_finalize();
        EVP_cleanup();
        ERR_free_strings();
        gen_mutex_unlock(&security_init_mutex);
        return -PVFS_EIO;
    }

#endif /* SECURITY_ENCRYPTION_NONE */

    security_init_status = 1;
    gen_mutex_unlock(&security_init_mutex);
 
    return 0;
}

/*  PINT_security_finalize	
 *
 *  Finalizes the security module
 *	
 *  returns PVFS_EALREADY if already finalized
 *  returns 0 on sucess
 */
int PINT_security_finalize(void)
{
    gen_mutex_lock(&security_init_mutex);
    if (!security_init_status)
    {
        gen_mutex_unlock(&security_init_mutex);
        return -PVFS_EALREADY;
    }

    SECURITY_hash_finalize();

#ifndef SECURITY_ENCRYPTION_NONE
    EVP_PKEY_free(security_privkey);
#endif

    EVP_cleanup();
    ERR_free_strings();

    security_init_status = 0;
    gen_mutex_unlock(&security_init_mutex);
    
    return 0;
}

#ifndef SECURITY_ENCRYPTION_NONE

/*  PINT_sign_capability
 *
 *  Takes in a PVFS_capability structure and creates a signature
 *  based on the input data
 *
 *  returns 0 on success
 *  returns -1 on error
 */
int PINT_sign_capability(PVFS_capability *cap)
{
    const struct server_configuration_s *conf;
    EVP_MD_CTX mdctx;
    char buf[256];
    const EVP_MD *md;
    int ret;

    assert(security_privkey);

    conf = PINT_get_server_config();

    cap->timeout = PINT_util_get_current_time();
    cap->timeout += conf->security_timeout;

#if defined(SECURITY_ENCRYPTION_RSA)
    md = EVP_sha1();
#elif defined(SECURITY_ENCRYPTION_DSA)
    md = EVP_dss1();
#endif

    EVP_MD_CTX_init(&mdctx);

    ret = EVP_SignInit_ex(&mdctx, md, NULL);
    if (!ret)
    {
        gossip_debug(GOSSIP_SECURITY_DEBUG, "Error signing capability: "
                         "%s\n", ERR_error_string(ERR_get_error(), buf));
        EVP_MD_CTX_cleanup(&mdctx);
        return -1;
    }

    ret = EVP_SignUpdate(&mdctx, &cap->owner, sizeof(PVFS_handle));
    ret &= EVP_SignUpdate(&mdctx, &cap->fsid, sizeof(PVFS_fs_id));
    ret &= EVP_SignUpdate(&mdctx, &cap->timeout, sizeof(PVFS_time));
    ret &= EVP_SignUpdate(&mdctx, &cap->op_mask, sizeof(uint32_t));
    ret &= EVP_SignUpdate(&mdctx, &cap->num_handles, sizeof(uint32_t));
    if (cap->num_handles)
    {
        ret &= EVP_SignUpdate(&mdctx, cap->handle_array, cap->num_handles * 
                          sizeof(PVFS_handle));
    }

    if (!ret)
    {
        gossip_debug(GOSSIP_SECURITY_DEBUG, "Error signing capability: "
                         "%s\n", ERR_error_string(ERR_get_error(), buf));
        EVP_MD_CTX_cleanup(&mdctx);
        return -1;
    }

    ret = EVP_SignFinal(&mdctx, cap->signature, &cap->sig_size, 
                        security_privkey);
    if (!ret)
    {
        gossip_debug(GOSSIP_SECURITY_DEBUG, "Error signing capability: "
                         "%s\n", ERR_error_string(ERR_get_error(), buf));
        EVP_MD_CTX_cleanup(&mdctx);
        return -1;
    }

    EVP_MD_CTX_cleanup(&mdctx);

    return 0;
}

/*  PINT_verify_capability
 *
 *  Takes in a PVFS_capability structure and checks to see if the
 *  signature matches the contents based on the data within
 *
 *  returns 1 on success
 *  returns 0 on error or failure to verify
 */
int PINT_verify_capability(PVFS_capability *data)
{
    EVP_MD_CTX mdctx;
    const EVP_MD *md;
    int ret;
    char *buf;
    EVP_PKEY *pubkey;
    
    if (!data)
    {
        return 0;
    }

    if (PINT_util_get_current_time() > data->timeout)
    {
        return 0;
    }
    
    buf = (char *)malloc(sizeof(char) * 1024);
    
    if (buf == NULL)
    {
        return 0;
    }
    
    ret = PINT_cached_config_get_server_name(buf, 1024, data->owner,
                                             data->fsid);
    
    if (ret < 0)
    {
        gossip_debug(GOSSIP_SECURITY_DEBUG, "Server name lookup failed.\n");
        free(buf);
        return 0;
    }
    
    pubkey = SECURITY_lookup_pubkey(buf);
        
    if (pubkey == NULL)
    {
        gossip_debug(GOSSIP_SECURITY_DEBUG,
                     "Public key not found in lookup. Name used: %s\n", buf);
        return 0;
    }
    free(buf);

#if defined(SECURITY_ENCRYPTION_RSA)
    md = EVP_sha1();
#elif defined(SECURITY_ENCRYPTION_DSA)
    md = EVP_dss1();
#endif

    EVP_MD_CTX_init(&mdctx);
    ret = EVP_VerifyInit_ex(&mdctx, md, NULL);
    if (ret)
    {
        ret = EVP_VerifyUpdate(&mdctx, &(data->owner), sizeof(PVFS_handle));
        ret &= EVP_VerifyUpdate(&mdctx, &(data->fsid), sizeof(PVFS_fs_id));
        ret &= EVP_VerifyUpdate(&mdctx, &(data->timeout), sizeof(PVFS_time));
        ret &= EVP_VerifyUpdate(&mdctx, &(data->op_mask), sizeof(uint32_t));
        ret &= EVP_VerifyUpdate(&mdctx, &(data->num_handles),
                                sizeof(uint32_t));
        if (data->num_handles)
        {
            ret &= EVP_VerifyUpdate(&mdctx, data->handle_array,
                                sizeof(PVFS_handle) * data->num_handles);
        }
        if (ret)
        {
            ret = EVP_VerifyFinal(&mdctx, data->signature, data->sig_size, 
                                  pubkey);
        }
        else 
        {
            gossip_debug(GOSSIP_SECURITY_DEBUG, "VerifyUpdate failure.\n");
            EVP_MD_CTX_cleanup(&mdctx);
            return 0;
        }
    }
    else
    {
        gossip_debug(GOSSIP_SECURITY_DEBUG, "VerifyInit failure.\n");
        EVP_MD_CTX_cleanup(&mdctx);
        return 0;
    }
    
    EVP_MD_CTX_cleanup(&mdctx);

    return (ret == 1);
}

/*  PINT_verify_credential
 *
 *  Takes in a PVFS_credential structure and checks to see if the
 *  signature matches the contents based on the data within
 *
 *  returns 1 on success
 *  returns 0 on error or failure to verify
 */
int PINT_verify_credential(PVFS_credential *cred)
{
    EVP_MD_CTX mdctx;
    const EVP_MD *md;
    EVP_PKEY *pubkey;
    int ret;

    if (!cred)
    {
        return 0;
    }

    if (PINT_util_get_current_time() > cred->timeout)
    {
        return 0;
    }

    /* TODO: check revocation list against cred->serial */

    pubkey = SECURITY_lookup_pubkey(cred->issuer_id);
    if (pubkey == NULL)
    {
        gossip_debug(GOSSIP_SECURITY_DEBUG,
                     "Public key not found for issuer: %s\n", 
                     cred->issuer_id);
        return 0;
    }

#if defined(SECURITY_ENCRYPTION_RSA)
    md = EVP_sha1();
#elif defined(SECURITY_ENCRYPTION_DSA)
    md = EVP_dss1();
#endif

    EVP_MD_CTX_init(&mdctx);
    ret = EVP_VerifyInit_ex(&mdctx, md, NULL);
    if (!ret)
    {
        gossip_debug(GOSSIP_SECURITY_DEBUG, "VerifyInit failure.\n");
        EVP_MD_CTX_cleanup(&mdctx);
        return 0;
    }

    ret = EVP_VerifyUpdate(&mdctx, &cred->serial, sizeof(uint32_t));
    ret &= EVP_VerifyUpdate(&mdctx, &cred->userid, sizeof(PVFS_uid));
    ret &= EVP_VerifyUpdate(&mdctx, &cred->num_groups, sizeof(uint32_t));
    if (cred->num_groups)
    {
        ret &= EVP_VerifyUpdate(&mdctx, cred->group_array,
                                cred->num_groups * sizeof(PVFS_gid));
    }
    ret &= EVP_VerifyUpdate(&mdctx, cred->issuer_id,
                            (strlen(cred->issuer_id) + 1) * sizeof(char));
    ret &= EVP_VerifyUpdate(&mdctx, &cred->timeout, sizeof(PVFS_time));
    ret &= EVP_VerifyUpdate(&mdctx, &cred->sig_size, sizeof(uint32_t));
    if (!ret)
    {
        gossip_debug(GOSSIP_SECURITY_DEBUG, "VerifyUpdate failure.\n");
        EVP_MD_CTX_cleanup(&mdctx);
        return 0;
    }

    ret = EVP_VerifyFinal(&mdctx, cred->signature, cred->sig_size, pubkey);

    EVP_MD_CTX_cleanup(&mdctx);

    return (ret == 1);
}

/* load_private_key
 *
 * Reads the private key from a file in PEM format.
 *
 * returns -1 on error
 * returns 0 on success
 */
static int load_private_key(const char *path)
{
    FILE *keyfile;
    char buf[256];

    keyfile = fopen(path, "r");
    if (keyfile == NULL)
    {
        gossip_err("%s: %s\n", path, strerror(errno));
        return -1;
    }

    security_privkey = PEM_read_PrivateKey(keyfile, NULL, NULL, NULL);
    if (security_privkey == NULL)
    {
        gossip_debug(GOSSIP_SECURITY_DEBUG, "Error loading private key: "
                         "%s\n", ERR_error_string(ERR_get_error(), buf));
        fclose(keyfile);
    }

    fclose(keyfile);

    return 0;
}

/*  load_public_keys
 *
 *  Internal function to load keys from a file.
 *  File path includes the filename
 *  When finished without error, hash table will be filled
 *  with all host ID / public key pairs.
 *	
 *  returns -1 on error
 *  returns 0 on success
 */
static int load_public_keys(const char *path)
{
    FILE *keyfile;
    int ch, ptr;
    static char buf[1024];
    EVP_PKEY *key;
    char *host;
    int ret;

    keyfile = fopen(path, "r");
    if (keyfile == NULL)
    {
        gossip_err("%s: %s\n", path, strerror(errno));
        return -1;
    }

    ch = fgetc(keyfile);

    while (ch != EOF)
    {
        while (isspace(ch))
        {
            ch = fgetc(keyfile);
        }

        if (ch == EOF)
        {
            break;
        }

        if (!isalnum(ch))
        {
            fclose(keyfile);
            return -1;
        }

        for (ptr = 0; (ptr < 1023) && isalnum(ch); ptr++)
        {
            buf[ptr] = (char)ch;
            ch = fgetc(keyfile);
            if (ch == EOF)
            {
                fclose(keyfile);
                return -1;
            }
        }
        buf[ptr] = '\0';

        do
        {
            ch = fgetc(keyfile);
        } while(isspace(ch));

        ungetc(ch, keyfile);

        key = PEM_read_PUBKEY(keyfile, NULL, NULL, NULL);
        if (key == NULL)
        {
            gossip_debug(GOSSIP_SECURITY_DEBUG, "Error loading public key: "
                         "%s\n", ERR_error_string(ERR_get_error(), buf));
            fclose(keyfile);
            return -1;
        }

        host = PINT_config_get_host_addr_ptr(PINT_get_server_config(), buf);
        if (host == NULL)
        {
            gossip_debug(GOSSIP_SECURITY_DEBUG, "Alias '%s' not found "
                         "in configuration\n", buf);
        }
        else
        {
            ret = SECURITY_add_pubkey(host, key);
            if (ret < 0)
            {
                PVFS_strerror_r(ret, buf, 1024);
                gossip_debug(GOSSIP_SECURITY_DEBUG, "Error inserting public "
                             "key: %s\n", buf);
                fclose(keyfile);
                return -1;
            }
        }

        ch = fgetc(keyfile);
    }

    fclose(keyfile);

    return 0;
}

#else /* SECURITY_ENCRYPTION_NONE */

/*  PINT_sign_capability
 *
 *  placeholder for when security is disabled, sets sig size to zero
 */
int PINT_sign_capability(PVFS_capability *cap)
{
    cap->sig_size = 0;

    return 0;
}

/*  PINT_verify_capability
 *
 *  placeholder for when security is disabled, always verifies
 */
int PINT_verify_capability(PVFS_capability *cap)
{
    int ret;
    
    if (PINT_util_get_current_time() > cap->timeout)
    {
        ret = 0;
    }
    else
    {
        ret = 1;
    }

    return ret;
}

/*  PINT_verify_credential
 *
 *  placeholder for when security is disabled, always verifies
 */
int PINT_verify_credential(PVFS_credential *cred)
{
    int ret;
    
    if (PINT_util_get_current_time() > cred->timeout)
    {
        ret = 0;
    }
    else
    {
        ret = 1;
    }

    return ret;
}

#endif /* SECURITY_ENCRYPTION_NONE */

/*  PINT_init_capability
 *
 *  Function to call after creating an initial capability
 *  structure to initialize needed memory space for the signature.
 *  Sets all fields to 0 or NULL to be safe
 *	
 *  returns -PVFS_ENOMEM on error
 *  returns -PVFS_EINVAL if passed an invalid structure
 *  returns 0 on success
 */
int PINT_init_capability(PVFS_capability *cap)
{
    int ret = 0;
    
    if (cap)
    {
        memset(cap, 0, sizeof(PVFS_capability));

#ifndef SECURITY_ENCRYPTION_NONE
        cap->signature = 
            (unsigned char*)calloc(1, EVP_PKEY_size(security_privkey));
        if (cap->signature == NULL)
        {
            ret = -PVFS_ENOMEM;
        }
#endif /* SECURITY_ENCRYPTION_NONE */
    }
    else
    {
        ret = -PVFS_EINVAL;
    }

    return ret;
}

/*  PINT_dup_capability
 *
 *  When passed a valid capability pointer this function will duplicate
 *  it and return the copy.  User must make sure to free both the new and
 *  old capabilities as normal.
 *	
 *  returns NULL on error
 *  returns valid PVFS_capability * on success
 */
PVFS_capability *PINT_dup_capability(const PVFS_capability *cap)
{
    PVFS_capability *ret = NULL;
    
    if (!cap)
    {
        return NULL;
    }

    ret = (PVFS_capability*)malloc(sizeof(PVFS_capability));
    if (!ret)
    {
        return NULL;
    }
    memcpy(ret, cap, sizeof(PVFS_capability));
    ret->signature = NULL;
    ret->handle_array = NULL;

#ifndef SECURITY_ENCRYPTION_NONE
    ret->signature = (unsigned char*)malloc(EVP_PKEY_size(security_privkey));
    if (!ret->signature)
    {
        free(ret);
        return NULL;
    }
    memcpy(ret->signature, cap->signature, cap->sig_size);
#endif /* SECURITY_ENCRYPTION_NONE */

    if (cap->num_handles)
    {
        ret->handle_array = calloc(cap->num_handles, sizeof(PVFS_handle));
        if (!ret->handle_array)
        {
            free(ret->signature);
            free(ret);
            return NULL;
        }
        memcpy(ret->handle_array, 
               cap->handle_array, 
               cap->num_handles * sizeof(PVFS_handle));
    }
    
    return ret;
}

/*  PINT_release_capability
 *
 *  Frees any memory associated with a capability structure.
 *	
 *  no return value
 */
void PINT_release_capability(PVFS_capability *cap)
{
    if (cap)
    {
    	free(cap->signature);
    	free(cap->handle_array);
    	free(cap);
    }
}

/*  PINT_get_max_sigsize
 *
 *  This function probably won't get used, was initially used in the encode
 *  and decode process although a workaround was found to avoid linking the
 *  security module in.  Possibly useful later down the road.
 *	
 *  returns < 0 on error
 *  returns maximum signature size on success
 */
int PINT_get_max_sigsize(void)
{
#ifndef SECURITY_ENCRYPTION_NONE
    return EVP_PKEY_size(security_privkey);
#else /* security disabled */
    return 0;
#endif /* SECURITY_ENCRYPTION_NONE */
}

void PINT_release_credential(PVFS_credential *cred)
{
    if (cred)
    {
        free(cred->group_array);
        free(cred->issuer_id);
        free(cred->signature);
        free(cred);
    }
}

PVFS_credential *PINT_dup_credential(const PVFS_credential *cred)
{
    PVFS_credential *ret = NULL;

    if (!cred)
    {
        return NULL;
    }

    ret = (PVFS_credential*)malloc(sizeof(PVFS_credential));
    if (!ret)
    {
        return NULL;
    }

    memcpy(ret, cred, sizeof(PVFS_credential));
    ret->group_array = NULL;
    ret->issuer_id = NULL;
    ret->signature = NULL;

    if (cred->num_groups)
    {
        ret->group_array = calloc(cred->num_groups, sizeof(PVFS_gid));
        if (!ret->group_array)
        {
            free(ret);
            return NULL;
        }
        memcpy(ret->group_array, cred->group_array, 
               cred->num_groups * sizeof(PVFS_gid));
    }

    ret->issuer_id = strdup(cred->issuer_id);
    if (!ret->issuer_id)
    {
        free(ret->group_array);
        free(ret);
        return NULL;
    }

    ret->signature = (unsigned char*)calloc(cred->sig_size, 1);
    if (!ret->signature)
    {
        free(ret->issuer_id);
        free(ret->group_array);
        free(ret);
        return NULL;
    }
    memcpy(ret->signature, cred->signature, cred->sig_size);

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
