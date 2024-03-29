/* -*-pgsql-c-*- */
/*
 * pgpool: a language independent connection pool server for PostgreSQL
 * written by Tatsuo Ishii
 *
 * Copyright (c) 2003-2014	PgPool Global Development Group
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that copyright notice and this permission
 * notice appear in supporting documentation, and that the name of the
 * author not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior
 * permission. The author makes no representations about the
 * suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * pool_memqcache.c: query cache on shmem or memcached
 *
 */
#define DATABASE_TO_OID_QUERY "SELECT oid FROM pg_database WHERE datname = '%s'"

#include "pool.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <dirent.h>

#ifdef USE_MEMCACHED
#include <libmemcached/memcached.h>
#endif

#include "auth/md5.h"
#include "pool_config.h"
#include "protocol/pool_proto_modules.h"
#include "parser/parsenodes.h"
#include "context/pool_session_context.h"
#include "query_cache/pool_memqcache.h"
#include "utils/pool_relcache.h"
#include "utils/pool_select_walker.h"
#include "utils/pool_stream.h"
#include "utils/pool_stream.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/memutils.h"


#ifdef USE_MEMCACHED
memcached_st *memc;
#endif

static char* encode_key(const char *s, char *buf, POOL_CONNECTION_POOL *backend);
#ifdef DEBUG
static void dump_cache_data(const char *data, size_t len);
#endif
static int pool_commit_cache(POOL_CONNECTION_POOL *backend, char *query, char *data, size_t datalen, int num_oids, int *oids);
static int pool_fetch_cache(POOL_CONNECTION_POOL *backend, const char *query, char **buf, size_t *len);
static int send_cached_messages(POOL_CONNECTION *frontend, const char *qcache, int qcachelen);
static void send_message(POOL_CONNECTION *conn, char kind, int len, const char *data);
#ifdef USE_MEMCACHED
static int delete_cache_on_memcached(const char *key);
#endif
static int pool_get_dml_table_oid(int **oid);
static int pool_get_dropdb_table_oids(int **oids, int dboid);
static void pool_discard_dml_table_oid(void);
static void pool_invalidate_query_cache(int num_table_oids, int *table_oid, bool unlink, int dboid);
static int pool_get_database_oid(void);
static void pool_add_table_oid_map(POOL_CACHEKEY *cachkey, int num_table_oids, int *table_oids);
static void pool_reset_memqcache_buffer(void);
static POOL_CACHEID *pool_add_item_shmem_cache(POOL_QUERY_HASH *query_hash, char *data, int size);
static POOL_CACHEID *pool_find_item_on_shmem_cache(POOL_QUERY_HASH *query_hash);
static char *pool_get_item_shmem_cache(POOL_QUERY_HASH *query_hash, int *size, int *sts);
static POOL_QUERY_CACHE_ARRAY * pool_add_query_cache_array(POOL_QUERY_CACHE_ARRAY *cache_array, POOL_TEMP_QUERY_CACHE *cache);
static void pool_add_temp_query_cache(POOL_TEMP_QUERY_CACHE *temp_cache, char kind, char *data, int data_len);
static void pool_add_oids_temp_query_cache(POOL_TEMP_QUERY_CACHE *temp_cache, int num_oids, int *oids);
static POOL_INTERNAL_BUFFER *pool_create_buffer(void);
static void pool_discard_buffer(POOL_INTERNAL_BUFFER *buffer);
static void pool_add_buffer(POOL_INTERNAL_BUFFER *buffer, void *data, size_t len);
static void *pool_get_buffer(POOL_INTERNAL_BUFFER *buffer, size_t *len);
static char *pool_get_buffer_pointer(POOL_INTERNAL_BUFFER *buffer);
static char *pool_get_current_cache_buffer(size_t *len);
static size_t pool_get_buffer_length(POOL_INTERNAL_BUFFER *buffer);
static void pool_check_and_discard_cache_buffer(int num_oids, int *oids);

static void pool_set_memqcache_blocks(int num_blocks);
static int pool_get_memqcache_blocks(void);
static void *pool_memory_cache_address(void);
static void pool_reset_fsmm(size_t size);
static void *pool_fsmm_address(void);
static void pool_update_fsmm(POOL_CACHE_BLOCKID blockid, size_t free_space);
static POOL_CACHE_BLOCKID pool_get_block(size_t free_space);
static POOL_CACHE_ITEM_HEADER *pool_cache_item_header(POOL_CACHEID *cacheid);
static int pool_init_cache_block(POOL_CACHE_BLOCKID blockid);
#if NOT_USED
static void pool_wipe_out_cache_block(POOL_CACHE_BLOCKID blockid);
#endif
static int pool_delete_item_shmem_cache(POOL_CACHEID *cacheid);
static char *block_address(int blockid);
static POOL_CACHE_ITEM_POINTER *item_pointer(char *block, int i);
static POOL_CACHE_ITEM_HEADER *item_header(char *block, int i);
static POOL_CACHE_BLOCKID pool_reuse_block(void);
#ifdef SHMEMCACHE_DEBUG
static void dump_shmem_cache(POOL_CACHE_BLOCKID blockid);
#endif

static int pool_hash_reset(int nelements);
static int pool_hash_insert(POOL_QUERY_HASH *key, POOL_CACHEID *cacheid, bool update);
static uint32 create_hash_key(POOL_QUERY_HASH *key);
static volatile POOL_HASH_ELEMENT *get_new_hash_element(void);
static void put_back_hash_element(volatile POOL_HASH_ELEMENT *element);
static bool is_free_hash_element(void);
static char *get_relation_without_alias(RangeVar *relation);

/*
 * Connect to Memcached
 */
int memcached_connect (void)
{
	char *memqcache_memcached_host;
	int memqcache_memcached_port;
#ifdef USE_MEMCACHED
	memcached_server_st *servers;
	memcached_return rc;

	/* Already connected? */
	if (memc)
	{
		return 0;
	}
#endif

	memqcache_memcached_host = pool_config->memqcache_memcached_host;
	memqcache_memcached_port = pool_config->memqcache_memcached_port;

	ereport(DEBUG1,
            (errmsg("memcached_connect : memqcache_memcached_host = %s", memqcache_memcached_host)));
	ereport(DEBUG1,
            (errmsg("memcached_connect : memqcache_memcached_port = %d", memqcache_memcached_port)));

#ifdef USE_MEMCACHED
	memc = memcached_create(NULL);
	servers = memcached_server_list_append(NULL,
										   memqcache_memcached_host,
										   memqcache_memcached_port,
										   &rc);

	rc = memcached_server_push(memc, servers);
	if (rc != MEMCACHED_SUCCESS)
	{
		pool_error("memcached_connect: server_push %s\n", memcached_strerror(memc, rc));
		memc = (memcached_st *)-1;
		return -1;
	}
	memcached_server_list_free(servers);
#else
	pool_error("memcached_connect: memcached support is not enabled");
	return -1;
#endif
	return 0;
}

/*
 * Disconnect to Memcached
 */
void memcached_disconnect (void)
{
#ifdef USE_MEMCACHED
	if (!memc)
	{
		return;
	}
	memcached_free(memc);
#else
	pool_error("memcached_disconnect: memcached support is not enabled");
#endif
}

/*
 * Register buffer data for query cache on memory cache
 */
void memqcache_register(char kind,
                        POOL_CONNECTION *frontend,
                        char *data,
                        int data_len)
{
	POOL_TEMP_QUERY_CACHE *cache;

	cache = pool_get_current_cache();

	pool_add_temp_query_cache(cache, kind, data, data_len);
}

/*
 * Commit SELECT results to cache storage.
 */
static int pool_commit_cache(POOL_CONNECTION_POOL *backend, char *query, char *data, size_t datalen, int num_oids, int *oids)
{
#ifdef USE_MEMCACHED
	memcached_return rc;
#endif
	POOL_CACHEKEY cachekey;
	char tmpkey[MAX_KEY];
	time_t memqcache_expire;

	/*
	 * get_buflen() will return -1 if query result exceeds memqcache_maxcache
	 */
	if (datalen == -1)
	{
		return -1;
	}

	/* query disabled */
	if (strlen(query) <= 0)
	{
		return -1;
	}

	pool_debug("pool_commit_cache: Query=%s", query);
#ifdef DEBUG
	dump_cache_data(data, datalen);
#endif

	/* encode md5key for memcached */
	encode_key(query, tmpkey, backend);
	pool_debug("pool_commit_cache: search key ==%s==", tmpkey);
	memcpy(cachekey.hashkey, tmpkey, 32);

	memqcache_expire = pool_config->memqcache_expire;
	pool_debug("pool_commit_cache : memqcache_expire = %ld", memqcache_expire);

	if (pool_is_shmem_cache())
	{
		POOL_CACHEID *cacheid;
		POOL_QUERY_HASH query_hash;

		memcpy(query_hash.query_hash, tmpkey, sizeof(query_hash.query_hash));

		cacheid = pool_hash_search(&query_hash);

		if (cacheid != NULL)
		{
			pool_debug("pool_commit_cache: the item already exists");
			return 0;
		}
		else
		{
			cacheid = pool_add_item_shmem_cache(&query_hash, data, datalen);
			if (cacheid == NULL)
			{
				pool_error("pool_commit_cache: pool_add_item_shmem_cache failed");
				return -1;
			}
			else
			{
				pool_debug("pool_commit_cache: blockid: %d itemid: %d",
						   cacheid->blockid, cacheid->itemid);
			}
			cachekey.cacheid.blockid = cacheid->blockid;
			cachekey.cacheid.itemid = cacheid->itemid;
		}
	}

#ifdef USE_MEMCACHED
	else
	{
		rc = memcached_set(memc, tmpkey, 32,
						   data, datalen, (time_t)memqcache_expire, 0);
		if (rc != MEMCACHED_SUCCESS)
		{
			pool_error("pool_commit_cache: memcached_set error %s", memcached_strerror(memc, rc));
			return -1;
		}
		pool_debug("pool_commit_cache: set cache succeeded.");
	}
#endif

	/*
	 * Register cache id to oid map
	 */
	pool_add_table_oid_map(&cachekey, num_oids, oids);

	return 0;
}

/*
 * Fetch from memory cache.
 * 0: fetch success, 1: not found -1: error
 */
static int pool_fetch_cache(POOL_CONNECTION_POOL *backend, const char *query, char **buf, size_t *len)
{
	char *ptr;
	char tmpkey[MAX_KEY];
	int sts;
	char *p;

	if (strlen(query) <= 0)
	{
		return -1;
	}

	/* encode md5key for memcached */
	encode_key(query, tmpkey, backend);
	pool_debug("pool_fetch_cache: search key ==%s==", tmpkey);

	if (pool_is_shmem_cache())
	{
		POOL_QUERY_HASH query_hash;
		int mylen;

		memcpy(query_hash.query_hash, tmpkey, sizeof(query_hash.query_hash));

		ptr = pool_get_item_shmem_cache(&query_hash, &mylen, &sts);
		if (ptr == NULL)
		{
			pool_debug("pool_fetch_cache: cache not found on shmem");
			return 1;
		}
		*len = mylen;
	}
#ifdef USE_MEMCACHED
	else
	{
		memcached_return rc;
		unsigned int flags;

		ptr = memcached_get(memc, tmpkey, strlen(tmpkey), len, &flags, &rc);

		if (rc != MEMCACHED_SUCCESS)
		{
			if (rc != MEMCACHED_NOTFOUND)
			{
				pool_error("pool_fetch_cache: memcached_get failed %s", memcached_strerror(memc, rc));
				/*
				 * Turn off memory cache support to prevent future errors.
				 */
				pool_config->memory_cache_enabled = 0;
				/* Behave as if cache not found */
				return 1;
			}
			else
			{
				/* Not found */
				pool_debug("pool_fetch_cache: not found: query:%s key:%s", query, tmpkey);
				return 1;
			}
		}
	}
#else
	else
	{
		pool_error("pool_fetch_cache: memcached support is not enabled");
		return -1;
	}
#endif

	p = palloc(*len);

	memcpy(p, ptr, *len);

	if (!pool_is_shmem_cache())
	{
		free(ptr);
	}

	pool_debug("pool_fetch_cache: query=%s len:%zd", query, *len);
#ifdef DEBUG
	dump_cache_data(p, *len);
#endif

	*buf = p;

	return 0;
}

/*
 * encode key.
 * create cache key as md5(username + query string + database name)
 */
static char* encode_key(const char *s, char *buf, POOL_CONNECTION_POOL *backend)
{
	char* strkey;
	int u_length;
	int d_length;
	int q_length;
	int length;

	u_length = strlen(backend->info->user);
	pool_debug("encode_key: username %s", backend->info->user);

	d_length = strlen(backend->info->database);
	pool_debug("encode_key: database_name %s", backend->info->database);

	q_length = strlen(s);
	pool_debug("encode_key: query %s", s);

	length = u_length + d_length + q_length + 1;

	strkey = (char*)palloc(sizeof(char) * length);

	snprintf(strkey, length, "%s%s%s", backend->info->user, s, backend->info->database);

	pool_md5_hash(strkey, strlen(strkey), buf);
	pool_debug("encode_key: `%s' -> `%s'", strkey, buf);
	pfree(strkey);
	return buf;
}

#ifdef DEBUG
/*
 * dump cache data
 */
static void dump_cache_data(const char *data, size_t len)
{
	int i;
	int plen;
	

	fprintf(stderr,"shmem: len = %zd\n", len);

	while (len > 0)
	{
		fprintf(stderr,"shmem: kind:%c\n", *data++);
		len--;
		memmove(&plen, data, 4);
		len -= 4;
		data += 4;
		plen = ntohl(plen);
		fprintf(stderr,"shmem: len:%d\n", plen);
		plen -= 4;

		fprintf(stderr, "shmem: ");
		for (i=0;i<plen;i++)
		{
			fprintf(stderr, "%02x ", (unsigned char)(*data++));
			len--;
		}
		fprintf(stderr, "\n");
	}
}
#endif

/*
 * send cached messages
 */
static int send_cached_messages(POOL_CONNECTION *frontend, const char *qcache, int qcachelen)
{
	int msg = 0;
	int i = 0;
	int is_prepared_stmt = 0;
	int len;
	const char *p;

	while (i < qcachelen)
	{
		char tmpkind;
		int tmplen;

		tmpkind = qcache[i];
		i++;

		memcpy(&tmplen, qcache+i, sizeof(tmplen));
		i += sizeof(tmplen);
		len = ntohl(tmplen);
		p = qcache + i;
		i += len - sizeof(tmplen);

		/* No need to cache PARSE and BIND responses */
		if (tmpkind == '1' || tmpkind == '2')
		{
			is_prepared_stmt = 1;
			continue;
		}

		/*
		 * In the prepared statement execution, there is no need to send
		 * 'T' response to the frontend.
		 */
		if (is_prepared_stmt && tmpkind == 'T')
		{
			continue;
		}

		/* send message to frontend */
		pool_debug("send_cached_messages: %c len: %d", tmpkind, len);
		send_message(frontend, tmpkind, len, p);

		msg++;
	}

	return msg;
}

/*
 * send message to frontend
 */
static void send_message(POOL_CONNECTION *conn, char kind, int len, const char *data)
{
	pool_debug("send_message: kind=%c, len=%d, data=%p", kind, len, data);

	pool_write(conn, &kind, 1);

	len = htonl(len);
	pool_write(conn, &len, sizeof(len));

	len = ntohl(len);
	pool_write(conn, (void *)data, len-sizeof(len));
}

#ifdef USE_MEMCACHED
/*
 * delete query cache on memcached
 */
static int delete_cache_on_memcached(const char *key)
{

	memcached_return rc;

	pool_debug("delete_cache_on_memcached: key: %s", key);


	/* delete cache data on memcached. key is md5 hash query */
    rc= memcached_delete(memc, key, 32, (time_t)0);

	/* delete cache data on memcached is failed */
    if (rc != MEMCACHED_SUCCESS && rc != MEMCACHED_BUFFERED)
    {
        pool_error ("delete_cache_on_memcached: %s\n", memcached_strerror(memc, rc));
        return 0;
    }
    return 1;

}
#endif

/*
 * Fetch SELECT data from cache if possible.
 */
POOL_STATUS pool_fetch_from_memory_cache(POOL_CONNECTION *frontend,
										 POOL_CONNECTION_POOL *backend,
										 char *contents, bool *foundp)
{
	char *qcache;
	size_t qcachelen;
	int sts;
#ifdef HAVE_SIGPROCMASK
	sigset_t oldmask;
#else
	int	oldmask;
#endif

	*foundp = false;
    
    POOL_SETMASK2(&BlockSig, &oldmask);
	pool_shmem_lock();

    PG_TRY();
    {
        sts = pool_fetch_cache(backend, contents, &qcache, &qcachelen);
    }
    PG_CATCH();
    {
        pool_shmem_unlock();
        POOL_SETMASK(&oldmask);
        PG_RE_THROW();
    }
    PG_END_TRY();

	pool_shmem_unlock();
	POOL_SETMASK(&oldmask);

	if (sts == 0)
	{
		/*
		 * Cache found. send each messages to frontend
		 */
		send_cached_messages(frontend, qcache, qcachelen);
		pfree(qcache);

		/*
		 * If we are doing extended query, wait and discard Sync
		 * message from frontend. This is necessary to prevent
		 * receiving Sync message after Sending Ready for query.
		 */
		if (pool_is_doing_extended_query_message())
		{
			char kind;
			int32 len;

			if (pool_flush(frontend))
				return POOL_END;
			if (pool_read(frontend, &kind, 1))
				return POOL_END;
			pool_debug("pool_fetch_from_memory_cache: expecting sync: %c", kind);
			if (pool_read(frontend, &len, sizeof(len)))
				return POOL_END;
		}

		/*
		 * send a "READY FOR QUERY"
		 */
		if (MAJOR(backend) == PROTO_MAJOR_V3)
		{
			signed char state;

			/*
			 * We keep previous transaction state.
			 */
			state = MASTER(backend)->tstate;
			send_message(frontend, 'Z', 5, (char *)&state);
		}
		else
		{
			pool_write(frontend, "Z", 1);
		}
		if (pool_flush(frontend))
		{
			return POOL_END;
		}

		*foundp = true;

		if (pool_config->log_per_node_statement)
			pool_log("query result fetched from cache. statement: %s", contents);

		pool_debug("pool_fetch_from_memory_cache: a query result found in the query cache, %s", contents);

		return POOL_CONTINUE;
	}
	else if (sts == -1)
	{
		pool_error("pool_fetch_from_memory_cache: pool_fetch_cache() failed. %s", contents);
		return POOL_END;
	}

	/* Cache not found */
	return POOL_CONTINUE;
}

/*
 * Simple and rough(thus unreliable) check if the query is likely
 * SELECT. Just check if the query starts with SELECT or WITH. This
 * can be used before parse tree is available.
 */
bool pool_is_likely_select(char *query)
{
	bool do_continue = false;

	if (query == NULL)
		return false;

	if (pool_config->ignore_leading_white_space)
	{
		/* Ignore leading white spaces */
		while (*query && isspace(*query))
			query++;
	}
	if (! *query) { return false; }

	/*
	 * Get rid of head comment.
	 * It is sure that the query is in correct format, because the parser
	 * has rejected bad queries such as the one with not-ended comment.
	 */
	while (*query)
	{
		/* Ignore spaces and return marks */
		do_continue = false;
		while (*query && isspace(*query))
		{
			query++;
			do_continue = true;
		}
		if (do_continue) { continue; }

		while (*query && !strncmp(query, "\n", 2))
		{
			query++;
			do_continue = true;
		}
		if (do_continue)
		{
			query += 2;
			continue;
		}

		/* Ignore comments like C */
		if (!strncmp(query, "/*", 2))
		{
			while (*query && strncmp(query, "*/", 2))
				query++;

			query += 2;
			continue;
		}

		/* Ignore SQL comments */
		if (!strncmp(query, "--", 2))
		{
			while (*query && strncmp(query, "\n", 2))
				query++;

			query += 2;
			continue;
		}

		if (!strncasecmp(query, "SELECT", 6) || !strncasecmp(query, "WITH", 4))
		{
			return true;
		}

		query++;
	}

	return false;
}

/*
 * Return true if SELECT can be cached.  "node" is the parse tree for
 * the query and "query" is the query string.
 * The query must be SELECT or WITH.
 */
bool pool_is_allow_to_cache(Node *node, char *query)
{
	int i = 0;
	int num_oids = -1;
	SelectContext ctx;

	/*
	 * If NO QUERY CACHE comment exists, do not cache.
	 */
	if (!strncasecmp(query, NO_QUERY_CACHE, NO_QUERY_CACHE_COMMENT_SZ))
		return false;
	/*
	 * Check black table list first.
	 */
	if (pool_config->num_black_memqcache_table_list > 0)
	{
		/* Extract oids in from clause of SELECT, and check if SELECT to them could be cached. */
		num_oids = pool_extract_table_oids_from_select_stmt(node, &ctx);
		if (num_oids > 0)
		{
			for (i = 0; i < num_oids; i++)
			{
				pool_debug("pool_is_allow_to_cache: check table_names[%d] = %s", i, ctx.table_names[i]);
				if (pool_is_table_in_black_list(ctx.table_names[i]) == true)
				{
					pool_debug("pool_is_allow_to_cache: false");
					return false;
				}
			}
		}
	}

	/* SELECT INTO or SELECT FOR SHARE or UPDATE cannot be cached */
	if (pool_has_insertinto_or_locking_clause(node))
		return false;

	/*
	 * If SELECT uses non immutable functions, it's not allowed to
	 * cache.
	 */
	if (pool_has_non_immutable_function_call(node))
		return false;

	/*
	 * If SELECT uses temporary tables it's not allowed to cache.
	 */
	if (pool_config->check_temp_table && pool_has_temp_table(node))
		return false;

	/*
	 * If SELECT uses system catalogs, it's not allowed to cache.
	 */
	if (pool_has_system_catalog(node))
		return false;

	/*
	 * If the table is in the while list, allow to cache even if it is
	 * VIEW or unlogged table.
	 */
	if (pool_config->num_white_memqcache_table_list > 0)
	{
		if (num_oids < 0)
			num_oids = pool_extract_table_oids_from_select_stmt(node, &ctx);

		if (num_oids > 0)
		{
			for (i = 0; i < num_oids; i++)
			{
				char *table = ctx.table_names[i];
				pool_debug("pool_is_allow_to_cache: check table_names[%d] = %s", i, table);
				if (is_view(table) || is_unlogged_table(table))
				{
					if (pool_is_table_in_white_list(table) == false)
					{
						pool_debug("pool_is_allow_to_cache: false");
						return false;
					}
				}
			}
		}
	}
	else
	{
		/*
		 * If SELECT uses views, it's not allowed to cache.
		 */
		if (pool_has_view(node))
			return false;

		/*
		 * If SELECT uses unlogged tables, it's not allowed to cache.
		 */
		if (pool_has_unlogged_table(node))
			return false;
	}
	return true;
}


/*
 * Return true If the SELECTed table is in back list.
 */
bool pool_is_table_in_black_list(const char *table_name)
{

	if (pool_config->num_black_memqcache_table_list > 0 &&
	    pattern_compare((char *)table_name, BLACKLIST, "black_memqcache_table_list") == 1)
	{
		return true;
	}

	return false;
}

/*
 * Return true If the SELECTed table is in white list.
 */
bool pool_is_table_in_white_list(const char *table_name)
{
	if (pool_config->num_white_memqcache_table_list > 0 &&
		pattern_compare((char *)table_name, WHITELIST, "white_memqcache_table_list") == 1)
	{
		return true;
	}

	return false;
}

/*
 * Extract table oid from INSERT/UPDATE/DELETE/TRUNCATE/
 * DROP TABLE/ALTER TABLE/COPY FROM statement.
 * Returns number of oids.
 * In case of error, returns 0(InvalidOid).
 * oids buffer(oidsp) will be discarded by subsequent call.
 */
int pool_extract_table_oids(Node *node, int **oidsp)
{
#define POOL_MAX_DML_OIDS 128
	char *table;
	static int oids[POOL_MAX_DML_OIDS];
	int num_oids;
	int oid;

	if (node == NULL)
	{
		pool_error("pool_extract_table_oids: statement is NULL");
		return 0;
	}

	num_oids = 0;
	*oidsp = oids;

	if (IsA(node, InsertStmt))
	{
		InsertStmt *stmt = (InsertStmt *)node;
		table = nodeToString(stmt->relation);
	}
	else if (IsA(node, UpdateStmt))
	{
		UpdateStmt *stmt = (UpdateStmt *)node;
		table = get_relation_without_alias(stmt->relation);
	}
	else if (IsA(node, DeleteStmt))
	{
		DeleteStmt *stmt = (DeleteStmt *)node;
		table = get_relation_without_alias(stmt->relation);
	}

#ifdef NOT_USED
	/*
	 * We do not handle CREATE TABLE here.  It is possible that
	 * pool_extract_table_oids() is called before CREATE TABLE gets
	 * executed.
	 */
	else if (IsA(node, CreateStmt))
	{
		CreateStmt *stmt = (CreateStmt *)node;
		table = nodeToString(stmt->relation);
	}
#endif

	else if (IsA(node, AlterTableStmt))
	{
		AlterTableStmt *stmt = (AlterTableStmt *)node;
		table = nodeToString(stmt->relation);
	}

	else if (IsA(node, CopyStmt))
	{
		CopyStmt *stmt = (CopyStmt *)node;
		if (stmt->is_from)		/* COPY FROM? */
		{
			table = nodeToString(stmt->relation);
		}
		else
		{
			return 0;
		}
	}

	else if (IsA(node, DropStmt))
	{
		ListCell *cell;

		DropStmt *stmt = (DropStmt *)node;

		if (stmt->removeType != OBJECT_TABLE)
		{
			return 0;
		}

		/* Here, stmt->objects is list of target relation info.  The
		 * first cell of target relation info is a list (possibly)
		 * consists of database, schema and relation.  We need to call
		 * makeRangeVarFromNameList() before passing to nodeToString.
		 * Otherwise we get weird excessively decorated relation name
		 * (''table_name'').
		 */
		foreach(cell, stmt->objects)
		{
			if (num_oids > POOL_MAX_DML_OIDS)
			{
				pool_error("pool_extract_table_oids: too many oids:%d", num_oids);
				return 0;
			}

			table = nodeToString(makeRangeVarFromNameList(lfirst(cell)));
			oid = pool_table_name_to_oid(table);
			if (oid > 0)
			{
				oids[num_oids++] = pool_table_name_to_oid(table);
				pool_debug("pool_extract_table_oids: table:%s oid:%d", table, oids[num_oids-1]);
			}
		}
		return num_oids;
	}
	else if (IsA(node, TruncateStmt))
	{
		ListCell *cell;

		TruncateStmt *stmt = (TruncateStmt *)node;

		foreach(cell, stmt->relations)
		{
			if (num_oids > POOL_MAX_DML_OIDS)
			{
				pool_error("pool_extract_table_oids: too many oids:%d", num_oids);
				return 0;
			}

			table = nodeToString(lfirst(cell));
			oid = pool_table_name_to_oid(table);
			if (oid > 0)
			{
				oids[num_oids++] = pool_table_name_to_oid(table);
				pool_debug("pool_extract_table_oids: table:%s oid:%d", table, oids[num_oids-1]);
			}
		}
		return num_oids;
	}
	else
	{
		pool_debug("pool_extract_table_oids: other than INSERT/UPDATE/DELETE/TRUNCATE/DROP TABLE/ALTER TABLE statement");
		return 0;
	}

	oid = pool_table_name_to_oid(table);
	if (oid > 0)
	{
		oids[num_oids++] = pool_table_name_to_oid(table);
		pool_debug("pool_extract_table_oids: table:%s oid:%d", table, oid);
	}
	return num_oids;
}

/*
 * Get relation name without alias.  NodeToString() calls
 * _outRangeVar(). Unfortunately _outRangeVar() returns table with
 * alias ("t1 AS foo") as a table name if table alias is used. This
 * function just trim down "AS..." part from the table name when table
 * alias is used.
 */
static char *get_relation_without_alias(RangeVar *relation)
{
	char *table;
	char *p;

	if (!IsA(relation, RangeVar))
	{
		pool_error("get_relation_without_alias: not RangeVar(%d)", relation->type);
		return "";
	}
	table = nodeToString(relation);
	if (relation->alias)
	{
		p = strchr(table, ' ');
		if (!p)
		{
			pool_error("get_relation_without_alias: cannot locate space;%s", table);
			return "";
		}
		*p = '\0';
	}
	return table;		
}

#define POOL_OIDBUF_SIZE 1024
static int* oidbuf;
static int oidbufp;
static int oidbuf_size;

/*
 * Add table oid to internal buffer
 */
void pool_add_dml_table_oid(int oid)
{
	int i;
	int* tmp;

	if (oid == 0)
		return;

	if (oidbufp >= oidbuf_size)
	{
		oidbuf_size += POOL_OIDBUF_SIZE;
		tmp = repalloc(oidbuf, sizeof(int) * oidbuf_size);
		if (tmp == NULL)
			return;

		oidbuf = tmp;
	}

	for (i=0;i<oidbufp;i++)
	{
		if (oidbuf[i] == oid)
			/* Already same oid exists */
			return;
	}
	oidbuf[oidbufp++] = oid;
}

/*
 * Get table oid buffer
 */
static int pool_get_dml_table_oid(int **oid)
{
	*oid = oidbuf;
	return oidbufp;
}

static int pool_get_dropdb_table_oids(int **oids, int dboid)
{
	int *rtn = 0;
	int oids_size = 0;
	int *tmp;

	int num_oids = 0;
	DIR *dir;
	struct dirent *dp;
	char path[1024];

	snprintf(path, sizeof(path), "%s/%d", pool_config->memqcache_oiddir, dboid);
	if ((dir = opendir(path)) == NULL)
	{
		pool_debug("pool_get_dropdb_table_oids: Failed to open dir: %s", path);
		return 0;
	}

	while ((dp = readdir(dir)) != NULL)
	{
		if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0)
			continue;

		if (num_oids >= oids_size)
		{
			oids_size += POOL_OIDBUF_SIZE;
			tmp = repalloc(rtn, sizeof(int) * oids_size);
			if (tmp == NULL)
			{
				closedir(dir);
				return 0;
			}
			rtn = tmp;
		}

		rtn[num_oids] = atol(dp->d_name);
		num_oids++;
	}

	closedir(dir);
	*oids = rtn;

	return num_oids;
}

/* Discard oid internal buffer */
static void pool_discard_dml_table_oid(void)
{
	oidbufp = 0;
}

/*
 * Management modules for oid map.  When caching SELECT results, we
 * record table oids to file, which has following structure.
 *
 * memqcache_oiddir -+- database_oid -+-table_oid_file1
 *                                    |
 *                                    +-table_oid_file2
 *                                    |
 *                                    +-table_oid_file3...
 *
 * table_oid_file's name is table oid, which was used by the SELECT
 * statement. The file has 1 or more cacheid(s). When SELECT result is
 * cached, the file is created and cache id is appended. Later SELECT
 * using same table oid will add to the same file. If the SELECT uses
 * multiple tables, multiple table_oid_file will be created. When
 * INSERT/UPDATE/DELETE is executed, corresponding caches must be
 * deleted(cache invalidation) (when DROP TABLE, ALTER TABLE is
 * executed, the caches must be deleted as well). When database is
 * dropped, all caches belonging to the database must be deleted.
 */

/*
 * Get oid of current database
 */
static int pool_get_database_oid(void)
{
/*
 * Query to convert table name to oid
 */
	int oid = 0;
	static POOL_RELCACHE *relcache;
	POOL_CONNECTION_POOL *backend;

	backend = pool_get_session_context(false)->backend;

	/*
	 * If relcache does not exist, create it.
	 */
	if (!relcache)
	{
		relcache = pool_create_relcache(pool_config->relcache_size, DATABASE_TO_OID_QUERY,
										int_register_func, int_unregister_func,
										false);
		if (relcache == NULL)
		{
			pool_error("pool_get_database_oid: pool_create_relcache error");
			return oid;
		}
	}

	/*
	 * Search relcache.
	 */
	oid = (int)(intptr_t)pool_search_relcache(relcache, backend,
											  MASTER_CONNECTION(backend)->sp->database);
	return oid;
}

/*
 * Get oid of current database for discarding cache files
 * after executing DROP DATABASE
 */
int pool_get_database_oid_from_dbname(char *dbname)
{
	int dboid = 0;
	POOL_SELECT_RESULT *res;
	POOL_STATUS status;
	char query[1024];

	POOL_CONNECTION_POOL *backend;
	backend = pool_get_session_context(false)->backend;

	snprintf(query, sizeof(query), DATABASE_TO_OID_QUERY, dbname); 
	do_query(MASTER(backend), query, &res, MAJOR(backend));

	if (res->numrows != 1)
	{    
		pool_debug("pool_discard_oid_maps_by_db: Failed. Received %d rows", res->numrows);
		free_select_result(res);
		return 0;
	}    

	dboid = atol(res->data[0]);
	free_select_result(res);

	return dboid;
}

/*
 * Add cache id(shmem case) or hash key(memcached case) to table oid
 * map file.  Caller must hold shmem lock before calling this function
 * to avoid file extension conflict among different pgpool child
 * process.
 * As of pgpool-II 3.2, pool_handle_query_cache is responsible for that.
 * (pool_handle_query_cache -> pool_commit_cache -> pool_add_table_oid_map)
 */
static void pool_add_table_oid_map(POOL_CACHEKEY *cachekey, int num_table_oids, int *table_oids)
{
	char *dir;
	int dboid;
	char path[1024];
	int i;
	int len;

	/*
	 * Create memqcache_oiddir
	 */
	dir = pool_config->memqcache_oiddir;

	if (mkdir(dir, S_IREAD|S_IWRITE|S_IEXEC) == -1)
	{
		if (errno != EEXIST)
		{
			pool_error("pool_add_table_oid_map: failed to create %s. Reason:%s", dir, strerror(errno));
			return;
		}
	}

	/*
	 * Create memqcache_oiddir/database_oid
	 */
	dboid = pool_get_database_oid();

	pool_debug("pool_add_table_oid_map: dboid %d", dboid);
	if (dboid <= 0)
	{
		pool_error("pool_add_table_oid_map: could not get database oid");
		return;
	}

	snprintf(path, sizeof(path), "%s/%d", dir, dboid);
	if (mkdir(path, S_IREAD|S_IWRITE|S_IEXEC) == -1)
	{
		if (errno != EEXIST)
		{
			pool_error("pool_add_table_oid_map: failed to create %s. reason:%s", path, strerror(errno));
			return;
		}
	}

	if (pool_is_shmem_cache())
	{
		len = sizeof(cachekey->cacheid);
	}
	else
	{
		len = sizeof(cachekey->hashkey);
	}

	for (i=0;i<num_table_oids;i++)
	{
		int fd;
		int oid = table_oids[i];
		int sts;
		struct flock fl;

		/*
		 * Create or open each memqcache_oiddir/database_oid/table_oid
		 */
		snprintf(path, sizeof(path), "%s/%d/%d", dir, dboid, oid);
		if ((fd = open(path, O_CREAT|O_RDWR, S_IRUSR|S_IWUSR)) == -1)
		{
			pool_error("pool_add_table_oid_map: failed to create or open %s. reason:%s",
					   path, strerror(errno));
			return;
		}

		fl.l_type   = F_WRLCK;
		fl.l_whence = SEEK_SET;
		fl.l_start  = 0;        	/* Offset from l_whence         */
		fl.l_len    = 0;        	/* length, 0 = to EOF           */

		sts = fcntl(fd, F_SETLKW, &fl);
		if (sts == -1)
		{
			pool_error("pool_add_table_oid_map: failed to lock %s. reason:%s",
					   path, strerror(errno));
			close(fd);
			return;
		}

		/*
		 * Below was ifdef-out because of a performance reason.
		 * Looking for duplicate cache entries in a file needed
		 * unacceptably high cost. So we gave up this and decided not
		 * to care about duplicate entries in the file.
		 */
#ifdef NOT_USED
		for (;;)
		{
			sts = read(fd, (char *)&buf, len);
			if (sts == -1)
			{
				pool_error("pool_add_table_oid_map: failed to read %s. reason:%s",
						   path, strerror(errno));
				close(fd);
				return;
			}
			else if (sts == len)
			{
				if (memcmp(cachekey, &buf, len) == 0)
				{
					/* Same key found. Skip this */
					close(fd);
					return;
				}
				continue;
			}
			/*
			 * Must be EOF
			 */
			if (sts != 0)
			{
				pool_error("pool_add_table_oid_map: invalid read length %d for %s", sts, path);
				close(fd);
				return;
			}
			break;
		}
#endif

		if (lseek(fd, 0, SEEK_END) == -1)
		{
			pool_error("pool_add_table_oid_map: failed to lseek %s. reason:%s",
					   path, strerror(errno));
			close(fd);
			return;
		}

		/*
		 * Write cache_id or cache key at the end of file
		 */
		sts = write(fd, (char *)cachekey, len);
		if (sts == -1 || sts != len)
		{
			pool_error("pool_add_table_oid_map: failed to write %s. reason:%s",
					   path, strerror(errno));
			close(fd);
			return;
		}
		close(fd);
	}
}

/*
 * Discard all oid maps at pgpool-II startup.
 * This is necessary for shmem case.
 */
void pool_discard_oid_maps(void)
{
	char command[1024];

	snprintf(command, sizeof(command), "/bin/rm -fr %s/[0-9]*",
			 pool_config->memqcache_oiddir);
	if(system(command) == -1)
        ereport(WARNING,
            (errmsg("unable to execute command \"%s\"",command),
             errdetail("system() command failed with error \"%s\"",strerror(errno))));
    

}

void pool_discard_oid_maps_by_db(int dboid)
{
	char command[1024];

	if (pool_is_shmem_cache())
	{
		snprintf(command, sizeof(command), "/bin/rm -fr %s/%d/",
				 pool_config->memqcache_oiddir, dboid);
		if(system(command) == -1)
            ereport(WARNING,
                    (errmsg("unable to execute command \"%s\"",command),
                     errdetail("system() command failed with error \"%s\"",strerror(errno))));

        pool_debug("command: %s", command);
	}
}

/*
 * Reading cache id(shmem case) or hash key(memcached case) from table
 * oid map file according to table_oids and discard cache entries.  If
 * unlink is true, the file will be unlinked after successful cache
 * removal.
 */
static void pool_invalidate_query_cache(int num_table_oids, int *table_oid, bool unlinkp, int dboid)
{
	char *dir;
	char path[1024];
	int i;
	int len;
	POOL_CACHEKEY buf;

	/*
	 * Create memqcache_oiddir
	 */
	dir = pool_config->memqcache_oiddir;
	if (mkdir(dir, S_IREAD|S_IWRITE|S_IEXEC) == -1)
	{
		if (errno != EEXIST)
		{
			pool_error("pool_invalidate_query_cache: failed to create %s", dir);
			return;
		}
	}

	/*
	 * Create memqcache_oiddir/database_oid
	 */
	if (dboid == 0) {
		dboid = pool_get_database_oid();

		pool_debug("pool_invalidate_query_cache: dboid %d", dboid);
		if (dboid <= 0)
		{
			pool_error("pool_invalidate_query_cache: could not get database oid");
			return;
		}
	}

	snprintf(path, sizeof(path), "%s/%d", dir, dboid);
	if (mkdir(path, S_IREAD|S_IWRITE|S_IEXEC) == -1)
	{
		if (errno != EEXIST)
		{
			pool_error("pool_invalidate_query_cache: failed to create %s. reason:%s", path, strerror(errno));
			return;
		}
	}

	if (pool_is_shmem_cache())
	{
		len = sizeof(buf.cacheid);
	}
	else
	{
		len = sizeof(buf.hashkey);
	}

	for (i=0;i<num_table_oids;i++)
	{
		int fd;
		int oid = table_oid[i];
		int sts;
		struct flock fl;

		/*
		 * Open each memqcache_oiddir/database_oid/table_oid
		 */
		snprintf(path, sizeof(path), "%s/%d/%d", dir, dboid, oid);
		if ((fd = open(path, O_RDONLY)) == -1)
		{
			/* This may be normal. It is possible that no SELECT has
			 * been issued since the table has been created or since
			 * pgpool-II started up.
			 */
			pool_debug("pool_invalidate_query_cache: failed to open %s. reason:%s",
					   path, strerror(errno));
			continue;
		}

		fl.l_type   = F_RDLCK;
		fl.l_whence = SEEK_SET;
		fl.l_start  = 0;        	/* Offset from l_whence         */
		fl.l_len    = 0;        	/* length, 0 = to EOF           */

		sts = fcntl(fd, F_SETLKW, &fl);
		if (sts == -1)
		{
			pool_error("pool_invalidate_query_cache: failed to lock %s. reason:%s",
					   path, strerror(errno));
			close(fd);
			return;
		}
		for (;;)
		{
			sts = read(fd, (char *)&buf, len);
			if (sts == -1)
			{
				pool_error("pool_invalidate_query_cache: failed to read %s. reason:%s",
						   path, strerror(errno));
				close(fd);
				return;
			}
			else if (sts == len)
			{
				if (pool_is_shmem_cache())
				{
					pool_debug("pool_invalidate_query_cache: deleting cacheid:%d itemid:%d",
							   buf.cacheid.blockid, buf.cacheid.itemid);
					pool_delete_item_shmem_cache(&buf.cacheid);
				}
#ifdef USE_MEMCACHED
				else
				{
					char delbuf[33];

					memcpy(delbuf, buf.hashkey, 32);
					delbuf[32] = 0;
					pool_debug("pool_invalidate_query_cache: deleting %s", delbuf);
					delete_cache_on_memcached(delbuf);
				}
#endif
				continue;
			}

			/*
			 * Must be EOF
			 */
			if (sts != 0)
			{
				pool_error("pool_invalidate_query_cache: invalid read length %d for %s", sts, path);
				close(fd);
				return;
			}
			break;
		}

		if (unlinkp)
		{
			unlink(path);
		}
		close(fd);
	}
#ifdef SHMEMCACHE_DEBUG
	dump_shmem_cache(0);
#endif
}

/*
 * Reset SELECT data buffers
 */
static void pool_reset_memqcache_buffer(void)
{
	POOL_SESSION_CONTEXT * session_context;

	session_context = pool_get_session_context(true);

	if (session_context && session_context->query_cache_array)
	{
		POOL_TEMP_QUERY_CACHE *cache;

		pool_debug("pool_reset_memqcache_buffer: discard: %p", session_context->query_cache_array);

		pool_discard_query_cache_array(session_context->query_cache_array);
		session_context->query_cache_array = pool_create_query_cache_array();

		pool_debug("pool_reset_memqcache_buffer: create: %p", session_context->query_cache_array);

		/*
		 * if the query context is still under use, we cannot discard
		 * temporary cache.
		 */
		if (can_query_context_destroy(session_context->query_context))
		{
			pool_debug("pool_reset_memqcache_buffer: discard temp buffer of %p (%s)",
					   session_context->query_context, session_context->query_context->original_query);
			cache = pool_get_current_cache();
			pool_discard_temp_query_cache(cache);
			/*
			 * Reset temp_cache pointer in the current query context
			 * so that we don't double free memory.
			 */
			session_context->query_context->temp_cache = NULL;
		}
	}
	pool_discard_dml_table_oid();
	pool_tmp_stats_reset_num_selects();
}

/*
 * Return true if memory cache method is "shmem".  The purpose of this
 * function is to cache the result of stcmp and to save a few cycle.
 */
bool pool_is_shmem_cache(void)
{
	static int result = -1;

	if (result == -1)
	{
		result = strcmp(pool_config->memqcache_method, "shmem");
	}
	return (result==0)?true:false;
}

/*
 * Remember memory cache number of blocks.
 */
static int memqcache_num_blocks;
static void pool_set_memqcache_blocks(int num_blocks)
{
	memqcache_num_blocks = num_blocks;
}

/*
 * Return memory cache number of blocks.
 */
static int pool_get_memqcache_blocks(void)
{
	return memqcache_num_blocks;
}

/*
 * Query cache on shared memory management modules.
 */

/*
 * Calculate necessary shared memory size.
 */
size_t pool_shared_memory_cache_size(void)
{
	int num_blocks;
	size_t size;

	if (pool_config->memqcache_maxcache > pool_config->memqcache_cache_block_size)
		ereport(FATAL,
			(errmsg("invalid memory cache configuration"),
					errdetail("memqcache_cache_block_size %d should be greater or equal to memqcache_maxcache %d",
								pool_config->memqcache_cache_block_size,
								pool_config->memqcache_maxcache)));


	num_blocks = pool_config->memqcache_total_size/
		pool_config->memqcache_cache_block_size;
	if (num_blocks == 0)
		ereport(FATAL,
			(errmsg("invalid memory cache configuration"),
					errdetail("memqcache_total_size %d should be greater or equal to memqcache_cache_block_size %d",
								pool_config->memqcache_total_size,
								pool_config->memqcache_cache_block_size)));

		ereport(LOG,
			(errmsg("memory cache initialized"),
					errdetail("memcache blocks :%d",num_blocks)));
	/* Remember # of blocks */
	pool_set_memqcache_blocks(num_blocks);
	size = pool_config->memqcache_cache_block_size * num_blocks;
	return size;
}

/*
 * Acquire and initialize shared memory cache. This should be called
 * only once from pgpool main process at the process staring up time.
 */
static void *shmem;
int pool_init_memory_cache(size_t size)
{
	ereport(DEBUG1,
		(errmsg("memory cache request size : %zd",size)));

	shmem = pool_shared_memory_create(size);
	return 0;
}

/*
 * Clear all the shared memory cache and reset FSMM and hash table.
 */
void
pool_clear_memory_cache(void)
{
	size_t size;
#ifdef HAVE_SIGPROCMASK
	sigset_t oldmask;
#else
	int	oldmask;
#endif

	POOL_SETMASK2(&BlockSig, &oldmask);
	pool_shmem_lock();

    PG_TRY();
    {
        size = pool_shared_memory_cache_size();
        memset(shmem, 0, size);

        size = pool_shared_memory_fsmm_size();
        pool_reset_fsmm(size);

        pool_discard_oid_maps();

        pool_hash_reset(pool_config->memqcache_max_num_cache);
    }
    PG_CATCH();
    {
        pool_shmem_unlock();
        POOL_SETMASK(&oldmask);
        PG_RE_THROW();
    }
    PG_END_TRY();

	pool_shmem_unlock();
	POOL_SETMASK(&oldmask);
}

/*
 * Return shared memory cache address
 */
static void *pool_memory_cache_address(void)
{
	return shmem;
}

/*
 * Initialize new block
 */

/*
 * Free space management map
 * 
 * Free space management map (FSMM) consists of bytes. Each byte
 * corresponds to block id. For example, if you have 1GB cache and
 * block size is 8Kb, number of blocks = 131,072, thus total size of
 * FSMM is 128Kb.  Each FSMM entry has value from 0 to 255. Those
 * values describes total free space in each block.
 * For example, if the value is 2, the free space can be between 64
 * bytes and 95 bytes.
 *
 * value free space(in bytes)
 * 0     0-31
 * 1     32-63
 * 2     64-95
 * 3     96-127
 * :
 * 255   8160-8192
 */

/*
 * Calculate necessary shared memory size for FSMM. Should be called after 
 * pool_shared_memory_cache_size.
 */
size_t pool_shared_memory_fsmm_size(void)
{
	size_t size;

	size = pool_get_memqcache_blocks() * sizeof(char);
	return size;
}

/*
 * Acquire and initialize shared memory cache for FSMM. This should be
 * called after pool_shared_memory_cache_size only once from pgpool
 * main process at the process staring up time.
 */
static void *fsmm;
int pool_init_fsmm(size_t size)
{
	int maxblock = 	pool_get_memqcache_blocks();
	int encode_value;

	fsmm = pool_shared_memory_create(size);
	if (fsmm == NULL)
	{
		pool_error("pool_init_fsmm: failed to allocate shared memory cache. request size: %zd", size);
		return -1;
	}

	encode_value = POOL_MAX_FREE_SPACE/POOL_FSMM_RATIO;
	memset(fsmm, encode_value, maxblock);
	return 0;
}

/*
 * Return shared memory fsmm address
 */
static void *pool_fsmm_address(void)
{
	return fsmm;
}

/*
 * Clock algorithm shared query cache management modules.
 */

/*
 * Clock hand pointing to next victim block
 */
static int *pool_fsmm_clock_hand;

/*
 * Allocate and initialize clock hand on shmem
 */
void pool_allocate_fsmm_clock_hand(void)
{
	pool_fsmm_clock_hand = pool_shared_memory_create(sizeof(*pool_fsmm_clock_hand));
	*pool_fsmm_clock_hand = 0;
}

/*
 * Reset FSMM.
 */
static void
pool_reset_fsmm(size_t size)
{
	int encode_value;

	encode_value = POOL_MAX_FREE_SPACE/POOL_FSMM_RATIO;
	memset(fsmm, encode_value, size);

	*pool_fsmm_clock_hand = 0;
}

/*
 * Find victim block using clock algorithm and make it free.
 * Returns new free block id.
 */
static POOL_CACHE_BLOCKID pool_reuse_block(void)
{
	int maxblock = pool_get_memqcache_blocks();
	char *block = block_address(*pool_fsmm_clock_hand);
	POOL_CACHE_BLOCK_HEADER *bh = (POOL_CACHE_BLOCK_HEADER *)block;
	POOL_CACHE_BLOCKID reused_block;
	POOL_CACHE_ITEM_POINTER *cip;
	char *p;
	int i;

	bh->flags = 0;
	reused_block = *pool_fsmm_clock_hand;
	p = block_address(reused_block);

	for (i=0;i<bh->num_items;i++)
	{
		cip = item_pointer(p, i);

		if (!(POOL_ITEM_DELETED & cip->flags))
		{
			pool_hash_delete(&cip->query_hash);
			pool_debug("pool_reuse_block: blockid: %d item: %d", reused_block, i);
		}
	}

	pool_init_cache_block(reused_block);
	pool_update_fsmm(reused_block, POOL_MAX_FREE_SPACE);

	(*pool_fsmm_clock_hand)++;
	if (*pool_fsmm_clock_hand >= maxblock)
		*pool_fsmm_clock_hand = 0;

	pool_log("pool_reuse_block: blockid: %d", reused_block);

	return reused_block;
}

/*
 * Get block id which has enough space
 */
static POOL_CACHE_BLOCKID pool_get_block(size_t free_space)
{
	int encode_value;
	unsigned char *p = pool_fsmm_address();
	int i;
	int maxblock = pool_get_memqcache_blocks();
	POOL_CACHE_BLOCK_HEADER *bh;

	if (p == NULL)
	{
		pool_error("pool_get_block: FSMM is not initialized");
		return -1;
	}

	if (free_space > POOL_MAX_FREE_SPACE)
	{
		pool_error("pool_get_block: invalid free space %zd", free_space);
		return -1;
	}

	encode_value = free_space/POOL_FSMM_RATIO;

	for (i=0;i<maxblock;i++)
	{
		if (p[i] >= encode_value)
		{
			/*
			 * This block *may" have enough space.
			 * We need to make sure it actually has enough space.
			 */
			bh = (POOL_CACHE_BLOCK_HEADER *)block_address(i);
			if (bh->free_bytes >= free_space)
			{
				return (POOL_CACHE_BLOCKID)i;
			}
		}
	}

	/*
	 * No enough space found. Reuse victim block
	 */
	return pool_reuse_block();
}

/*
 * Update free space info for specified block
 */
static void pool_update_fsmm(POOL_CACHE_BLOCKID blockid, size_t free_space)
{
	int encode_value;
	char *p = pool_fsmm_address();

	if (p == NULL)
	{
		pool_error("pool_set_free_space: FSMM is not initialized");
		return;
	}

	if (blockid >= pool_get_memqcache_blocks())
	{
		pool_error("pool_set_free_space: invalid block id %d", blockid);
		return;
	}

	if (free_space < 0 || free_space > POOL_MAX_FREE_SPACE)
	{
		pool_error("pool_set_free_space: invalid free space %zd", free_space);
		return;
	}

	encode_value = free_space/POOL_FSMM_RATIO;

	p[blockid] = encode_value;

	return;
}

/*
 * Add item data to shared memory cache.
 * On successful registration, returns cache id.
 * The cache id is overwritten by the subsequent call to this function.
 * On error returns NULL.
 */
static POOL_CACHEID *pool_add_item_shmem_cache(POOL_QUERY_HASH *query_hash, char *data, int size)
{
	static POOL_CACHEID cacheid;
	POOL_CACHE_BLOCKID blockid;
	POOL_CACHE_BLOCK_HEADER *bh;
	POOL_CACHE_ITEM_POINTER *cip;

	POOL_CACHE_ITEM ci;
	POOL_CACHE_ITEM_POINTER cip_body;
	char *item;

	int request_size;
	char *p;
	int i;
	char *src;
	char *dst;
	int num_deleted;
	char *dcip;
	char *dci;
	bool need_pack;
	char *work_buffer;
	int index;

	if (query_hash == NULL)
	{
		pool_error("pool_add_item_shmem_cache: query hash is NULL");
		return NULL;
	}

	if (data == NULL)
	{
		pool_error("pool_add_item_shmem_cache: data NULL");
		return NULL;
	}

	if (size <= 0)
	{
		pool_error("pool_add_item_shmem_cache: invalid request size: %d", size);
		return NULL;
	}

	/* Add overhead */
	request_size = size + sizeof(POOL_CACHE_ITEM_POINTER) + sizeof(POOL_CACHE_ITEM_HEADER);

	/* Get cache block which has enough space */
	blockid = pool_get_block(request_size);

	if (blockid == -1)
	{
		return NULL;
	}

	/*
	 * Initialize the block if necessary. If no live items are
	 * remained, we also initialize the block. If there's contiguous
	 * deleted items, we turn them into free space as well.
	 */
	pool_init_cache_block(blockid);

	/*
	 * Make sure that we have at least one free hash element.
	 */
	while (!is_free_hash_element())
	{
		/* If not, reuse next victim block */
		blockid = pool_reuse_block();
		pool_init_cache_block(blockid);
	}

	/* Get block address on shmem */
	p = block_address(blockid);
	bh = (POOL_CACHE_BLOCK_HEADER *)p;

	/*
	 * Create contiguous free space. We assume that item bodies are
	 * ordered from bottom to top of the block, and corresponding item
	 * pointers are ordered from the youngest to the oldest in the
	 * beginning of the block.
	 */

	/*
	 * Optimization. If there's no deleted items, we don't need to
	 * pack it to create contiguous free space.
	 */
	need_pack = false;
	for (i=0;i<bh->num_items;i++)
	{
		cip = item_pointer(p, i);

		if (POOL_ITEM_DELETED & cip->flags)		/* Deleted item? */
		{
			need_pack = true;
			pool_debug("pool_add_item_shmem_cache: start creating contiguous space");
			break;
		}
	}

	/*
	 * We disable packing for now.
	 * Revisit and remove following code fragment later.
	 */
	need_pack = false;

	if (need_pack)
	{
		/*
		 * Pack and create contiguous free space.
		 */
		dcip = calloc(1, pool_config->memqcache_cache_block_size);
		if (!dcip)
		{
			pool_error("pool_add_item_shmem_cache: calloc failed");
			return NULL;
		}

		work_buffer = dcip;
		dci = dcip + pool_config->memqcache_cache_block_size;
		num_deleted = 0;
		index = 0;

		for (i=0;i<bh->num_items;i++)
		{
			int total_length;
			POOL_CACHEID cid;

			cip = item_pointer(p, i);

			if (POOL_ITEM_DELETED & cip->flags)		/* Deleted item? */
			{
				num_deleted++;
				continue;
			}

			/* Copy item body */
			src = p + cip->offset;
			total_length = item_header(p, i)->total_length;
			dst = dci - total_length;
			cip->offset = dst - work_buffer;
			memcpy(dst, src, total_length);

			dci -= total_length;

			/* Copy item pointer */
			src = (char *)cip;
			dst = (char *)item_pointer(dcip, index);
			memcpy(dst, src, sizeof(POOL_CACHE_ITEM_POINTER));

			/* Update hash index */
			cid.blockid = blockid;
			cid.itemid = index;
			pool_hash_insert(&cip->query_hash, &cid, true);
			pool_debug("pool_add_item_shmem_cache: item cid updated. old:%d %d new:%d %d",
					   blockid, i, blockid, index);

			index++;
		}
	
		/* All items deleted? */
		if (num_deleted > 0 && num_deleted == bh->num_items)
		{
			pool_debug("pool_add_item_shmem_cache: all items deleted num_deleted:%d", num_deleted);
			bh->flags = 0;
			pool_init_cache_block(blockid);
			pool_update_fsmm(blockid, POOL_MAX_FREE_SPACE);
		}
		else
		{
			/* Update number of items */
			bh->num_items -= num_deleted;

			/* Copy back the packed block except block header */
			memcpy(p+sizeof(POOL_CACHE_BLOCK_HEADER),
				   work_buffer+sizeof(POOL_CACHE_BLOCK_HEADER),
				   pool_config->memqcache_cache_block_size-sizeof(POOL_CACHE_BLOCK_HEADER));
		}
		free(work_buffer);
	}

	/*
	 * Make sure that we have enough free space
	 */
	if (bh->free_bytes < request_size)
	{
		/* This should not happen */
		pool_error("pool_add_item_shmem_cache: not enough free space. Free space: %d required: %d block id:%d",
				   bh->free_bytes, request_size, blockid);
		return NULL;
	}

	/*
	 * At this point, new item can be located at block_header->num_items
	 */

	/* Fill in cache item header */
	ci.header.timestamp = time(NULL);
	ci.header.total_length = sizeof(POOL_CACHE_ITEM_HEADER) + size;

	/* Calculate item body address */
	if (bh->num_items == 0)
	{
		/* This is the #0 item. So address is block_bottom -
		 * data_length */
		item = p + pool_config->memqcache_cache_block_size - ci.header.total_length;

		/* Mark this block used */
		bh->flags = POOL_BLOCK_USED;
	}
	else
	{
		cip = item_pointer(p, bh->num_items-1);
		item = p + cip->offset - ci.header.total_length;
	}

	/* Copy item header */
	memcpy(item, &ci, sizeof(POOL_CACHE_ITEM_HEADER));
	bh->free_bytes -= sizeof(POOL_CACHE_ITEM_HEADER);

	/* Copy item body */
	memcpy(item + sizeof(POOL_CACHE_ITEM_HEADER), data, size);
	bh->free_bytes -= size;

	/* Copy cache item pointer */
	memcpy(&cip_body.query_hash, query_hash, sizeof(POOL_QUERY_HASH));
	memset(&cip_body.next, 0, sizeof(POOL_CACHEID));
	cip_body.offset = item - p;
	cip_body.flags = POOL_ITEM_USED;
	memcpy(item_pointer(p, bh->num_items), &cip_body, sizeof(POOL_CACHE_ITEM_POINTER));
	bh->free_bytes -= sizeof(POOL_CACHE_ITEM_POINTER);

	/* Update FSMM */
	pool_update_fsmm(blockid, bh->free_bytes);

	cacheid.blockid = blockid;
	cacheid.itemid = bh->num_items;
	pool_debug("pool_add_item_shmem_cache: new item inserted. blockid: %d itemid:%d", 
			   cacheid.blockid, cacheid.itemid);

	/* Add up number of items */
	bh->num_items++;

	/* Update hash table */
	if (pool_hash_insert(query_hash, &cacheid, false) < 0)
	{
		pool_error("pool_add_item_shmem_cache: pool_hash_insert failed");

		/* Since we have failed to insert hash index entry, we need to
		 * undo the addition of cache entry.
		 */
		pool_delete_item_shmem_cache(&cacheid);
		return NULL;
	}

	pool_debug("pool_add_item_shmem_cache: block: %d item: %d", cacheid.blockid, cacheid.itemid);

#ifdef SHMEMCACHE_DEBUG
	dump_shmem_cache(blockid);
#endif
	return &cacheid;
}

/*
 * Returns item data address on shared memory cache specified by query hash.
 * Also data length is set to *size.
 * On error or data not found case returns NULL.
 * Detail is set to *sts. (0: success, 1: not found, -1: error)
 */
static char *pool_get_item_shmem_cache(POOL_QUERY_HASH *query_hash, int *size, int *sts)
{
	POOL_CACHEID *cacheid;
	POOL_CACHE_ITEM_HEADER *cih;

	if (sts == NULL)
	{
		pool_error("pool_get_item_shmem_cache: sts is NULL");
		return NULL;
	}

	*sts = -1;

	if (query_hash == NULL)
	{
		pool_error("pool_get_item_shmem_cache: query hash is NULL");
		return NULL;
	}

	if (size == NULL)
	{
		pool_error("pool_get_item_shmem_cache: size is NULL");
		return NULL;
	}

	/*
	 * Find cache header by using hash table
	 */
	cacheid = pool_find_item_on_shmem_cache(query_hash);
	if (cacheid == NULL)
	{
		/* Not found */
		*sts = 1;
		return NULL;
	}

	cih = pool_cache_item_header(cacheid);

	*size = cih->total_length - sizeof(POOL_CACHE_ITEM_HEADER);
	return (char *)cih + sizeof(POOL_CACHE_ITEM_HEADER);
}

/*
 * Find data on shared memory cache specified query hash.
 * On success returns cache id.
 * The cache id is overwritten by the subsequent call to this function.
 */
static POOL_CACHEID *pool_find_item_on_shmem_cache(POOL_QUERY_HASH *query_hash)
{
	static POOL_CACHEID cacheid;
	POOL_CACHEID *c;
	POOL_CACHE_ITEM_HEADER *cih;
	time_t now;

	c = pool_hash_search(query_hash);
	if (!c)
	{
		return NULL;
	}

	cih = item_header(block_address(c->blockid), c->itemid);

	/* Check cache expiration */
	if (pool_config->memqcache_expire > 0)
	{
		now = time(NULL);
		if (now > (cih->timestamp + pool_config->memqcache_expire))
		{
			pool_debug("pool_find_item_on_shmem_cache: cache expired");
			pool_debug("pool_find_item_on_shmem_cache: now: %ld timestamp: %ld",
					   now, cih->timestamp + pool_config->memqcache_expire);
			pool_delete_item_shmem_cache(c);
			return NULL;
		}
	}

	cacheid.blockid = c->blockid;
	cacheid.itemid = c->itemid;
	return &cacheid;
}

/*
 * Delete item data specified cache id from shmem.
 * On successful deletion, returns 0.
 * Other wise return -1.
 * FSMM is also updated.
 */
static int pool_delete_item_shmem_cache(POOL_CACHEID *cacheid)
{
	POOL_CACHE_BLOCK_HEADER *bh;
	POOL_CACHE_ITEM_POINTER *cip;
	POOL_CACHE_ITEM_HEADER *cih;
	POOL_QUERY_HASH key;
	int size;

	pool_debug("pool_delete_item_shmem_cache: cacheid:%d itemid:%d",
			   cacheid->blockid, cacheid->itemid);

	if (cacheid->blockid >= pool_get_memqcache_blocks())
	{
		pool_error("pool_delete_item_shmem_cache: invalid block id %d",
				   cacheid->blockid);
		return -1;
	}

	bh = (POOL_CACHE_BLOCK_HEADER *)block_address(cacheid->blockid);
	if (!(bh->flags & POOL_BLOCK_USED))
	{
		pool_error("pool_delete_item_shmem_cache: block %d is not used",
				   cacheid->blockid);
		return -1;
	}

	if (cacheid->itemid < 0 || cacheid->itemid >= bh->num_items)
	{
		/*
		 * This could happen if the block is reused.  Since contents
		 * of oid map file is not updated when the block is reused.
		 */
		pool_debug("pool_delete_item_shmem_cache: invalid item id %d in block:%d",
				   cacheid->itemid, cacheid->blockid);
		return -1;
	}

	cip = item_pointer(block_address(cacheid->blockid), cacheid->itemid);
	if (!(cip->flags & POOL_ITEM_USED))
	{
		pool_error("pool_delete_item_shmem_cache: item %d was not used",
				   cacheid->itemid);
		return -1;
	}

	if (cip->flags & POOL_ITEM_DELETED)
	{
		pool_error("pool_delete_item_shmem_cache: item %d was deleted",
				   cacheid->itemid);
		return -1;
	}

	/* Save cache key */
	memcpy(&key, &cip->query_hash, sizeof(POOL_QUERY_HASH));

	cih = pool_cache_item_header(cacheid);
	size = cih->total_length + sizeof(POOL_CACHE_ITEM_POINTER);

	/* Delete item pointer */
	cip->flags |= POOL_ITEM_DELETED;

	/*
	 * We do NOT count down bh->num_items here. The deleted space will be recycled
	 * by pool_add_item_shmem_cache(). However, if this is the last item, we can
	 * recycle whole block.
	 *
	 * 2012/4/1: Now we do not pack data in
	 * pool_add_item_shmem_cache() for performance reason. Also we
	 * count down num_items if it is the last one.
	 */
	if ((bh->num_items -1) == 0)
	{
		pool_debug("pool_delete_item_shmem_cache: no item remains. So initialize block");
		bh->flags = 0;
		pool_init_cache_block(cacheid->blockid);
	}

	/* Remove hash index */
	pool_hash_delete(&key);

	/*
	 * If the deleted item is last one in the block, we add it to the free space.
	 */
	if (cacheid->itemid == (bh->num_items -1))
	{
		bh->free_bytes += size;
		pool_debug("pool_delete_item_shmem_cache: after deleting %d bytes, free_bytes is %d",
				   size, bh->free_bytes);
		bh->num_items--;
	}

	/* Update FSMM */
	pool_update_fsmm(cacheid->blockid, bh->free_bytes);

	return 0;
}

/*
 * Returns item header specified by cache id.
 */
static POOL_CACHE_ITEM_HEADER *pool_cache_item_header(POOL_CACHEID *cacheid)
{
	POOL_CACHE_BLOCK_HEADER *bh;

	if (cacheid->blockid >= pool_get_memqcache_blocks())
	{
		pool_error("pool_cache_item_header: invalid block id %d", cacheid->blockid);
		return NULL;
	}

	bh = (POOL_CACHE_BLOCK_HEADER *)block_address(cacheid->blockid);
	if (cacheid->itemid < 0 || cacheid->itemid >= bh->num_items)
	{
		pool_error("pool_cache_item_header: invalid item id %d", cacheid->itemid);
		return NULL;
	}

	return item_header((char *)bh, cacheid->itemid);
}

/*
 * Initialize specified block.
 */
static int pool_init_cache_block(POOL_CACHE_BLOCKID blockid)
{
	char *p;
	POOL_CACHE_BLOCK_HEADER *bh;

	if (blockid >= pool_get_memqcache_blocks())
	{
		pool_error("pool_init_cache_block: invalid block id %d", blockid);
		return -1;
	}

	p = block_address(blockid);
	bh = (POOL_CACHE_BLOCK_HEADER *)p;

	/* Is this block used? */
	if (!(bh->flags & POOL_BLOCK_USED))
	{
		/* Initialize empty block */
		memset(p, 0, pool_config->memqcache_cache_block_size);
		bh->free_bytes = pool_config->memqcache_cache_block_size -
			sizeof(POOL_CACHE_BLOCK_HEADER);
	}
	return 0;
}

#if NOT_USED
/*
 * Delete all items in the block.
 */
static void pool_wipe_out_cache_block(POOL_CACHE_BLOCKID blockid)
{
	char *p;
	POOL_CACHE_BLOCK_HEADER *bh;
	POOL_CACHE_ITEM_POINTER *cip;
	POOL_CACHEID cacheid;
	int i;

	/* Get block address on shmem */
	p = block_address(blockid);
	bh = (POOL_CACHE_BLOCK_HEADER *)p;
	cacheid.blockid = blockid;

	for (i=0;i<bh->num_items;i++)
	{
		cip = item_pointer(p, i);

		if ((POOL_ITEM_DELETED & cip->flags) == 0)		/* Not deleted item? */
		{
			cacheid.itemid = i;
			pool_delete_item_shmem_cache(&cacheid);
		}
	}

	bh->flags = 0;
	pool_init_cache_block(blockid);
	pool_update_fsmm(blockid, POOL_MAX_FREE_SPACE);
}
#endif

/*
 * Acquire lock: XXX giant lock
 */
void pool_shmem_lock(void)
{
	if (pool_is_shmem_cache())
	{
		pool_semaphore_lock(SHM_CACHE_SEM);

	}
}

/*
 * Release lock
 */
void pool_shmem_unlock(void)
{
	if (pool_is_shmem_cache())
	{
		pool_semaphore_unlock(SHM_CACHE_SEM);
	}
}

/*
 * Returns cache block address specified by block id 
 */
static char *block_address(int blockid)
{
	char *p;

	p = pool_memory_cache_address() +
		blockid *	pool_config->memqcache_cache_block_size;
	return p;
}

/*
 * Returns i th item pointer in block address block
 */
static POOL_CACHE_ITEM_POINTER *item_pointer(char *block, int i)
{
	return (POOL_CACHE_ITEM_POINTER *)(block + sizeof(POOL_CACHE_BLOCK_HEADER) +
									   sizeof(POOL_CACHE_ITEM_POINTER) * i);
}

/*
 * Returns i th item header in block address block
 */
static POOL_CACHE_ITEM_HEADER *item_header(char *block, int i)
{
	POOL_CACHE_ITEM_POINTER *cip;

	cip = item_pointer(block, i);
	return (POOL_CACHE_ITEM_HEADER *)(block + cip->offset);
}

#ifdef SHMEMCACHE_DEBUG
/*
 * Dump shmem cache block
 */
static void dump_shmem_cache(POOL_CACHE_BLOCKID blockid)
{
	POOL_CACHE_BLOCK_HEADER *bh;
	POOL_CACHE_ITEM_POINTER *cip;
	POOL_CACHE_ITEM_HEADER *cih;
	int i;

	bh = (POOL_CACHE_BLOCK_HEADER *)block_address(blockid);
	fprintf(stderr, "shmem: block header(%lu bytes): flags:%x num_items:%d free_bytes:%d\n",
			sizeof(*bh), bh->flags, bh->num_items, bh->free_bytes);
	for (i=0;i<bh->num_items;i++)
	{
		cip = item_pointer((char *)bh, i);
		fprintf(stderr, "shmem: block: %d %d th item pointer(%lu bytes): offset:%d flags:%x\n",
				blockid, i, sizeof(*cip), cip->offset, cip->flags);
		cih = item_header((char *)bh, i);
		fprintf(stderr, "shmem: block: %d %d th item header(%lu bytes): timestamp:%ld length:%d\n",
				blockid, i, sizeof(*cih), cih->timestamp, cih->total_length);
	}
}
#endif

/*
 * SELECT query result array modules
 */
POOL_QUERY_CACHE_ARRAY *pool_create_query_cache_array(void)
{
#define POOL_QUERY_CACHE_ARRAY_ALLOCATE_NUM 128
#define POOL_QUERY_CACHE_ARRAY_HEADER_SIZE (sizeof(int)+sizeof(int))
    MemoryContext oldContext = MemoryContextSwitchTo(ProcessLoopContext);
	size_t size;
	POOL_QUERY_CACHE_ARRAY *p;

	size = POOL_QUERY_CACHE_ARRAY_HEADER_SIZE + POOL_QUERY_CACHE_ARRAY_ALLOCATE_NUM *
		sizeof(POOL_TEMP_QUERY_CACHE *);
	p = palloc(size);
	p->num_caches = 0;
	p->array_size = POOL_QUERY_CACHE_ARRAY_ALLOCATE_NUM;
    MemoryContextSwitchTo(oldContext);
	return p;
}

/*
 * Discard query cache array
 */
void pool_discard_query_cache_array(POOL_QUERY_CACHE_ARRAY *cache_array)
{
	int i;

	if (!cache_array)
		return;

	pool_debug("pool_discard_query_cache_array: num_caches: %d", cache_array->num_caches);

	for (i=0;i<cache_array->num_caches;i++)
	{
		pool_debug("pool_discard_query_cache_array: i: %d cache: %p", i, cache_array->caches[i]);
		pool_discard_temp_query_cache(cache_array->caches[i]);
	}
	pfree(cache_array);
}

/*
 * Add query cache array
 */
static POOL_QUERY_CACHE_ARRAY * pool_add_query_cache_array(POOL_QUERY_CACHE_ARRAY *cache_array, POOL_TEMP_QUERY_CACHE *cache)
{
	size_t size;
	POOL_QUERY_CACHE_ARRAY *cp = cache_array;

	if (!cache_array)
		return cp;

	pool_debug("pool_add_query_cache_array: num_caches: %d cache: %p", cache_array->num_caches, cache);

	if (cache_array->num_caches >= 	cache_array->array_size)
	{
		cache_array->array_size += POOL_QUERY_CACHE_ARRAY_ALLOCATE_NUM;
		size = POOL_QUERY_CACHE_ARRAY_HEADER_SIZE + cache_array->array_size *
			sizeof(POOL_TEMP_QUERY_CACHE *);
		cache_array = repalloc(cache_array, size);
	}
	cache_array->caches[cache_array->num_caches++] = cache;
	return cache_array;
}

/*
 * SELECT query result temporary cache modules
 */

/*
 * Create SELECT result temporary cache
 */
POOL_TEMP_QUERY_CACHE *pool_create_temp_query_cache(char *query)
{
	POOL_TEMP_QUERY_CACHE *p;
    MemoryContext oldContext = MemoryContextSwitchTo(QueryContext);
	p = palloc(sizeof(*p));
    p->query = pstrdup(query);

    p->buffer = pool_create_buffer();
    p->oids = pool_create_buffer();
    p->num_oids = 0;
    p->is_exceeded = false;
    p->is_discarded = false;

    MemoryContextSwitchTo(oldContext);

	return p;
}

/*
 * Discard temp query cache
 */
void pool_discard_temp_query_cache(POOL_TEMP_QUERY_CACHE *temp_cache)
{
	if (!temp_cache)
		return;

	if (temp_cache->query)
		pfree(temp_cache->query);
	if (temp_cache->buffer)
		pool_discard_buffer(temp_cache->buffer);
	if (temp_cache->oids)
		pool_discard_buffer(temp_cache->oids);
	pfree(temp_cache);
}

/*
 * Add data to temp query cache.
 * Data must be FE/BE protocol packet.
 */
static void pool_add_temp_query_cache(POOL_TEMP_QUERY_CACHE *temp_cache, char kind, char *data, int data_len)
{
	POOL_INTERNAL_BUFFER *buffer;
	size_t buflen;
	int send_len;

	if (temp_cache == NULL)
	{
		/* This could happen if cache exceeded in previous query
		 * execution in the same unnamed portal.
		 */
		pool_debug("pool_add_temp_query_cache: POOL_TEMP_QUERY_CACHE is NULL");
		return;
	}

	if (temp_cache->is_exceeded)
	{
		pool_debug("pool_add_temp_query_cache: memqcache_maxcache exceeds");
		return;
	}

	/*
	 * We only store T(Table Description), D(Data row), C(Command Complete),
	 * 1(ParseComplete), 2(BindComplete)
	 */
    if (kind != 'T' && kind != 'D' && kind != 'C' && kind != '1' && kind != '2')
    {
		return;
	}

	/* Check data limit */
	buffer = temp_cache->buffer;
	buflen = pool_get_buffer_length(buffer);

	if ((buflen+data_len+sizeof(int)+1) > pool_config->memqcache_maxcache)
	{
		pool_debug("pool_add_temp_query_cache: data size exceeds memqcache_maxcache. current:%zd requested:%zd memq_maxcache:%d",
				 buflen, data_len+sizeof(int)+1, pool_config->memqcache_maxcache);
		temp_cache->is_exceeded = true;
		return;
	}

	pool_add_buffer(buffer, &kind, 1);
	send_len = htonl(data_len + sizeof(int));
	pool_add_buffer(buffer, (char *)&send_len, sizeof(int));
	pool_add_buffer(buffer, data, data_len);

	return;
}

/*
 * Add table oids used by SELECT to temp query cache.
 */
static void pool_add_oids_temp_query_cache(POOL_TEMP_QUERY_CACHE *temp_cache, int num_oids, int *oids)
{
	POOL_INTERNAL_BUFFER *buffer;

	if (!temp_cache || num_oids <= 0)
		return;

	buffer = temp_cache->oids;
	pool_add_buffer(buffer, oids, num_oids*sizeof(int));
	temp_cache->num_oids = num_oids;
}

/*
 * Internal buffer management modules.
 * Usage:
 * 1) Create buffer using pool_create_buffer().
 * 2) Add data to buffer using pool_add_buffer().
 * 3) Extract (copied) data from buffer using pool_get_buffer().
 * 4) Optionally you can:
 *		Obtain buffer length by using pool_get_buffer_length().
 *		Obtain buffer pointer by using pool_get_buffer_pointer().
 * 5) Discard buffer using pool_discard_buffer().
 */

/*
 * Create and return internal buffer
 */
static POOL_INTERNAL_BUFFER *pool_create_buffer(void)
{
	POOL_INTERNAL_BUFFER *p;
	p = palloc0(sizeof(*p));
	return p;
}

/*
 * Discard internal buffer
 */
static void pool_discard_buffer(POOL_INTERNAL_BUFFER *buffer)
{
	if (buffer)
	{
		if (buffer->buf)
			pfree(buffer->buf);
		pfree(buffer);
	}
}

/*
 * Add data to internal buffer
 */
static void pool_add_buffer(POOL_INTERNAL_BUFFER *buffer, void *data, size_t len)
{
#define POOL_ALLOCATE_UNIT 8192

	/* Sanity check */
	if (!buffer || !data || len == 0)
		return;

	/* Check if we need to increase the buffer size */
	if ((buffer->buflen + len) > buffer->bufsize)
	{
		size_t allocate_size = ((buffer->buflen + len)/POOL_ALLOCATE_UNIT +1)*POOL_ALLOCATE_UNIT;
		pool_debug("pool_add_buffer: realloc old size:%zd new size:%zd",
				   buffer->bufsize, allocate_size);
		buffer->bufsize = allocate_size;
		buffer->buf = (char *)repalloc(buffer->buf, buffer->bufsize);
	}
	/* Add data to buffer */
	memcpy(buffer->buf+buffer->buflen, data, len);
	buffer->buflen += len;

	pool_debug("pool_add_buffer: len:%zd, total:%zd bufsize:%zd",
			   len, buffer->buflen, buffer->bufsize);

	return;
}

/*
 * Get data from internal buffer.
 * Data is stored in newly malloc memory.
 * Data length is returned to len.
 */
static void *pool_get_buffer(POOL_INTERNAL_BUFFER *buffer, size_t *len)
{
	void *p;

	if (buffer->bufsize == 0 || buffer->buflen == 0 ||
		buffer->buf == NULL)
	{
		*len = 0;
		return NULL;
	}

	p = palloc(buffer->buflen);
	memcpy(p, buffer->buf, buffer->buflen);
	*len = buffer->buflen;
	return p;
}

/*
 * Get internal buffer length.
 */
static size_t pool_get_buffer_length(POOL_INTERNAL_BUFFER *buffer)
{
	if (buffer == NULL)
		return 0;

	return buffer->buflen;
}

/*
 * Get internal buffer pointer.
 */
static char *pool_get_buffer_pointer(POOL_INTERNAL_BUFFER *buffer)
{
	if (buffer == NULL)
		return NULL;

	return buffer->buf;
}

/*
 * Get query cache buffer struct of current query context
 */
POOL_TEMP_QUERY_CACHE *pool_get_current_cache(void)
{
	POOL_SESSION_CONTEXT *session_context;
	POOL_QUERY_CONTEXT *query_context;
	POOL_TEMP_QUERY_CACHE *p = NULL;

	session_context = pool_get_session_context(true);
	if (session_context)
	{
		query_context = session_context->query_context;
		if (query_context)
		{
			p = query_context->temp_cache;
		}
	}
	return p;
}

/*
 * Get query cache buffer of current query context
 */
static char *pool_get_current_cache_buffer(size_t *len)
{
	char *p = NULL;
	*len = 0;
	POOL_TEMP_QUERY_CACHE *cache;

	cache = pool_get_current_cache();
	if (cache)
	{
		p = pool_get_buffer(cache->buffer, len);
	}
	return p;
}

/*
 * Mark this temporary query cache buffer discarded if the SELECT
 * uses the table oid specified by oids.
 */
static void pool_check_and_discard_cache_buffer(int num_oids, int *oids)
{
	POOL_SESSION_CONTEXT *session_context;
	POOL_TEMP_QUERY_CACHE *cache;
	int num_caches;
	size_t len;
	int *soids;
	int i, j, k;

	session_context = pool_get_session_context(true);

	if (!session_context || !session_context->query_cache_array)
		return;

	num_caches = session_context->query_cache_array->num_caches;

	for (i=0;i<num_caches;i++)
	{
		cache = session_context->query_cache_array->caches[i];
		if (!cache || cache->is_discarded)
			continue;

		soids = (int *)pool_get_buffer(cache->oids, &len);

		for(j=0;j<cache->num_oids;j++)
		{
			if (cache->is_discarded)
				break;

			for (k=0;k<num_oids;k++)
			{
				if (soids[j] == oids[k])
				{
					pool_debug("pool_check_and_discard_cache_buffer: discard cache for %s",
							   cache->query);
					cache->is_discarded = true;
					break;
				}
			}
		}
		pfree(soids);
	}
}

/*
 * At Ready for Query handle query cache.
 * Supposed to be called from ReadyForQuery().
 */
void pool_handle_query_cache(POOL_CONNECTION_POOL *backend, char *query, Node *node, char state)
{
	POOL_SESSION_CONTEXT *session_context;
	char *cache_buffer;
	size_t len;
	int num_oids;
	int *oids;
	int i;
#ifdef HAVE_SIGPROCMASK
	sigset_t oldmask;
#else
	int	oldmask;
#endif

	session_context = pool_get_session_context(true);

	/* Ok to cache SELECT result? */
	if (pool_is_cache_safe())
	{
		SelectContext ctx;
		MemoryContext old_context;
		old_context = MemoryContextSwitchTo(session_context->memory_context);
		num_oids = pool_extract_table_oids_from_select_stmt(node, &ctx);
		MemoryContextSwitchTo(old_context);
		oids = ctx.table_oids;;
		pool_debug("num_oids: %d oid: %d", num_oids, *oids);

		if (state == 'I')		/* Not inside a transaction? */
		{
			/*
			 * Make sure that temporary cache is not exceeded.
			 */
			if (!pool_is_cache_exceeded())
			{
				/*
				 * If we are not inside a transaction, we can
				 * immediately register to cache storage.
				 */
				/* Register to memcached or shmem */
				POOL_SETMASK2(&BlockSig, &oldmask);
				pool_shmem_lock();

				cache_buffer =  pool_get_current_cache_buffer(&len);
				if (cache_buffer)
				{
					if (pool_commit_cache(backend, query, cache_buffer, len, num_oids, oids) != 0)
					{
						pool_error("ReadyForQuery: pool_commit_cache failed");
					}
					pfree(cache_buffer);
				}
				pool_shmem_unlock();
				POOL_SETMASK(&oldmask);
			}

			/* Count up SELECT stats */
			pool_stats_count_up_num_selects(1);

			/* Reset temp buffer */
			pool_reset_memqcache_buffer();
		}
		else
		{
			POOL_TEMP_QUERY_CACHE *cache = pool_get_current_cache();

			/* In transaction. Keep to temp query cache array */
			pool_add_oids_temp_query_cache(cache, num_oids, oids);

			/* 
			 * If temp cache has been overflowed, just trash the half
			 * baked temp cache.
			 */
			if (pool_is_cache_exceeded())
			{
				POOL_TEMP_QUERY_CACHE *cache;

				cache = pool_get_current_cache();
				pool_discard_temp_query_cache(cache);
				/*
				 * Reset temp_cache pointer in the current query context
				 * so that we don't double free memory.
				 */
				session_context->query_context->temp_cache = NULL;

			}
			/*
			 * Otherwise add to the temp cache array.
			 */
			else
			{
				session_context->query_cache_array = 
					pool_add_query_cache_array(session_context->query_cache_array, cache);
				/*
				 * Reset temp_cache pointer in the current query
				 * context so that we don't add the same temp cache to
				 * the cache array. This is necessary such that case
				 * when next query is just a "bind message", without
				 * "parse message". In the case the query context is
				 * reused and same cache pointer will be added to the
				 * query_cache_array which we do not want.
				 */
				session_context->query_context->temp_cache = NULL;
			}

			/* Count up temporary SELECT stats */
			pool_tmp_stats_count_up_num_selects();
		}
	}
	else if (is_rollback_query(node))	/* Rollback? */
	{
		/* Discard buffered data */
		pool_reset_memqcache_buffer();
	}
	else if (is_commit_query(node))		/* Commit? */
	{
		int num_caches;

		POOL_SETMASK2(&BlockSig, &oldmask);
		pool_shmem_lock();

		/* Invalidate query cache */
		if (pool_config->memqcache_auto_cache_invalidation)
		{
			num_oids = pool_get_dml_table_oid(&oids);
			pool_invalidate_query_cache(num_oids, oids, true, 0);
		}

		/*
		 * If we have something in the query cache buffer, that means
		 * either:
		 * - We only had SELECTs in the transaction
		 * - We had only SELECTs after last DML
		 * Thus we can register SELECT results to cache storage.
		 */
		num_caches = session_context->query_cache_array->num_caches;
		for (i=0;i<num_caches;i++)
		{
			POOL_TEMP_QUERY_CACHE *cache;

			cache = session_context->query_cache_array->caches[i];
			if (!cache || cache->is_discarded)
				continue;

			num_oids = cache->num_oids;
			oids = pool_get_buffer(cache->oids, &len);
			cache_buffer = pool_get_buffer(cache->buffer, &len);
						
			if (pool_commit_cache(backend, cache->query, cache_buffer, len, num_oids, oids) != 0)
			{
				pool_error("ReadyForQuery: pool_commit_cache failed");
			}
			if (oids)
				pfree(oids);
			if (cache_buffer)
				pfree(cache_buffer);
		}
		pool_shmem_unlock();
		POOL_SETMASK(&oldmask);

		/* Count up number of SELECT stats */
		pool_stats_count_up_num_selects(pool_tmp_stats_get_num_selects());

		pool_reset_memqcache_buffer();
	}
	else		/* Non cache safe queries */
	{
		/* Non cachable SELECT */
		if (node && IsA(node, SelectStmt))
		{
			if (state == 'I')
			{
				/* Count up SELECT stats */
				pool_stats_count_up_num_selects(1);
				pool_reset_memqcache_buffer();
			}
			else
			{
				/* Count up temporary SELECT stats */
				pool_tmp_stats_count_up_num_selects();
			}
		}
		/*
		 * If the query is DROP DATABASE, discard both of caches in shmem/memcached and
		 * oidmap in memqcache_oiddir.
		 */
		else if (is_drop_database(node) && session_context->query_context->dboid != 0)
		{
			int dboid = session_context->query_context->dboid;
			num_oids = pool_get_dropdb_table_oids(&oids, dboid);

			if (num_oids > 0 && pool_config->memqcache_auto_cache_invalidation)
			{
				pool_shmem_lock();
				pool_invalidate_query_cache(num_oids, oids, true, dboid);
				pool_discard_oid_maps_by_db(dboid);
				pool_shmem_unlock();
				pool_reset_memqcache_buffer();

				pfree(oids);
				pool_debug("ReadyForQuery: deleted all cache files for the DROPped DB");
			}
		}
		else
		{
			/*
			 * DML/DCL/DDL case
			 */

			/* Extract table oids from buffer */
			num_oids = pool_get_dml_table_oid(&oids);
			if (num_oids > 0 && pool_config->memqcache_auto_cache_invalidation)
			{
				/*
				 * If we are not inside a transaction, we can
				 * immediately invalidate query cache.
				 */
				if (state == 'I')
				{
					POOL_SETMASK2(&BlockSig, &oldmask);
					pool_shmem_lock();
					pool_invalidate_query_cache(num_oids, oids, true, 0);
					pool_shmem_unlock();
					POOL_SETMASK(&oldmask);
					pool_reset_memqcache_buffer();
				}
				else
				{
					/*
					 * If we are inside a transaction, we
					 * cannot invalidate query cache
					 * yet. However we can clear cache buffer,
					 * if DML/DDL modifies the TABLE which SELECT uses.
					 */
					pool_check_and_discard_cache_buffer(num_oids, oids);
				} 
			}
		}
	}
}

/*
 * Create and initialize query cache stats
 */
static POOL_QUERY_CACHE_STATS *stats;
int pool_init_memqcache_stats(void)
{
	stats = pool_shared_memory_create(sizeof(POOL_QUERY_CACHE_STATS));
	pool_reset_memqcache_stats();
	return 0;
}

/*
 * Returns copy of stats area. The copy is in static area and will be
 * overwritten by next call to this function.
 */
POOL_QUERY_CACHE_STATS *pool_get_memqcache_stats(void)
{
	static POOL_QUERY_CACHE_STATS mystats;
#ifdef HAVE_SIGPROCMASK
	sigset_t oldmask;
#else
	int	oldmask;
#endif

	memset(&mystats, 0, sizeof(POOL_QUERY_CACHE_STATS));

	if (stats)
	{
		POOL_SETMASK2(&BlockSig, &oldmask);
		pool_semaphore_lock(QUERY_CACHE_STATS_SEM);
		memcpy(&mystats, stats, sizeof(POOL_QUERY_CACHE_STATS));
		pool_semaphore_unlock(QUERY_CACHE_STATS_SEM);
		POOL_SETMASK(&oldmask);
	}

	return &mystats;
}

/*
 * Reset query cache stats. Caller must lock QUERY_CACHE_STATS_SEM if
 * necessary.
 */
void pool_reset_memqcache_stats(void)
{
	memset(stats, 0, sizeof(POOL_QUERY_CACHE_STATS));
	stats->start_time = time(NULL);
}

/*
 * Count up number of successful SELECTs and returns the number.
 * QUERY_CACHE_STATS_SEM lock is acquired in this function.
 */
long long int pool_stats_count_up_num_selects(long long int num)
{
#ifdef HAVE_SIGPROCMASK
	sigset_t oldmask;
#else
	int	oldmask;
#endif

	POOL_SETMASK2(&BlockSig, &oldmask);
	pool_semaphore_lock(QUERY_CACHE_STATS_SEM);
	stats->num_selects += num;
	pool_semaphore_unlock(QUERY_CACHE_STATS_SEM);
	POOL_SETMASK(&oldmask);
	return stats->num_selects;
}

/*
 * Count up number of successful SELECTs in temporary area and returns
 * the number.
 */
long long int pool_tmp_stats_count_up_num_selects(void)
{
	POOL_SESSION_CONTEXT *session_context;

	session_context = pool_get_session_context(false);
	session_context->num_selects++;
	return 	session_context->num_selects;
}

/*
 * Return number of successful SELECTs in temporary area.
 */
long long int pool_tmp_stats_get_num_selects(void)
{
	POOL_SESSION_CONTEXT *session_context;

	session_context = pool_get_session_context(false);
	return session_context->num_selects;
}

/*
 * Reset number of successful SELECTs in temporary area.
 */
void pool_tmp_stats_reset_num_selects(void)
{
	POOL_SESSION_CONTEXT *session_context;

	session_context = pool_get_session_context(false);
	session_context->num_selects = 0;
}

/*
 * Count up number of SELECTs extracted from cache returns the number.
 * QUERY_CACHE_STATS_SEM lock is acquired in this function.
 */
long long int pool_stats_count_up_num_cache_hits(void)
{
#ifdef HAVE_SIGPROCMASK
	sigset_t oldmask;
#else
	int	oldmask;
#endif

	POOL_SETMASK2(&BlockSig, &oldmask);
	pool_semaphore_lock(QUERY_CACHE_STATS_SEM);
	stats->num_cache_hits++;
	pool_semaphore_unlock(QUERY_CACHE_STATS_SEM);
	POOL_SETMASK(&oldmask);
	return stats->num_cache_hits;
}

/*
 * On shared memory hash table implementation.  We use sub part of md5
 * hash key as hash function.  The experiment has shown that has_any()
 * of PostgreSQL is a little bit better than the method using part of
 * md5 hash value, but it seems adding some cpu cycles to call
 * hash_any() is not worth the trouble.
 */

static volatile POOL_HASH_HEADER *hash_header;
static volatile POOL_HASH_ELEMENT *hash_elements;
static volatile POOL_HASH_ELEMENT hash_free_body;
static volatile POOL_HASH_ELEMENT *hash_free;

/*
 * Initialize hash table on shared memory "nelements" is max number of
 * hash keys. The actual number of hash key is rounded up to power of
 * 2.
 */
#undef POOL_HASH_DEBUG

int pool_hash_init(int nelements)
{
	size_t size;
	int nelements2;		/* number of rounded up hash keys */
	int shift;
	uint32 mask;
	POOL_HASH_HEADER hh;
	int i;

	if (nelements <= 0)
	{
		pool_error("pool_hash_init: invalid nelements:%d", nelements);
		return -1;
	}

	/* Round up to power of 2 */
	shift = 32;
	nelements2 = 1;
	do
	{
		nelements2 <<= 1;
		shift--;
	} while (nelements2 < nelements);

	mask = ~0;
	mask >>= shift;
	size = (char *)&hh.elements - (char *)&hh + sizeof(POOL_HEADER_ELEMENT)*nelements2;
	hash_header = pool_shared_memory_create(size);
	if (hash_header == NULL)
	{
		pool_error("pool_hash_init: failed to allocate shared memory cache for hash header. request size: %zd", size);
		return -1;
	}
	hash_header->nhash = nelements2;
    hash_header->mask = mask;

#ifdef POOL_HASH_DEBUG
	pool_log("pool_hash_init: size:%zd nelements2:%d", size, nelements2);
#endif

	size = sizeof(POOL_HASH_ELEMENT)*nelements2;
	hash_elements = pool_shared_memory_create(size);
	if (hash_elements == NULL)
	{
		pool_error("pool_hash_init: failed to allocate shared memory cache for free list. request size: %zd", size);
		return -1;
	}

#ifdef POOL_HASH_DEBUG
	pool_log("pool_hash_init: size:%zd nelements2:%d", size, nelements2);
#endif

	for (i=0;i<nelements2-1;i++)
	{
		hash_elements[i].next = (POOL_HASH_ELEMENT *)&hash_elements[i+1];
	}
	hash_elements[nelements2-1].next = NULL;
	hash_free = hash_elements;

	return 0;
}

/*
 * Reset hash table on shared memory "nelements" is max number of
 * hash keys. The actual number of hash key is rounded up to power of
 * 2.
 */
static int
pool_hash_reset(int nelements)
{
	size_t size;
	int nelements2;		/* number of rounded up hash keys */
	int shift;
	uint32 mask;
	POOL_HASH_HEADER hh;
	int i;

	if (nelements <= 0)
	{
		pool_error("pool_hash_clear: invalid nelements:%d", nelements);
		return -1;
	}

	/* Round up to power of 2 */
	shift = 32;
	nelements2 = 1;
	do
	{
		nelements2 <<= 1;
		shift--;
	} while (nelements2 < nelements);

	mask = ~0;
	mask >>= shift;

	size = (char *)&hh.elements - (char *)&hh + sizeof(POOL_HEADER_ELEMENT)*nelements2;
	memset((void *)hash_header, 0, size);

	hash_header->nhash = nelements2;
    	hash_header->mask = mask;

	size = sizeof(POOL_HASH_ELEMENT)*nelements2;
	memset((void *)hash_elements, 0, size);

	for (i=0;i<nelements2-1;i++)
	{
		hash_elements[i].next = (POOL_HASH_ELEMENT *)&hash_elements[i+1];
	}
	hash_elements[nelements2-1].next = NULL;
	hash_free = hash_elements;

	return 0;
}

/*
 * Search cacheid by MD5 hash key string
 * If found, returns cache id, otherwise NULL.
 */
POOL_CACHEID *pool_hash_search(POOL_QUERY_HASH *key)
{
	volatile POOL_HASH_ELEMENT *element;

	uint32 hash_key = create_hash_key(key);

	if (hash_key >= hash_header->nhash)
	{
		pool_error("pool_hash_search: invalid hash key: %uld nhash: %ld",
				   hash_key, hash_header->nhash);
		return NULL;
	}

	{
		char md5[POOL_MD5_HASHKEYLEN+1];
		memcpy(md5, key->query_hash, POOL_MD5_HASHKEYLEN);
		md5[POOL_MD5_HASHKEYLEN] = '\0';
#ifdef POOL_HASH_DEBUG
		pool_log("pool_hash_search: hash_key:%d md5:%s", hash_key, md5);
#endif
	}

	element = hash_header->elements[hash_key].element;
	while (element)
	{
		{
			char md5[POOL_MD5_HASHKEYLEN+1];
			memcpy(md5, key->query_hash, POOL_MD5_HASHKEYLEN);
			md5[POOL_MD5_HASHKEYLEN] = '\0';
#ifdef POOL_HASH_DEBUG
			pool_log("pool_hash_search: element md5:%s", md5);
#endif
		}

		if (memcmp((const void *)element->hashkey.query_hash,
				   (const void *)key->query_hash, sizeof(key->query_hash)) == 0)
		{
			return (POOL_CACHEID *)&element->cacheid;
		}
		element = element->next;
	}
	return NULL;
}

/*
 * Insert MD5 key and associated cache id into shmem hash table.  If
 * "update" is true, replace cacheid associated with the MD5 key,
 * rather than throw an error.
 */
static int pool_hash_insert(POOL_QUERY_HASH *key, POOL_CACHEID *cacheid, bool update)
{
	POOL_HASH_ELEMENT *element;
	POOL_HASH_ELEMENT *new_element;

	uint32 hash_key = create_hash_key(key);

	if (hash_key >= hash_header->nhash)
	{
		pool_error("pool_hash_insert: invalid hash key: %uld nhash: %ld",
				   hash_key, hash_header->nhash);
		return -1;
	}

	{
		char md5[POOL_MD5_HASHKEYLEN+1];
		memcpy(md5, key->query_hash, POOL_MD5_HASHKEYLEN);
		md5[POOL_MD5_HASHKEYLEN] = '\0';
#ifdef POOL_HASH_DEBUG
		pool_log("pool_hash_insert: hash_key:%d md5:%s block:%d item:%d", hash_key, md5, cacheid->blockid, cacheid->itemid);
#endif
	}

	/*
	 * Look for hash key.
	 */
	element = hash_header->elements[hash_key].element;

	while (element)
	{
		if (memcmp((const void *)element->hashkey.query_hash,
				   (const void *)key->query_hash, sizeof(key->query_hash)) == 0)
		{
			/* Hash key found. If "update" is false, just throw an error. */
			char md5[POOL_MD5_HASHKEYLEN+1];

			if (!update)
			{
				memcpy(md5, key->query_hash, POOL_MD5_HASHKEYLEN);
				md5[POOL_MD5_HASHKEYLEN] = '\0';
				pool_error("pool_hash_insert: the key:==%s== already exists", md5);
				return -1;
			}
			else
			{
				/* Update cache id */
				memcpy((void *)&element->cacheid, cacheid, sizeof(POOL_CACHEID));
				return 0;
			}
		}
		element = element->next;
	}

	/*
	 * Ok, same key did not exist. Just insert new hash key.
	 */
	new_element = (POOL_HASH_ELEMENT *)get_new_hash_element();
	if (!new_element)
	{
		pool_error("pool_hash_insert: could not get new element");
		return -1;
	}

	element = hash_header->elements[hash_key].element;

	hash_header->elements[hash_key].element = new_element;
	new_element->next = element;

	memcpy((void *)new_element->hashkey.query_hash, key->query_hash, POOL_MD5_HASHKEYLEN);
	memcpy((void *)&new_element->cacheid, cacheid, sizeof(POOL_CACHEID));

	return 0;
}

/*
 * Delete MD5 key and associated cache id into shmem hash table.
 */
int pool_hash_delete(POOL_QUERY_HASH *key)
{
	POOL_HASH_ELEMENT *element;
	POOL_HASH_ELEMENT **delete_point;
	bool found;

	uint32 hash_key = create_hash_key(key);

	if (hash_key >= hash_header->nhash)
	{
		pool_error("pool_hash_delete: invalid hash key: %uld nhash: %ld",
				   hash_key, hash_header->nhash);
		return -1;
	}

	/*
	 * Look for delete location
	 */
	found = false;
	delete_point = (POOL_HASH_ELEMENT **)&(hash_header->elements[hash_key].element);
	element = hash_header->elements[hash_key].element;

	while (element)
	{
		if (memcmp(element->hashkey.query_hash, key->query_hash, sizeof(key->query_hash)) == 0)
		{
			found = true;
			break;
		}
		delete_point = &element->next;
		element = element->next;
	}

	if (!found)
	{
		char md5[POOL_MD5_HASHKEYLEN+1];

		memcpy(md5, key->query_hash, POOL_MD5_HASHKEYLEN);
		md5[POOL_MD5_HASHKEYLEN] = '\0';
		pool_error("pool_hash_delete: the key:==%s== not found", md5);
		return -1;
	}

	/*
	 * Put back the element to free list
	 */
	*delete_point = element->next;
	put_back_hash_element(element);

	return 0;
}

/* 
 * Calculate 32bit binary hash key(i.e. location in hash header) from MD5
 * string. We use top most 8 characters of MD5 string for calculation.
*/
static uint32 create_hash_key(POOL_QUERY_HASH *key)
{
#define POOL_HASH_NCHARS 8

	char md5[POOL_HASH_NCHARS+1];
	uint32 mask;

	memcpy(md5, key->query_hash, POOL_HASH_NCHARS);
	md5[POOL_HASH_NCHARS] = '\0';
	mask = strtoul(md5, NULL, 16);
	mask &= hash_header->mask;
	return mask;
}

/*
 * Get new free hash element from free list.
 */
static volatile POOL_HASH_ELEMENT *get_new_hash_element(void)
{
	volatile POOL_HASH_ELEMENT *elm;

	if (!hash_free->next)
	{
		/* No free element */
		return NULL;
	}

#ifdef POOL_HASH_DEBUG
	pool_log("get_new_hash_element: hash_free->next:%p hash_free->next->next:%p",
			 hash_free->next, hash_free->next->next);
#endif

	elm = hash_free->next;
	hash_free->next = elm->next;

	return elm;
}

/*
 * Put back hash element to free list.
 */
static void put_back_hash_element(volatile POOL_HASH_ELEMENT *element)
{
	POOL_HASH_ELEMENT *elm;

#ifdef POOL_HASH_DEBUG
	pool_log("put_back_hash_element: hash_free->next:%p hash_free->next->next:%p",
			 hash_free->next, hash_free->next->next);
#endif

	elm = hash_free->next;
	hash_free->next = (POOL_HASH_ELEMENT *)element;
	element->next = elm;
}

/*
 * Return true if there's a free hash element.
 */
static bool is_free_hash_element(void)
{
	return hash_free->next != NULL;
}

/*
 * Returns shared memory cache stats.
 * Subsequent call to this function will break return value
 * because its in static memory.
 * Caller must hold shmem_lock before calling this function.
 * If on memory query cache is not enabled, all stats are 0.
 */
POOL_SHMEM_STATS *pool_get_shmem_storage_stats(void)
{
	static POOL_SHMEM_STATS mystats;
	POOL_HASH_ELEMENT *element;
	int nblocks;
	int i;

	memset(&mystats, 0, sizeof(POOL_SHMEM_STATS));

	if (!pool_config-> memory_cache_enabled)
		return &mystats;

	/*
	 * Cop cache hit data
	 */
	mystats.cache_stats.num_selects = stats->num_selects;
	mystats.cache_stats.num_cache_hits = stats->num_cache_hits;

	if (strcmp(pool_config-> memqcache_method, "shmem"))
		return &mystats;

	/* number of total hash entries */
	mystats.num_hash_entries = hash_header->nhash;

	/* number of used hash entries */
	for (i=0;i<hash_header->nhash;i++)
	{
		element = hash_header->elements[i].element;
		while (element)
		{
			mystats.used_hash_entries++;
			element = element->next;
		}
	}

	nblocks = pool_get_memqcache_blocks();

	for (i=0;i<nblocks;i++)
	{
		POOL_CACHE_BLOCK_HEADER *bh;
		POOL_CACHE_ITEM_POINTER *cip;
		char *p = block_address(i);
		bh = (POOL_CACHE_BLOCK_HEADER *)p;
		int j;

		if (bh->flags & POOL_BLOCK_USED)
		{
			for (j=0;j<bh->num_items;j++)
			{
				cip = item_pointer(p, j);
				if (POOL_ITEM_DELETED & cip->flags)
				{
					mystats.fragment_cache_entries_size += item_header(p, j)->total_length;
				}
				else
				{
					/* number of used cache entries */
					mystats.num_cache_entries++;
					/* total size of used cache entries */
					mystats.used_cache_entries_size += (item_header(p, j)->total_length + sizeof(POOL_CACHE_ITEM_POINTER));
				}
			}
			mystats.used_cache_entries_size += sizeof(POOL_CACHE_BLOCK_HEADER);
			/* total size of free(usable) cache entries */
			mystats.free_cache_entries_size += bh->free_bytes;
		}
		else
		{
			mystats.free_cache_entries_size += pool_config->memqcache_cache_block_size;
		}
	}

	/*
	 * Copy POOL_QUERY_CACHE_STATS
	 */
	memcpy(&mystats.cache_stats, stats, sizeof(mystats.cache_stats));

	return &mystats;
}
