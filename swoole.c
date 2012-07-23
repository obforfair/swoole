/*
 +----------------------------------------------------------------------+
 | PHP Version 5                                                        |
 +----------------------------------------------------------------------+
 | Copyright (c) 1997-2012 The PHP Group                                |
 +----------------------------------------------------------------------+
 | This source file is subject to version 3.01 of the PHP license,      |
 | that is bundled with this package in the file LICENSE, and is        |
 | available through the world-wide-web at the following url:           |
 | http://www.php.net/license/3_01.txt                                  |
 | If you did not receive a copy of the PHP license and are unable to   |
 | obtain it through the world-wide-web, please send a note to          |
 | license@php.net so we can mail you a copy immediately.               |
 +----------------------------------------------------------------------+
 | Author:                                                              |
 +----------------------------------------------------------------------+
 */

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/sockets/php_sockets.h"
#include "php_swoole.h"
#include "swoole.h"
#include "Server.h"

/* If you declare any globals in php_swoole.h uncomment this:
 ZEND_DECLARE_MODULE_GLOBALS(swoole)
 */


/* True global resources - no need for thread safety here */

#define le_serv_name "SwooleServer"
#define PHP_CALLBACK_NUM  5

#define PHP_CB_onStart          0
#define PHP_CB_onConnect        1
#define PHP_CB_onReceive        2
#define PHP_CB_onClose          3
#define PHP_CB_onShutdown       4
#define SW_HOST_SIZE            64


int swoole_running = 1;
static int le_serv;
static zval *php_sw_callback[PHP_CALLBACK_NUM];
static void ***sw_thread_ctx;

static int php_swoole_onReceive(swFactory *, swEventData *);
static void php_swoole_onStart(swServer *);
static void php_swoole_onShutdown(swServer *);
static void php_swoole_onConnect(swServer *, int fd, int from_id);
static void php_swoole_onClose(swServer *, int fd, int from_id);
static void sw_destory_server(zend_rsrc_list_entry *rsrc TSRMLS_DC);

const zend_function_entry swoole_functions[] =
{
	PHP_FE(swoole_server_create, NULL)
	PHP_FE(swoole_server_set, NULL)
	PHP_FE(swoole_server_start, NULL)
	PHP_FE(swoole_server_send, NULL)
	PHP_FE(swoole_server_close, NULL)
	PHP_FE(swoole_server_getsock, NULL)
	PHP_FE(swoole_server_handler, NULL)
	PHP_FE_END /* Must be the last line in swoole_functions[] */
};

zend_module_entry swoole_module_entry =
{
#if ZEND_MODULE_API_NO >= 20010901
	STANDARD_MODULE_HEADER,
#endif
	"swoole",
	swoole_functions,
	PHP_MINIT(swoole),
	PHP_MSHUTDOWN(swoole),
	PHP_RINIT(swoole), /* Replace with NULL if there's nothing to do at request start */
	PHP_RSHUTDOWN(swoole), /* Replace with NULL if there's nothing to do at request end */
	PHP_MINFO(swoole),
#if ZEND_MODULE_API_NO >= 20010901
		"0.1", /* Replace with version number for your extension */
#endif
		STANDARD_MODULE_PROPERTIES };
/* }}} */

#ifdef COMPILE_DL_SWOOLE
ZEND_GET_MODULE(swoole)
#endif

/* {{{ PHP_INI
 */
/* Remove comments and fill if you need to have entries in php.ini
 PHP_INI_BEGIN()
 STD_PHP_INI_ENTRY("swoole.global_value",      "42", PHP_INI_ALL, OnUpdateLong, global_value, zend_swoole_globals, swoole_globals)
 STD_PHP_INI_ENTRY("swoole.global_string", "foobar", PHP_INI_ALL, OnUpdateString, global_string, zend_swoole_globals, swoole_globals)
 PHP_INI_END()
 */
/* }}} */

/* {{{ php_swoole_init_globals
 */
/* Uncomment this function if you have INI entries
 static void php_swoole_init_globals(zend_swoole_globals *swoole_globals)
 {
 swoole_globals->global_value = 0;
 swoole_globals->global_string = NULL;
 }
 */
/* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(swoole)
{
	/* If you have INI entries, uncomment these lines 
	 REGISTER_INI_ENTRIES();
	 */
	le_serv = zend_register_list_destructors_ex(sw_destory_server, NULL, le_serv_name, module_number);
	REGISTER_LONG_CONSTANT("SWOOLE_BASE", SW_MODE_CALL, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("SWOOLE_THREAD", SW_MODE_THREAD, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("SWOOLE_PROCESS", SW_MODE_PROCESS, CONST_CS | CONST_PERSISTENT);
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(swoole)
{
	/* uncomment this line if you have INI entries
	 UNREGISTER_INI_ENTRIES();
	 */
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request start */
/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(swoole)
{
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request end */
/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(swoole)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(swoole)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "swoole support", "enabled");
	php_info_print_table_row(2, "Version", "1.0.1");
	php_info_print_table_row(2, "Author", "tianfeng.han[email: mikan.tenny@gmail.com]");
	php_info_print_table_end();

	/* Remove comments if you have entries in php.ini
	 DISPLAY_INI_ENTRIES();
	 */
}
/* }}} */

static void sw_destory_server(zend_rsrc_list_entry *rsrc TSRMLS_DC)
{
	swServer *serv = (swServer *) rsrc->ptr;
	swServer_free(serv);

	sw_free(serv->host);
	sw_free(serv);
	swTrace("sw_destory_server\n");
}


PHP_FUNCTION(swoole_server_create)
{
	swServer *serv = sw_malloc(sizeof(swServer));
	int host_len;
	char *host;
	swServer_init(serv);

	serv->host = sw_malloc(SW_HOST_SIZE);
	bzero(serv->host, SW_HOST_SIZE);

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sll", &host, &host_len, &serv->port, &serv->factory_mode) == FAILURE)
	{
		return;
	}
	strncpy(serv->host, host, host_len);
	swTrace("Create host=%s,port=%d,mode=%d\n",serv->host, serv->port, serv->factory_mode);
	sw_thread_ctx = TSRMLS_C;
	ZEND_REGISTER_RESOURCE(return_value, serv, le_serv);
}

PHP_FUNCTION(swoole_server_set)
{
	zval *zset = NULL;
	zval *zserv = NULL;
	HashTable * vht;
	swServer *serv;
	zval **v;
	double timeout;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ra", &zserv, &zset ) == FAILURE)
	{
		return;
	}
	ZEND_FETCH_RESOURCE(serv, swServer *, &zserv, -1, le_serv_name, le_serv);
	serv->ptr2 = zserv;
	zval_add_ref(&zserv);

	vht = Z_ARRVAL_P(zset);
	//timeout
	if (zend_hash_find(vht, ZEND_STRS("timeout"), (void **)&v) == SUCCESS)
	{
		timeout = Z_DVAL_PP(v);
		serv->timeout_sec = (int)timeout;
		serv->timeout_usec = (int)((timeout*1000*1000) - (serv->timeout_sec*1000*1000));
	}
	//backlog
	if (zend_hash_find(vht, ZEND_STRS("backlog"), (void **)&v) == SUCCESS)
	{
		serv->backlog = (int)Z_LVAL_PP(v);
	}
	//poll_thread_num
	if (zend_hash_find(vht, ZEND_STRS("poll_thread_num"), (void **)&v) == SUCCESS)
	{
		serv->poll_thread_num = (int)Z_LVAL_PP(v);
	}
	//writer_num
	if (zend_hash_find(vht, ZEND_STRS("writer_num"), (void **)&v) == SUCCESS)
	{
		serv->writer_num = (int)Z_LVAL_PP(v);
	}
	//writer_num
	if (zend_hash_find(vht, ZEND_STRS("worker_num"), (void **)&v) == SUCCESS)
	{
		serv->worker_num = (int)Z_LVAL_PP(v);
	}
	//max_conn
	if (zend_hash_find(vht, ZEND_STRS("max_conn"), (void **)&v) == SUCCESS)
	{
		serv->max_conn = (int)Z_LVAL_PP(v);
	}
	RETURN_TRUE;
}

PHP_FUNCTION(swoole_server_handler)
{
	zval *zserv = NULL;
	char *ha_name = NULL;
	int len;
	swServer *serv;
	zval *cb;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rsz", &zserv, &ha_name, &len, &cb) == FAILURE)
	{
		return;
	}
	ZEND_FETCH_RESOURCE(serv, swServer *, &zserv, -1, le_serv_name, le_serv);

	//add ref
	zval_add_ref(&cb);

	if(strncasecmp("onStart", ha_name, len) == 0)
	{
		php_sw_callback[PHP_CB_onStart] = cb;
	}
	else if(strncasecmp("onConnect", ha_name, len) == 0)
	{
		php_sw_callback[PHP_CB_onConnect] = cb;
	}
	else if(strncasecmp("onReceive", ha_name, len) == 0)
	{
		php_sw_callback[PHP_CB_onReceive] = cb;
	}
	else if(strncasecmp("onClose", ha_name, len) == 0)
	{
		php_sw_callback[PHP_CB_onClose] = cb;
	}
	else if(strncasecmp("onShutdown", ha_name, len) == 0)
	{
		php_sw_callback[PHP_CB_onShutdown] = cb;
	}
	else
	{
		zend_error(E_ERROR, "unkown handler");
	}
	RETURN_TRUE;
}

PHP_FUNCTION(swoole_server_close)
{
	zval *zserv = NULL;
	swServer *serv;
	swEvent ev;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rll", &zserv, &ev.fd, &ev.from_id) == FAILURE)
	{
		return;
	}
	//zserv resource
	ZEND_FETCH_RESOURCE(serv, swServer *, &zserv, -1, le_serv_name, le_serv);
	swServer_close(serv,&ev);
	return;
}

int php_swoole_onReceive(swFactory *factory, swEventData *req)
{
	swServer *serv = factory->ptr;
	zval *zserv = (zval *)serv->ptr2;
	zval *args[4];

	zval zfd;
	zval zfrom_id;
	zval zdata;
	zval retval;

	ZVAL_LONG(&zfd, req->fd);
	ZVAL_LONG(&zfrom_id, req->from_id);
	ZVAL_STRINGL(&zdata, req->data, req->len, 0);

	Z_ADDREF_P(&zfd);
	Z_ADDREF_P(&zfrom_id);
	Z_ADDREF_P(&zdata);

	args[0] = zserv;
	args[1] = &zfd;
	args[2] = &zfrom_id;
	args[3] = &zdata;

	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
	if (call_user_function(EG(function_table), NULL, php_sw_callback[PHP_CB_onReceive], &retval, 4, args TSRMLS_CC) == SUCCESS)
	{
		zval_dtor(&retval);
	}
	else
	{
		zend_error(E_WARNING, "SwooleServer: onReceive handler error");
	}
	return SW_OK;
}

void php_swoole_onStart(swServer *serv)
{
	zval *zserv = (zval *)serv->ptr2;
	zval *args[1];
	zval retval;

	args[0] = zserv;
	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx);

	if (call_user_function(EG(function_table), NULL, php_sw_callback[PHP_CB_onStart], &retval, 1, args TSRMLS_CC) == SUCCESS)
	{
		zval_dtor(&retval);
	}
	else
	{
		zend_error(E_WARNING, "SwooleServer: onStart handler error");
	}
}

void php_swoole_onShutdown(swServer *serv)
{
	zval *zserv = (zval *)serv->ptr2;
	zval *args[1];
	zval retval;

	args[0] = zserv;
	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);

	if (call_user_function(EG(function_table), NULL, php_sw_callback[PHP_CB_onShutdown], &retval, 1, args TSRMLS_CC) == SUCCESS)
	{
		zval_dtor(&retval);
	}
	else
	{
		zend_error(E_WARNING, "SwooleServer: onShutdown handler error");
	}
}

void php_swoole_onConnect(swServer *serv, int fd, int from_id)
{
	zval *zserv = (zval *)serv->ptr2;
	zval zfd;
	zval zfrom_id;
	zval *args[3];
	zval retval;

	ZVAL_LONG(&zfd, fd);
	ZVAL_LONG(&zfrom_id, from_id);

	args[0] = zserv;
	args[1] = &zfd;
	args[2] = &zfrom_id;

	Z_ADDREF_P(&zfd);
	Z_ADDREF_P(&zfrom_id);

	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
	if (call_user_function(EG(function_table), NULL, php_sw_callback[PHP_CB_onConnect], &retval, 3, args TSRMLS_CC) == SUCCESS)
	{
		zval_dtor(&retval);
	}
	else
	{
		zend_error(E_WARNING, "SwooleServer: onConnect handler error");
	}
}

void php_swoole_onClose(swServer *serv, int fd, int from_id)
{
	zval *zserv = (zval *)serv->ptr2;
	zval zfd;
	zval zfrom_id;
	zval *args[3];
	zval retval;

	ZVAL_LONG(&zfd, fd);
	ZVAL_LONG(&zfrom_id, from_id);

	Z_ADDREF_P(&zfd);
	Z_ADDREF_P(&zfrom_id);

	args[0] = zserv;
	args[1] = &zfd;
	args[2] = &zfrom_id;

	TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
	if (call_user_function(EG(function_table), NULL, php_sw_callback[PHP_CB_onClose], &retval, 3, args TSRMLS_CC) == SUCCESS)
	{
		zval_dtor(&retval);
	}
	else
	{
		zend_error(E_WARNING, "SwooleServer: onClose handler error");
	}
}

PHP_FUNCTION(swoole_server_start)
{
	zval *zserv = NULL;
	swServer *serv;
	int ret;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &zserv) == FAILURE)
	{
		return;
	}
	ZEND_FETCH_RESOURCE(serv, swServer *, &zserv, -1, le_serv_name, le_serv);
	swTrace("Create host=%s,port=%d,mode=%d \n",serv->host, serv->port, serv->factory_mode);

	if(serv->port==0)
	{
		php_printf("Fail\n");
		exit(2);
	}

	serv->onClose = php_swoole_onClose;
	serv->onStart = php_swoole_onStart;
	serv->onShutdown = php_swoole_onShutdown;
	serv->onConnect = php_swoole_onConnect;
	serv->onReceive = php_swoole_onReceive;

	ret = swServer_create(serv);
	if (ret < 0)
	{
		zend_error(E_ERROR, "create server fail[errno=%d].\n", ret);
		RETURN_LONG(ret);
	}
	ret = swServer_start(serv);
	if (ret < 0)
	{
		zend_error(E_ERROR, "start server fail[errno=%d].\n", ret);
		RETURN_LONG(ret);
	}
	RETURN_TRUE;
}

PHP_FUNCTION(swoole_server_send)
{
	zval *zserv = NULL;
	swServer *serv = NULL;
	swFactory *factory = NULL;
	swSendData send_data;
	int ret;
	int conn_fd;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rls", &zserv, &send_data.fd, &send_data.data, &send_data.len) == FAILURE)
	{
		return;
	}
	ZEND_FETCH_RESOURCE(serv, swServer *, &zserv, -1, le_serv_name, le_serv);
	factory = &(serv->factory);
	ret = factory->finish(factory, &send_data);
	RETURN_LONG(ret);
}

PHP_FUNCTION(swoole_server_getsock)
{
	int fd;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &fd) == FAILURE)
	{
		return;
	}
	php_socket	*php_sock = (php_socket*)emalloc(sizeof(php_socket));
	php_sock->bsd_socket = fd;
	php_sock->type = 0;
	php_sock->error = 0;
	php_sock->blocking = 0;
	ZEND_REGISTER_RESOURCE(return_value, php_sock, php_sockets_le_socket());
}
/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */