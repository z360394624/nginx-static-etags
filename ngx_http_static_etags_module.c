/*
 *  Copyright 2008 Mike West ( http://mikewest.org/ )
 *
 *  The following is released under the Creative Commons BSD license,
 *  available for your perusal at `http://creativecommons.org/licenses/BSD/`
 */
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <sys/stat.h>
#include <openssl/ssl.h>
#include <openssl/hmac.h>
#include <openssl/md5.h>

/*
 *  Two configuration elements: `enable_etags` and `etag_format`, specified in
 *  the `Location` block.
 */
typedef struct {
    ngx_uint_t  FileETag;
    ngx_str_t   etag_format;
} ngx_http_static_etags_loc_conf_t;

static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
/*static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;*/

static void * ngx_http_static_etags_create_loc_conf(ngx_conf_t *cf);
static char * ngx_http_static_etags_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static ngx_int_t ngx_http_static_etags_init(ngx_conf_t *cf);
static ngx_int_t ngx_http_static_etags_header_filter(ngx_http_request_t *r);
static char * get_file_md5(char *path, ngx_log_t *log);


static ngx_command_t  ngx_http_static_etags_commands[] = {
    { ngx_string( "FileETag" ),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof( ngx_http_static_etags_loc_conf_t, FileETag ),
      NULL },

    { ngx_string( "etag_format" ),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof( ngx_http_static_etags_loc_conf_t, etag_format ),
      NULL },

      ngx_null_command
};

static ngx_http_module_t  ngx_http_static_etags_module_ctx = {
    NULL,                                   /* preconfiguration */
    ngx_http_static_etags_init,             /* postconfiguration */

    NULL,                                   /* create main configuration */
    NULL,                                   /* init main configuration */

    NULL,                                   /* create server configuration */
    NULL,                                   /* merge server configuration */

    ngx_http_static_etags_create_loc_conf,  /* create location configuration */
    ngx_http_static_etags_merge_loc_conf,   /* merge location configuration */
};

ngx_module_t  ngx_http_static_etags_module = {
    NGX_MODULE_V1,
    &ngx_http_static_etags_module_ctx,  /* module context */
    ngx_http_static_etags_commands,     /* module directives */
    NGX_HTTP_MODULE,                    /* module type */
    NULL,                               /* init master */
    NULL,                               /* init module */
    NULL,                               /* init process */
    NULL,                               /* init thread */
    NULL,                               /* exit thread */
    NULL,                               /* exit process */
    NULL,                               /* exit master */
    NGX_MODULE_V1_PADDING
};


static void * ngx_http_static_etags_create_loc_conf(ngx_conf_t *cf) {
    ngx_http_static_etags_loc_conf_t    *conf;

    conf = ngx_pcalloc( cf->pool, sizeof( ngx_http_static_etags_loc_conf_t ) );
    if ( NULL == conf ) {
        return NGX_CONF_ERROR;
    }
    conf->FileETag   = NGX_CONF_UNSET_UINT;
    return conf;
}

static char * ngx_http_static_etags_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child) {
    ngx_http_static_etags_loc_conf_t *prev = parent;
    ngx_http_static_etags_loc_conf_t *conf = child;

    ngx_conf_merge_uint_value( conf->FileETag, prev->FileETag, 0 );
    ngx_conf_merge_str_value(  conf->etag_format, prev->etag_format, "%s_%X_%X" );

    if ( conf->FileETag != 0 && conf->FileETag != 1 ) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, 
            "FileETag must be 'on' or 'off'");
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

static char* get_file_md5(char *path, ngx_log_t *log) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0,
        "http md5 filename: \"%s\"", path);
    MD5_CTX ctx;
    int len = 0;
    unsigned char buffer[1024] = { 0 };
    unsigned char num[16] = { 0 };
    FILE *pFile = fopen(path, "rb");
	MD5_Init(&ctx);
	while ((len = fread(buffer, 1, 1024, pFile)) > 0) {
		MD5_Update(&ctx, buffer, len);
	}
	MD5_Final(num, &ctx);
	fclose(pFile);
	int i = 0;
	char *buf = (char *) malloc(33);;
	char tmp[3] = { 0 };
	for (i = 0; i < 16; i++) {
		sprintf(tmp, "%02X", num[i]);
		strcat(buf, tmp);
    }
    return buf;
}

static ngx_int_t ngx_http_static_etags_init(ngx_conf_t *cf) {
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_static_etags_header_filter;

    return NGX_OK;
}

static ngx_int_t ngx_http_static_etags_header_filter(ngx_http_request_t *r) {
    int                                 status;
    ngx_log_t                          *log;
    u_char                             *p;
    size_t                              root;
    ngx_str_t                           path;
    ngx_http_static_etags_loc_conf_t   *loc_conf;
    struct stat                         stat_result;
    char                               *str_buffer;
    int                                 str_len;
    char                               *md5;

    log = r->connection->log;
    
    loc_conf = ngx_http_get_module_loc_conf( r, ngx_http_static_etags_module );
    
    // Is the module active?
    if ( 1 == loc_conf->FileETag ) {
        p = ngx_http_map_uri_to_path( r, &path, &root, 0 );
        if ( NULL == p ) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }


        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0,
                        "http filename: \"%s\"", path.data);
    
        status = stat( (char *) path.data, &stat_result );

        md5 = get_file_md5((char*)path.data, log);
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0,
            "file uri: \"%s\"", "help------");

        // Did the `stat` succeed?
        if ( 0 == status) {
            str_len    = 1000;
            str_buffer = malloc( str_len + sizeof(char) );
            sprintf( str_buffer, (char *) loc_conf->etag_format.data, r->uri.data, (unsigned int) stat_result.st_size, (unsigned int) stat_result.st_mtime );
            
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0,
                "file uri: \"%s\"", (char *)r->uri.data);

            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0,
                            "stat returned: \"%d\"", status);
    
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0,
                         "st_size: '%d'", stat_result.st_size);
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0,
                         "st_mtime: '%d'", stat_result.st_mtime);
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0,
                         "Concatted: '%s'", str_buffer );
                    
            r->headers_out.etag = ngx_list_push(&r->headers_out.headers);
            if (r->headers_out.etag == NULL) {
                return NGX_ERROR;
            }
            r->headers_out.etag->hash = 1;
            r->headers_out.etag->key.len = sizeof("ETag") - 1;
            r->headers_out.etag->key.data = (u_char *) "ETag";
            // r->headers_out.etag->value.len = strlen( str_buffer );
            // r->headers_out.etag->value.data = (u_char *) str_buffer;
            r->headers_out.etag->value.len = strlen( md5 );
            r->headers_out.etag->value.data = (u_char *) md5;
        }
    }

    return ngx_http_next_header_filter(r);
}