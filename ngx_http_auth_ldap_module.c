/**
 * Copyright (C) 2011-2013 Valery Komarov <komarov@valerka.net>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_sha1.h>
#include <ldap.h>

typedef struct {
    ngx_str_t username;
    ngx_str_t password;
} ngx_ldap_userinfo;

typedef struct {
    ngx_str_t   value;
    ngx_array_t *lengths;
    ngx_array_t *values;
} ngx_ldap_require_t;

typedef struct {
    LDAPURLDesc *ludpp;
    ngx_str_t url;
    ngx_str_t alias;

    ngx_str_t bind_dn;
    ngx_str_t bind_dn_passwd;

    ngx_str_t group_attribute;
    ngx_flag_t group_attribute_dn;

    ngx_array_t *require_group;     /* array of ngx_ldap_require_t */
    ngx_array_t *require_user;      /* array of ngx_ldap_require_t */
    ngx_flag_t require_valid_user;
    ngx_flag_t satisfy_all;
} ngx_ldap_server;

typedef struct {
    ngx_str_t realm;
    ngx_array_t *servers;
} ngx_http_auth_ldap_loc_conf_t;

typedef struct {
    ngx_array_t *servers;     /* array of ngx_ldap_server */
    ngx_hash_t srv;
} ngx_http_auth_ldap_conf_t;



// the shm segment that houses the used cache nodes tree
static ngx_uint_t      ngx_http_auth_ldap_shm_size;
static ngx_shm_zone_t *ngx_http_auth_ldap_shm_zone;
static ngx_rbtree_t   *ngx_http_auth_ldap_rbtree;
// nonce cleanup
#define NGX_HTTP_AUTH_LDAP_CLEANUP_INTERVAL 3000
#define NGX_HTTP_AUTH_LDAP_CLEANUP_BATCH_SIZE 2048
ngx_event_t *ngx_http_auth_ldap_cleanup_timer;
static ngx_array_t *ngx_http_auth_ldap_cleanup_list;
static ngx_atomic_t *ngx_http_auth_ldap_cleanup_lock;

// nonce entries in the rbtree
typedef struct {
    ngx_rbtree_node_t node;    // the node's .key is derived from the nonce val
    time_t            expires; // time at which the node should be evicted
    u_char username[100];
    u_char password_hash[SHA_DIGEST_LENGTH+1];
    u_char server_alias[100];
    u_char client_ip[30];
} ngx_http_auth_ldap_node_t;

static void * ngx_http_auth_ldap_create_conf(ngx_conf_t *cf);
static char * ngx_http_auth_ldap_ldap_server_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char * ngx_http_auth_ldap_parse_url(ngx_conf_t *cf, ngx_ldap_server *server);
static char * ngx_http_auth_ldap_parse_require(ngx_conf_t *cf, ngx_ldap_server *server);
static char * ngx_http_auth_ldap_parse_satisfy(ngx_conf_t *cf, ngx_ldap_server *server);
static char * ngx_http_auth_ldap_ldap_server(ngx_conf_t *cf, ngx_command_t *dummy, void *conf);
static ngx_int_t ngx_http_auth_ldap_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_auth_ldap_init(ngx_conf_t *cf);
static void * ngx_http_auth_basic_create_loc_conf(ngx_conf_t *);
static char * ngx_http_auth_ldap_merge_loc_conf(ngx_conf_t *, void *, void *);
static ngx_int_t ngx_http_auth_ldap_authenticate_against_server(ngx_http_request_t *r, ngx_ldap_server *server,
        ngx_ldap_userinfo *uinfo, ngx_http_auth_ldap_loc_conf_t *conf);
static ngx_int_t ngx_http_auth_ldap_set_realm(ngx_http_request_t *r, ngx_str_t *realm);
static ngx_ldap_userinfo * ngx_http_auth_ldap_get_user_info(ngx_http_request_t *);
static ngx_int_t ngx_http_auth_ldap_authenticate(ngx_http_request_t *r, ngx_http_auth_ldap_loc_conf_t *conf,
        ngx_http_auth_ldap_conf_t *mconf);
static char * ngx_http_auth_ldap(ngx_conf_t *cf, void *post, void *data);
static ngx_conf_post_handler_pt ngx_http_auth_ldap_p = ngx_http_auth_ldap;
static ngx_int_t ngx_http_auth_ldap_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data);
static void ngx_http_auth_ldap_rbtree_insert(ngx_rbtree_node_t *temp,
       ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);
static int ngx_http_auth_ldap_rbtree_cmp(const ngx_rbtree_node_t *v_left,
       const ngx_rbtree_node_t *v_right);
static void ngx_rbtree_generic_insert(ngx_rbtree_node_t *temp, ngx_rbtree_node_t *node,
       ngx_rbtree_node_t *sentinel, int (*compare)(const ngx_rbtree_node_t *left, const ngx_rbtree_node_t *right));
void ngx_http_auth_ldap_cleanup(ngx_event_t *ev);
static ngx_int_t ngx_http_auth_ldap_worker_init(ngx_cycle_t *cycle);
static void ngx_http_auth_ldap_rbtree_prune(ngx_log_t *log);
static void ngx_http_auth_ldap_rbtree_prune_walk(ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel, time_t now, ngx_log_t *log);
static ngx_http_auth_ldap_node_t ngx_http_auth_ldap_cache_store(ngx_http_request_t *r, ngx_ldap_userinfo *uinfo, ngx_ldap_server *server);
static ngx_rbtree_node_t * ngx_http_auth_ldap_rbtree_find(ngx_rbtree_key_t key, ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);
static ngx_uint_t nginx_http_auth_ldap_get_cache_key (ngx_ldap_userinfo *uinfo);
//static ngx_str_t ngx_http_auth_ldap_get_password_hash (ngx_str_t *username, ngx_str_t *password);
//static void ngx_http_auth_ldap_get_password_hash (const ngx_str_t *username, const ngx_str_t *password, ngx_str_t *hash);
static void ngx_http_auth_ldap_get_password_hash (ngx_http_request_t *r, const ngx_str_t *username, const ngx_str_t *password, u_char *hash);

static ngx_command_t ngx_http_auth_ldap_commands[] = {
    {
        ngx_string("ldap_server"),
        NGX_HTTP_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_TAKE1,
        ngx_http_auth_ldap_ldap_server_block,
        NGX_HTTP_MAIN_CONF_OFFSET,
        0,
        NULL
    },
    {
        ngx_string("auth_ldap"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LMT_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_auth_ldap_loc_conf_t, realm),
        &ngx_http_auth_ldap_p
    },
    {
        ngx_string("auth_ldap_servers"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LMT_CONF | NGX_CONF_TAKE1234 | NGX_CONF_TAKE5 |NGX_CONF_TAKE6|NGX_CONF_TAKE7,
        ngx_conf_set_str_array_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_auth_ldap_loc_conf_t, servers),
        NULL
    },
    ngx_null_command
};

static ngx_http_module_t ngx_http_auth_ldap_module_ctx = {
    NULL, /* preconfiguration */
    ngx_http_auth_ldap_init, /* postconfiguration */
    ngx_http_auth_ldap_create_conf, /* create main configuration */
    NULL, /* init main configuration */
    NULL, //ngx_http_auth_ldap_create_server_conf, /* create server configuration */
    NULL, //ngx_http_auth_ldap_merge_server_conf, /* merge server configuration */
    ngx_http_auth_basic_create_loc_conf, /* create location configuration */
    ngx_http_auth_ldap_merge_loc_conf /* merge location configuration */
};

ngx_module_t ngx_http_auth_ldap_module = {
    NGX_MODULE_V1,
    &ngx_http_auth_ldap_module_ctx, /* module context */
    ngx_http_auth_ldap_commands, /* module directives */
    NGX_HTTP_MODULE, /* module type */
    NULL, /* init master */
    NULL, /* init module */
    ngx_http_auth_ldap_worker_init, /* init process */
    NULL, /* init thread */
    NULL, /* exit thread */
    NULL, /* exit process */
    NULL, /* exit master */
    NGX_MODULE_V1_PADDING /**/
};


/**
 * Reads ldap_server block and sets ngx_http_auth_ldap_ldap_server as a handler of each conf value
 */
static char *
ngx_http_auth_ldap_ldap_server_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char *rv;
    ngx_str_t                 *value, name;
    ngx_conf_t                save;
    ngx_ldap_server           *s;
    ngx_http_auth_ldap_conf_t *cnf = conf;

    value = cf->args->elts;

    name = value[1];

    if (ngx_strlen(name.data) == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "Error: no name of ldap server specified");
        return NGX_CONF_ERROR;
    }

    if (cnf->servers == NULL) {
        cnf->servers = ngx_array_create(cf->pool, 7, sizeof(ngx_ldap_server));
        if (cnf->servers == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    s = ngx_array_push(cnf->servers);
    if (s == NULL) {
        return NGX_CONF_ERROR;
    }
    s->alias = name;

    save = *cf;
    cf->handler = ngx_http_auth_ldap_ldap_server;
    cf->handler_conf = conf;
    rv = ngx_conf_parse(cf, NULL);
    *cf = save;

    if (rv != NGX_CONF_OK) {
        return rv;
    }

    return NGX_CONF_OK;
}

/**
 * Called for every variable inside ldap_server block
 */
static char *
ngx_http_auth_ldap_ldap_server(ngx_conf_t *cf, ngx_command_t *dummy, void *conf)
{
    char                     *rv;
    ngx_str_t                *value;

    ngx_ldap_server *server;
    ngx_http_auth_ldap_conf_t *cnf = conf;

    // It should be safe to just use latest server from array
    server = ((ngx_ldap_server*)cnf->servers->elts + (cnf->servers->nelts - 1));

    value = cf->args->elts;

    // TODO: Add more validation
    if (ngx_strcmp(value[0].data, "url") == 0) {
        return ngx_http_auth_ldap_parse_url(cf, server);
    } else if(ngx_strcmp(value[0].data, "binddn") == 0) {
        server->bind_dn = value[1];
    } else if(ngx_strcmp(value[0].data, "binddn_passwd") == 0) {
        server->bind_dn_passwd = value[1];
    } else if(ngx_strcmp(value[0].data, "group_attribute") == 0) {
        server->group_attribute = value[1];
    } else if(ngx_strcmp(value[0].data, "group_attribute_is_dn") == 0 && ngx_strcmp(value[1].data, "on")) {
        server->group_attribute_dn = 1;
    } else if(ngx_strcmp(value[0].data, "require") == 0) {
        return ngx_http_auth_ldap_parse_require(cf, server);
    } else if(ngx_strcmp(value[0].data, "satisfy") == 0) {
        return ngx_http_auth_ldap_parse_satisfy(cf, server);
    }

    rv = NGX_CONF_OK;

    return rv;
}

/**
 * Parse auth_ldap directive
 */
static char *
ngx_http_auth_ldap(ngx_conf_t *cf, void *post, void *data) {
    ngx_str_t *realm = data;

    size_t len;
    u_char *basic, *p;

    if (ngx_strcmp(realm->data, "off") == 0) {
        realm->len = 0;
        realm->data = (u_char *) "";

        return NGX_CONF_OK;
    }

    len = sizeof("Basic realm=\"") - 1 + realm->len + 1;

    basic = ngx_pcalloc(cf->pool, len);
    if (basic == NULL) {
        return NGX_CONF_ERROR;
    }

    p = ngx_cpymem(basic, "Basic realm=\"", sizeof("Basic realm=\"") - 1);
    p = ngx_cpymem(p, realm->data, realm->len);
    *p = '"';

    realm->len = len;
    realm->data = basic;

    return NGX_CONF_OK;
}

/**
 * Parse URL conf parameter
 */
static char *
ngx_http_auth_ldap_parse_url(ngx_conf_t *cf, ngx_ldap_server *server) {
    ngx_str_t *value;
    u_char *p;
    value = cf->args->elts;

    server->url = *value;

    int rc = ldap_url_parse((const char*) value[1].data, &server->ludpp);
    if (rc != LDAP_SUCCESS) {
        switch (rc) {
            case LDAP_URL_ERR_MEM:
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "LDAP: Cannot allocate memory space.");
                break;

            case LDAP_URL_ERR_PARAM:
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "LDAP: Invalid parameter.");
                break;

            case LDAP_URL_ERR_BADSCHEME:
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "LDAP: URL doesnt begin with \"ldap[s]://\".");
                break;

            case LDAP_URL_ERR_BADENCLOSURE:
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "LDAP: URL is missing trailing \">\".");
                break;

            case LDAP_URL_ERR_BADURL:
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "LDAP: Invalid URL.");
                break;

            case LDAP_URL_ERR_BADHOST:
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "LDAP: Host port is invalid.");
                break;

            case LDAP_URL_ERR_BADATTRS:
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "LDAP: Invalid or missing attributes.");
                break;

            case LDAP_URL_ERR_BADSCOPE:
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "LDAP: Invalid or missing scope string.");
                break;

            case LDAP_URL_ERR_BADFILTER:
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "LDAP: Invalid or missing filter.");
                break;

            case LDAP_URL_ERR_BADEXTS:
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "LDAP: Invalid or missing extensions.");
                break;
        }
        return NGX_CONF_ERROR;
    }

    if (server->ludpp->lud_attrs == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "LDAP: No attrs in auth_ldap_url.");
        return NGX_CONF_ERROR;
    }

    server->url.len = ngx_strlen(server->ludpp->lud_scheme) + ngx_strlen(server->ludpp->lud_host) + 11; // 11 = len("://:/") + len("65535") + len("\0")
    server->url.data = ngx_pcalloc(cf->pool, server->url.len);
    p = ngx_sprintf(server->url.data, "%s://%s:%d/", (const char*) server->ludpp->lud_scheme,
        (const char*) server->ludpp->lud_host, server->ludpp->lud_port);
    *p = 0;

    return NGX_CONF_OK;
}

/**
 * Parse "require" conf parameter
 */
static char *
ngx_http_auth_ldap_parse_require(ngx_conf_t *cf, ngx_ldap_server *server) {

    ngx_http_script_compile_t   sc;
    ngx_str_t *value;

    value = cf->args->elts;

    if (server->require_user == NULL) {
        server->require_user = ngx_array_create(cf->pool, 4, sizeof(ngx_ldap_require_t));
        if (server->require_user == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    if (server->require_group == NULL) {
        server->require_group = ngx_array_create(cf->pool, 4, sizeof(ngx_ldap_require_t));
        if (server->require_group == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    if (ngx_strcmp(value[1].data, "valid_user") == 0) {
        server->require_valid_user = 1;
    }

    if (ngx_strcmp(value[1].data, "user") == 0 || ngx_strcmp(value[1].data, "group") == 0)
    {
        ngx_int_t n;
        ngx_ldap_require_t *rule = NULL;

        if (ngx_strcmp(value[1].data, "user") == 0) {
            rule = ngx_array_push(server->require_user);
        }

        if (ngx_strcmp(value[1].data, "group") == 0) {
            rule = ngx_array_push(server->require_group);
        }

        if (rule == NULL) {
           return NGX_CONF_ERROR;
        }

        rule->value.data = value[2].data;
        rule->value.len = value[2].len;
        rule->values = NULL;
        rule->lengths = NULL;

        n = ngx_http_script_variables_count(&value[2]);
        if(n > 0) {
            ngx_memzero(&sc, sizeof(ngx_http_script_compile_t));
            sc.cf = cf;
            sc.source = &value[2];
            sc.lengths = &rule->lengths;
            sc.values = &rule->values;
            sc.complete_lengths = 1;
            sc.complete_values = 1;

            if (ngx_http_script_compile(&sc) != NGX_OK) {
                return NGX_CONF_ERROR;
            }
        }
    }

    return NGX_CONF_OK;
}

/**
 * Parse "satisfy" conf parameter
 */
static char *
ngx_http_auth_ldap_parse_satisfy(ngx_conf_t *cf, ngx_ldap_server *server) {
    ngx_str_t *value;
    value = cf->args->elts;

    if (ngx_strcmp(value[1].data, "all") == 0) {
        server->satisfy_all = 1;
        return NGX_CONF_OK;
    }

    if (ngx_strcmp(value[1].data, "any") == 0) {
        server->satisfy_all = 0;
        return NGX_CONF_OK;
    }

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "Incorrect value for auth_ldap_satisfy ");
    return NGX_CONF_ERROR;
}

/**
 * Create main config which will store ldap_servers array
 */
static void *
ngx_http_auth_ldap_create_conf(ngx_conf_t *cf)
{
    ngx_http_auth_ldap_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_auth_ldap_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    return conf;
}

/**
 * Create location conf
 */
static void *
ngx_http_auth_basic_create_loc_conf(ngx_conf_t *cf) {
    ngx_http_auth_ldap_loc_conf_t *conf;
    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_auth_ldap_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }
    conf->servers = NGX_CONF_UNSET_PTR;

    return conf;
}

/**
 * Merge location conf
 */
static char *
ngx_http_auth_ldap_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child) {
    ngx_http_auth_ldap_loc_conf_t *prev = parent;
    ngx_http_auth_ldap_loc_conf_t *conf = child;

    if (conf->realm.data == NULL) {
        conf->realm = prev->realm;
    }
    ngx_conf_merge_ptr_value(conf->servers, prev->servers, NULL);

    return NGX_CONF_OK;
}

/**
 * LDAP Authentication handler
 */
static ngx_int_t ngx_http_auth_ldap_handler(ngx_http_request_t *r) {
    int rc;
    ngx_http_auth_ldap_loc_conf_t *alcf;

    alcf = ngx_http_get_module_loc_conf(r, ngx_http_auth_ldap_module);

    if (alcf->realm.len == 0) {
        return NGX_DECLINED;
    }

    ngx_http_auth_ldap_conf_t  *cnf;

    cnf = ngx_http_get_module_main_conf(r, ngx_http_auth_ldap_module);

    rc = ngx_http_auth_basic_user(r);

    if (rc == NGX_DECLINED) {
        return ngx_http_auth_ldap_set_realm(r, &alcf->realm);
    }

    if (rc == NGX_ERROR) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    return ngx_http_auth_ldap_authenticate(r, alcf, cnf);
}

/**
 * Get login and password from http request.
 */
static ngx_ldap_userinfo*
ngx_http_auth_ldap_get_user_info(ngx_http_request_t *r) {
    size_t len;
    ngx_ldap_userinfo* uinfo;
    u_char *uname_buf, *p;

    uinfo = ngx_palloc(r->pool, sizeof(ngx_ldap_userinfo));

    for (len = 0; len < r->headers_in.user.len; len++) {
        if (r->headers_in.user.data[len] == ':') {
            break;
        }
    }

    uname_buf = ngx_palloc(r->pool, len + 1);
    if (uname_buf == NULL) {
        return NULL;
    }
    p = ngx_cpymem(uname_buf, r->headers_in.user.data, len);
    *p = '\0';

    uinfo->username.data = uname_buf;
    uinfo->username.len = len;
    uinfo->password.data = r->headers_in.passwd.data;
    uinfo->password.len = r->headers_in.passwd.len;

    return uinfo;
}

/**
 * Read user credentials from request, set LDAP parameters and call authentication against required servers
 */
static ngx_int_t ngx_http_auth_ldap_authenticate(ngx_http_request_t *r, ngx_http_auth_ldap_loc_conf_t *conf,
        ngx_http_auth_ldap_conf_t *mconf) {

    ngx_ldap_server *server, *servers;
    servers = mconf->servers->elts;
    int rc;
    ngx_uint_t i, k;
    ngx_str_t *alias;

    int version = LDAP_VERSION3;
    int reqcert = LDAP_OPT_X_TLS_ALLOW;
    ngx_ldap_userinfo *uinfo;
    struct timeval timeOut = { 10, 0 };
    ngx_flag_t pass = NGX_CONF_UNSET;

    uinfo = ngx_http_auth_ldap_get_user_info(r);

    if (uinfo == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (uinfo->password.len == 0)
    {
        return ngx_http_auth_ldap_set_realm(r, &conf->realm);
    }

    ngx_slab_pool_t                        *shpool;
    ngx_uint_t                             key;

    shpool = (ngx_slab_pool_t *)ngx_http_auth_ldap_shm_zone->shm.addr;

    key = nginx_http_auth_ldap_get_cache_key(uinfo);

    ngx_shmtx_lock(&shpool->mutex);
	ngx_http_auth_ldap_node_t *cached_credentials = (ngx_http_auth_ldap_node_t *)ngx_http_auth_ldap_rbtree_find(
			key, ngx_http_auth_ldap_rbtree->root, ngx_http_auth_ldap_rbtree->sentinel);
	ngx_shmtx_unlock(&shpool->mutex);

	if (cached_credentials != NULL) {
		if (ngx_strncmp(uinfo->username.data, cached_credentials->username, uinfo->username.len) == 0) {

			// Check that client ip is same first
			if (ngx_strncmp(r->connection->addr_text.data, cached_credentials->client_ip, r->connection->addr_text.len) == 0) {

				unsigned char hash[SHA_DIGEST_LENGTH];
				ngx_http_auth_ldap_get_password_hash(r, &uinfo->username, &uinfo->password, hash);
				ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "Comparing password hashes: %s == %s", hash, cached_credentials->password_hash);

				if (ngx_strncmp(hash, cached_credentials->password_hash, SHA_DIGEST_LENGTH) == 0) {
					int alias_found = 0;
					for (k = 0; k < conf->servers->nelts; k++) {
			            server = &servers[k];
			            if (ngx_strncmp(server->alias.data, cached_credentials->server_alias, server->alias.len) == 0) {
			                alias_found = 1;
			            }
					}
					if (alias_found == 1) {
						ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "User %s passed all checks, using cache to allow access", r->connection->addr_text.data, cached_credentials->username);
						return NGX_OK;
					} else {
						ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0, "LDAP: User %s passed all checks, but cached data is from different LDAP server", r->connection->addr_text.data, cached_credentials->username);
					}

				} else {
					// TODO: Really?
					// TODO: Delete cache?
					ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "User %s was found in ldap cache, but password does not match", uinfo->username.data);
					cached_credentials->expires = ngx_time();
					//return ngx_http_auth_ldap_set_realm(r, &conf->realm);
				}
			} else {
				ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0, "LDAP: User %s was found in ldap cache, but IP does not match: %s != %s", uinfo->username.data, r->connection->addr_text.data, cached_credentials->client_ip);
				cached_credentials->expires = ngx_time();
				// TODO: Delete cache
				// TODO: Really?
				//return ngx_http_auth_ldap_set_realm(r, &conf->realm);
			}
		} else {
			ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "LDAP: Possible hash collision: hash keys match, but usernames are not the same (%s != %s)!", uinfo->username.data, cached_credentials->username);
			cached_credentials->expires = ngx_time();
			// TODO: Delete cache
		}
	}

	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "Nothing found in cache, using LDAP auth");


    /// Set LDAP version to 3 and set connection timeout.
    ldap_set_option(NULL, LDAP_OPT_PROTOCOL_VERSION, &version);
    ldap_set_option(NULL, LDAP_OPT_NETWORK_TIMEOUT, &timeOut);

    rc = ldap_set_option(NULL, LDAP_OPT_X_TLS_REQUIRE_CERT, &reqcert);
    if (rc != LDAP_OPT_SUCCESS) {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "LDAP: unable to set require cert option: %s",
            ldap_err2string(rc));
    }

    // TODO: We might be using hash here, cause this loops is quite ugly, but it is simple and it works
    int found;
    for (k = 0; k < conf->servers->nelts; k++) {
        alias = ((ngx_str_t*)conf->servers->elts + k);
        found = 0;
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "CLIENT IP: %s", r->connection->addr_text.data);
        for (i = 0; i < mconf->servers->nelts; i++) {
            server = &servers[i];
            if (server->alias.len == alias->len && ngx_strncmp(server->alias.data, alias->data, server->alias.len) == 0) {
                found = 1;

                pass = ngx_http_auth_ldap_authenticate_against_server(r, server, uinfo, conf);
                if (pass == 1) {
                    ngx_http_auth_ldap_cache_store(r, uinfo, server);
                    return NGX_OK;
                } else if (pass == NGX_HTTP_INTERNAL_SERVER_ERROR) {
                   return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }
            }
        }

        // If requested ldap server is not found, return 500 and write to log
        if (found == 0) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "LDAP: Server \"%s\" is not defined!", alias->data);
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
    }

    return ngx_http_auth_ldap_set_realm(r, &conf->realm);
}

/**
 * Actual authentication against LDAP server
 */
static ngx_int_t ngx_http_auth_ldap_authenticate_against_server(ngx_http_request_t *r, ngx_ldap_server *server, ngx_ldap_userinfo *uinfo, ngx_http_auth_ldap_loc_conf_t *conf) {
    LDAPURLDesc *ludpp = server->ludpp;
    int rc;
    LDAP *ld;
    LDAPMessage *searchResult;
    char *dn;
    u_char *p, *filter;
    ngx_ldap_require_t *value;
    ngx_uint_t i;
    struct berval bvalue;
    ngx_flag_t pass = NGX_CONF_UNSET;
    struct timeval timeOut = { 10, 0 };

    if (server->ludpp == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "LDAP: URL: %s", server->url.data);

    rc = ldap_initialize(&ld, (const char*) server->url.data);
    if (rc != LDAP_SUCCESS) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "LDAP: Session initializing failed: %d, %s, (%s)", rc,
            ldap_err2string(rc), (const char*) server->url.data);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "LDAP: Session initialized", NULL);

    /// Bind to the server
    rc = ldap_simple_bind_s(ld, (const char *) server->bind_dn.data, (const char *) server->bind_dn_passwd.data);
    if (rc != LDAP_SUCCESS) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "LDAP [%s]: ldap_simple_bind_s error: %d, %s", server->url.data, rc,
            ldap_err2string(rc));
        ldap_unbind_s(ld);
        // Do not throw 500 in case connection failure, multiple servers might be used for failover scenario
        return 0;
    }
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "LDAP: Bind successful", NULL);

    /// Create filter for search users by uid
    filter = ngx_pcalloc(
        r->pool,
        (ludpp->lud_filter != NULL ? ngx_strlen(ludpp->lud_filter) : ngx_strlen("(objectClass=*)")) + ngx_strlen("(&(=))")  + ngx_strlen(ludpp->lud_attrs[0])
               + uinfo->username.len + 1);

    p = ngx_sprintf(filter, "(&%s(%s=%s))", ludpp->lud_filter != NULL ? ludpp->lud_filter : "(objectClass=*)", ludpp->lud_attrs[0], uinfo->username.data);
    *p = 0;
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "LDAP: filter %s", (const char*) filter);

    /// Search the directory
    rc = ldap_search_ext_s(ld, ludpp->lud_dn, ludpp->lud_scope, (const char*) filter, NULL, 0, NULL, NULL, &timeOut, 0,
        &searchResult);

    if (rc != LDAP_SUCCESS) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "LDAP: ldap_search_ext_s: %d, %s", rc, ldap_err2string(rc));
        ldap_msgfree(searchResult);
        ldap_unbind_s(ld);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (ldap_count_entries(ld, searchResult) > 0) {
    dn = ldap_get_dn(ld, searchResult);
        if (dn != NULL) {
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "LDAP: result DN %s", dn);

            /// Check require user
            if (server->require_user != NULL) {
                value = server->require_user->elts;
                for (i = 0; i < server->require_user->nelts; i++) {
                    ngx_str_t val;
                    if (value[i].lengths == NULL) {
                        val = value[i].value;
                    } else {
                        if (ngx_http_script_run(r, &val, value[i].lengths->elts, 0,
                            value[i].values->elts) == NULL)
                        {
                            ldap_memfree(dn);
                            ldap_msgfree(searchResult);
                            ldap_unbind_s(ld);
                            return NGX_HTTP_INTERNAL_SERVER_ERROR;
                        }
                        val.data[val.len] = '\0';
                    }

                    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "LDAP: compare with: %s", val.data);
                    if (ngx_strncmp(val.data, dn, val.len) == 0) {
                        pass = 1;
                        if (server->satisfy_all == 0) {
                            break;
                        }
                    } else {
                        if (server->satisfy_all == 1) {
                            ldap_memfree(dn);
                            ldap_msgfree(searchResult);
                            ldap_unbind_s(ld);
                            return 0;
                        }
                    }
                }
            }

            /// Check require group
            if (server->require_group != NULL) {
                if (server->group_attribute_dn == 1) {
                    bvalue.bv_val = dn;
                    bvalue.bv_len = ngx_strlen(dn);
                } else {
                    bvalue.bv_val = (char*) uinfo->username.data;
                    bvalue.bv_len = uinfo->username.len;
                }

                value = server->require_group->elts;

                for (i = 0; i < server->require_group->nelts; i++) {
                    ngx_str_t val;
                    if (value[i].lengths == NULL) {
                        val = value[i].value;
                    } else {
                        if (ngx_http_script_run(r, &val, value[i].lengths->elts, 0,
                            value[i].values->elts) == NULL)
                        {
                            ldap_memfree(dn);
                            ldap_msgfree(searchResult);
                            ldap_unbind_s(ld);
                            return NGX_HTTP_INTERNAL_SERVER_ERROR;
                        }
                        val.data[val.len] = '\0';
                    }

                    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "LDAP: group compare with: %s", val.data);

                    rc = ldap_compare_ext_s(ld, (const char*) val.data, (const char*) server->group_attribute.data,
                        &bvalue, NULL, NULL);

                    /*if (rc != LDAP_COMPARE_TRUE && rc != LDAP_COMPARE_FALSE && rc != LDAP_NO_SUCH_ATTRIBUTE ) {
                    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "LDAP: ldap_search_ext_s: %d, %s", rc,
                            ldap_err2string(rc));
                    ldap_memfree(dn);
                    ldap_msgfree(searchResult);
                    ldap_unbind_s(ld);
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                    }*/

                    if (rc == LDAP_COMPARE_TRUE) {
                        pass = 1;
                        if (server->satisfy_all == 0) {
                            break;
                        }
                    } else {
                        if (server->satisfy_all == 1) {
                            pass = 0;
                            break;
                        }
                    }
                }
            }

            /// Check valid user
            if ( pass != 0 || (server->require_valid_user == 1 && server->satisfy_all == 0 && pass == 0)) {
                /// Bind user to the server
                rc = ldap_simple_bind_s(ld, dn, (const char *) uinfo->password.data);
                if (rc != LDAP_SUCCESS) {
                    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "LDAP: ldap_simple_bind_s error: %d, %s", rc,
                        ldap_err2string(rc));
                    pass = 0;
                } else {
                    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "LDAP: User bind successful", NULL);
                    if (server->require_valid_user == 1) pass = 1;
                }
            }

        }
        ldap_memfree(dn);
    }

    ldap_msgfree(searchResult);
    ldap_unbind_s(ld);

    return pass;
}

/**
 * Respond with forbidden and add correct headers
 */
static ngx_int_t ngx_http_auth_ldap_set_realm(ngx_http_request_t *r, ngx_str_t *realm) {
    r->headers_out.www_authenticate = ngx_list_push(&r->headers_out.headers);
    if (r->headers_out.www_authenticate == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    r->headers_out.www_authenticate->hash = 1;
    r->headers_out.www_authenticate->key.len = sizeof("WWW-Authenticate") - 1;
    r->headers_out.www_authenticate->key.data = (u_char *) "WWW-Authenticate";
    r->headers_out.www_authenticate->value = *realm;

    return NGX_HTTP_UNAUTHORIZED;
}

/**
 * Init shared memory zone
 */
static ngx_int_t
ngx_http_auth_ldap_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_slab_pool_t                *shpool;
    ngx_rbtree_t                   *tree;
    ngx_rbtree_node_t              *sentinel;
    ngx_atomic_t                   *lock;
    if (data) {
        shm_zone->data = data;
        return NGX_OK;
    }

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;
    tree = ngx_slab_alloc(shpool, sizeof *tree);
    if (tree == NULL) {
        return NGX_ERROR;
    }

    sentinel = ngx_slab_alloc(shpool, sizeof *sentinel);
    if (sentinel == NULL) {
        return NGX_ERROR;
    }

    ngx_rbtree_init(tree, sentinel, ngx_http_auth_ldap_rbtree_insert);
    shm_zone->data = tree;
    ngx_http_auth_ldap_rbtree = tree;


    lock = ngx_slab_alloc(shpool, sizeof(ngx_atomic_t));
    if (lock == NULL) {
        return NGX_ERROR;
    }
    ngx_http_auth_ldap_cleanup_lock = lock;

    return NGX_OK;
}

/**
 * Insert new node into rbtree
 */
static void
ngx_http_auth_ldap_rbtree_insert(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel) {

    ngx_rbtree_generic_insert(temp, node, sentinel, ngx_http_auth_ldap_rbtree_cmp);
}

/**
 * Find rbtree nodes by key
 */
static ngx_rbtree_node_t *
ngx_http_auth_ldap_rbtree_find(ngx_rbtree_key_t key, ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel){

    if (node==sentinel) return NULL;

    ngx_rbtree_node_t *found = (node->key==key) ? node : NULL;
    if (found==NULL && node->left != sentinel){
        found = ngx_http_auth_ldap_rbtree_find(key, node->left, sentinel);
    }
    if (found==NULL && node->right != sentinel){
        found = ngx_http_auth_ldap_rbtree_find(key, node->right, sentinel);
    }

    return found;
}

/**
 * Compare rbtree nodes
 */
static int
ngx_http_auth_ldap_rbtree_cmp(const ngx_rbtree_node_t *v_left,
    const ngx_rbtree_node_t *v_right)
{
    if (v_left->key == v_right->key) return 0;
    else return (v_left->key < v_right->key) ? -1 : 1;
}

/**
 * Insert new node into rbtree
 */
static void
ngx_rbtree_generic_insert(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel,
    int (*compare)(const ngx_rbtree_node_t *left, const ngx_rbtree_node_t *right))
{
    for ( ;; ) {
        if (node->key < temp->key) {

            if (temp->left == sentinel) {
                temp->left = node;
                break;
            }

            temp = temp->left;

        } else if (node->key > temp->key) {

            if (temp->right == sentinel) {
                temp->right = node;
                break;
            }

            temp = temp->right;

        } else { /* node->key == temp->key */
            if (compare(node, temp) < 0) {

                if (temp->left == sentinel) {
                    temp->left = node;
                    break;
                }

                temp = temp->left;

            } else {

                if (temp->right == sentinel) {
                    temp->right = node;
                    break;
                }

                temp = temp->right;
            }
        }
    }

    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}

/**
 * Cleanup handler for ldap authentication cache
 */
void ngx_http_auth_ldap_cleanup(ngx_event_t *ev){
  if (ev->timer_set) ngx_del_timer(ev);
  ngx_add_timer(ev, NGX_HTTP_AUTH_LDAP_CLEANUP_INTERVAL);

  if (ngx_trylock(ngx_http_auth_ldap_cleanup_lock)){
    ngx_http_auth_ldap_rbtree_prune(ev->log);
    ngx_unlock(ngx_http_auth_ldap_cleanup_lock);
  }
}

/**
 * Prunes rbtree with ldap authentication cache
 */
static void ngx_http_auth_ldap_rbtree_prune(ngx_log_t *log){
    ngx_uint_t i;
    time_t now = ngx_time();
    ngx_slab_pool_t *shpool = (ngx_slab_pool_t *)ngx_http_auth_ldap_shm_zone->shm.addr;

    ngx_shmtx_lock(&shpool->mutex);
    ngx_http_auth_ldap_cleanup_list->nelts = 0;
    ngx_http_auth_ldap_rbtree_prune_walk(ngx_http_auth_ldap_rbtree->root, ngx_http_auth_ldap_rbtree->sentinel, now, log);

    ngx_rbtree_node_t **elts = (ngx_rbtree_node_t **)ngx_http_auth_ldap_cleanup_list->elts;
    for (i=0; i<ngx_http_auth_ldap_cleanup_list->nelts; i++){
        ngx_rbtree_delete(ngx_http_auth_ldap_rbtree, elts[i]);
        ngx_slab_free_locked(shpool, elts[i]);
    }
    ngx_shmtx_unlock(&shpool->mutex);

    // if the cleanup array grew during the run, shrink it back down
    if (ngx_http_auth_ldap_cleanup_list->nalloc > NGX_HTTP_AUTH_LDAP_CLEANUP_BATCH_SIZE){
        ngx_array_t *old_list = ngx_http_auth_ldap_cleanup_list;
        ngx_array_t *new_list = ngx_array_create(old_list->pool, NGX_HTTP_AUTH_LDAP_CLEANUP_BATCH_SIZE, sizeof(ngx_rbtree_node_t *));
        if (new_list!=NULL){
            ngx_array_destroy(old_list);
            ngx_http_auth_ldap_cleanup_list = new_list;
        } else {
            ngx_log_error(NGX_LOG_ERR, log, 0, "auth_ldap ran out of cleanup space");
        }
    }
}

/**
 * Walk through the tree and find elements which are expired
 */
static void ngx_http_auth_ldap_rbtree_prune_walk(ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel, time_t now, ngx_log_t *log){
    if (node==sentinel) return;

    if (node->left != sentinel){
        ngx_http_auth_ldap_rbtree_prune_walk(node->left, sentinel, now, log);
    }

    if (node->right != sentinel){
        ngx_http_auth_ldap_rbtree_prune_walk(node->right, sentinel, now, log);
    }

    ngx_http_auth_ldap_node_t *dnode = (ngx_http_auth_ldap_node_t*) node;
    if (dnode->expires <= ngx_time()){
        ngx_rbtree_node_t **dropnode = ngx_array_push(ngx_http_auth_ldap_cleanup_list);
        dropnode[0] = node;
    }
}

/**
 * Returns simple hash key to use in rbtree
 */
static ngx_uint_t nginx_http_auth_ldap_get_cache_key (ngx_ldap_userinfo *uinfo)
{
    return ngx_crc32_long(uinfo->username.data, uinfo->username.len);

}

/**
 * Stores ldap authentication cache to rbtree
 */
static ngx_http_auth_ldap_node_t ngx_http_auth_ldap_cache_store(ngx_http_request_t *r, ngx_ldap_userinfo *uinfo, ngx_ldap_server *server){
    ngx_slab_pool_t                        *shpool;
    ngx_uint_t                             key;
    ngx_http_auth_ldap_node_t              *node;

    shpool = (ngx_slab_pool_t *)ngx_http_auth_ldap_shm_zone->shm.addr;

    // create a cache record
    key = nginx_http_auth_ldap_get_cache_key(uinfo);
    ngx_shmtx_lock(&shpool->mutex);

    ngx_http_auth_ldap_node_t *found = (ngx_http_auth_ldap_node_t *)ngx_http_auth_ldap_rbtree_find(key, ngx_http_auth_ldap_rbtree->root, ngx_http_auth_ldap_rbtree->sentinel);
	if (found!=NULL){
        ngx_shmtx_unlock(&shpool->mutex);
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "Trying to store cache which already exist. This is so wrong and looks like hash collision! Username in cache - %s, username provided - %s", found->username, uinfo->username.data);
    }

    node = ngx_slab_alloc_locked(shpool, sizeof(ngx_http_auth_ldap_node_t));
    if (node==NULL){
        ngx_shmtx_unlock(&shpool->mutex);
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                    "auth_ldap ran out of shm space. Increase the auth_digest_shm_size limit.");
        //TODO: Really?
        return *node;
    }

    //TODO: make this configurable
    node->expires = ngx_time() + 300;
    //node->server_alias = ngx_slab_alloc_locked(shpool, server->alias.len);
    ngx_memcpy(node->server_alias, server->alias.data, server->alias.len);
    ngx_memcpy(node->username, uinfo->username.data, uinfo->username.len);
    ngx_memcpy(node->client_ip, r->connection->addr_text.data, r->connection->addr_text.len);

    ngx_http_auth_ldap_get_password_hash(r, &uinfo->username, &uinfo->password, node->password_hash);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "GET HASH RESULT: %s", node->password_hash);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "GET HASH RESULT: %d", ngx_strlen(node->password_hash));

    ((ngx_rbtree_node_t *)node)->key = key;
    ngx_rbtree_insert(ngx_http_auth_ldap_rbtree, &node->node);

    ngx_shmtx_unlock(&shpool->mutex);
    return *node;
}

static void ngx_http_auth_ldap_get_password_hash (ngx_http_request_t *r, const ngx_str_t *username, const ngx_str_t *password, u_char *hash)
{

	 u_char buf[password->len + username->len + sizeof('|')];
	 u_char *p;
     p = buf;

	 p = ngx_snprintf(p, sizeof buf, "%s|%s", username->data, password->data);
	 *p = '\0';
    SHA1(buf, ngx_strlen(buf), hash);
    hash[SHA_DIGEST_LENGTH] = '\0';
}

static ngx_int_t
ngx_http_auth_ldap_worker_init(ngx_cycle_t *cycle){
    if (ngx_process != NGX_PROCESS_WORKER){
        return NGX_OK;
    }

    // create a cleanup queue big enough for the max number of tree nodes in the shm
    ngx_http_auth_ldap_cleanup_list = ngx_array_create(cycle->pool,
                                                      NGX_HTTP_AUTH_LDAP_CLEANUP_BATCH_SIZE,
                                                      sizeof(ngx_rbtree_node_t *));
    if (ngx_http_auth_ldap_cleanup_list==NULL){
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0, "Could not allocate shared memory for auth_ldap");
        return NGX_ERROR;
    }

    ngx_connection_t  *dummy;
    dummy = ngx_pcalloc(cycle->pool, sizeof(ngx_connection_t));
    if (dummy == NULL) return NGX_ERROR;
    dummy->fd = (ngx_socket_t) -1;
    dummy->data = cycle;

    ngx_http_auth_ldap_cleanup_timer->log = ngx_cycle->log;
    ngx_http_auth_ldap_cleanup_timer->data = dummy;
    ngx_http_auth_ldap_cleanup_timer->handler = ngx_http_auth_ldap_cleanup;
    ngx_add_timer(ngx_http_auth_ldap_cleanup_timer, NGX_HTTP_AUTH_LDAP_CLEANUP_INTERVAL);
    return NGX_OK;
}

/**
 * Init module and add ldap auth handler to NGX_HTTP_ACCESS_PHASE
 */
static ngx_int_t ngx_http_auth_ldap_init(ngx_conf_t *cf) {
    ngx_http_handler_pt *h;
    ngx_http_core_main_conf_t *cmcf;
    ngx_str_t                  *shm_name;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_auth_ldap_handler;

  ngx_http_auth_ldap_cleanup_timer = ngx_pcalloc(cf->pool, sizeof(ngx_event_t));
  if (ngx_http_auth_ldap_cleanup_timer == NULL) {
    return NGX_ERROR;
  }

  shm_name = ngx_palloc(cf->pool, sizeof *shm_name);
  shm_name->len = sizeof("auth_ldap");
  shm_name->data = (unsigned char *) "auth_ldap";

  if (ngx_http_auth_ldap_shm_size == 0) {
    ngx_http_auth_ldap_shm_size = 4 * 256 * ngx_pagesize; // default to 4mb
  }

  ngx_http_auth_ldap_shm_zone = ngx_shared_memory_add(
    cf, shm_name, ngx_http_auth_ldap_shm_size, &ngx_http_auth_ldap_module);
  if (ngx_http_auth_ldap_shm_zone == NULL) {
    return NGX_ERROR;
  }
  ngx_http_auth_ldap_shm_zone->init = ngx_http_auth_ldap_init_shm_zone;

  return NGX_OK;
}
