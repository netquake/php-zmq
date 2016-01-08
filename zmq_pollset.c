/*
+-----------------------------------------------------------------------------------+
|  ZMQ extension for PHP                                                            |
|  Copyright (c) 2010-2013, Mikko Koppanen <mkoppanen@php.net>                      |
|  All rights reserved.                                                             |
+-----------------------------------------------------------------------------------+
|  Redistribution and use in source and binary forms, with or without               |
|  modification, are permitted provided that the following conditions are met:      |
|     * Redistributions of source code must retain the above copyright              |
|       notice, this list of conditions and the following disclaimer.               |
|     * Redistributions in binary form must reproduce the above copyright           |
|       notice, this list of conditions and the following disclaimer in the         |
|       documentation and/or other materials provided with the distribution.        |
|     * Neither the name of the copyright holder nor the                            |
|       names of its contributors may be used to endorse or promote products        |
|       derived from this software without specific prior written permission.       |
+-----------------------------------------------------------------------------------+
|  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND  |
|  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED    |
|  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE           |
|  DISCLAIMED. IN NO EVENT SHALL MIKKO KOPPANEN BE LIABLE FOR ANY                   |
|  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES       |
|  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;     |
|  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND      |
|  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT       |
|  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS    |
|  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.                     |
+-----------------------------------------------------------------------------------+
*/

#include "php_zmq.h"
#include "php_zmq_private.h"
#include "php_zmq_pollset.h"
#include "zmq_object_access.c"
#include "ext/spl/php_spl.h"

#define PHP_ZMQ_ALLOC_SIZE 5

void php_zmq_pollset_init(php_zmq_pollset *set)
{
	array_init(&set->errors);
	zend_hash_init(&(set->php_items), PHP_ZMQ_ALLOC_SIZE, NULL, ZVAL_PTR_DTOR, 0);
	set->items = NULL;
}

size_t php_zmq_pollset_num_items(php_zmq_pollset *set)
{
	return zend_hash_num_elements(&(set->php_items));
}

static
size_t s_index_for_key(php_zmq_pollset *set, zend_string *needle)
{
	zend_string *key;
	size_t i = 0;

	ZEND_HASH_FOREACH_STR_KEY(&(set->php_items), key) {
		if (key && zend_string_equals (key, needle)) {
			return i;
		}
		i++;
	}
	ZEND_HASH_FOREACH_END();
	return zend_hash_num_elements(&(set->php_items)) + 1;
}

static
zend_string *s_key_for_idx(php_zmq_pollset *set, size_t idx)
{
	zend_string *key;
	size_t i = 0;

	if (idx >= php_zmq_pollset_num_items(set)) {
		return NULL;
	}

	ZEND_HASH_FOREACH_STR_KEY(&(set->php_items), key) {
		if (i == idx) {
			return zend_string_copy(key);
		}
		i++;
	}
	ZEND_HASH_FOREACH_END();
	return NULL;
}

static
zval *s_zval_for_idx(php_zmq_pollset *set, size_t idx)
{
	zval *zv;
	size_t i = 0;

	if (idx >= php_zmq_pollset_num_items(set)) {
		return NULL;
	}

	ZEND_HASH_FOREACH_VAL(&(set->php_items), zv) {
		if (i == idx) {
			return zv;
		}
		i++;
	}
	ZEND_HASH_FOREACH_END();
	return NULL;
}


static
void s_pollset_clear(php_zmq_pollset *set)
{
	// Clear all items
	zend_hash_clean(&(set->php_items));

	// Free the pollset
	if (set->items) {
		efree(set->items);
		set->items = NULL;
	}
}

static
zend_string *s_create_key(zval *entry)
{
	if (Z_TYPE_P(entry) == IS_RESOURCE) {
		return strpprintf(0, "r:%ld", Z_RES_P(entry)->handle);
	}
	else {
		zend_string *hash = php_spl_object_hash(entry);
		zend_string *key = strpprintf(0, "o:%s", hash->val);
		zend_string_release(hash);
		return key;
	}
}

zend_string *php_zmq_pollset_add(php_zmq_pollset *set, zval *entry, int events, int *error) 
{
	int i;
	zend_string *key;
	size_t num_items;
	zmq_pollitem_t item;
	zval *dest_element;

	*error = 0;
	key = s_create_key(entry);

	// Does this already exist?
	if (zend_hash_exists(&(set->php_items), key)) {
		return key;
	}

	num_items = php_zmq_pollset_num_items(set);
	memset(&item, 0, sizeof(zmq_pollitem_t));

	if (Z_TYPE_P(entry) == IS_RESOURCE) {
		int fd;
		php_stream *stream;

		php_stream_from_zval_no_verify(stream, entry);

		if (!stream) {
			*error = PHP_ZMQ_POLLSET_ERR_NO_STREAM;
			zend_string_release(key);
			return NULL;
		}

		if (php_stream_can_cast(stream, (PHP_STREAM_AS_FD | PHP_STREAM_CAST_INTERNAL | PHP_STREAM_AS_SOCKETD) & ~REPORT_ERRORS) == FAILURE) {
			*error = PHP_ZMQ_POLLSET_ERR_CANNOT_CAST;
			zend_string_release(key);
			return NULL;
		}

		if (php_stream_cast(stream, (PHP_STREAM_AS_FD | PHP_STREAM_CAST_INTERNAL | PHP_STREAM_AS_SOCKETD) & ~REPORT_ERRORS, (void*)&fd, 0) == FAILURE) {
			*error = PHP_ZMQ_POLLSET_ERR_CAST_FAILED;
			zend_string_release(key);
			return NULL;
		}
		item.fd = fd;
		item.socket = NULL;
	}
	else {
		php_zmq_socket_object *intern = php_zmq_socket_fetch_object(Z_OBJ_P(entry));
		item.socket = intern->socket->z_socket;
		item.fd = 0;
	}
	item.events = events;

	dest_element = zend_hash_add(&(set->php_items), key, entry);
	Z_ADDREF_P(dest_element);

	set->items = erealloc (set->items, (num_items + 1) * sizeof(zmq_pollitem_t));
	memcpy (&(set->items[num_items]), &item, sizeof(zmq_pollitem_t));

	return key;
}

zend_bool php_zmq_pollset_delete_by_key(php_zmq_pollset *set, zend_string *key)
{
	int result;
	size_t num_items;
	size_t index;

	index     = s_index_for_key(set, key);
	num_items = zend_hash_num_elements(&(set->php_items));
	result    = zend_hash_del(&(set->php_items), key);

	if (result == SUCCESS) {
		memmove(
			set->items + (sizeof (zmq_pollitem_t) * index),
			set->items + (sizeof (zmq_pollitem_t) * (index + 1)),
			(num_items - index) * sizeof (zmq_pollitem_t));
		set->items = erealloc(set->items, (num_items - 1) * sizeof (zmq_pollitem_t));
	}
	return (result == SUCCESS);
}

zend_bool php_zmq_pollset_delete(php_zmq_pollset *set, zval *entry)
{
	zend_bool retval;
	zend_string *key = s_create_key(entry);

	retval = php_zmq_pollset_delete_by_key(set, key);
	zend_string_release(key);

	return retval;
}

int php_zmq_pollset_poll(php_zmq_pollset *set, int timeout, zval *r_array, zval *w_array)
{
	size_t num_elements;
	int rc, i;
	zend_bool readable = 0, writable = 0;

	if (!set->items) {
		return -1;
	}

	zend_hash_clean(Z_ARRVAL(set->errors));

	if (r_array && Z_TYPE_P(r_array) == IS_ARRAY) {
		if (zend_hash_num_elements(Z_ARRVAL_P(r_array)) > 0) {
			zend_hash_clean(Z_ARRVAL_P(r_array));
		}
		readable = 1;
	}

	if (w_array && Z_TYPE_P(w_array) == IS_ARRAY) {
		if (zend_hash_num_elements(Z_ARRVAL_P(w_array)) > 0) {
			zend_hash_clean(Z_ARRVAL_P(w_array));
		}
		writable = 1;
	}

	num_elements = zend_hash_num_elements(&(set->php_items));
	rc = zmq_poll(set->items, num_elements, timeout);

	if (rc == -1) {
		return -1;
	}

	if (rc > 0) {
		for (i = 0; i < num_elements; i++) {
			if (readable && set->items[i].revents & ZMQ_POLLIN) {
				zval *zv = s_zval_for_idx(set, i);
				Z_ADDREF_P(zv);
				add_next_index_zval(r_array, zv);
			}

			if (writable && set->items[i].revents & ZMQ_POLLOUT) {
				zval *zv = s_zval_for_idx(set, i);
				Z_ADDREF_P(zv);
				add_next_index_zval(w_array, zv);
			}

			if (set->items[i].revents & ZMQ_POLLERR) {
				add_next_index_str(&set->errors, s_key_for_idx(set, i));
			}
		}
	}
	return rc;
}

void php_zmq_pollset_delete_all(php_zmq_pollset *set)
{
	s_pollset_clear(set);
}

void php_zmq_pollset_deinit(php_zmq_pollset *set)
{
	// Destroy the hashtable
	zend_hash_destroy(&(set->php_items));

	// Errors
	zval_dtor(&(set->errors));

	if (set->items) {
		efree (set->items);
		set->items = NULL;
	}
}
