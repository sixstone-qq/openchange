/*
   MAPI Proxy - Named properties backend MySQL implementation

   OpenChange Project

   Copyright (C) Jesús García Sáez 2014

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "namedprops_mysql.h"
#include "../mapistore.h"
#include "../mapistore_private.h"
#include "mapiproxy/util/mysql.h"
#include "mapiproxy/util/schema_migration.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <mysql/mysql.h>
#include <mysql/mysqld_error.h>
#include <ldb.h>


static enum mapistore_error get_mapped_id(struct namedprops_context *self,
					  struct MAPINAMEID nameid,
					  uint16_t *mapped_id)
{
	TALLOC_CTX *mem_ctx = talloc_zero(NULL, TALLOC_CTX);
	int type = nameid.ulKind;
	char *guid = GUID_string(mem_ctx, &nameid.lpguid);
	MYSQL *conn = self->data;

	char *sql = NULL;
	if (type == MNID_ID) {
		uint32_t prop_id = nameid.kind.lid;
		sql = talloc_asprintf(mem_ctx,
			"SELECT mappedId FROM "NAMEDPROPS_MYSQL_TABLE" "
			"WHERE `type`=%d AND `oleguid`='%s' AND `propId`=%d",
			type, guid, prop_id);
	} else if (type == MNID_STRING) {
		const char *prop_name = nameid.kind.lpwstr.Name;
		sql = talloc_asprintf(mem_ctx,
			"SELECT mappedId FROM "NAMEDPROPS_MYSQL_TABLE" "
			"WHERE `type`=%d AND `oleguid`='%s' AND `propName`='%s'",
			type, guid, prop_name);
	} else {
		MAPISTORE_RETVAL_IF(true, MAPISTORE_ERROR, mem_ctx);
	}

	if (mysql_query(conn, sql) != 0) {
		MAPISTORE_RETVAL_IF(true, MAPISTORE_ERR_DATABASE_OPS, mem_ctx);
	}

	MYSQL_RES *res = mysql_store_result(conn);
	if (mysql_num_rows(res) == 0) {
		// Not found
		mysql_free_result(res);
		MAPISTORE_RETVAL_IF(true, MAPISTORE_ERR_NOT_FOUND, mem_ctx);
	}
	MYSQL_ROW row = mysql_fetch_row(res);
	*mapped_id = strtol(row[0], NULL, 10);
	mysql_free_result(res);

	talloc_free(mem_ctx);
	return MAPISTORE_SUCCESS;
}


/**
   \details Return the next unused namedprops ID

   \param nprops pointer to the namedprops creontext
   \param highest_id pointer to the next ID to return

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE error
 */
static enum mapistore_error next_unused_id(struct namedprops_context *nprops,
					   uint16_t *highest_id)
{
	TALLOC_CTX	*mem_ctx;
	MYSQL		*conn;
	MYSQL_RES	*res;
	MYSQL_ROW	row;
	char		*sql_query;
	int		ret;

	/* Sanity checks */
	MAPISTORE_RETVAL_IF(!nprops, MAPISTORE_ERR_INVALID_PARAMETER, NULL);
	MAPISTORE_RETVAL_IF(!highest_id, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	conn = (MYSQL *) nprops->data;
	MAPISTORE_RETVAL_IF(!conn, MAPISTORE_ERR_DATABASE_OPS, NULL);

	mem_ctx = talloc_named(NULL, 0, "next_unused_id");
	MAPISTORE_RETVAL_IF(!mem_ctx, MAPISTORE_ERR_NO_MEMORY, NULL);

	sql_query = talloc_asprintf(mem_ctx, "SELECT max(mappedId) FROM %s", NAMEDPROPS_MYSQL_TABLE);
	MAPISTORE_RETVAL_IF(!sql_query, MAPISTORE_ERR_NO_MEMORY, mem_ctx);

	ret = mysql_query(conn, sql_query);
	talloc_free(sql_query);
	MAPISTORE_RETVAL_IF(ret, MAPISTORE_ERR_DATABASE_OPS, mem_ctx);

	res = mysql_store_result(conn);
	MAPISTORE_RETVAL_IF(!res, MAPISTORE_ERR_DATABASE_OPS, mem_ctx);

	row = mysql_fetch_row(res);
	if (!row || !row[0]) {
		mysql_free_result(res);
		mapistore_set_errno(MAPISTORE_ERR_DATABASE_OPS);
		talloc_free(mem_ctx);
		return MAPISTORE_ERR_DATABASE_OPS;
	}

	*highest_id = strtol(row[0], NULL, 10);

	mysql_free_result(res);
	talloc_free(mem_ctx);

	*highest_id = *highest_id + 1;
	return MAPISTORE_SUCCESS;
}


static enum mapistore_error create_id(struct namedprops_context *self,
				      struct MAPINAMEID nameid,
				      uint16_t mapped_id)
{
	TALLOC_CTX *mem_ctx = talloc_zero(NULL, TALLOC_CTX);
	const char **fields = (const char **) str_list_make_empty(mem_ctx);

	fields = str_list_add(fields, talloc_asprintf(mem_ctx, "type=%d",
						      nameid.ulKind));
	fields = str_list_add(fields, talloc_asprintf(mem_ctx, "propType=%d",
						      PT_NULL));
	char *guid = GUID_string(mem_ctx, &nameid.lpguid);
	fields = str_list_add(fields, talloc_asprintf(mem_ctx, "oleguid='%s'",
						      guid));
	fields = str_list_add(fields, talloc_asprintf(mem_ctx, "mappedId=%u",
						      mapped_id));
	if (nameid.ulKind == MNID_ID) {
		fields = str_list_add(fields,
				      talloc_asprintf(mem_ctx, "propId=%u",
						      nameid.kind.lid));
	} else if (nameid.ulKind == MNID_STRING) {
		fields = str_list_add(fields,
				      talloc_asprintf(mem_ctx, "propName='%s'",
						      nameid.kind.lpwstr.Name));
	} else {
		MAPISTORE_RETVAL_IF(true, MAPISTORE_ERROR, mem_ctx);
	}

	char *fields_sql = str_list_join(mem_ctx, fields, ',');
	char *sql = talloc_asprintf(mem_ctx,
		"INSERT INTO " NAMEDPROPS_MYSQL_TABLE " SET %s", fields_sql);
	OC_DEBUG(5, "Inserting record:\n%s\n", sql);
	MYSQL *conn = self->data;
	if (mysql_query(conn, sql) != 0) {
		MAPISTORE_RETVAL_IF(true, MAPISTORE_ERR_DATABASE_OPS, mem_ctx);
	}

	talloc_free(mem_ctx);
	return MAPISTORE_SUCCESS;
}

static enum mapistore_error get_nameid(struct namedprops_context *self,
				       uint16_t mapped_id,
				       TALLOC_CTX *mem_ctx,
				       struct MAPINAMEID **nameidp)
{
	TALLOC_CTX *local_mem_ctx = talloc_zero(NULL, TALLOC_CTX);
	MYSQL *conn = self->data;
	const char *sql = talloc_asprintf(local_mem_ctx,
		"SELECT type, oleguid, propName, propId FROM "NAMEDPROPS_MYSQL_TABLE" "
		"WHERE mappedId=%d", mapped_id);
	if (mysql_query(conn, sql) != 0) {
		MAPISTORE_RETVAL_IF(true, MAPISTORE_ERR_DATABASE_OPS,
				    local_mem_ctx);
	}
	MYSQL_RES *res = mysql_store_result(conn);
	if (mysql_num_rows(res) == 0) {
		// Not found
		mysql_free_result(res);
		MAPISTORE_RETVAL_IF(true, MAPISTORE_ERR_NOT_FOUND,
				    local_mem_ctx);
	}
	MYSQL_ROW row = mysql_fetch_row(res);

	enum mapistore_error ret = MAPISTORE_SUCCESS;
	struct MAPINAMEID *nameid = talloc_zero(mem_ctx, struct MAPINAMEID);
	const char *guid = row[1];
	GUID_from_string(guid, &nameid->lpguid);
	int type = strtol(row[0], NULL, 10);
	nameid->ulKind = type;
	if (type == MNID_ID) {
		nameid->kind.lid = strtol(row[3], NULL, 10);
	} else if (type == MNID_STRING) {
		const char *propName = row[2];
		nameid->kind.lpwstr.NameSize = strlen(propName) * 2 + 2;//FIXME WHY *2+2 and not just +1?
		nameid->kind.lpwstr.Name = talloc_strdup(nameid, propName);
	} else {
		nameid = NULL;
		ret = MAPISTORE_ERROR;
	}

	*nameidp = nameid;

	mysql_free_result(res);
	talloc_free(local_mem_ctx);

	return ret;
}

static enum mapistore_error get_nameid_type(struct namedprops_context *self,
					    uint16_t mapped_id,
					    uint16_t *prop_type)
{
	TALLOC_CTX *mem_ctx = talloc_zero(NULL, TALLOC_CTX);
	MYSQL *conn = self->data;
	const char *sql = talloc_asprintf(mem_ctx,
		//FIXME mappedId or propId? mappedId is not unique
		"SELECT propType FROM "NAMEDPROPS_MYSQL_TABLE" WHERE mappedId=%d",
		mapped_id);
	if (mysql_query(conn, sql) != 0) {
		MAPISTORE_RETVAL_IF(true, MAPISTORE_ERR_DATABASE_OPS, mem_ctx);
	}
	MYSQL_RES *res = mysql_store_result(conn);
	if (mysql_num_rows(res) == 0) {
		// Not found
		mysql_free_result(res);
		MAPISTORE_RETVAL_IF(true, MAPISTORE_ERR_NOT_FOUND, mem_ctx);
	}
	MYSQL_ROW row = mysql_fetch_row(res);
	*prop_type = strtol(row[0], NULL, 10);
	mysql_free_result(res);
	talloc_free(mem_ctx);
	return MAPISTORE_SUCCESS;
}

static enum mapistore_error transaction_start(struct namedprops_context *self)
{
	MYSQL *conn = self->data;
	int res = mysql_query(conn, "START TRANSACTION");
	MAPISTORE_RETVAL_IF(res, MAPISTORE_ERR_DATABASE_OPS, NULL);
	return MAPISTORE_SUCCESS;
}

static enum mapistore_error transaction_commit(struct namedprops_context *self)
{
	MYSQL *conn = self->data;
	int res = mysql_query(conn, "COMMIT");
	MAPISTORE_RETVAL_IF(res, MAPISTORE_ERR_DATABASE_OPS, NULL);
	return MAPISTORE_SUCCESS;
}

static int mapistore_namedprops_mysql_destructor(struct namedprops_context *self)
{
	OC_DEBUG(5, "Destroying namedprops mysql context\n");
	if (self && self->data) {
		MYSQL *conn = self->data;
		release_connection(conn);
	} else {
		OC_DEBUG(0, "Error: tried to destroy corrupted namedprops mysql context\n");
	}
	return 0;
}

/**
   \details Retrieve MySQL backend parametric options from
   configuration file and store them into a data structure.

   \param lp_ctx Pointer to the loadparm context
   \param p pointer to the structure with individual
   parameters to return

   \return MAPISTORE_SUCCES on success, otherwise MAPISTORE error
 */
enum mapistore_error mapistore_namedprops_mysql_parameters(struct loadparm_context *lp_ctx,
							   struct namedprops_mysql_params *p)
{
	/* Sanity checks */
	MAPISTORE_RETVAL_IF(!lp_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);
	MAPISTORE_RETVAL_IF(!p, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Retrieve parametric options */
	p->data = lpcfg_parm_string(lp_ctx, NULL, "namedproperties", "mysql_data");
	p->sock = lpcfg_parm_string(lp_ctx, NULL, "namedproperties", "mysql_sock");
	p->user = lpcfg_parm_string(lp_ctx, NULL, "namedproperties", "mysql_user");
	p->pass = lpcfg_parm_string(lp_ctx, NULL, "namedproperties", "mysql_pass");
	p->host = lpcfg_parm_string(lp_ctx, NULL, "namedproperties", "mysql_host");
	p->port = lpcfg_parm_int(lp_ctx, NULL, "namedproperties", "mysql_port", 3306);
	p->db = lpcfg_parm_string(lp_ctx, NULL, "namedproperties", "mysql_db");

	/* Enforce the logic */
	MAPISTORE_RETVAL_IF(!p->user, MAPISTORE_ERR_BACKEND_INIT, NULL);
	MAPISTORE_RETVAL_IF(!p->db, MAPISTORE_ERR_BACKEND_INIT, NULL);
	MAPISTORE_RETVAL_IF(!p->host && !p->sock, MAPISTORE_ERR_BACKEND_INIT, NULL);

	return MAPISTORE_SUCCESS;
}

static char *connection_string_from_parameters(TALLOC_CTX *mem_ctx, struct namedprops_mysql_params *parms)
{
	char *connection_string;

	connection_string = talloc_asprintf(mem_ctx, "mysql://%s", parms->user);
	if (!connection_string) return NULL;
	if (parms->pass && parms->pass[0]) {
		connection_string = talloc_asprintf_append(connection_string, ":%s", parms->pass);
		if (!connection_string) return NULL;
	}
	connection_string = talloc_asprintf_append(connection_string, "@%s", parms->host);
	if (!connection_string) return NULL;
	if (parms->port) {
		connection_string = talloc_asprintf_append(connection_string, ":%d", parms->port);
		if (!connection_string) return NULL;
	}
	return talloc_asprintf_append(connection_string, "/%s", parms->db);
}

/**
   \details Initialize mapistore named properties MySQL backend

   \param mem_ctx pointer to the memory context
   \param lp_ctx pointer to the loadparm context
   \param nprops_ctx pointer on pointer to the namedprops context to
   return

   \return MAPISTORE_SUCCESS on success, otherwise MAPISTORE error
 */
enum mapistore_error mapistore_namedprops_mysql_init(TALLOC_CTX *mem_ctx,
						     struct loadparm_context *lp_ctx,
						     struct namedprops_context **nprops_ctx)
{
	enum mapistore_error		retval;
	struct namedprops_context	*nprops = NULL;
	struct namedprops_mysql_params	parms;
	MYSQL				*conn = NULL;
	char				*connection_string = NULL;
	int				schema_created_ret;

	/* Sanity checks */
	MAPISTORE_RETVAL_IF(!lp_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);
	MAPISTORE_RETVAL_IF(!nprops_ctx, MAPISTORE_ERR_INVALID_PARAMETER, NULL);

	/* Retrieve smb.conf arguments */
	retval = mapistore_namedprops_mysql_parameters(lp_ctx, &parms);
	if (retval != MAPISTORE_SUCCESS) {
		OC_DEBUG(0, "ERROR: parsing MySQL named properties "
			    "parametric option failed with %s\n",
			    mapistore_errstr(retval));
		MAPISTORE_RETVAL_ERR(retval, NULL);
	}

	/* Establish MySQL connection */
	connection_string = connection_string_from_parameters(mem_ctx, &parms);
	MAPISTORE_RETVAL_IF(!connection_string, MAPISTORE_ERR_NOT_INITIALIZED, NULL);
	if (parms.sock) {
		// FIXME
		OC_DEBUG(0, "Not implemented connect through unix socket to mysql");
		MAPISTORE_RETVAL_ERR(MAPISTORE_ERR_DATABASE_INIT, NULL);
	} else {
		create_connection(connection_string, &conn);
	}
	MAPISTORE_RETVAL_IF(!conn, MAPISTORE_ERR_NOT_INITIALIZED, NULL);

	/* Initialize the database */
	if (!table_exists(conn, NAMEDPROPS_MYSQL_TABLE)) {
		OC_DEBUG(3, "Creating schema for named_properties on mysql %s\n",
			 connection_string);
		schema_created_ret = migrate_named_properties_schema(connection_string);
		if (schema_created_ret) {
			OC_DEBUG(1, "Failed named properties schema creation using "
				 "migration framework: %d\n", schema_created_ret);
			MAPISTORE_RETVAL_ERR(MAPISTORE_ERR_DATABASE_INIT, connection_string);
		}
	}

	talloc_free(connection_string);

	/* Create context */
	nprops = talloc_zero(mem_ctx, struct namedprops_context);
	MAPISTORE_RETVAL_IF(!nprops, MAPISTORE_ERR_NO_MEMORY, NULL);

	nprops->backend_type = NAMEDPROPS_BACKEND_MYSQL;

	nprops->create_id = create_id;
	nprops->get_mapped_id = get_mapped_id;
	nprops->get_nameid = get_nameid;
	nprops->get_nameid_type = get_nameid_type;
	nprops->next_unused_id = next_unused_id;
	nprops->transaction_commit = transaction_commit;
	nprops->transaction_start = transaction_start;

	nprops->data = conn;
	talloc_set_destructor(nprops, mapistore_namedprops_mysql_destructor);

	*nprops_ctx = nprops;
	return MAPISTORE_SUCCESS;
}
