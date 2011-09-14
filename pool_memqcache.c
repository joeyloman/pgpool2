/* -*-pgsql-c-*- */
/*
 * pgpool: a language independent connection pool server for PostgreSQL
 * written by Tatsuo Ishii
 *
 * Copyright (c) 2003-2011	PgPool Global Development Group
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

#ifdef USE_MEMCACHED
#include <libmemcached/memcached.h>
#endif

#include "md5.h"
#include "pool_config.h"
#include "pool_stream.h"
#include "pool_proto_modules.h"
#include "pool_memqcache.h"
#include "parser/parsenodes.h"
#include "pool_session_context.h"
#include "pool_relcache.h"
#include "pool_select_walker.h"
#include "pool_stream.h"
#include "pool_proto_modules.h"

#ifdef USE_MEMCACHED
memcached_st *memc;
#endif

static char* encode_key(const char *s, char *buf, POOL_CONNECTION_POOL *backend);
static void dump_cache_data(const char *data, size_t len);
static int pool_commit_cache(POOL_CONNECTION_POOL *backend, char *query, char *data, size_t datalen, int num_oids, int *oids);
static int pool_fetch_cache(POOL_CONNECTION_POOL *backend, const char *query, char **buf, size_t *len);
static int send_cached_messages(POOL_CONNECTION *frontend, const char *qcache, int qcachelen);
static void send_message(POOL_CONNECTION *conn, char kind, int len, const char *data);
static int delete_cache_on_memcached(const char *key);
static int pool_get_dml_table_oid(int **oid);
static void pool_discard_dml_table_oid(void);
static void pool_invalidate_query_cache(int num_table_oids, int *table_oid, bool unlink);
static int pool_get_database_oid(void);
static void pool_add_table_oid_map(POOL_CACHEKEY *cachkey, int num_table_oids, int *table_oids);
static void pool_reset_memqcache_buffer(void);
static POOL_CACHEID *pool_add_item_shmem_cache(POOL_QUERY_HASH *query_hash, char *data, int size);
static POOL_CACHEID *pool_find_item_on_shmem_cache(POOL_QUERY_HASH *query_hash);
static char *pool_get_item_shmem_cache(POOL_QUERY_HASH *query_hash, int *size, int *sts);
static void pool_shmem_lock(void);
static void pool_shmem_unlock(void);
static void pool_add_query_cache_array(POOL_QUERY_CACHE_ARRAY *cache_array, POOL_TEMP_QUERY_CACHE *cache);
static void pool_discard_temp_query_cache(POOL_TEMP_QUERY_CACHE *temp_cache);
static void pool_add_temp_query_cache(POOL_TEMP_QUERY_CACHE *temp_cache, char kind, char *data, int data_len);
static void pool_add_oids_temp_query_cache(POOL_TEMP_QUERY_CACHE *temp_cache, int num_oids, int *oids);
static POOL_INTERNAL_BUFFER *pool_create_buffer(void);
static void pool_discard_buffer(POOL_INTERNAL_BUFFER *buffer);
static void pool_add_buffer(POOL_INTERNAL_BUFFER *buffer, void *data, size_t len);
static void *pool_get_buffer(POOL_INTERNAL_BUFFER *buffer, size_t *len);
static size_t pool_get_buffer_length(POOL_INTERNAL_BUFFER *buffer);
static POOL_TEMP_QUERY_CACHE *pool_get_current_cache(void);
static char *pool_get_current_cache_buffer(size_t *len);
static void pool_check_and_discard_cache_buffer(int num_oids, int *oids);

static void pool_set_memqcache_blocks(int num_blocks);
static int pool_get_memqcache_blocks(void);
static void *pool_memory_cache_address(void);
static void *pool_fsmm_address(void);
static void pool_update_fsmm(POOL_CACHE_BLOCKID blockid, size_t free_space);
static POOL_CACHE_BLOCKID pool_get_block(size_t free_space);
static POOL_CACHE_ITEM_HEADER *pool_cache_item_header(POOL_CACHEID *cacheid);
static int pool_init_cache_block(POOL_CACHE_BLOCKID blockid);
static int pool_delete_item_shmem_cache(POOL_CACHEID *cacheid);
static char *block_address(int blockid);
static POOL_CACHE_ITEM_POINTER *item_pointer(char *block, int i);
static POOL_CACHE_ITEM_HEADER *item_header(char *block, int i);
static POOL_CACHE_BLOCKID pool_reuse_block(void);
static void dump_shmem_cache(POOL_CACHE_BLOCKID blockid);

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
#endif

	/* Already connected? */
	if (memc)
	{
		return 0;
	}

	memqcache_memcached_host = pool_config->memqcache_memcached_host;
	memqcache_memcached_port = pool_config->memqcache_memcached_port;

	pool_debug("memcached_connect : memqcache_memcached_host = %s", memqcache_memcached_host);
	pool_debug("memcached_connect : memqcache_memcached_port = %d", memqcache_memcached_port);

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
	if (!memc)
	{
		return;
	}

#ifdef USE_MEMCACHED
	memcached_free(memc);
#else
	pool_error("memcached_disconnect: memcached support is not enabled");
#endif
}

/*
 * Regist buffer data for query cache on memory cache
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
#ifdef SHMEMCACHE_DEBUG
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
				return -1;
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

	p = malloc(*len);
	if (!p)
	{
		pool_error("pool_fetch_cache: malloc failed");
		return -1;
	}

	memcpy(p, ptr, *len);

	if (!pool_is_shmem_cache())
	{
		free(ptr);
	}

	pool_debug("pool_fetch_cache: query=%s len:%zd", query, *len);
#ifdef SHMEMCACHE_DEBUG
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
	char key[34];
	char* username;
	char* database_name;
	char* strkey;
	int u_length = 0;
	int d_length = 0;
	int q_length = 0;
	int length = 0;

	u_length = strlen(backend->info->user);
	username = (char*)malloc(sizeof(char) * (u_length+1));
	strcpy(username, backend->info->user);

	pool_debug("encode_key: username %s",username);

	d_length = strlen(backend->info->database);
	database_name = (char*)malloc(sizeof(char) * (d_length+1));
	strcpy(database_name, backend->info->database);

	pool_debug("encode_key: database_name %s",database_name);

	q_length = strlen(s);
	length = u_length + d_length + q_length;

	pool_debug("encode_key: username length %d",u_length);
	pool_debug("encode_key: database length %d",d_length);
	pool_debug("encode_key: query length %d",q_length);
	pool_debug("encode_key: length %d",length);

	strkey = (char*)malloc(sizeof(char) * (length+1));

	sprintf(strkey, "%s%s%s", username, s, database_name);

	pool_md5_hash(strkey, strlen(strkey), key);
	pool_debug("encode_key: strkey %s",strkey);
	pool_debug("encode_key: key %s",key);
	strcpy(buf, key);
	pool_debug("encode_key: `%s' -> `%s'", s, buf);
	return buf;
}

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

/*
 * send cached messages
 */
static int send_cached_messages(POOL_CONNECTION *frontend, const char *qcache, int qcachelen)
{
	int msg = 0;
	int i = 0;
	int is_prepared_stmt = 0;

	while (i < qcachelen)
	{
		char tmpkind;
		int tmplen;
		char tmpbuf[MAX_VALUE];

		tmpkind = qcache[i];
		i += 1;

		memcpy(&tmplen, qcache+i, sizeof(tmplen));
		i += sizeof(tmplen);

		tmplen = ntohl(tmplen);

		memcpy(tmpbuf, qcache+i, tmplen - sizeof(tmplen));
		i += tmplen - sizeof(tmplen);

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
		send_message(frontend, tmpkind, tmplen, tmpbuf);

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

/*
 * delete query cache on memcached
 */
static int delete_cache_on_memcached(const char *key)
{
#ifdef USE_MEMCACHED
	memcached_return rc;
#endif

	pool_debug("delete_cache_on_memcached: key: %s", key);

#ifdef USE_MEMCACHED
	/* delete cache data on memcached. key is md5 hash query */
    rc= memcached_delete(memc, key, 32, (time_t)0);

	/* delete cache data on memcached is failed */
    if (rc != MEMCACHED_SUCCESS && rc != MEMCACHED_BUFFERED)
    {
        pool_error ("delete_cache_on_memcached: %s\n", memcached_strerror(memc, rc));
        return 0;
    }
    return 1;
#else
	pool_error("delete_cache_on_memcached: memecahced support is not enabled");
	return 0;
#endif
}


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

	*foundp = false;

	pool_shmem_lock();


	sts = pool_fetch_cache(backend, contents, &qcache, &qcachelen);
	pool_shmem_unlock();

	if (sts == 0)
	{
		/*
		 * Cache found. send each messages to frontend
		 */
		send_cached_messages(frontend, qcache, qcachelen);
		free(qcache);

		/*
		 * send a "READY FOR QUERY"
		 */
		if (MAJOR(backend) == PROTO_MAJOR_V3)
		{
			signed char state;

			state = 'I';

			/* set transaction state */
			MASTER(backend)->tstate = state;
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
	if (query == NULL)
		return false;

	if (pool_config->ignore_leading_white_space)
	{
		/* Ignore leading white spaces */
		while (*query && isspace(*query))
			query++;
	}

	if (!strncasecmp(query, "SELECT", 6) || !strncasecmp(query, "WITH", 4))
	{
		return true;
	}
	return false;
}

/*
 * Return true if SELECT can be cached.
 */
bool pool_is_allow_to_cache(Node *node, char *query)
{
	SelectStmt	*stmt;

	if (!pool_is_likely_select(query))
		return false;

	if (!IsA(node, SelectStmt))
		return false;

	stmt = (SelectStmt *)node;

	/* SELECT INTO or SELECT FOR SHARE or UPDATE cannot be cached */
	if (pool_has_insertinto_or_locking_clause(node))
		return false;

	/*
	 * If SELECT uses non immutable functions or temporary tables
	 * it's not allowed to cache.
	 */
	if (pool_has_non_immutable_function_call(node) ||
		pool_has_temp_table(node))
		return false;

	return true;
}

/*
 * Extract table oid from INSERT/UPDATE/DELETE/TRUNCATE/
 * DROP TABLE/ALTER TABLE statement.
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
		table = nodeToString(stmt->relation);
	}
	else if (IsA(node, DeleteStmt))
	{
		DeleteStmt *stmt = (DeleteStmt *)node;
		table = nodeToString(stmt->relation);
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

#define POOL_OIDBUF_SIZE 1024
static int oidbuf[POOL_OIDBUF_SIZE];
static int oidbufp;

/*
 * Add table oid to internal buffer
 */
void pool_add_dml_table_oid(int oid)
{
	int i;

	if (oid == 0)
		return;

	if (oidbufp >= POOL_OIDBUF_SIZE)
		return;

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
#define DATABASE_TO_OID_QUERY "SELECT oid FROM pg_database WHERE datname = '%s'"

	int oid = 0;
	static POOL_RELCACHE *relcache;
	POOL_CONNECTION_POOL *backend;

	backend = pool_get_session_context()->backend;

	/*
	 * If relcache does not exist, create it.
	 */
	if (!relcache)
	{
		relcache = pool_create_relcache(128, DATABASE_TO_OID_QUERY,
										int_register_func, int_unregister_func,
										true);
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
 * Add cache id(shmem case) or hash key(memcached case) to table oid map file.
 */
static void pool_add_table_oid_map(POOL_CACHEKEY *cachekey, int num_table_oids, int *table_oids)
{
	char *dir;
	int dboid;
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
			pool_error("pool_add_table_oid_map: failed to create %s", dir);
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
		sts = flock(fd, LOCK_EX);
		if (sts == -1)
		{
			pool_error("pool_add_table_oid_map: failed to flock %s. reason:%s",
					   path, strerror(errno));
			close(fd);
			return;
		}

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
 * This is neccessary for shmem case.
 */
void pool_discard_oid_maps(void)
{
	char command[1024];

	if (pool_is_shmem_cache())
	{
		snprintf(command, sizeof(command), "/bin/rm -fr %s/[0-9]*",
				 pool_config->memqcache_oiddir);
		system(command);
	}
}

/*
 * Reading cache id(shmem case) or hash key(memcached case) from table
 * oid map file according to table_oids and discard cache entries.  If
 * unlink is true, the file will be unlinked after successfull cache
 * removal.
 */
static void pool_invalidate_query_cache(int num_table_oids, int *table_oid, bool unlinkp)
{
	char *dir;
	int dboid;
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
	dboid = pool_get_database_oid();

	pool_debug("pool_invalidate_query_cache: dboid %d", dboid);
	if (dboid <= 0)
	{
		pool_error("pool_invalidate_query_cache: could not get database oid");
		return;
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
			return;
		}
		sts = flock(fd, LOCK_EX);
		if (sts == -1)
		{
			pool_error("pool_invalidate_query_cache: failed to flock %s. reason:%s",
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

	session_context = pool_get_session_context();

	if (session_context && session_context->query_cache_array)
	{
		pool_discard_query_cache_array(session_context->query_cache_array);
		session_context->query_cache_array = pool_create_query_cache_array();
	}
	pool_discard_dml_table_oid();
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
 * Remeber memory cache number of blocks.
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

	num_blocks = pool_config->memqcache_total_size/
		pool_config->memqcache_cache_block_size;
	if (num_blocks == 0)
	{
		pool_error("pool_shared_memory_cache_size: wrong memqcache_total_size %d or memqcache_cache_block_size %d",
				   pool_config->memqcache_total_size, pool_config->memqcache_cache_block_size);
		return 0;
	}

	/* Remenber # of blocks */
	pool_set_memqcache_blocks(num_blocks);
	size = pool_config->memqcache_cache_block_size * num_blocks;
	return size;
}

/*
 * Aquire and initialize shared memory cache. This should be called
 * only once from pgpool main process at the process staring up time.
 */
static void *shmem;
int pool_init_memory_cache(size_t size)
{
	pool_debug("pool_init_memory_cache: request size:%zd", size);
	shmem = pool_shared_memory_create(size);
	if (shmem == NULL)
	{
		pool_error("pool_init_memory_cache: failed to allocate shared memory cache. request size: %zd", size);
		return -1;
	}
	return 0;
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
 * block size is 8Kb, number of blocks = 131,072, thus total sizeo of
 * FSMM is 128Kb.  Each FSMM entry has value from 0 to 255. Those
 * valus describes total free space in each block.
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
 */
size_t pool_shared_memory_fsmm_size(void)
{
	size_t size;

	size = pool_get_memqcache_blocks() * sizeof(char);
	return size;
}

/*
 * Aquire and initialize shared memory cache for FSMM. This should be
 * called after pool_shared_memory_cache_size only once from pgpool
 * main process at the proces staring up time.
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
 * Clock algorythm shared query cache management modules.
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
	pool_fsmm_clock_hand = pool_shared_memory_create(sizeof(pool_fsmm_clock_hand));
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

	bh->flags = 0;
	reused_block = *pool_fsmm_clock_hand;
	pool_init_cache_block(reused_block);
	pool_update_fsmm(reused_block, POOL_MAX_FREE_SPACE);

	(*pool_fsmm_clock_hand)++;
	if (*pool_fsmm_clock_hand >= maxblock)
		*pool_fsmm_clock_hand = 0;

	pool_debug("pool_reuse_block: blockid: %d", reused_block);

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

	if (free_space < 0 || free_space > POOL_MAX_FREE_SPACE)
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

	if (blockid < 0 || blockid >= pool_get_memqcache_blocks())
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
 * On successfull registration, returns cache id.
 * The cache id is overwritten by the subsequent call to this function.
 * On error returns NULL.
 */
static POOL_CACHEID *pool_add_item_shmem_cache(POOL_QUERY_HASH *query_hash, char *data, int size)
{
	static POOL_CACHEID cacheid;
	POOL_CACHE_BLOCKID blockid;
	POOL_CACHE_BLOCK_HEADER *bh;
	POOL_CACHE_ITEM_POINTER *cip;
	POOL_CACHE_ITEM_POINTER *cip2;

	POOL_CACHE_ITEM ci;
	POOL_CACHE_ITEM_POINTER cip_body;
	char *item;

	int request_size;
	char *p;
	int i, j;
	char *src;
	char *dst;
	int movelen;
	int num_deleted = 0;
	char *flags;

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
	 * Initialize the block if neccessary. If no live items are
	 * remained, we also initialize the block. If there's contiguous
	 * deleted items, we turn them into free space as well.
	 */
	pool_init_cache_block(blockid);

	/* Get block address on shmem */
	p = pool_memory_cache_address() + blockid *	pool_config->memqcache_cache_block_size;
	bh = (POOL_CACHE_BLOCK_HEADER *)p;

	/*
	 * Create contiguous free space. We assume that item bodies are
	 * ordered from bottom to top of the block, and corresponding item
	 * pointers are ordered from the youngest to the oldest in the
	 * begging of the block.
	 */

	/* First, pack cache items */
	for (i=0;i<bh->num_items;i++)
	{
		int total_length;

		cip = item_pointer(p, i);

		if (POOL_ITEM_DELETED & cip->flags)		/* Deleted item? */
		{
			/* Pack the unsed space */
			src = p + item_pointer(p, bh->num_items-1)->offset;
			movelen = cip->offset - item_pointer(p, bh->num_items-1)->offset;
			total_length = item_header(p, i)->total_length;
			dst = src + total_length;
			pool_debug("i:%d src:%lu dst:%lu movelen:%d", i, (unsigned long int)src, (unsigned long int)dst, movelen);
			memmove(dst, src, movelen);

			for (j=i+1;j<bh->num_items;j++)
			{
				/* Rewrite pointer to cache item */
				cip2 = item_pointer(p, j);
				cip2->offset += total_length;
			}
		}
	}

	/* Second, pack cache item pointers */
	flags = malloc(bh->num_items);
	if (flags == NULL)
	{
		pool_error("pool_add_item_shmem_cache: malloc failed");
		return NULL;
	}

	for (i=0;i<bh->num_items;i++)
	{
		flags[i] = item_pointer(p, i)->flags;
	}

	cip = item_pointer(p, 0);

	for (i=0;i<bh->num_items;i++)
	{
		if (POOL_ITEM_DELETED & flags[i])		/* Deleted item? */
		{
			num_deleted++;

			movelen = sizeof(POOL_CACHE_ITEM_POINTER)*(bh->num_items - i -1);

			if (movelen <= 0)
				break;

			src = (char *)(cip+1);
			dst = (char *)cip;
			memmove(dst, src, movelen);
			pool_debug("i:%d src:%lu dst:%lu movelen:%d num_deleted:%d", i, (unsigned long int)src, (unsigned long int)dst, movelen, num_deleted);
		}
		movelen -= sizeof(POOL_CACHE_ITEM_POINTER);
		cip++;
	}

	free(flags);

	/* All items deleted? */
	if (num_deleted > 0 && num_deleted == bh->num_items)
	{
		pool_debug("all items deleted num_deleted:%d", num_deleted);
		bh->flags = 0;
		pool_init_cache_block(blockid);
		pool_update_fsmm(blockid, POOL_MAX_FREE_SPACE);
	}
	else
	{
		bh->num_items -= num_deleted;
	}

	/*
	 * Make sure that we have enough free space
	 */
	if (bh->free_bytes < request_size)
	{
		/* This shoud not happen */
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
	pool_debug("pool_add_item_shmem_cache: blockid: %d itemid:%d", 
			   cacheid.blockid, cacheid.itemid);

	/* Add up number of items */
	bh->num_items++;

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
	POOL_CACHE_BLOCK_HEADER *bh;
	POOL_CACHE_ITEM_POINTER *cip;
	POOL_CACHE_ITEM_HEADER *cih;
	time_t now;
	int maxblock = pool_get_memqcache_blocks();
	char *p;
	int i, j;

	/* XXX Sequential scan. Need hash table for faster access */
	for (i=0;i<maxblock;i++)
	{
		p = block_address(i);
		bh = (POOL_CACHE_BLOCK_HEADER *)p;
		if (!(bh->flags & POOL_BLOCK_USED))
		{
			/* Unused block */
			continue;
		}

		for (j=0;j<bh->num_items;j++)
		{
			cip = item_pointer(p, j);

			if (!(cip->flags & POOL_ITEM_USED) || cip->flags & POOL_ITEM_DELETED)
			{
				/* Item not used */
				continue;
			}

			if (!memcmp(cip->query_hash.query_hash, query_hash, sizeof(POOL_QUERY_HASH)))
			{
				cih = item_header(p, j);
				pool_debug("pool_find_item_on_shmem_cache: found in cache blockid:%d itemid:%d",
						   i, j);
				/* Check cache expiration */
				now = time(NULL);
				if (now > (cih->timestamp + pool_config->memqcache_expire))
				{
					pool_debug("pool_find_item_on_shmem_cache: cache expired");
					pool_debug("pool_find_item_on_shmem_cache: now: %ld timestamp: %ld",
							   now, cih->timestamp + pool_config->memqcache_expire);
					cacheid.blockid = i;
					cacheid.itemid = j;
					pool_delete_item_shmem_cache(&cacheid);
					return NULL;
				}

				cacheid.blockid = i;
				cacheid.itemid = j;
				return &cacheid;
			}
		}
	}
	return NULL;
}

/*
 * Delete item data specifed cache id from shmem.
 * On sccessfull deletion, returns 0.
 * Other wise return -1.
 * FSMM is also updated.
 */
static int pool_delete_item_shmem_cache(POOL_CACHEID *cacheid)
{
	POOL_CACHE_BLOCK_HEADER *bh;
	POOL_CACHE_ITEM_POINTER *cip;
	POOL_CACHE_ITEM_HEADER *cih;
	int size;

	pool_debug("pool_delete_item_shmem_cache: cacheid:%d itemid:%d",
			   cacheid->blockid, cacheid->itemid);

	if (cacheid->blockid < 0 || cacheid->blockid >= pool_get_memqcache_blocks())
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

	cih = pool_cache_item_header(cacheid);
	size = cih->total_length + sizeof(POOL_CACHE_ITEM_POINTER);

	/* Delete item pointer */
	cip->flags |= POOL_ITEM_DELETED;

	bh->free_bytes += size;
	pool_debug("pool_delete_item_shmem_cache: after deleting %d bytes, free_bytes is %d",
			   size, bh->free_bytes);

	/*
	 * We do NOT count down bh->num_items here. The deleted space will be recycled
	 * by pool_add_item_shmem_cache(). However, if this is the last item, we can
	 * recyle whole block.
	 */
	if ((bh->num_items -1) == 0)
	{
		pool_debug("pool_delete_item_shmem_cache: no item remains. So initialize block");
		bh->flags = 0;
		pool_init_cache_block(cacheid->blockid);
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

	if (cacheid->blockid < 0 || cacheid->blockid >= pool_get_memqcache_blocks())
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
 * Initialize the block if neccessary. If no live items are
 * remained, we also initialize the block. If there's contiguous
 * deleted items, we turn them into free space as well.
 */
static int pool_init_cache_block(POOL_CACHE_BLOCKID blockid)
{
	char *p;
	POOL_CACHE_BLOCK_HEADER *bh;
	POOL_CACHE_ITEM_POINTER *cip;
	int i;
	int contiguous;

	if (blockid < 0 || blockid >= pool_get_memqcache_blocks())
	{
		pool_error("pool_init_cache_block: invalid block id %d", blockid);
		return -1;
	}

	p = pool_memory_cache_address() + blockid *	pool_config->memqcache_cache_block_size;
	bh = (POOL_CACHE_BLOCK_HEADER *)p;

	/* Is this block used? */
	if (!(bh->flags & POOL_BLOCK_USED))
	{
		/* Initialize empty block */
		memset(p, 0, pool_config->memqcache_cache_block_size);
		bh->free_bytes = pool_config->memqcache_cache_block_size -
			sizeof(POOL_CACHE_BLOCK_HEADER);
	}

	/* Find contiguous deleted items and turn them into free space */
	contiguous = -1;
	for (i=0;i<bh->num_items;i++)
	{
		cip = (POOL_CACHE_ITEM_POINTER *)(p + sizeof(POOL_CACHE_BLOCK_HEADER) +
										  sizeof(POOL_CACHE_ITEM_POINTER) * i);

		if (cip->flags & POOL_ITEM_USED)
		{
			contiguous = -1;
		}
		else
		{
			if (contiguous == -1)
			{
				contiguous = i;
			}
		}
	}

	if (contiguous >= 0)
	{
		/* There are contiguous deleted items starting from
		 * "contiguous".
		 */
		for (i=contiguous;i<bh->num_items;i++)
		{
			cip = (POOL_CACHE_ITEM_POINTER *)(p + sizeof(POOL_CACHE_BLOCK_HEADER) +
											  sizeof(POOL_CACHE_ITEM_POINTER) * i);
			memset(cip, 0, sizeof(POOL_CACHE_ITEM_POINTER));
		}
	}
	return 0;
}

/*
 * Aquire lock: XXX giant lock
 */
static void pool_shmem_lock(void)
{
	if (pool_is_shmem_cache())
		pool_semaphore_lock(SHM_CACHE_SEM);
}

/*
 * Release lock
 */
static void pool_shmem_unlock(void)
{
	if (pool_is_shmem_cache())
		pool_semaphore_unlock(SHM_CACHE_SEM);
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

/*
 * SELECT query result array modules
 */
POOL_QUERY_CACHE_ARRAY *pool_create_query_cache_array(void)
{
#define POOL_QUERY_CACHE_ARRAY_ALLOCATE_NUM 128
	size_t size;
	POOL_QUERY_CACHE_ARRAY *p;

	size = sizeof(int) + POOL_QUERY_CACHE_ARRAY_ALLOCATE_NUM *
		sizeof(POOL_TEMP_QUERY_CACHE *);
	p = malloc(size);
	if (!p)
	{
		pool_error("pool_create_query_cache_array: malloc failed");
		return p;
	}
	p->num_caches = 0;
	p->array_size = POOL_QUERY_CACHE_ARRAY_ALLOCATE_NUM;
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

	for (i=0;i<cache_array->num_caches;i++)
	{
		pool_discard_temp_query_cache(cache_array->caches[i]);
	}
	free(cache_array);
}

/*
 * Add query cache array
 */
static void pool_add_query_cache_array(POOL_QUERY_CACHE_ARRAY *cache_array, POOL_TEMP_QUERY_CACHE *cache)
{
	size_t size;

	if (!cache_array)
		return;

	if (cache_array->num_caches >= 	cache_array->array_size)
	{
		cache_array->array_size += POOL_QUERY_CACHE_ARRAY_ALLOCATE_NUM;
		size = cache_array->array_size + sizeof(int) + cache_array->array_size *
			sizeof(POOL_TEMP_QUERY_CACHE *);
		cache_array = realloc(cache_array, size);
		if (!cache_array)
		{
			pool_error("pool_add_query_cache_array: malloc failed");
			return;
		}
	}
	cache_array->caches[cache_array->num_caches++] = cache;
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

	p = malloc(sizeof(*p));
	if (p)
	{
		p->query = strdup(query);
		if (!p->query)
		{
			pool_error("pool_create_temp_query_cache: strdup failed");
			return NULL;
		}
		p->buffer = pool_create_buffer();
		if (!p->buffer)
		{
			free(p->query);
			free(p);
			return NULL;
		}
		p->oids = pool_create_buffer();
		if (!p->oids)
		{
			free(p->query);
			free(p->buffer);
			free(p);
			return NULL;
		}
		p->num_oids = 0;
		p->is_exceeded = false;
		p->is_discarded = false;
	}
	return p;
}

/*
 * Discard temp query cache
 */
static void pool_discard_temp_query_cache(POOL_TEMP_QUERY_CACHE *temp_cache)
{
	if (!temp_cache)
		return;

	if (temp_cache->query)
		free(temp_cache->query);
	if (temp_cache->buffer)
		pool_discard_buffer(temp_cache->buffer);
	if (temp_cache->oids)
		pool_discard_buffer(temp_cache->oids);
	free(temp_cache);
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
		pool_error("pool_add_temp_query_cache: POOL_TEMP_QUERY_CACHE is NULL");
		return;
	}

	if (temp_cache->is_exceeded)
	{
		pool_debug("pool_add_temp_query_cache: memqcache_maxcache exceeds");
		return;
	}

	/*
	 * We only store T(Table Description), D(Data row), C(Command
	 * Complete)
	 */
    if (kind != 'T' && kind != 'D' && kind != 'C')
    {
		return;
	}

	/* Check data limit */
	buffer = temp_cache->buffer;
	buflen = pool_get_buffer_length(buffer);

	if ((buflen+data_len+sizeof(int)+1) > pool_config->memqcache_maxcache)
	{
		pool_log("pool_add_temp_query_cache: data size exceeds memqcache_maxcache. current:%zd requested:%zd memq_maxcache:%d",
				 buflen, data_len+sizeof(int)+1, pool_config->memqcache_maxcache);
		temp_cache->is_exceeded = true;
		return;
	}

	pool_add_buffer(buffer, &kind, 1);
	send_len = htonl(data_len + sizeof(int));
	pool_add_buffer(buffer, (char *)&send_len, sizeof(int));
	pool_add_buffer(buffer, data, data_len);
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
 * Internal buffer management modules
 */

/*
 * Create and return internal buffer
 */
static POOL_INTERNAL_BUFFER *pool_create_buffer(void)
{
	POOL_INTERNAL_BUFFER *p;
	p = malloc(sizeof(*p));
	if (p)
	{
		memset(p, 0, sizeof(*p));
	}
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
			free(buffer->buf);
	}
	free(buffer);
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
		buffer->buf = (char *)realloc(buffer->buf, buffer->bufsize);
		if (!buffer->buf)
		{
			pool_error("pool_add_buffer: realloc failed(request size:%zd",
					   buffer->bufsize);
			return;
		}
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

	p = malloc(buffer->buflen);
	if (!p)
	{
		pool_error("pool_get_buffer: malloc failed. Request size:%zd", buffer->buflen);
		*len = 0;
		return NULL;
	}
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
 * Get query cache buffer struct of current query context
 */
static POOL_TEMP_QUERY_CACHE *pool_get_current_cache(void)
{
	POOL_SESSION_CONTEXT *session_context;
	POOL_QUERY_CONTEXT *query_context;
	POOL_TEMP_QUERY_CACHE *p = NULL;

	session_context = pool_get_session_context();
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
 * uses the table oid specied by oids.
 */
static void pool_check_and_discard_cache_buffer(int num_oids, int *oids)
{
	POOL_SESSION_CONTEXT *session_context;
	POOL_TEMP_QUERY_CACHE *cache;
	int num_caches;
	size_t len;
	int *soids;
	int i, j, k;

	session_context = pool_get_session_context();

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
		free(soids);
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

	session_context = pool_get_session_context();

	/* Ok to cache SELECT result? */
	if (pool_is_cache_safe())
	{
		num_oids = pool_extract_table_oids_from_select_stmt(node, &oids);
		pool_debug("num_oids: %d oid: %d", num_oids, *oids);

		if (state == 'I')		/* Not inside a transaction? */
		{
			/*
			 * If we are not inside a transaction, we can
			 * immediately register to cache storage.
			 */
			/* Register to memcached or shmem */
			pool_shmem_lock();

			cache_buffer =  pool_get_current_cache_buffer(&len);
			if (cache_buffer)
			{
				if (pool_commit_cache(backend, query, cache_buffer, len, num_oids, oids) != 0)
				{
					pool_error("ReadyForQuery: pool_commit_cache failed");
				}
				free(cache_buffer);
			}
			pool_shmem_unlock();
			pool_reset_memqcache_buffer();
		}
		else
		{
			POOL_TEMP_QUERY_CACHE *cache = pool_get_current_cache();

			/* In transaction. Keep to temp query cache array */
			pool_add_oids_temp_query_cache(cache, num_oids, oids);
			pool_add_query_cache_array(session_context->query_cache_array, cache);
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

		/* Invalidate query cache */
		num_oids = pool_get_dml_table_oid(&oids);
		pool_shmem_lock();
		pool_invalidate_query_cache(num_oids, oids, true);

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
			free(oids);
			free(cache_buffer);
		}
		pool_shmem_unlock();
		pool_reset_memqcache_buffer();
	}
	else		/* Non cache safe queries */
	{
		/*
		 * DML/DCL/DDL/non cache safe SELECT case
		 */

		/* Extract table oids from buffer */
		num_oids = pool_get_dml_table_oid(&oids);
		if (num_oids > 0)
		{
			/*
			 * If we are not inside a transaction, we can
			 * immediately invalidate query cache.
			 */
			if (state == 'I')
			{
				pool_shmem_lock();
				pool_invalidate_query_cache(num_oids, oids, true);
				pool_shmem_unlock();
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