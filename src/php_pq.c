/*
    +--------------------------------------------------------------------+
    | PECL :: pq                                                         |
    +--------------------------------------------------------------------+
    | Redistribution and use in source and binary forms, with or without |
    | modification, are permitted provided that the conditions mentioned |
    | in the accompanying LICENSE file are met.                          |
    +--------------------------------------------------------------------+
    | Copyright (c) 2013, Michael Wallner <mike@php.net>                |
    +--------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif

#include <php.h>
#include <Zend/zend_interfaces.h>
#include <ext/standard/info.h>
#include <ext/spl/spl_array.h>

#include <libpq-events.h>
#include <fnmatch.h>

#include "php_pq.h"

typedef int STATUS; /* SUCCESS/FAILURE */

/*
ZEND_DECLARE_MODULE_GLOBALS(pq)
*/


/* {{{ PHP_INI
 */
/* Remove comments and fill if you need to have entries in php.ini
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("pq.global_value",      "42", PHP_INI_ALL, OnUpdateLong, global_value, zend_pq_globals, pq_globals)
    STD_PHP_INI_ENTRY("pq.global_string", "foobar", PHP_INI_ALL, OnUpdateString, global_string, zend_pq_globals, pq_globals)
PHP_INI_END()
*/
/* }}} */

/* {{{ php_pq_init_globals
 */
/* Uncomment this function if you have INI entries
static void php_pq_init_globals(zend_pq_globals *pq_globals)
{
	pq_globals->global_value = 0;
	pq_globals->global_string = NULL;
}
*/
/* }}} */

static zend_class_entry *php_pqconn_class_entry;
static zend_class_entry *php_pqres_class_entry;
static zend_class_entry *php_pqstm_class_entry;
static zend_class_entry *php_pqtxn_class_entry;
static zend_class_entry *php_pqcancel_class_entry;
static zend_class_entry *php_pqevent_class_entry;

static zend_object_handlers php_pqconn_object_handlers;
static zend_object_handlers php_pqres_object_handlers;
static zend_object_handlers php_pqstm_object_handlers;
static zend_object_handlers php_pqtxn_object_handlers;
static zend_object_handlers php_pqcancel_object_handlers;
static zend_object_handlers php_pqevent_object_handlers;

typedef struct php_pq_callback {
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
	void *data;
} php_pq_callback_t;

typedef struct php_pq_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	void *intern;
} php_pq_object_t;

typedef struct php_pqconn {
	PGconn *conn;
	int (*poller)(PGconn *);
	HashTable listeners;
	HashTable eventhandlers;
	php_pq_callback_t onevent;
	unsigned unbuffered:1;
} php_pqconn_t;

typedef struct php_pqconn_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqconn_t *intern;
} php_pqconn_object_t;

typedef struct php_pqconn_event_data {
	php_pqconn_object_t *obj;
#ifdef ZTS
	void ***ts;
#endif
} php_pqconn_event_data_t;

typedef enum php_pqres_fetch {
	PHP_PQRES_FETCH_ARRAY,
	PHP_PQRES_FETCH_ASSOC,
	PHP_PQRES_FETCH_OBJECT
} php_pqres_fetch_t;

typedef struct php_pqres_iterator {
	zend_object_iterator zi;
	zval *current_val;
	unsigned index;
	php_pqres_fetch_t fetch_type;
} php_pqres_iterator_t;

typedef struct php_pqres {
	PGresult *res;
	php_pqres_iterator_t *iter;
} php_pqres_t;

typedef struct php_pqres_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqres_t *intern;
} php_pqres_object_t;

typedef struct php_pqstm {
	php_pqconn_object_t *conn;
	char *name;
} php_pqstm_t;

typedef struct php_pqstm_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqstm_t *intern;
} php_pqstm_object_t;

typedef enum php_pqtxn_isolation {
	PHP_PQTXN_READ_COMMITTED,
	PHP_PQTXN_REPEATABLE_READ,
	PHP_PQTXN_SERIALIZABLE,
} php_pqtxn_isolation_t;

typedef struct php_pqtxn {
	php_pqconn_object_t *conn;
	php_pqtxn_isolation_t isolation;
	unsigned readonly:1;
	unsigned deferrable:1;
} php_pqtxn_t;

typedef struct php_pqtxn_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqtxn_t *intern;
} php_pqtxn_object_t;

typedef struct php_pqcancel {
	PGcancel *cancel;
	php_pqconn_object_t *conn;
} php_pqcancel_t;

typedef struct php_pqcancel_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqcancel_t *intern;
} php_pqcancel_object_t;

typedef struct php_pqevent {
	php_pq_callback_t cb;
	php_pqconn_object_t *conn;
	char *type;
} php_pqevent_t;

typedef struct php_pqevent_object {
	zend_object zo;
	zend_object_value zv;
	HashTable *prophandler;
	php_pqevent_t *intern;
} php_pqevent_object_t;

static HashTable php_pqconn_object_prophandlers;
static HashTable php_pqres_object_prophandlers;
static HashTable php_pqstm_object_prophandlers;
static HashTable php_pqtxn_object_prophandlers;
static HashTable php_pqcancel_object_prophandlers;
static HashTable php_pqevent_object_prophandlers;

typedef void (*php_pq_object_prophandler_func_t)(zval *object, void *o, zval *return_value TSRMLS_DC);

typedef struct php_pq_object_prophandler {
	php_pq_object_prophandler_func_t read;
	php_pq_object_prophandler_func_t write;
} php_pq_object_prophandler_t;

static zend_object_iterator_funcs php_pqres_iterator_funcs;

static zend_object_iterator *php_pqres_iterator_init(zend_class_entry *ce, zval *object, int by_ref TSRMLS_DC)
{
	php_pqres_iterator_t *iter;
	zval *prop, *zfetch_type;

	iter = ecalloc(1, sizeof(*iter));
	iter->zi.funcs = &php_pqres_iterator_funcs;
	iter->zi.data = object;
	Z_ADDREF_P(object);

	zfetch_type = prop = zend_read_property(ce, object, ZEND_STRL("fetchType"), 0 TSRMLS_CC);
	if (Z_TYPE_P(zfetch_type) != IS_LONG) {
		convert_to_long_ex(&zfetch_type);
	}
	iter->fetch_type = Z_LVAL_P(zfetch_type);
	if (zfetch_type != prop) {
		zval_ptr_dtor(&zfetch_type);
	}
	if (Z_REFCOUNT_P(prop)) {
		zval_ptr_dtor(&prop);
	} else {
		zval_dtor(prop);
		FREE_ZVAL(prop);
	}

	return (zend_object_iterator *) iter;
}

static void php_pqres_iterator_dtor(zend_object_iterator *i TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	if (iter->current_val) {
		zval_ptr_dtor(&iter->current_val);
		iter->current_val = NULL;
	}
	zval_ptr_dtor((zval **) &iter->zi.data);
	efree(iter);
}

static STATUS php_pqres_iterator_valid(zend_object_iterator *i TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;
	php_pqres_object_t *obj = zend_object_store_get_object(iter->zi.data TSRMLS_CC);

	if (PQresultStatus(obj->intern->res) != PGRES_TUPLES_OK) {
		return FAILURE;
	}
	if (PQntuples(obj->intern->res) <= iter->index) {
		return FAILURE;
	}

	return SUCCESS;
}

static zval *php_pqres_row_to_zval(PGresult *res, unsigned row, php_pqres_fetch_t fetch_type TSRMLS_DC)
{
	zval *data;
	int c, cols;

	MAKE_STD_ZVAL(data);
	if (PHP_PQRES_FETCH_OBJECT == fetch_type) {
		object_init(data);
	} else {
		array_init(data);
	}

	for (c = 0, cols = PQnfields(res); c < cols; ++c) {
		if (PQgetisnull(res, row, c)) {
			switch (fetch_type) {
			case PHP_PQRES_FETCH_OBJECT:
				add_property_null(data, PQfname(res, c));
				break;

			case PHP_PQRES_FETCH_ASSOC:
				add_assoc_null(data, PQfname(res, c));
				break;

			case PHP_PQRES_FETCH_ARRAY:
				add_index_null(data, c);
				break;
			}
		} else {
			char *val = PQgetvalue(res, row, c);
			int len = PQgetlength(res, row, c);

			switch (fetch_type) {
			case PHP_PQRES_FETCH_OBJECT:
				add_property_stringl(data, PQfname(res, c), val, len, 1);
				break;

			case PHP_PQRES_FETCH_ASSOC:
				add_assoc_stringl(data, PQfname(res, c), val, len, 1);
				break;

			case PHP_PQRES_FETCH_ARRAY:
				add_index_stringl(data, c, val, len ,1);
				break;
			}
		}
	}

	return data;
}

static void php_pqres_iterator_current(zend_object_iterator *i, zval ***data_ptr TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;
	php_pqres_object_t *obj = zend_object_store_get_object(iter->zi.data TSRMLS_CC);

	if (iter->current_val) {
		zval_ptr_dtor(&iter->current_val);
	}
	iter->current_val = php_pqres_row_to_zval(obj->intern->res, iter->index, iter->fetch_type TSRMLS_CC);
	*data_ptr = &iter->current_val;
}

static int php_pqres_iterator_key(zend_object_iterator *i, char **key_str, uint *key_len, ulong *key_num TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	*key_num = (ulong) iter->index;

	return HASH_KEY_IS_LONG;
}

static void php_pqres_iterator_next(zend_object_iterator *i TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	++iter->index;
}

static void php_pqres_iterator_rewind(zend_object_iterator *i TSRMLS_DC)
{
	php_pqres_iterator_t *iter = (php_pqres_iterator_t *) i;

	iter->index = 0;
}

static zend_object_iterator_funcs php_pqres_iterator_funcs = {
	php_pqres_iterator_dtor,
	/* check for end of iteration (FAILURE or SUCCESS if data is valid) */
	php_pqres_iterator_valid,
	/* fetch the item data for the current element */
	php_pqres_iterator_current,
	/* fetch the key for the current element (return HASH_KEY_IS_STRING or HASH_KEY_IS_LONG) (optional, may be NULL) */
	php_pqres_iterator_key,
	/* step forwards to next element */
	php_pqres_iterator_next,
	/* rewind to start of data (optional, may be NULL) */
	php_pqres_iterator_rewind,
	/* invalidate current value/key (optional, may be NULL) */
	NULL
};

static STATUS php_pqres_success(PGresult *res TSRMLS_DC)
{
	switch (PQresultStatus(res)) {
	case PGRES_BAD_RESPONSE:
	case PGRES_NONFATAL_ERROR:
	case PGRES_FATAL_ERROR:
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "%s", PQresultErrorMessage(res));
		return FAILURE;
	default:
		return SUCCESS;
	}
}

static void php_pq_callback_dtor(php_pq_callback_t *cb) {
	if (cb->fci.size > 0) {
		zend_fcall_info_args_clear(&cb->fci, 1);
		zval_ptr_dtor(&cb->fci.function_name);
		if (cb->fci.object_ptr) {
			zval_ptr_dtor(&cb->fci.object_ptr);
		}
	}
	cb->fci.size = 0;
}

static void php_pq_callback_addref(php_pq_callback_t *cb)
{
	Z_ADDREF_P(cb->fci.function_name);
	if (cb->fci.object_ptr) {
		Z_ADDREF_P(cb->fci.object_ptr);
	}
}

static void php_pq_object_to_zval(void *o, zval **zv TSRMLS_DC)
{
	php_pq_object_t *obj = o;

	if (!*zv) {
		MAKE_STD_ZVAL(*zv);
	}

	zend_objects_store_add_ref_by_handle(obj->zv.handle TSRMLS_CC);

	(*zv)->type = IS_OBJECT;
	(*zv)->value.obj = obj->zv;
}

static void php_pq_object_addref(void *o TSRMLS_DC)
{
	php_pq_object_t *obj = o;
	zend_objects_store_add_ref_by_handle(obj->zv.handle TSRMLS_CC);
}

static void php_pq_object_delref(void *o TSRMLS_DC)
{
	php_pq_object_t *obj = o;
	zend_objects_store_del_ref_by_handle_ex(obj->zv.handle, obj->zv.handlers TSRMLS_CC);
}

static void php_pqconn_object_free(void *o TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	if (obj->intern) {
		PQfinish(obj->intern->conn);
		php_pq_callback_dtor(&obj->intern->onevent);
		zend_hash_destroy(&obj->intern->listeners);
		zend_hash_destroy(&obj->intern->eventhandlers);
		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

static int php_pqconn_event(PGEventId id, void *e, void *data);

static void php_pqres_object_free(void *o TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	if (obj->intern) {
		if (obj->intern->res) {
			zval *res = PQresultInstanceData(obj->intern->res, php_pqconn_event);
			if (res) {
				PQresultSetInstanceData(obj->intern->res, php_pqconn_event, NULL);
				zval_ptr_dtor(&res);
			} else {
				PQclear(obj->intern->res);
				obj->intern->res = NULL;
			}
		}

		if (obj->intern->iter) {
			php_pqres_iterator_dtor((zend_object_iterator *) obj->intern->iter TSRMLS_CC);
			obj->intern->iter = NULL;
		}

		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

static void php_pqstm_object_free(void *o TSRMLS_DC)
{
	php_pqstm_object_t *obj = o;

	if (obj->intern) {
		php_pq_object_delref(obj->intern->conn TSRMLS_CC);
		efree(obj->intern->name);
		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

static void php_pqtxn_object_free(void *o TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;

	if (obj->intern) {
		php_pq_object_delref(obj->intern->conn TSRMLS_CC);
		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

static void php_pqcancel_object_free(void *o TSRMLS_DC)
{
	php_pqcancel_object_t *obj = o;

	if (obj->intern) {
		PQfreeCancel(obj->intern->cancel);
		php_pq_object_delref(obj->intern->conn TSRMLS_CC);
		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}

static void php_pqevent_object_free(void *o TSRMLS_DC)
{
	php_pqevent_object_t *obj = o;

	if (obj->intern) {
		php_pq_callback_dtor(&obj->intern->cb);
		php_pq_object_delref(obj->intern->conn TSRMLS_CC);
		efree(obj->intern->type);
		efree(obj->intern);
		obj->intern = NULL;
	}
	zend_object_std_dtor((zend_object *) o TSRMLS_CC);
	efree(obj);
}


static zend_object_value php_pqconn_create_object_ex(zend_class_entry *ce, php_pqconn_t *intern, php_pqconn_object_t **ptr TSRMLS_DC)
{
	php_pqconn_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);
	o->prophandler = &php_pqconn_object_prophandlers;

	if (ptr) {
		*ptr = o;
	}

	if (intern) {
		o->intern = intern;
	}

	o->zv.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqconn_object_free, NULL TSRMLS_CC);
	o->zv.handlers = &php_pqconn_object_handlers;

	return o->zv;
}

static zend_object_value php_pqres_create_object_ex(zend_class_entry *ce, php_pqres_t *intern, php_pqres_object_t **ptr TSRMLS_DC)
{
	php_pqres_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);
	o->prophandler = &php_pqres_object_prophandlers;

	if (ptr) {
		*ptr = o;
	}

	if (intern) {
		o->intern = intern;
	}

	o->zv.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqres_object_free, NULL TSRMLS_CC);
	o->zv.handlers = &php_pqres_object_handlers;

	return o->zv;
}

static zend_object_value php_pqstm_create_object_ex(zend_class_entry *ce, php_pqstm_t *intern, php_pqstm_object_t **ptr TSRMLS_DC)
{
	php_pqstm_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);
	o->prophandler = &php_pqstm_object_prophandlers;

	if (ptr) {
		*ptr = o;
	}

	if (intern) {
		o->intern = intern;
	}

	o->zv.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqstm_object_free, NULL TSRMLS_CC);
	o->zv.handlers = &php_pqstm_object_handlers;

	return o->zv;
}

static zend_object_value php_pqtxn_create_object_ex(zend_class_entry *ce, php_pqtxn_t *intern, php_pqtxn_object_t **ptr TSRMLS_DC)
{
	php_pqtxn_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);
	o->prophandler = &php_pqtxn_object_prophandlers;

	if (ptr) {
		*ptr = o;
	}

	if (intern) {
		o->intern = intern;
	}

	o->zv.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqtxn_object_free, NULL TSRMLS_CC);
	o->zv.handlers = &php_pqtxn_object_handlers;

	return o->zv;
}

static zend_object_value php_pqcancel_create_object_ex(zend_class_entry *ce, php_pqcancel_t *intern, php_pqcancel_object_t **ptr TSRMLS_DC)
{
	php_pqcancel_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);
	o->prophandler = &php_pqcancel_object_prophandlers;

	if (ptr) {
		*ptr = o;
	}

	if (intern) {
		o->intern = intern;
	}

	o->zv.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqcancel_object_free, NULL TSRMLS_CC);
	o->zv.handlers = &php_pqcancel_object_handlers;

	return o->zv;
}

static zend_object_value php_pqevent_create_object_ex(zend_class_entry *ce, php_pqevent_t *intern, php_pqevent_object_t **ptr TSRMLS_DC)
{
	php_pqevent_object_t *o;

	o = ecalloc(1, sizeof(*o));
	zend_object_std_init((zend_object *) o, ce TSRMLS_CC);
	object_properties_init((zend_object *) o, ce);
	o->prophandler = &php_pqevent_object_prophandlers;

	if (ptr) {
		*ptr = o;
	}

	if (intern) {
		o->intern = intern;
	}

	o->zv.handle = zend_objects_store_put((zend_object *) o, NULL, php_pqevent_object_free, NULL TSRMLS_CC);
	o->zv.handlers = &php_pqevent_object_handlers;

	return o->zv;
}

static zend_object_value php_pqconn_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqconn_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static zend_object_value php_pqres_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqres_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static zend_object_value php_pqstm_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqstm_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static zend_object_value php_pqtxn_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqtxn_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static zend_object_value php_pqcancel_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqcancel_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static zend_object_value php_pqevent_create_object(zend_class_entry *class_type TSRMLS_DC)
{
	return php_pqevent_create_object_ex(class_type, NULL, NULL TSRMLS_CC);
}

static int apply_ph_to_debug(void *p TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	php_pq_object_prophandler_t *ph = p;
	HashTable *ht = va_arg(argv, HashTable *);
	zval **return_value, *object = va_arg(argv, zval *);
	php_pq_object_t *obj = va_arg(argv, php_pq_object_t *);

	if (SUCCESS == zend_hash_find(ht, key->arKey, key->nKeyLength, (void *) &return_value)) {

		if (ph->read) {
			zval_ptr_dtor(return_value);
			MAKE_STD_ZVAL(*return_value);
			ZVAL_NULL(*return_value);

			ph->read(object, obj, *return_value TSRMLS_CC);
		}
	}

	return ZEND_HASH_APPLY_KEEP;
}

static int apply_pi_to_debug(void *p TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	zend_property_info *pi = p;
	HashTable *ht = va_arg(argv, HashTable *);
	zval *object = va_arg(argv, zval *);
	php_pq_object_t *obj = va_arg(argv, php_pq_object_t *);
	zval *property = zend_read_property(obj->zo.ce, object, pi->name, pi->name_length, 0 TSRMLS_CC);

	if (!Z_REFCOUNT_P(property)) {
		Z_ADDREF_P(property);
	}
	zend_hash_add(ht, pi->name, pi->name_length + 1, (void *) &property, sizeof(zval *), NULL);

	return ZEND_HASH_APPLY_KEEP;
}

static HashTable *php_pq_object_debug_info(zval *object, int *temp TSRMLS_DC)
{
	HashTable *ht;
	php_pq_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);

	*temp = 1;
	ALLOC_HASHTABLE(ht);
	ZEND_INIT_SYMTABLE(ht);

	zend_hash_apply_with_arguments(&obj->zo.ce->properties_info TSRMLS_CC, apply_pi_to_debug, 3, ht, object, obj);
	zend_hash_apply_with_arguments(obj->prophandler TSRMLS_CC, apply_ph_to_debug, 3, ht, object, obj);

	return ht;
}

static void php_pqconn_object_read_status(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_LONG(PQstatus(obj->intern->conn));
}

static void php_pqconn_object_read_transaction_status(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_LONG(PQtransactionStatus(obj->intern->conn));
}

static void php_pqconn_object_read_error_message(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	char *error = PQerrorMessage(obj->intern->conn);

	if (error) {
		RETVAL_STRING(error, 1);
	} else {
		RETVAL_NULL();
	}
}

static int apply_notify_listener(void *p, void *arg TSRMLS_DC)
{
	php_pq_callback_t *listener = p;
	PGnotify *nfy = arg;
	zval *zpid, *zchannel, *zmessage;

	MAKE_STD_ZVAL(zpid);
	ZVAL_LONG(zpid, nfy->be_pid);
	MAKE_STD_ZVAL(zchannel);
	ZVAL_STRING(zchannel, nfy->relname, 1);
	MAKE_STD_ZVAL(zmessage);
	ZVAL_STRING(zmessage, nfy->extra, 1);

	zend_fcall_info_argn(&listener->fci TSRMLS_CC, 3, &zchannel, &zmessage, &zpid);
	zend_fcall_info_call(&listener->fci, &listener->fcc, NULL, NULL TSRMLS_CC);

	zval_ptr_dtor(&zchannel);
	zval_ptr_dtor(&zmessage);
	zval_ptr_dtor(&zpid);

	return ZEND_HASH_APPLY_KEEP;
}

static int apply_notify_listeners(void *p TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	HashTable *listeners = p;
	PGnotify *nfy = va_arg(argv, PGnotify *);

	if (0 == fnmatch(key->arKey, nfy->relname, 0)) {
		zend_hash_apply_with_argument(listeners, apply_notify_listener, nfy TSRMLS_CC);
	}

	return ZEND_HASH_APPLY_KEEP;
}

static void php_pqconn_notify_listeners(php_pqconn_object_t *obj TSRMLS_DC)
{
	PGnotify *nfy;

	while ((nfy = PQnotifies(obj->intern->conn))) {
		zend_hash_apply_with_arguments(&obj->intern->listeners TSRMLS_CC, apply_notify_listeners, 1, nfy);
		PQfreemem(nfy);
	}
}

/* FIXME: extend to types->nspname->typname */
#define PHP_PQ_TYPES_QUERY \
	"select t.oid, t.* " \
	"from pg_type t join pg_namespace n on t.typnamespace=n.oid " \
	"where typisdefined " \
	"and typrelid=0 " \
	"and nspname in ('public', 'pg_catalog')"
static void php_pqconn_object_read_types(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	PGresult *res = PQexec(obj->intern->conn, PHP_PQ_TYPES_QUERY);

	php_pqconn_notify_listeners(obj TSRMLS_CC);

	/* FIXME: cache that */
	if (res) {
		if (PGRES_TUPLES_OK == PQresultStatus(res)) {
			int r, rows;
			zval *byoid, *byname;

			MAKE_STD_ZVAL(byoid);
			MAKE_STD_ZVAL(byname);
			object_init(byoid);
			object_init(byname);
			object_init(return_value);
			for (r = 0, rows = PQntuples(res); r < rows; ++r) {
				zval *row = php_pqres_row_to_zval(res, r, PHP_PQRES_FETCH_OBJECT TSRMLS_CC);

				add_property_zval(byoid, PQgetvalue(res, r, 0), row);
				add_property_zval(byname, PQgetvalue(res, r, 1), row);
				zval_ptr_dtor(&row);
			}

			add_property_zval(return_value, "byOid", byoid);
			add_property_zval(return_value, "byName", byname);
			zval_ptr_dtor(&byoid);
			zval_ptr_dtor(&byname);
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not fetch types: %s", PQresultErrorMessage(res));
		}
		PQclear(res);
	} else {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not fetch types: %s", PQerrorMessage(obj->intern->conn));
	}
}

static void php_pqconn_object_read_busy(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_BOOL(PQisBusy(obj->intern->conn));
}

static void php_pqconn_object_read_encoding(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_STRING(pg_encoding_to_char(PQclientEncoding(obj->intern->conn)), 1);
}

static void php_pqconn_object_write_encoding(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;
	zval *zenc = value;

	if (Z_TYPE_P(value) != IS_STRING) {
		convert_to_string_ex(&zenc);
	}

	if (0 > PQsetClientEncoding(obj->intern->conn, Z_STRVAL_P(zenc))) {
		zend_error(E_NOTICE, "Unrecognized encoding '%s'", Z_STRVAL_P(zenc));
	}

	if (zenc != value) {
		zval_ptr_dtor(&zenc);
	}
}

static void php_pqconn_object_read_unbuffered(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	RETVAL_BOOL(obj->intern->unbuffered);
}

static void php_pqconn_object_write_unbuffered(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqconn_object_t *obj = o;

	obj->intern->unbuffered = zend_is_true(value);
}

static void php_pqres_object_read_status(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(PQresultStatus(obj->intern->res));
}

static void php_pqres_object_read_error_message(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;
	char *error = PQresultErrorMessage(obj->intern->res);

	if (error) {
		RETVAL_STRING(error, 1);
	} else {
		RETVAL_NULL();
	}
}

static void php_pqres_object_read_num_rows(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(PQntuples(obj->intern->res));
}

static void php_pqres_object_read_num_cols(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(PQnfields(obj->intern->res));
}

static void php_pqres_object_read_affected_rows(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	RETVAL_LONG(atoi(PQcmdTuples(obj->intern->res)));
}

static void php_pqres_object_read_fetch_type(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;

	if (obj->intern->iter) {
		RETVAL_LONG(obj->intern->iter->fetch_type);
	} else {
		RETVAL_LONG(PHP_PQRES_FETCH_ARRAY);
	}
}

static void php_pqres_object_write_fetch_type(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqres_object_t *obj = o;
	zval *zfetch_type = value;

	if (Z_TYPE_P(zfetch_type) != IS_LONG) {
		convert_to_long_ex(&zfetch_type);
	}

	if (!obj->intern->iter) {
		obj->intern->iter = (php_pqres_iterator_t *) php_pqres_iterator_init(Z_OBJCE_P(object), object, 0 TSRMLS_CC);
		obj->intern->iter->zi.funcs->rewind((zend_object_iterator *) obj->intern->iter TSRMLS_CC);
	}
	obj->intern->iter->fetch_type = Z_LVAL_P(zfetch_type);

	if (zfetch_type != value) {
		zval_ptr_dtor(&zfetch_type);
	}
}

static void php_pqstm_object_read_name(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqstm_object_t *obj = o;

	RETVAL_STRING(obj->intern->name, 1);
}

static void php_pqstm_object_read_connection(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqstm_object_t *obj = o;

	php_pq_object_to_zval(obj->intern->conn, &return_value TSRMLS_CC);
}

static void php_pqtxn_object_read_connection(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;

	php_pq_object_to_zval(obj->intern->conn, &return_value TSRMLS_CC);
}

static void php_pqtxn_object_read_isolation(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;

	RETVAL_LONG(obj->intern->isolation);
}

static void php_pqtxn_object_read_readonly(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;

	RETVAL_LONG(obj->intern->readonly);
}

static void php_pqtxn_object_read_deferrable(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;

	RETVAL_LONG(obj->intern->deferrable);
}

static void php_pqtxn_object_write_isolation(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;
	php_pqtxn_isolation_t orig = obj->intern->isolation;
	zval *zisolation = value;
	PGresult *res;

	if (Z_TYPE_P(zisolation) != IS_LONG) {
		convert_to_long_ex(&zisolation);
	}

	switch ((obj->intern->isolation = Z_LVAL_P(zisolation))) {
	case PHP_PQTXN_READ_COMMITTED:
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION READ COMMITED");
		break;
	case PHP_PQTXN_REPEATABLE_READ:
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION REPEATABLE READ");
		break;
	case PHP_PQTXN_SERIALIZABLE:
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION SERIALIZABLE");
		break;
	default:
		obj->intern->isolation = orig;
		res = NULL;
		break;
	}

	if (zisolation != value) {
		zval_ptr_dtor(&zisolation);
	}

	if (res) {
		php_pqres_success(res TSRMLS_CC);
		PQclear(res);
	}
}

static void php_pqtxn_object_write_readonly(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;
	PGresult *res;

	if ((obj->intern->readonly = zend_is_true(value))) {
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION READ ONLY");
	} else {
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION READ WRITE");
	}

	if (res) {
		php_pqres_success(res TSRMLS_CC);
		PQclear(res);
	}
}

static void php_pqtxn_object_write_deferrable(zval *object, void *o, zval *value TSRMLS_DC)
{
	php_pqtxn_object_t *obj = o;
	PGresult *res;

	if ((obj->intern->deferrable = zend_is_true(value))) {
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION DEFERRABLE");
	} else {
		res = PQexec(obj->intern->conn->intern->conn, "SET TRANSACTION NOT DEFERRABLE");
	}

	if (res) {
		php_pqres_success(res TSRMLS_CC);
		PQclear(res);
	}
}

static void php_pqcancel_object_read_connection(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqcancel_object_t *obj = o;

	php_pq_object_to_zval(obj->intern->conn, &return_value TSRMLS_CC);
}

static void php_pqevent_object_read_connection(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqevent_object_t *obj = o;

	php_pq_object_to_zval(obj->intern->conn, &return_value TSRMLS_CC);
}

static void php_pqevent_object_read_type(zval *object, void *o, zval *return_value TSRMLS_DC)
{
	php_pqevent_object_t *obj = o;

	RETVAL_STRING(obj->intern->type, 1);
}

static zend_class_entry *ancestor(zend_class_entry *ce) {
	while (ce->parent) {
		ce = ce->parent;
	}
	return ce;
}

static zval *php_pq_object_read_prop(zval *object, zval *member, int type, const zend_literal *key TSRMLS_DC)
{
	php_pq_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);
	php_pq_object_prophandler_t *handler;
	zval *return_value;

	if (!obj->intern) {
		zend_error(E_WARNING, "%s not initialized", ancestor(obj->zo.ce)->name);
	} else if ((SUCCESS == zend_hash_find(obj->prophandler, Z_STRVAL_P(member), Z_STRLEN_P(member)+1, (void *) &handler)) && handler->read) {
		if (type == BP_VAR_R) {
			ALLOC_ZVAL(return_value);
			Z_SET_REFCOUNT_P(return_value, 0);
			Z_UNSET_ISREF_P(return_value);

			handler->read(object, obj, return_value TSRMLS_CC);
		} else {
			zend_error(E_ERROR, "Cannot access %s properties by reference or array key/index", ancestor(obj->zo.ce)->name);
			return_value = NULL;
		}
	} else {
		return_value = zend_get_std_object_handlers()->read_property(object, member, type, key TSRMLS_CC);
	}

	return return_value;
}

static void php_pq_object_write_prop(zval *object, zval *member, zval *value, const zend_literal *key TSRMLS_DC)
{
	php_pq_object_t *obj = zend_object_store_get_object(object TSRMLS_CC);
	php_pq_object_prophandler_t *handler;

	if (SUCCESS == zend_hash_find(obj->prophandler, Z_STRVAL_P(member), Z_STRLEN_P(member)+1, (void *) &handler)) {
		if (handler->write) {
			handler->write(object, obj, value TSRMLS_CC);
		}
	} else {
		zend_get_std_object_handlers()->write_property(object, member, value, key TSRMLS_CC);
	}
}

static STATUS php_pqconn_update_socket(zval *this_ptr, php_pqconn_object_t *obj TSRMLS_DC)
{
	zval *zsocket, zmember;
	php_stream *stream;
	STATUS retval;
	int socket;
	
	if (!obj) {
		obj = zend_object_store_get_object(getThis() TSRMLS_CC);
	}
	
	INIT_PZVAL(&zmember);
	ZVAL_STRINGL(&zmember, "socket", sizeof("socket")-1, 0);
	MAKE_STD_ZVAL(zsocket);
	
	if ((CONNECTION_BAD != PQstatus(obj->intern->conn))
	&&	(-1 < (socket = PQsocket(obj->intern->conn)))
	&&	(stream = php_stream_fopen_from_fd(socket, "r+b", NULL))) {
		php_stream_to_zval(stream, zsocket);
		retval = SUCCESS;
	} else {
		ZVAL_NULL(zsocket);
		retval = FAILURE;
	}
	zend_get_std_object_handlers()->write_property(getThis(), &zmember, zsocket, NULL TSRMLS_CC);
	zval_ptr_dtor(&zsocket);
	
	return retval;
}

#ifdef ZTS
#	define TSRMLS_DF(d) TSRMLS_D = (d)->ts
#	define TSRMLS_CF(d) (d)->ts = TSRMLS_C
#else
#	define TSRMLS_DF(d)
#	define TSRMLS_CF(d)
#endif

static void php_pqconn_event_register(PGEventRegister *event, php_pqconn_event_data_t *data)
{
	PQsetInstanceData(event->conn, php_pqconn_event, data);
}
static void php_pqconn_event_conndestroy(PGEventConnDestroy *event, php_pqconn_event_data_t *data)
{
	PQsetInstanceData(event->conn, php_pqconn_event, NULL);
	efree(data);
}
static void php_pqconn_event_resultcreate(PGEventResultCreate *event, php_pqconn_event_data_t *data)
{
	TSRMLS_DF(data);

	if (data->obj->intern->onevent.fci.size > 0) {
		zval *res;
		php_pqres_t *r = ecalloc(1, sizeof(*r));

		MAKE_STD_ZVAL(res);
		r->res = event->result;
		res->type = IS_OBJECT;
		res->value.obj = php_pqres_create_object_ex(php_pqres_class_entry, r, NULL TSRMLS_CC);

		Z_ADDREF_P(res);
		PQresultSetInstanceData(event->result, php_pqconn_event, res);

		zend_fcall_info_argn(&data->obj->intern->onevent.fci TSRMLS_CC, 1, &res);
		zend_fcall_info_call(&data->obj->intern->onevent.fci, &data->obj->intern->onevent.fcc, NULL, NULL TSRMLS_CC);
		zval_ptr_dtor(&res);
	}
}

static int php_pqconn_event(PGEventId id, void *e, void *data)
{
	switch (id) {
	case PGEVT_REGISTER:
		php_pqconn_event_register(e, data);
		break;
	case PGEVT_CONNDESTROY:
		php_pqconn_event_conndestroy(e, data);
		break;
	case PGEVT_RESULTCREATE:
		php_pqconn_event_resultcreate(e, data);
		break;
	default:
		break;
	}

	return 1;
}

static php_pqconn_event_data_t *php_pqconn_event_data_init(php_pqconn_object_t *obj TSRMLS_DC)
{
	php_pqconn_event_data_t *data = emalloc(sizeof(*data));

	data->obj = obj;
	TSRMLS_CF(data);

	return data;
}

static int apply_notice_event(void *p, void *a TSRMLS_DC)
{
	zval **evh = p;
	zval *args = a;
	zval *retval = NULL;

	zend_call_method_with_1_params(evh, Z_OBJCE_PP(evh), NULL, "trigger", &retval, args);
	if (retval) {
		zval_ptr_dtor(&retval);
	}

	return ZEND_HASH_APPLY_KEEP;
}

static void php_pqconn_notice_recv(void *p, const PGresult *res)
{
	php_pqconn_event_data_t *data = p;
	zval **evhs;
	TSRMLS_DF(data);

	if (SUCCESS == zend_hash_find(&data->obj->intern->eventhandlers, ZEND_STRS("notice"), (void *) &evhs)) {
		zval *args, *connection = NULL;

		MAKE_STD_ZVAL(args);
		array_init(args);
		php_pq_object_to_zval(data->obj, &connection TSRMLS_CC);
		add_next_index_zval(args, connection);
		add_next_index_string(args, PQresultErrorMessage(res), 1);
		zend_hash_apply_with_argument(Z_ARRVAL_PP(evhs), apply_notice_event, args TSRMLS_CC);
		zval_ptr_dtor(&args);
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_construct, 0, 0, 1)
	ZEND_ARG_INFO(0, dsn)
	ZEND_ARG_INFO(0, async)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, __construct) {
	zend_error_handling zeh;
	char *dsn_str;
	int dsn_len;
	zend_bool async = 0;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|b", &dsn_str, &dsn_len, &async)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);
		php_pqconn_event_data_t *data =  php_pqconn_event_data_init(obj TSRMLS_CC);

		obj->intern = ecalloc(1, sizeof(*obj->intern));

		zend_hash_init(&obj->intern->listeners, 0, NULL, (dtor_func_t) zend_hash_destroy, 0);
		zend_hash_init(&obj->intern->eventhandlers, 0, NULL, ZVAL_PTR_DTOR, 0);


		if (async) {
			obj->intern->conn = PQconnectStart(dsn_str);
			obj->intern->poller = (int (*)(PGconn*)) PQconnectPoll;
		} else {
			obj->intern->conn = PQconnectdb(dsn_str);
		}

		PQsetNoticeReceiver(obj->intern->conn, php_pqconn_notice_recv, data);
		PQregisterEventProc(obj->intern->conn, php_pqconn_event, "ext-pq", data);

		if (SUCCESS != php_pqconn_update_socket(getThis(), obj TSRMLS_CC)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Connection failed: %s", PQerrorMessage(obj->intern->conn));
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_reset, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, reset) {
	if (SUCCESS == zend_parse_parameters_none()) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			PQreset(obj->intern->conn);

			if (CONNECTION_OK == PQstatus(obj->intern->conn)) {
				RETURN_TRUE;
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Connection reset failed: %s", PQerrorMessage(obj->intern->conn));
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
		}
		RETURN_FALSE;
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_reset_async, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, resetAsync) {
	if (SUCCESS == zend_parse_parameters_none()) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			if (PQresetStart(obj->intern->conn)) {
				obj->intern->poller = (int (*)(PGconn*)) PQresetPoll;
				RETURN_TRUE;
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
		}
		RETURN_FALSE;
	}
}

static void php_pqconn_add_listener(php_pqconn_object_t *obj, const char *channel_str, size_t channel_len, php_pq_callback_t *listener TSRMLS_DC)
{
	HashTable ht, *existing_listeners;

	php_pq_callback_addref(listener);

	if (SUCCESS == zend_hash_find(&obj->intern->listeners, channel_str, channel_len + 1, (void *) &existing_listeners)) {
		zend_hash_next_index_insert(existing_listeners, (void *) listener, sizeof(*listener), NULL);
	} else {
		zend_hash_init(&ht, 1, NULL, (dtor_func_t) php_pq_callback_dtor, 0);
		zend_hash_next_index_insert(&ht, (void *) listener, sizeof(*listener), NULL);
		zend_hash_add(&obj->intern->listeners, channel_str, channel_len + 1, (void *) &ht, sizeof(HashTable), NULL);
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_listen, 0, 0, 0)
	ZEND_ARG_INFO(0, channel)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, listen) {
	char *channel_str = NULL;
	int channel_len = 0;
	php_pq_callback_t listener;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sf", &channel_str, &channel_len, &listener.fci, &listener.fcc)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		obj->intern->poller = PQconsumeInput;

		if (obj->intern) {
			char *quoted_channel = PQescapeIdentifier(obj->intern->conn, channel_str, channel_len);

			if (quoted_channel) {
				PGresult *res;
				char *cmd;

				spprintf(&cmd, 0, "LISTEN %s", channel_str);
				res = PQexec(obj->intern->conn, cmd);

				efree(cmd);
				PQfreemem(quoted_channel);

				if (res) {
					if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
						php_pqconn_add_listener(obj, channel_str, channel_len, &listener TSRMLS_CC);
						RETVAL_TRUE;
					} else {
						RETVAL_FALSE;
					}
					PQclear(res);
				} else {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not install listener: %s", PQerrorMessage(obj->intern->conn));
					RETVAL_FALSE;
				}

				php_pqconn_notify_listeners(obj TSRMLS_CC);
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not escape channel identifier: %s", PQerrorMessage(obj->intern->conn));
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
			RETVAL_FALSE;
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_notify, 0, 0, 2)
	ZEND_ARG_INFO(0, channel)
	ZEND_ARG_INFO(0, message)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, notify) {
	char *channel_str, *message_str;
	int channel_len, message_len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &channel_str, &channel_len, &message_str, &message_len)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			PGresult *res;
			char *params[2] = {channel_str, message_str};

			res = PQexecParams(obj->intern->conn, "select pg_notify($1, $2)", 2, NULL, (const char *const*) params, NULL, NULL, 0);

			if (res) {
				if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
					RETVAL_TRUE;
				} else {
					RETVAL_FALSE;
				}
				PQclear(res);
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not notify listeners: %s", PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			}

			php_pqconn_notify_listeners(obj TSRMLS_CC);

		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
			RETVAL_FALSE;
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_poll, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, poll) {
	if (SUCCESS == zend_parse_parameters_none()) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			if (obj->intern->poller) {
				if (obj->intern->poller == PQconsumeInput) {
					RETVAL_LONG(obj->intern->poller(obj->intern->conn) * PGRES_POLLING_OK);
					php_pqconn_notify_listeners(obj TSRMLS_CC);
					return;
				} else {
					RETURN_LONG(obj->intern->poller(obj->intern->conn));
				}
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "No asynchronous operation active");
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
		}
		RETURN_FALSE;
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_exec, 0, 0, 1)
	ZEND_ARG_INFO(0, query)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, exec) {
	zend_error_handling zeh;
	char *query_str;
	int query_len;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &query_str, &query_len)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			PGresult *res = PQexec(obj->intern->conn, query_str);

			php_pqconn_notify_listeners(obj TSRMLS_CC);

			if (res) {
				if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
					php_pqres_t *r = ecalloc(1, sizeof(*r));

					r->res = res;
					return_value->type = IS_OBJECT;
					return_value->value.obj = php_pqres_create_object_ex(php_pqres_class_entry, r, NULL TSRMLS_CC);
				}
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not execute query: %s", PQerrorMessage(obj->intern->conn));
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_get_result, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, getResult) {
	if (SUCCESS == zend_parse_parameters_none()) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			PGresult *res = PQgetResult(obj->intern->conn);

			if (res) {
				php_pqres_t *r = ecalloc(1, sizeof(*r));

				r->res = res;
				return_value->type = IS_OBJECT;
				return_value->value.obj = php_pqres_create_object_ex(php_pqres_class_entry, r, NULL TSRMLS_CC);
			} else {
				RETVAL_NULL();
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
			RETVAL_FALSE;
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_exec_async, 0, 0, 1)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, execAsync) {
	zend_error_handling zeh;
	php_pq_callback_t resolver = {{0}};
	char *query_str;
	int query_len;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|f", &query_str, &query_len, &resolver.fci, &resolver.fcc)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			php_pq_callback_dtor(&obj->intern->onevent);
			if (resolver.fci.size > 0) {
				obj->intern->onevent = resolver;
				php_pq_callback_addref(&obj->intern->onevent);
			}

			obj->intern->poller = PQconsumeInput;

			if (PQsendQuery(obj->intern->conn, query_str)) {
				if (obj->intern->unbuffered) {
					if (!PQsetSingleRowMode(obj->intern->conn)) {
						php_error_docref(NULL TSRMLS_CC, E_NOTICE, "Could not enable unbuffered mode: %s", PQerrorMessage(obj->intern->conn));
					}
				}
				RETVAL_TRUE;
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not execute query: %s", PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
			RETVAL_FALSE;
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

static int apply_to_oid(void *p, void *arg TSRMLS_DC)
{
	Oid **types = arg;
	zval **ztype = p;

	if (Z_TYPE_PP(ztype) != IS_LONG) {
		convert_to_long_ex(ztype);
	}

	**types = Z_LVAL_PP(ztype);
	++*types;

	if (*ztype != *(zval **)p) {
		zval_ptr_dtor(ztype);
	}
	return ZEND_HASH_APPLY_KEEP;
}

static int apply_to_param(void *p TSRMLS_DC, int argc, va_list argv, zend_hash_key *key)
{
	char ***params;
	HashTable *zdtor;
	zval **zparam = p;

	params = (char ***) va_arg(argv, char ***);
	zdtor = (HashTable *) va_arg(argv, HashTable *);

	if (Z_TYPE_PP(zparam) == IS_NULL) {
		**params = NULL;
		++*params;
	} else {
		if (Z_TYPE_PP(zparam) != IS_STRING) {
			convert_to_string_ex(zparam);
		}

		**params = Z_STRVAL_PP(zparam);
		++*params;

		if (*zparam != *(zval **)p) {
			zend_hash_next_index_insert(zdtor, zparam, sizeof(zval *), NULL);
		}
	}
	return ZEND_HASH_APPLY_KEEP;
}

static int php_pq_types_to_array(HashTable *ht, Oid **types TSRMLS_DC)
{
	int count = zend_hash_num_elements(ht);
	
	*types = NULL;

	if (count) {
		Oid *tmp;

		/* +1 for when less types than params are specified */
		*types = tmp = ecalloc(count + 1, sizeof(Oid));
		zend_hash_apply_with_argument(ht, apply_to_oid, &tmp TSRMLS_CC);
	}
	
	return count;
}

static int php_pq_params_to_array(HashTable *ht, char ***params, HashTable *zdtor TSRMLS_DC)
{
	int count = zend_hash_num_elements(ht);
	
	*params = NULL;

	if (count) {
		char **tmp;

		*params = tmp = ecalloc(count, sizeof(char *));
		zend_hash_apply_with_arguments(ht TSRMLS_CC, apply_to_param, 2, &tmp, zdtor);
	}
	
	return count;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_exec_params, 0, 0, 2)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_ARRAY_INFO(0, params, 0)
	ZEND_ARG_ARRAY_INFO(0, types, 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, execParams) {
	zend_error_handling zeh;
	char *query_str;
	int query_len;
	zval *zparams;
	zval *ztypes = NULL;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sa/|a/!", &query_str, &query_len, &zparams, &ztypes)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			PGresult *res;
			int count;
			Oid *types = NULL;
			char **params = NULL;
			HashTable zdtor;

			ZEND_INIT_SYMTABLE(&zdtor);
			count = php_pq_params_to_array(Z_ARRVAL_P(zparams), &params, &zdtor TSRMLS_CC);

			if (ztypes) {
				php_pq_types_to_array(Z_ARRVAL_P(ztypes), &types TSRMLS_CC);
			}

			res = PQexecParams(obj->intern->conn, query_str, count, types, (const char *const*) params, NULL, NULL, 0);

			zend_hash_destroy(&zdtor);
			if (types) {
				efree(types);
			}
			if (params) {
				efree(params);
			}

			php_pqconn_notify_listeners(obj TSRMLS_CC);

			if (res) {
				if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
					php_pqres_t *r = ecalloc(1, sizeof(*r));

					r->res = res;
					return_value->type = IS_OBJECT;
					return_value->value.obj = php_pqres_create_object_ex(php_pqres_class_entry, r, NULL TSRMLS_CC);
				}
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not execute query: %s", PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
			RETVAL_FALSE;
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_exec_params_async, 0, 0, 2)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_ARRAY_INFO(0, params, 0)
	ZEND_ARG_ARRAY_INFO(0, types, 1)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, execParamsAsync) {
	zend_error_handling zeh;
	php_pq_callback_t resolver = {{0}};
	char *query_str;
	int query_len;
	zval *zparams;
	zval *ztypes = NULL;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sa/|a/!f", &query_str, &query_len, &zparams, &ztypes, &resolver.fci, &resolver.fcc)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			int count;
			Oid *types = NULL;
			char **params = NULL;
			HashTable zdtor;

			ZEND_INIT_SYMTABLE(&zdtor);
			count = php_pq_params_to_array(Z_ARRVAL_P(zparams), &params, &zdtor TSRMLS_CC);

			if (ztypes) {
				php_pq_types_to_array(Z_ARRVAL_P(ztypes), &types TSRMLS_CC);
			}

			php_pq_callback_dtor(&obj->intern->onevent);
			if (resolver.fci.size > 0) {
				obj->intern->onevent = resolver;
				php_pq_callback_addref(&obj->intern->onevent);
			}

			obj->intern->poller = PQconsumeInput;

			if (PQsendQueryParams(obj->intern->conn, query_str, count, types, (const char *const*) params, NULL, NULL, 0)) {
				if (obj->intern->unbuffered) {
					if (!PQsetSingleRowMode(obj->intern->conn)) {
						php_error_docref(NULL TSRMLS_CC, E_NOTICE, "Could not enable unbuffered mode: %s", PQerrorMessage(obj->intern->conn));
					}
				}
				RETVAL_TRUE;
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not execute query: %s", PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			}

			zend_hash_destroy(&zdtor);
			if (types) {
				efree(types);
			}
			if (params) {
				efree(params);
			}

			php_pqconn_notify_listeners(obj TSRMLS_CC);

		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
			RETVAL_FALSE;
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

static STATUS php_pqconn_prepare(zval *object, php_pqconn_object_t *obj, const char *name, const char *query, HashTable *typest TSRMLS_DC)
{
	Oid *types = NULL;
	int count = 0;
	PGresult *res;
	STATUS rv;

	if (!obj) {
		obj = zend_object_store_get_object(object TSRMLS_CC);
	}

	if (typest) {
		count = zend_hash_num_elements(typest);
		php_pq_types_to_array(typest, &types TSRMLS_CC);
	}
	
	res = PQprepare(obj->intern->conn, name, query, count, types);

	if (types) {
		efree(types);
	}

	if (res) {
		rv = php_pqres_success(res TSRMLS_CC);
		PQclear(res);
	} else {
		rv = FAILURE;
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not prepare statement: %s", PQerrorMessage(obj->intern->conn));
	}
	
	return rv;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_prepare, 0, 0, 2)
	ZEND_ARG_INFO(0, type)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_ARRAY_INFO(0, types, 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, prepare) {
	zend_error_handling zeh;
	zval *ztypes = NULL;
	char *name_str, *query_str;
	int name_len, *query_len;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss|a/!", &name_str, &name_len, &query_str, &query_len, &ztypes)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			if (SUCCESS == php_pqconn_prepare(getThis(), obj, name_str, query_str, ztypes ? Z_ARRVAL_P(ztypes) : NULL TSRMLS_CC)) {
				php_pqstm_t *stm = ecalloc(1, sizeof(*stm));

				php_pq_object_addref(obj TSRMLS_CC);
				stm->conn = obj;
				stm->name = estrdup(name_str);

				return_value->type = IS_OBJECT;
				return_value->value.obj = php_pqstm_create_object_ex(php_pqstm_class_entry, stm, NULL TSRMLS_CC);
			}
			php_pqconn_notify_listeners(obj TSRMLS_CC);
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

static STATUS php_pqconn_prepare_async(zval *object, php_pqconn_object_t *obj, const char *name, const char *query, HashTable *typest TSRMLS_DC)
{
	STATUS rv;
	int count;
	Oid *types = NULL;
	
	if (!obj) {
		obj = zend_object_store_get_object(object TSRMLS_CC);
	}

	if (typest) {
		count = php_pq_types_to_array(typest, &types TSRMLS_CC);
	}
	
	if (PQsendPrepare(obj->intern->conn, name, query, count, types)) {
		if (obj->intern->unbuffered) {
			if (!PQsetSingleRowMode(obj->intern->conn)) {
				php_error_docref(NULL TSRMLS_CC, E_NOTICE, "Could not enable unbuffered mode: %s", PQerrorMessage(obj->intern->conn));
			}
		}
		rv = SUCCESS;
	} else {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not prepare statement: %s", PQerrorMessage(obj->intern->conn));
		rv = FAILURE;
	}
	
	if (types) {
		efree(types);
	}
	
	return rv;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_prepare_async, 0, 0, 2)
ZEND_ARG_INFO(0, type)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_ARRAY_INFO(0, types, 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, prepareAsync) {
	zend_error_handling zeh;
	zval *ztypes = NULL;
	char *name_str, *query_str;
	int name_len, *query_len;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss|a/!", &name_str, &name_len, &query_str, &query_len, &ztypes)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			obj->intern->poller = PQconsumeInput;
			if (SUCCESS == php_pqconn_prepare_async(getThis(), obj, name_str, query_str, ztypes ? Z_ARRVAL_P(ztypes) : NULL TSRMLS_CC)) {
				php_pqstm_t *stm = ecalloc(1, sizeof(*stm));

				php_pq_object_addref(obj TSRMLS_CC);
				stm->conn = obj;
				stm->name = estrdup(name_str);

				return_value->type = IS_OBJECT;
				return_value->value.obj = php_pqstm_create_object_ex(php_pqstm_class_entry, stm, NULL TSRMLS_CC);
			}
			php_pqconn_notify_listeners(obj TSRMLS_CC);
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_quote, 0, 0, 1)
	ZEND_ARG_INFO(0, string)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, quote) {
	char *str;
	int len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &str, &len)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			char *quoted = PQescapeLiteral(obj->intern->conn, str, len);

			if (quoted) {
				RETVAL_STRING(quoted, 1);
				PQfreemem(quoted);
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not quote string: %s", PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
			RETVAL_FALSE;
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_quote_name, 0, 0, 1)
	ZEND_ARG_INFO(0, type)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, quoteName) {
	char *str;
	int len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &str, &len)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			char *quoted = PQescapeIdentifier(obj->intern->conn, str, len);

			if (quoted) {
				RETVAL_STRING(quoted, 1);
				PQfreemem(quoted);
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not quote name: %s", PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
			RETVAL_FALSE;
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_escape_bytea, 0, 0, 1)
	ZEND_ARG_INFO(0, bytea)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, escapeBytea) {
	char *str;
	int len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &str, &len)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			size_t escaped_len;
			char *escaped_str = (char *) PQescapeByteaConn(obj->intern->conn, (unsigned char *) str, len, &escaped_len);

			if (escaped_str) {
				RETVAL_STRINGL(escaped_str, escaped_len - 1, 1);
				PQfreemem(escaped_str);
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not escape bytea: %s", PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
			RETVAL_FALSE;
		}
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_unescape_bytea, 0, 0, 1)
	ZEND_ARG_INFO(0, bytea)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, unescapeBytea) {
	char *str;
	int len;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &str, &len)) {
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			size_t unescaped_len;
			char *unescaped_str = (char *) PQunescapeBytea((unsigned char *)str, &unescaped_len);

			if (unescaped_str) {
				RETVAL_STRINGL(unescaped_str, unescaped_len, 1);
				PQfreemem(unescaped_str);
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not unescape bytea: %s", PQerrorMessage(obj->intern->conn));
				RETVAL_FALSE;
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
			RETVAL_FALSE;
		}
	}
}

static const char *isolation_level(long *isolation) {
	switch (*isolation) {
	case PHP_PQTXN_SERIALIZABLE:
		return "SERIALIZABLE";
	case PHP_PQTXN_REPEATABLE_READ:
		return "REPEATABLE READ";
	default:
		*isolation = PHP_PQTXN_READ_COMMITTED;
		/* no break */
	case PHP_PQTXN_READ_COMMITTED:
		return "READ COMMITTED";
	}
}

static STATUS php_pqconn_start_transaction(zval *zconn, php_pqconn_object_t *conn_obj, long isolation, zend_bool readonly, zend_bool deferrable TSRMLS_DC)
{
	if (!conn_obj) {
		conn_obj = zend_object_store_get_object(zconn TSRMLS_CC);
	}

	if (conn_obj->intern) {
		PGresult *res;
		char *cmd;

		spprintf(&cmd, 0, "START TRANSACTION ISOLATION LEVEL %s, READ %s, %s DEFERRABLE",
				isolation_level(&isolation), readonly ? "ONLY" : "WRITE", deferrable ? "": "NOT");

		res = PQexec(conn_obj->intern->conn, cmd);

		efree(cmd);

		if (res) {
			STATUS rv = php_pqres_success(res TSRMLS_CC);

			PQclear(res);
			return rv;
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not start transaction: %s", PQerrorMessage(conn_obj->intern->conn));
			return FAILURE;
		}
	} else {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
		return FAILURE;
	}
}

static STATUS php_pqconn_start_transaction_async(zval *zconn, php_pqconn_object_t *conn_obj, long isolation, zend_bool readonly, zend_bool deferrable TSRMLS_DC)
{
	if (!conn_obj) {
		conn_obj = zend_object_store_get_object(zconn TSRMLS_CC);
	}

	if (conn_obj->intern->conn) {
		char *cmd;

		spprintf(&cmd, 0, "START TRANSACTION ISOLATION LEVEL %s, READ %s, %s DEFERRABLE",
				isolation_level(&isolation), readonly ? "ONLY" : "WRITE", deferrable ? "": "NOT");

		if (PQsendQuery(conn_obj->intern->conn, cmd)) {
			conn_obj->intern->poller = PQconsumeInput;
			efree(cmd);
			return SUCCESS;
		} else {
			efree(cmd);
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not start transaction: %s", PQerrorMessage(conn_obj->intern->conn));
			return FAILURE;
		}
	} else {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
		return FAILURE;
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_start_transaction, 0, 0, 0)
	ZEND_ARG_INFO(0, isolation)
	ZEND_ARG_INFO(0, readonly)
	ZEND_ARG_INFO(0, deferrable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, startTransaction) {
	zend_error_handling zeh;
	long isolation = PHP_PQTXN_READ_COMMITTED;
	zend_bool readonly = 0, deferrable = 0;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|lbb", &isolation, &readonly, &deferrable)) {
		STATUS rv;
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		rv = php_pqconn_start_transaction(getThis(), obj, isolation, readonly, deferrable TSRMLS_CC);

		if (SUCCESS == rv) {
			php_pqtxn_t *txn = ecalloc(1, sizeof(*txn));

			php_pq_object_addref(obj TSRMLS_CC);
			txn->conn = obj;
			txn->isolation = isolation;
			txn->readonly = readonly;
			txn->deferrable = deferrable;

			return_value->type = IS_OBJECT;
			return_value->value.obj = php_pqtxn_create_object_ex(php_pqtxn_class_entry, txn, NULL TSRMLS_CC);
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}


ZEND_BEGIN_ARG_INFO_EX(ai_pqconn_start_transaction_async, 0, 0, 0)
	ZEND_ARG_INFO(0, isolation)
	ZEND_ARG_INFO(0, readonly)
	ZEND_ARG_INFO(0, deferrable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqconn, startTransactionAsync) {
	zend_error_handling zeh;
	long isolation = PHP_PQTXN_READ_COMMITTED;
	zend_bool readonly = 0, deferrable = 0;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|lbb", &isolation, &readonly, &deferrable)) {
		STATUS rv;
		php_pqconn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		rv = php_pqconn_start_transaction_async(getThis(), obj, isolation, readonly, deferrable TSRMLS_CC);

		if (SUCCESS == rv) {
			php_pqtxn_t *txn = ecalloc(1, sizeof(*txn));

			php_pq_object_addref(obj TSRMLS_CC);
			txn->conn = obj;
			txn->isolation = isolation;
			txn->readonly = readonly;
			txn->deferrable = deferrable;

			return_value->type = IS_OBJECT;
			return_value->value.obj = php_pqtxn_create_object_ex(php_pqtxn_class_entry, txn, NULL TSRMLS_CC);
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

static zend_function_entry php_pqconn_methods[] = {
	PHP_ME(pqconn, __construct, ai_pqconn_construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqconn, reset, ai_pqconn_reset, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, resetAsync, ai_pqconn_reset_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, poll, ai_pqconn_poll, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, exec, ai_pqconn_exec, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, execAsync, ai_pqconn_exec_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, execParams, ai_pqconn_exec_params, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, execParamsAsync, ai_pqconn_exec_params_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, prepare, ai_pqconn_prepare, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, prepareAsync, ai_pqconn_prepare_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, listen, ai_pqconn_listen, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, notify, ai_pqconn_notify, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, getResult, ai_pqconn_get_result, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, quote, ai_pqconn_quote, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, quoteName, ai_pqconn_quote_name, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, escapeBytea, ai_pqconn_escape_bytea, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, unescapeBytea, ai_pqconn_unescape_bytea, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, startTransaction, ai_pqconn_start_transaction, ZEND_ACC_PUBLIC)
	PHP_ME(pqconn, startTransactionAsync, ai_pqconn_start_transaction_async, ZEND_ACC_PUBLIC)
	{0}
};

static zval **php_pqres_iteration(zval *this_ptr, php_pqres_object_t *obj, php_pqres_fetch_t fetch_type TSRMLS_DC)
{
	zval **row = NULL;
	php_pqres_fetch_t orig_fetch;

	if (!obj) {
	 obj = zend_object_store_get_object(getThis() TSRMLS_CC);
	}

	if (!obj->intern->iter) {
		obj->intern->iter = (php_pqres_iterator_t *) php_pqres_iterator_init(Z_OBJCE_P(getThis()), getThis(), 0 TSRMLS_CC);
		obj->intern->iter->zi.funcs->rewind((zend_object_iterator *) obj->intern->iter TSRMLS_CC);
	}
	orig_fetch = obj->intern->iter->fetch_type;
	obj->intern->iter->fetch_type = fetch_type;
	if (SUCCESS == obj->intern->iter->zi.funcs->valid((zend_object_iterator *) obj->intern->iter TSRMLS_CC)) {
		obj->intern->iter->zi.funcs->get_current_data((zend_object_iterator *) obj->intern->iter, &row TSRMLS_CC);
		obj->intern->iter->zi.funcs->move_forward((zend_object_iterator *) obj->intern->iter TSRMLS_CC);
	}
	obj->intern->iter->fetch_type = orig_fetch;

	return row ? row : NULL;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_fetch_row, 0, 0, 0)
	ZEND_ARG_INFO(0, fetch_type)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, fetchRow) {
	zend_error_handling zeh;
	php_pqres_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);
	long fetch_type = obj->intern->iter ? obj->intern->iter->fetch_type : PHP_PQRES_FETCH_ARRAY;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &fetch_type)) {
		zval **row = php_pqres_iteration(getThis(), obj, fetch_type TSRMLS_CC);

		if (row) {
			RETVAL_ZVAL(*row, 1, 0);
		} else {
			RETVAL_FALSE;
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

static zval **column_at(zval *row, int col TSRMLS_DC)
{
	zval **data = NULL;
	HashTable *ht = HASH_OF(row);
	int count = zend_hash_num_elements(ht);

	if (col < count) {
		zend_hash_internal_pointer_reset(ht);
		while (col-- > 0) {
			zend_hash_move_forward(ht);
		}
		zend_hash_get_current_data(ht, (void *) &data);
	} else {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Column index %d exceeds column count %d", col, count);
	}
	return data;
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqres_fetch_col, 0, 0, 0)
	ZEND_ARG_INFO(0, col_num)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqres, fetchCol) {
	zend_error_handling zeh;
	long fetch_col = 0;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &fetch_col)) {
		php_pqres_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);
		zval **row = php_pqres_iteration(getThis(), obj, obj->intern->iter ? obj->intern->iter->fetch_type : 0 TSRMLS_CC);

		if (row) {
			zval **col = column_at(*row, fetch_col TSRMLS_CC);

			if (col) {
				RETVAL_ZVAL(*col, 1, 0);
			} else {
				RETVAL_FALSE;
			}
		} else {
			RETVAL_FALSE;
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);

}

static zend_function_entry php_pqres_methods[] = {
	PHP_ME(pqres, fetchRow, ai_pqres_fetch_row, ZEND_ACC_PUBLIC)
	PHP_ME(pqres, fetchCol, ai_pqres_fetch_col, ZEND_ACC_PUBLIC)
	{0}
};

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_construct, 0, 0, 3)
	ZEND_ARG_OBJ_INFO(0, Connection, pq\\Connection, 0)
	ZEND_ARG_INFO(0, type)
	ZEND_ARG_INFO(0, query)
	ZEND_ARG_ARRAY_INFO(0, types, 1)
	ZEND_ARG_INFO(0, async)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, __construct) {
	zend_error_handling zeh;
	zval *zconn, *ztypes = NULL;
	char *name_str, *query_str;
	int name_len, *query_len;
	zend_bool async = 0;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Oss|a/!b", &zconn, php_pqconn_class_entry, &name_str, &name_len, &query_str, &query_len, &ztypes, &async)) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);
		php_pqconn_object_t *conn_obj = zend_object_store_get_object(zconn TSRMLS_CC);

		if (conn_obj->intern) {
			STATUS rv;
			if (async) {
				conn_obj->intern->poller = PQconsumeInput;
				rv = php_pqconn_prepare_async(zconn, conn_obj, name_str, query_str, ztypes ? Z_ARRVAL_P(ztypes) : NULL TSRMLS_CC);
			} else {
				rv = php_pqconn_prepare(zconn, conn_obj, name_str, query_str, ztypes ? Z_ARRVAL_P(ztypes) : NULL TSRMLS_CC);
				php_pqconn_notify_listeners(conn_obj TSRMLS_CC);
			}

			if (SUCCESS == rv) {
				php_pqstm_t *stm = ecalloc(1, sizeof(*stm));

				php_pq_object_addref(conn_obj TSRMLS_CC);
				stm->conn = conn_obj;
				stm->name = estrdup(name_str);
				obj->intern = stm;
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_exec, 0, 0, 0)
	ZEND_ARG_ARRAY_INFO(0, params, 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, exec) {
	zend_error_handling zeh;
	zval *zparams = NULL;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|a/!", &zparams)) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			if (obj->intern->conn->intern) {
				int count = 0;
				char **params = NULL;
				HashTable zdtor;
				PGresult *res;

				if (zparams) {
					ZEND_INIT_SYMTABLE(&zdtor);
					count = php_pq_params_to_array(Z_ARRVAL_P(zparams), &params, &zdtor TSRMLS_CC);
				}

				res = PQexecPrepared(obj->intern->conn->intern->conn, obj->intern->name, count, (const char *const*) params, NULL, NULL, 0);

				if (params) {
					efree(params);
				}
				if (zparams) {
					zend_hash_destroy(&zdtor);
				}

				php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);

				if (res) {
					if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
						php_pqres_t *r = ecalloc(1, sizeof(*r));

						r->res = res;
						return_value->type = IS_OBJECT;
						return_value->value.obj = php_pqres_create_object_ex(php_pqres_class_entry, r, NULL TSRMLS_CC);
					}
				} else {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not execute statement: %s", PQerrorMessage(obj->intern->conn->intern->conn));
				}
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Statement not initialized");
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_exec_async, 0, 0, 0)
	ZEND_ARG_ARRAY_INFO(0, params, 1)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, execAsync) {
	zend_error_handling zeh;
	zval *zparams = NULL;
	php_pq_callback_t resolver = {{0}};

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|a/!f", &zparams, &resolver.fci, &resolver.fcc)) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			if (obj->intern->conn->intern) {
				int count;
				char **params = NULL;
				HashTable zdtor;

				if (zparams) {
					ZEND_INIT_SYMTABLE(&zdtor);
					count = php_pq_params_to_array(Z_ARRVAL_P(zparams), &params, &zdtor TSRMLS_CC);
				}

				php_pq_callback_dtor(&obj->intern->conn->intern->onevent);
				if (resolver.fci.size > 0) {
					obj->intern->conn->intern->onevent = resolver;
					php_pq_callback_addref(&obj->intern->conn->intern->onevent);
				}

				obj->intern->conn->intern->poller = PQconsumeInput;

				if (PQsendQueryPrepared(obj->intern->conn->intern->conn, obj->intern->name, count, (const char *const*) params, NULL, NULL, 0)) {
					if (obj->intern->conn->intern->unbuffered) {
						if (!PQsetSingleRowMode(obj->intern->conn->intern->conn)) {
							php_error_docref(NULL TSRMLS_CC, E_NOTICE, "Could not enable unbuffered mode: %s", PQerrorMessage(obj->intern->conn->intern->conn));
						}
					}
					RETVAL_TRUE;
				} else {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not execute statement: %s", PQerrorMessage(obj->intern->conn->intern->conn));
					RETVAL_FALSE;
				}

				if (params) {
					efree(params);
				}
				if (zparams) {
					zend_hash_destroy(&zdtor);
				}

				php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);

			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
				RETVAL_FALSE;
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Statement not initialized");
			RETVAL_FALSE;
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqstm_desc, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqstm, desc) {
	zend_error_handling zeh;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters_none()) {
		php_pqstm_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			if (obj->intern->conn->intern) {
				PGresult *res = PQdescribePrepared(obj->intern->conn->intern->conn, obj->intern->name);

				php_pqconn_notify_listeners(obj->intern->conn TSRMLS_CC);

				if (res) {
					if (SUCCESS == php_pqres_success(res TSRMLS_CC)) {
						int p, params;

						array_init(return_value);
						for (p = 0, params = PQnparams(res); p < params; ++p) {
							add_next_index_long(return_value, PQparamtype(res, p));
						}
					}
				} else {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not describe statement: %s", PQerrorMessage(obj->intern->conn->intern->conn));
				}
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Statement not initialized");
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

static zend_function_entry php_pqstm_methods[] = {
	PHP_ME(pqstm, __construct, ai_pqstm_construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqstm, exec, ai_pqstm_exec, ZEND_ACC_PUBLIC)
	PHP_ME(pqstm, desc, ai_pqstm_desc, ZEND_ACC_PUBLIC)
	PHP_ME(pqstm, execAsync, ai_pqstm_exec_async, ZEND_ACC_PUBLIC)
	{0}
};

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_construct, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, connection, pq\\Connection, 0)
	ZEND_ARG_INFO(0, async)
	ZEND_ARG_INFO(0, isolation)
	ZEND_ARG_INFO(0, readonly)
	ZEND_ARG_INFO(0, deferrable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, __construct) {
	zend_error_handling zeh;
	zval *zconn;
	long isolation = PHP_PQTXN_READ_COMMITTED;
	zend_bool async = 0, readonly = 0, deferrable = 0;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O|blbb", &zconn, php_pqconn_class_entry, &async, &isolation, &readonly, &deferrable)) {
		STATUS rv;
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);
		php_pqconn_object_t *conn_obj = zend_object_store_get_object(zconn TSRMLS_CC);

		if (conn_obj->intern) {
			if (async) {
				rv = php_pqconn_start_transaction_async(zconn, conn_obj, isolation, readonly, deferrable TSRMLS_CC);
			} else {
				rv = php_pqconn_start_transaction(zconn, conn_obj, isolation, readonly, deferrable TSRMLS_CC);
			}

			if (SUCCESS == rv) {
				obj->intern = ecalloc(1, sizeof(*obj->intern));

				php_pq_object_addref(conn_obj TSRMLS_CC);
				obj->intern->conn = conn_obj;
				obj->intern->isolation = isolation;
				obj->intern->readonly = readonly;
				obj->intern->deferrable = deferrable;
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_commit, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, commit) {
	zend_error_handling zeh;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters_none()) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {

			if (obj->intern->conn->intern) {
				PGresult *res = PQexec(obj->intern->conn->intern->conn, "COMMIT");

				if (res) {
					php_pqres_success(res TSRMLS_CC);
					PQclear(res);
				} else {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not commit transaction: %s", PQerrorMessage(obj->intern->conn->intern->conn));
				}
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not intialized");
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Transaction not initialized");
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_commit_async, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, commitAsync) {
	zend_error_handling zeh;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters_none()) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			if (obj->intern->conn->intern) {
				obj->intern->conn->intern->poller = PQconsumeInput;

				if (!PQsendQuery(obj->intern->conn->intern->conn, "COMMIT")) {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not commit transaction: %s", PQerrorMessage(obj->intern->conn->intern->conn));
				}
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not intialized");
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Transaction not initialized");
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_rollback, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, rollback) {
	zend_error_handling zeh;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters_none()) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			if (obj->intern->conn->intern) {
				PGresult *res = PQexec(obj->intern->conn->intern->conn, "ROLLBACK");

				if (res) {
					php_pqres_success(res TSRMLS_CC);
					PQclear(res);
				} else {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not rollback transaction: %s", PQerrorMessage(obj->intern->conn->intern->conn));
				}
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not intialized");
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Transaction not initialized");
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqtxn_rollback_async, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqtxn, rollbackAsync) {
	zend_error_handling zeh;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters_none()) {
		php_pqtxn_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			if (obj->intern->conn->intern) {
				obj->intern->conn->intern->poller = PQconsumeInput;
				if (!PQsendQuery(obj->intern->conn->intern->conn, "REOLLBACK")) {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not rollback transaction: %s", PQerrorMessage(obj->intern->conn->intern->conn));
				}
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not intialized");
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Transaction not initialized");
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

static zend_function_entry php_pqtxn_methods[] = {
	PHP_ME(pqtxn, __construct, ai_pqtxn_construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqtxn, commit, ai_pqtxn_commit, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, rollback, ai_pqtxn_rollback, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, commitAsync, ai_pqtxn_commit_async, ZEND_ACC_PUBLIC)
	PHP_ME(pqtxn, rollbackAsync, ai_pqtxn_rollback_async, ZEND_ACC_PUBLIC)
	{0}
};

ZEND_BEGIN_ARG_INFO_EX(ai_pqcancel_construct, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, connection, pq\\Connection, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcancel, __construct) {
	zend_error_handling zeh;
	zval *zconn;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O", &zconn, php_pqconn_class_entry)) {
		php_pqconn_object_t *conn_obj = zend_object_store_get_object(zconn TSRMLS_CC);

		if (conn_obj->intern) {
			PGcancel *cancel = PQgetCancel(conn_obj->intern->conn);

			if (cancel) {
				php_pqcancel_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

				obj->intern = ecalloc(1, sizeof(*obj->intern));
				obj->intern->cancel = cancel;
				php_pq_object_addref(conn_obj TSRMLS_CC);
				obj->intern->conn = conn_obj;
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not acquire cancel: %s", PQerrorMessage(conn_obj->intern->conn));
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqcancel_cancel, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqcancel, cancel) {
	zend_error_handling zeh;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters_none()) {
		php_pqcancel_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			char err[256];

			if (!PQcancel(obj->intern->cancel, err, sizeof(err))) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not request cancellation: %s", err);
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Cancel not initialized");
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

static zend_function_entry php_pqcancel_methods[] = {
	PHP_ME(pqcancel, __construct, ai_pqcancel_construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqcancel, cancel, ai_pqcancel_cancel, ZEND_ACC_PUBLIC)
	{0}
};

static void php_pqconn_add_eventhandler(zval *zconn, php_pqconn_object_t *conn_obj, const char *type_str, size_t type_len, zval *zevent TSRMLS_DC)
{
	zval **evhs;

	if (SUCCESS == zend_hash_find(&conn_obj->intern->eventhandlers, type_str, type_len + 1, (void *) &evhs)) {
		Z_ADDREF_P(zevent);
		add_next_index_zval(*evhs, zevent);
	} else {
		zval *evh;

		MAKE_STD_ZVAL(evh);
		array_init(evh);
		Z_ADDREF_P(zevent);
		add_next_index_zval(evh, zevent);
		zend_hash_add(&conn_obj->intern->eventhandlers, type_str, type_len + 1, (void *) &evh, sizeof(zval *), NULL);
	}
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqevent_construct, 0, 0, 3)
	ZEND_ARG_OBJ_INFO(0, connection, pq\\Connection, 0)
	ZEND_ARG_INFO(0, type)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqevent, __construct) {
	zend_error_handling zeh;
	zval *zconn;
	char *type_str;
	int type_len;
	php_pq_callback_t cb;

	zend_replace_error_handling(EH_THROW, NULL, &zeh TSRMLS_CC);
	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Osf", &zconn, php_pqconn_class_entry, &type_str, &type_len, &cb.fci, &cb.fcc)) {
		php_pqconn_object_t *conn_obj = zend_object_store_get_object(zconn TSRMLS_CC);

		if (conn_obj->intern) {
			php_pqevent_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

			obj->intern = ecalloc(1, sizeof(*obj->intern));
			php_pq_callback_addref(&cb);
			obj->intern->cb = cb;
			php_pq_object_addref(conn_obj TSRMLS_CC);
			obj->intern->conn = conn_obj;
			obj->intern->type = estrdup(type_str);

			php_pqconn_add_eventhandler(zconn, conn_obj, type_str, type_len, getThis() TSRMLS_CC);

		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Connection not initialized");
		}
	}
	zend_restore_error_handling(&zeh TSRMLS_CC);
}

ZEND_BEGIN_ARG_INFO_EX(ai_pqevent_trigger, 0, 0, 1)
	ZEND_ARG_ARRAY_INFO(0, args, 1)
ZEND_END_ARG_INFO();
static PHP_METHOD(pqevent, trigger) {
	zval *args;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "a/", &args)) {
		php_pqevent_object_t *obj = zend_object_store_get_object(getThis() TSRMLS_CC);

		if (obj->intern) {
			zval *rv = NULL;

			if (SUCCESS == zend_fcall_info_call(&obj->intern->cb.fci, &obj->intern->cb.fcc, &rv, args TSRMLS_CC)) {
				if (rv) {
					RETVAL_ZVAL(rv, 0, 1);
				} else {
					RETVAL_TRUE;
				}
			}
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "pq\\Event not initialized");
			RETVAL_FALSE;
		}
	}
}

static zend_function_entry php_pqevent_methods[] = {
	PHP_ME(pqevent, __construct, ai_pqevent_construct, ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(pqevent, trigger, ai_pqevent_trigger, ZEND_ACC_PUBLIC)
	{0}
};

/* {{{ PHP_MINIT_FUNCTION
 */
static PHP_MINIT_FUNCTION(pq)
{
	zend_class_entry ce = {0};
	php_pq_object_prophandler_t ph = {0};

	INIT_NS_CLASS_ENTRY(ce, "pq", "Connection", php_pqconn_methods);
	php_pqconn_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqconn_class_entry->create_object = php_pqconn_create_object;

	memcpy(&php_pqconn_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqconn_object_handlers.read_property = php_pq_object_read_prop;
	php_pqconn_object_handlers.write_property = php_pq_object_write_prop;
	php_pqconn_object_handlers.clone_obj = NULL;
	php_pqconn_object_handlers.get_property_ptr_ptr = NULL;
	php_pqconn_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqconn_object_prophandlers, 8, NULL, NULL, 1);

	zend_declare_property_long(php_pqconn_class_entry, ZEND_STRL("status"), CONNECTION_BAD, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_status;
	zend_hash_add(&php_pqconn_object_prophandlers, "status", sizeof("status"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_long(php_pqconn_class_entry, ZEND_STRL("transactionStatus"), PQTRANS_UNKNOWN, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_transaction_status;
	zend_hash_add(&php_pqconn_object_prophandlers, "transactionStatus", sizeof("transactionStatus"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("socket"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = NULL; /* forward to std prophandler */
	zend_hash_add(&php_pqconn_object_prophandlers, "socket", sizeof("socket"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("errorMessage"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_error_message;
	zend_hash_add(&php_pqconn_object_prophandlers, "errorMessage", sizeof("errorMessage"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("types"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_types;
	zend_hash_add(&php_pqconn_object_prophandlers, "types", sizeof("types"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_bool(php_pqconn_class_entry, ZEND_STRL("busy"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_busy;
	zend_hash_add(&php_pqconn_object_prophandlers, "busy", sizeof("busy"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqconn_class_entry, ZEND_STRL("encoding"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_encoding;
	ph.write = php_pqconn_object_write_encoding;
	zend_hash_add(&php_pqconn_object_prophandlers, "encoding", sizeof("encoding"), (void *) &ph, sizeof(ph), NULL);
	ph.write = NULL;

	zend_declare_property_bool(php_pqconn_class_entry, ZEND_STRL("unbuffered"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqconn_object_read_unbuffered;
	ph.write = php_pqconn_object_write_unbuffered;
	zend_hash_add(&php_pqconn_object_prophandlers, "unbuffered", sizeof("unbuffered"), (void *) &ph, sizeof(ph), NULL);
	ph.write = NULL;

	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("OK"), CONNECTION_OK TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("BAD"), CONNECTION_BAD TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("STARTED"), CONNECTION_STARTED TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("MADE"), CONNECTION_MADE TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("AWAITING_RESPONSE"), CONNECTION_AWAITING_RESPONSE TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("AUTH_OK"), CONNECTION_AUTH_OK TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("SSL_STARTUP"), CONNECTION_SSL_STARTUP TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("SETENV"), CONNECTION_SETENV TSRMLS_CC);

	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_IDLE"), PQTRANS_IDLE TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_ACTIVE"), PQTRANS_ACTIVE TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_INTRANS"), PQTRANS_INTRANS TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_INERROR"), PQTRANS_INERROR TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("TRANS_UNKNOWN"), PQTRANS_UNKNOWN TSRMLS_CC);

	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("POLLING_FAILED"), PGRES_POLLING_FAILED TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("POLLING_READING"), PGRES_POLLING_READING TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("POLLING_WRITING"), PGRES_POLLING_WRITING TSRMLS_CC);
	zend_declare_class_constant_long(php_pqconn_class_entry, ZEND_STRL("POLLING_OK"), PGRES_POLLING_OK TSRMLS_CC);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq", "Result", php_pqres_methods);
	php_pqres_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqres_class_entry->create_object = php_pqres_create_object;
	php_pqres_class_entry->iterator_funcs.funcs = &php_pqres_iterator_funcs;
	php_pqres_class_entry->get_iterator = php_pqres_iterator_init;
	zend_class_implements(php_pqres_class_entry TSRMLS_CC, 1, zend_ce_traversable);

	memcpy(&php_pqres_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqres_object_handlers.read_property = php_pq_object_read_prop;
	php_pqres_object_handlers.write_property = php_pq_object_write_prop;
	php_pqres_object_handlers.clone_obj = NULL;
	php_pqres_object_handlers.get_property_ptr_ptr = NULL;
	php_pqres_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqres_object_prophandlers, 6, NULL, NULL, 1);

	zend_declare_property_null(php_pqres_class_entry, ZEND_STRL("status"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_status;
	zend_hash_add(&php_pqres_object_prophandlers, "status", sizeof("status"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqres_class_entry, ZEND_STRL("errorMessage"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_error_message;
	zend_hash_add(&php_pqres_object_prophandlers, "errorMessage", sizeof("errorMessage"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_long(php_pqres_class_entry, ZEND_STRL("numRows"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_num_rows;
	zend_hash_add(&php_pqres_object_prophandlers, "numRows", sizeof("numRows"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_long(php_pqres_class_entry, ZEND_STRL("numCols"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_num_cols;
	zend_hash_add(&php_pqres_object_prophandlers, "numCols", sizeof("numCols"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_long(php_pqres_class_entry, ZEND_STRL("affectedRows"), 0, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_affected_rows;
	zend_hash_add(&php_pqres_object_prophandlers, "affectedRows", sizeof("affectedRows"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_long(php_pqres_class_entry, ZEND_STRL("fetchType"), PHP_PQRES_FETCH_ARRAY, ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqres_object_read_fetch_type;
	ph.write = php_pqres_object_write_fetch_type;
	zend_hash_add(&php_pqres_object_prophandlers, "fetchType", sizeof("fetchType"), (void *) &ph, sizeof(ph), NULL);
	ph.write = NULL;

	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("EMPTY_QUERY"), PGRES_EMPTY_QUERY TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("COMMAND_OK"), PGRES_COMMAND_OK TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("TUPLES_OK"), PGRES_TUPLES_OK TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("COPY_OUT"), PGRES_COPY_OUT TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("COPY_IN"), PGRES_COPY_IN TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("BAD_RESPONSE"), PGRES_BAD_RESPONSE TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("NONFATAL_ERROR"), PGRES_NONFATAL_ERROR TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("FATAL_ERROR"), PGRES_FATAL_ERROR TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("COPY_BOTH"), PGRES_COPY_BOTH TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("SINGLE_TUPLE"), PGRES_SINGLE_TUPLE TSRMLS_CC);

	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("FETCH_ARRAY"), PHP_PQRES_FETCH_ARRAY TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("FETCH_ASSOC"), PHP_PQRES_FETCH_ASSOC TSRMLS_CC);
	zend_declare_class_constant_long(php_pqres_class_entry, ZEND_STRL("FETCH_OBJECT"), PHP_PQRES_FETCH_OBJECT TSRMLS_CC);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq", "Statement", php_pqstm_methods);
	php_pqstm_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqstm_class_entry->create_object = php_pqstm_create_object;

	memcpy(&php_pqstm_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqstm_object_handlers.read_property = php_pq_object_read_prop;
	php_pqstm_object_handlers.write_property = php_pq_object_write_prop;
	php_pqstm_object_handlers.clone_obj = NULL;
	php_pqstm_object_handlers.get_property_ptr_ptr = NULL;
	php_pqstm_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqstm_object_prophandlers, 2, NULL, NULL, 1);

	zend_declare_property_null(php_pqstm_class_entry, ZEND_STRL("name"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqstm_object_read_name;
	zend_hash_add(&php_pqstm_object_prophandlers, "name", sizeof("name"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqstm_class_entry, ZEND_STRL("connection"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqstm_object_read_connection;
	zend_hash_add(&php_pqstm_object_prophandlers, "connection", sizeof("connection"), (void *) &ph, sizeof(ph), NULL);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq", "Transaction", php_pqtxn_methods);
	php_pqtxn_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqtxn_class_entry->create_object = php_pqtxn_create_object;

	memcpy(&php_pqtxn_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqtxn_object_handlers.read_property = php_pq_object_read_prop;
	php_pqtxn_object_handlers.write_property = php_pq_object_write_prop;
	php_pqtxn_object_handlers.clone_obj = NULL;
	php_pqtxn_object_handlers.get_property_ptr_ptr = NULL;
	php_pqtxn_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqtxn_object_prophandlers, 4, NULL, NULL, 1);

	zend_declare_property_null(php_pqtxn_class_entry, ZEND_STRL("connection"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqtxn_object_read_connection;
	zend_hash_add(&php_pqtxn_object_prophandlers, "connection", sizeof("connection"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqtxn_class_entry, ZEND_STRL("isolation"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqtxn_object_read_isolation;
	ph.write = php_pqtxn_object_write_isolation;
	zend_hash_add(&php_pqtxn_object_prophandlers, "isolation", sizeof("isolation"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqtxn_class_entry, ZEND_STRL("readonly"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqtxn_object_read_readonly;
	ph.write = php_pqtxn_object_write_readonly;
	zend_hash_add(&php_pqtxn_object_prophandlers, "readonly", sizeof("readonly"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqtxn_class_entry, ZEND_STRL("deferrable"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqtxn_object_read_deferrable;
	ph.write = php_pqtxn_object_write_deferrable;
	zend_hash_add(&php_pqtxn_object_prophandlers, "deferrable", sizeof("deferrable"), (void *) &ph, sizeof(ph), NULL);
	ph.write = NULL;

	zend_declare_class_constant_long(php_pqtxn_class_entry, ZEND_STRL("READ_COMMITTED"), PHP_PQTXN_READ_COMMITTED TSRMLS_CC);
	zend_declare_class_constant_long(php_pqtxn_class_entry, ZEND_STRL("REPEATABLE READ"), PHP_PQTXN_REPEATABLE_READ TSRMLS_CC);
	zend_declare_class_constant_long(php_pqtxn_class_entry, ZEND_STRL("SERIALIZABLE"), PHP_PQTXN_SERIALIZABLE TSRMLS_CC);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq", "Cancel", php_pqcancel_methods);
	php_pqcancel_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqcancel_class_entry->create_object = php_pqcancel_create_object;

	memcpy(&php_pqcancel_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqcancel_object_handlers.read_property = php_pq_object_read_prop;
	php_pqcancel_object_handlers.write_property = php_pq_object_write_prop;
	php_pqcancel_object_handlers.clone_obj = NULL;
	php_pqcancel_object_handlers.get_property_ptr_ptr = NULL;
	php_pqcancel_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqcancel_object_prophandlers, 1, NULL, NULL, 1);

	zend_declare_property_null(php_pqcancel_class_entry, ZEND_STRL("connection"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqcancel_object_read_connection;
	zend_hash_add(&php_pqcancel_object_prophandlers, "connection", sizeof("connection"), (void *) &ph, sizeof(ph), NULL);

	memset(&ce, 0, sizeof(ce));
	INIT_NS_CLASS_ENTRY(ce, "pq", "Event", php_pqevent_methods);
	php_pqevent_class_entry = zend_register_internal_class_ex(&ce, NULL, NULL TSRMLS_CC);
	php_pqevent_class_entry->create_object = php_pqevent_create_object;

	memcpy(&php_pqevent_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_pqevent_object_handlers.read_property = php_pq_object_read_prop;
	php_pqevent_object_handlers.write_property = php_pq_object_write_prop;
	php_pqevent_object_handlers.clone_obj = NULL;
	php_pqevent_object_handlers.get_property_ptr_ptr = NULL;
	php_pqevent_object_handlers.get_debug_info = php_pq_object_debug_info;

	zend_hash_init(&php_pqevent_object_prophandlers, 2, NULL, NULL, 1);

	zend_declare_property_null(php_pqevent_class_entry, ZEND_STRL("connection"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqevent_object_read_connection;
	zend_hash_add(&php_pqevent_object_prophandlers, "connection", sizeof("connection"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_property_null(php_pqevent_class_entry, ZEND_STRL("type"), ZEND_ACC_PUBLIC TSRMLS_CC);
	ph.read = php_pqevent_object_read_type;
	zend_hash_add(&php_pqevent_object_prophandlers, "type", sizeof("type"), (void *) &ph, sizeof(ph), NULL);

	zend_declare_class_constant_stringl(php_pqevent_class_entry, ZEND_STRL("NOTICE"), ZEND_STRL("notice") TSRMLS_CC);

	/*
	REGISTER_INI_ENTRIES();
	*/
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
static PHP_MSHUTDOWN_FUNCTION(pq)
{
	/* uncomment this line if you have INI entries
	UNREGISTER_INI_ENTRIES();
	*/
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
static PHP_MINFO_FUNCTION(pq)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "pq support", "enabled");
	php_info_print_table_end();

	/* Remove comments if you have entries in php.ini
	DISPLAY_INI_ENTRIES();
	*/
}
/* }}} */

const zend_function_entry pq_functions[] = {
	{0}
};

/* {{{ pq_module_entry
 */
zend_module_entry pq_module_entry = {
	STANDARD_MODULE_HEADER,
	"pq",
	pq_functions,
	PHP_MINIT(pq),
	PHP_MSHUTDOWN(pq),
	NULL,/*PHP_RINIT(pq),*/
	NULL,/*PHP_RSHUTDOWN(pq),*/
	PHP_MINFO(pq),
	PHP_PQ_EXT_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_PQ
ZEND_GET_MODULE(pq)
#endif


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
