/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2003-2011 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2016 Planets Communications B.V.
   Copyright (C) 2013-2017 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
/*
 * Dan Langille, December 2003
 * based upon work done by Kern Sibbald, March 2000
 * Major rewrite by Marco van Wieringen, January 2010 for catalog refactoring.
 */
/**
 * @file
 * BAREOS Catalog Database routines specific to PostgreSQL
 * These are PostgreSQL specific routines
 */

#include "bareos.h"

#ifdef HAVE_POSTGRESQL

#include "cats.h"
#include "libpq-fe.h"
#include "postgres_ext.h"       /* needed for NAMEDATALEN */
#include "pg_config_manual.h"   /* get NAMEDATALEN on version 8.3 or later */
#include "bdb_postgresql.h"

/* pull in the generated queries definitions */
#include "postgresql_queries.inc"

/* -----------------------------------------------------------------------
 *
 *   PostgreSQL dependent defines and subroutines
 *
 * -----------------------------------------------------------------------
 */

/**
 * List of open databases
 */
static dlist *db_list = NULL;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

BareosDbPostgresql::BareosDbPostgresql(JobControlRecord *jcr,
                                 const char *db_driver,
                                 const char *db_name,
                                 const char *db_user,
                                 const char *db_password,
                                 const char *db_address,
                                 int db_port,
                                 const char *db_socket,
                                 bool mult_db_connections,
                                 bool disable_batch_insert,
                                 bool try_reconnect,
                                 bool exit_on_fatal,
                                 bool need_private
                                 )
{
   /*
    * Initialize the parent class members.
    */
   db_interface_type_ = SQL_INTERFACE_TYPE_POSTGRESQL;
   db_type_ = SQL_TYPE_POSTGRESQL;
   db_driver_ = bstrdup("PostgreSQL");
   db_name_ = bstrdup(db_name);
   db_user_ = bstrdup(db_user);
   if (db_password) {
      db_password_ = bstrdup(db_password);
   }
   if (db_address) {
      db_address_ = bstrdup(db_address);
   }
   if (db_socket) {
      db_socket_ = bstrdup(db_socket);
   }
   db_port_ = db_port;
   if (disable_batch_insert) {
      disabled_batch_insert_ = true;
      have_batch_insert_ = false;
   } else {
      disabled_batch_insert_ = false;
#if defined(USE_BATCH_FILE_INSERT)
#if defined(HAVE_POSTGRESQL_BATCH_FILE_INSERT) || defined(HAVE_PQISTHREADSAFE)
#ifdef HAVE_PQISTHREADSAFE
      have_batch_insert_ = PQisthreadsafe();
#else
      have_batch_insert_ = true;
#endif /* HAVE_PQISTHREADSAFE */
#else
      have_batch_insert_ = true;
#endif /* HAVE_POSTGRESQL_BATCH_FILE_INSERT || HAVE_PQISTHREADSAFE */
#else
      have_batch_insert_ = false;
#endif /* USE_BATCH_FILE_INSERT */
   }
   errmsg = get_pool_memory(PM_EMSG); /* get error message buffer */
   *errmsg = 0;
   cmd = get_pool_memory(PM_EMSG); /* get command buffer */
   cached_path = get_pool_memory(PM_FNAME);
   cached_path_id = 0;
   ref_count_ = 1;
   fname = get_pool_memory(PM_FNAME);
   path = get_pool_memory(PM_FNAME);
   esc_name = get_pool_memory(PM_FNAME);
   esc_path = get_pool_memory(PM_FNAME);
   esc_obj = get_pool_memory(PM_FNAME);
   buf_ =  get_pool_memory(PM_FNAME);
   allow_transactions_ = mult_db_connections;
   is_private_ = need_private;
   try_reconnect_ = try_reconnect;
   exit_on_fatal_ = exit_on_fatal;
   last_hash_key_ = 0;
   last_query_text_ = NULL;

   /*
    * Initialize the private members.
    */
   db_handle_ = NULL;
   result_ = NULL;

   /*
    * Put the db in the list.
    */
   if (db_list == NULL) {
      db_list = New(dlist(this, &this->link_));
   }
   db_list->append(this);

   /* make the queries available using the queries variable from the parent class */
   queries = query_definitions;
}

BareosDbPostgresql::~BareosDbPostgresql()
{
}

/**
 * Check that the database correspond to the encoding we want
 */
bool BareosDbPostgresql::check_database_encoding(JobControlRecord *jcr)
{
   SQL_ROW row;
   bool retval = false;

   if (!sql_query_without_handler("SELECT getdatabaseencoding()", QF_STORE_RESULT)) {
      Jmsg(jcr, M_ERROR, 0, "%s", errmsg);
      return false;
   }

   if ((row = sql_fetch_row()) == NULL) {
      Mmsg1(errmsg, _("error fetching row: %s\n"), errmsg);
      Jmsg(jcr, M_ERROR, 0, "Can't check database encoding %s", errmsg);
   } else {
      retval = bstrcmp(row[0], "SQL_ASCII");

      if (retval) {
         /*
          * If we are in SQL_ASCII, we can force the client_encoding to SQL_ASCII too
          */
         sql_query_without_handler("SET client_encoding TO 'SQL_ASCII'");
      } else {
         /*
          * Something is wrong with database encoding
          */
         Mmsg(errmsg, _("Encoding error for database \"%s\". Wanted SQL_ASCII, got %s\n"), get_db_name(), row[0]);
         Jmsg(jcr, M_WARNING, 0, "%s", errmsg);
         Dmsg1(50, "%s", errmsg);
      }
   }

   return retval;
}

/**
 * Now actually open the database.  This can generate errors, which are returned in the errmsg
 *
 * DO NOT close the database or delete mdb here !!!!
 */
bool BareosDbPostgresql::open_database(JobControlRecord *jcr)
{
   bool retval = false;
   int errstat;
   char buf[10], *port;

   P(mutex);
   if (connected_) {
      retval = true;
      goto bail_out;
   }

   if ((errstat = rwl_init(&lock_)) != 0) {
      berrno be;
      Mmsg1(errmsg, _("Unable to initialize DB lock. ERR=%s\n"), be.bstrerror(errstat));
      goto bail_out;
   }

   if (db_port_) {
      bsnprintf(buf, sizeof(buf), "%d", db_port_);
      port = buf;
   } else {
      port = NULL;
   }

   /*
    * If connection fails, try at 5 sec intervals for 30 seconds.
    */
   for (int retry = 0; retry < 6; retry++) {
      /*
       * Connect to the database
       */
      db_handle_ = PQsetdbLogin(db_address_,   /* default = localhost */
                                 port,           /* default port */
                                 NULL,           /* pg options */
                                 NULL,           /* tty, ignored */
                                 db_name_,      /* database name */
                                 db_user_,      /* login name */
                                 db_password_); /* password */

      /*
       * If no connect, try once more in case it is a timing problem
       */
      if (PQstatus(db_handle_) == CONNECTION_OK) {
         break;
      }

      bmicrosleep(5, 0);
   }

   Dmsg0(50, "pg_real_connect done\n");
   Dmsg3(50, "db_user=%s db_name=%s db_password=%s\n", db_user_, db_name_,
        (db_password_ == NULL) ? "(NULL)" : db_password_);

   if (PQstatus(db_handle_) != CONNECTION_OK) {
      Mmsg2(errmsg, _("Unable to connect to PostgreSQL server. Database=%s User=%s\n"
                       "Possible causes: SQL server not running; password incorrect; max_connections exceeded.\n"),
            db_name_, db_user_);
      goto bail_out;
   }

   connected_ = true;
   if (!check_tables_version(jcr)) {
      goto bail_out;
   }

   sql_query_without_handler("SET datestyle TO 'ISO, YMD'");
   sql_query_without_handler("SET cursor_tuple_fraction=1");

   /*
    * Tell PostgreSQL we are using standard conforming strings
    * and avoid warnings such as:
    *  WARNING:  nonstandard use of \\ in a string literal
    */
   sql_query_without_handler("SET standard_conforming_strings=on");

   /*
    * Check that encoding is SQL_ASCII
    */
   check_database_encoding(jcr);

   retval = true;

bail_out:
   V(mutex);
   return retval;
}

void BareosDbPostgresql::close_database(JobControlRecord *jcr)
{
   if (connected_) {
      end_transaction(jcr);
   }
   P(mutex);
   ref_count_--;
   if (ref_count_ == 0) {
      if (connected_) {
         sql_free_result();
      }
      db_list->remove(this);
      if (connected_ && db_handle_) {
         PQfinish(db_handle_);
      }
      if (rwl_is_init(&lock_)) {
         rwl_destroy(&lock_);
      }
      free_pool_memory(errmsg);
      free_pool_memory(cmd);
      free_pool_memory(cached_path);
      free_pool_memory(fname);
      free_pool_memory(path);
      free_pool_memory(esc_name);
      free_pool_memory(esc_path);
      free_pool_memory(esc_obj);
      free_pool_memory(buf_);
      if (db_driver_) {
         free(db_driver_);
      }
      if (db_name_) {
         free(db_name_);
      }
      if (db_user_) {
         free(db_user_);
      }
      if (db_password_) {
         free(db_password_);
      }
      if (db_address_) {
         free(db_address_);
      }
      if (db_socket_) {
         free(db_socket_);
      }
      delete this;
      if (db_list->size() == 0) {
         delete db_list;
         db_list = NULL;
      }
   }
   V(mutex);
}

bool BareosDbPostgresql::validate_connection(void)
{
   bool retval = false;

   /*
    * Perform a null query to see if the connection is still valid.
    */
   db_lock(this);
   if (!sql_query_without_handler("SELECT 1", true)) {
      /*
       * Try resetting the connection.
       */
      PQreset(db_handle_);
      if (PQstatus(db_handle_) != CONNECTION_OK) {
         goto bail_out;
      }

      sql_query_without_handler("SET datestyle TO 'ISO, YMD'");
      sql_query_without_handler("SET cursor_tuple_fraction=1");
      sql_query_without_handler("SET standard_conforming_strings=on");

      /*
       * Retry the null query.
       */
      if (!sql_query_without_handler("SELECT 1", true)) {
         goto bail_out;
      }
   }

   sql_free_result();
   retval = true;

bail_out:
   db_unlock(this);
   return retval;
}

/**
 * Escape strings so that PostgreSQL is happy
 *
 *   NOTE! len is the length of the old string. Your new
 *         string must be long enough (max 2*old+1) to hold
 *         the escaped output.
 */
void BareosDbPostgresql::escape_string(JobControlRecord *jcr, char *snew, char *old, int len)
{
   int error;

   PQescapeStringConn(db_handle_, snew, old, len, &error);
   if (error) {
      Jmsg(jcr, M_FATAL, 0, _("PQescapeStringConn returned non-zero.\n"));
      /* error on encoding, probably invalid multibyte encoding in the source string
        see PQescapeStringConn documentation for details. */
      Dmsg0(500, "PQescapeStringConn failed\n");
   }
}

/**
 * Escape binary so that PostgreSQL is happy
 *
 */
char *BareosDbPostgresql::escape_object(JobControlRecord *jcr, char *old, int len)
{
   size_t new_len;
   unsigned char *obj;

   obj = PQescapeByteaConn(db_handle_, (unsigned const char *)old, len, &new_len);
   if (!obj) {
      Jmsg(jcr, M_FATAL, 0, _("PQescapeByteaConn returned NULL.\n"));
   }

   esc_obj = check_pool_memory_size(esc_obj, new_len+1);
   memcpy(esc_obj, obj, new_len);
   esc_obj[new_len]=0;

   PQfreemem(obj);

   return (char *)esc_obj;
}

/**
 * Unescape binary object so that PostgreSQL is happy
 *
 */
void BareosDbPostgresql::unescape_object(JobControlRecord *jcr, char *from, int32_t expected_len,
                                      POOLMEM *&dest, int32_t *dest_len)
{
   size_t new_len;
   unsigned char *obj;

   if (!from) {
      dest[0] = '\0';
      *dest_len = 0;
      return;
   }

   obj = PQunescapeBytea((unsigned const char *)from, &new_len);

   if (!obj) {
      Jmsg(jcr, M_FATAL, 0, _("PQunescapeByteaConn returned NULL.\n"));
   }

   *dest_len = new_len;
   dest = check_pool_memory_size(dest, new_len + 1);
   memcpy(dest, obj, new_len);
   dest[new_len] = '\0';

   PQfreemem(obj);

   Dmsg1(010, "obj size: %d\n", *dest_len);
}

/**
 * Start a transaction. This groups inserts and makes things
 * much more efficient. Usually started when inserting
 * file attributes.
 */
void BareosDbPostgresql::start_transaction(JobControlRecord *jcr)
{
   if (!jcr->attr) {
      jcr->attr = get_pool_memory(PM_FNAME);
   }
   if (!jcr->ar) {
      jcr->ar = (AttributesDbRecord *)malloc(sizeof(AttributesDbRecord));
   }

   /*
    * This is turned off because transactions break
    * if multiple simultaneous jobs are run.
    */
   if (!allow_transactions_) {
      return;
   }

   db_lock(this);
   /*
    * Allow only 25,000 changes per transaction
    */
   if (transaction_ && changes > 25000) {
      end_transaction(jcr);
   }
   if (!transaction_) {
      sql_query_without_handler("BEGIN");  /* begin transaction */
      Dmsg0(400, "Start PosgreSQL transaction\n");
      transaction_ = true;
   }
   db_unlock(this);
}

void BareosDbPostgresql::end_transaction(JobControlRecord *jcr)
{
   if (jcr && jcr->cached_attribute) {
      Dmsg0(400, "Flush last cached attribute.\n");
      if (!create_attributes_record(jcr, jcr->ar)) {
         Jmsg1(jcr, M_FATAL, 0, _("Attribute create error. %s"), strerror());
      }
      jcr->cached_attribute = false;
   }

   if (!allow_transactions_) {
      return;
   }

   db_lock(this);
   if (transaction_) {
      sql_query_without_handler("COMMIT"); /* end transaction */
      transaction_ = false;
      Dmsg1(400, "End PostgreSQL transaction changes=%d\n", changes);
   }
   changes = 0;
   db_unlock(this);
}

/**
 * Submit a general SQL command (cmd), and for each row returned,
 * the result_handler is called with the ctx.
 */
bool BareosDbPostgresql::big_sql_query(const char *query, DB_RESULT_HANDLER *result_handler, void *ctx)
{
   SQL_ROW row;
   bool retval = false;
   bool in_transaction = transaction_;

   Dmsg1(500, "big_sql_query starts with '%s'\n", query);

   /* This code handles only SELECT queries */
   if (!bstrncasecmp(query, "SELECT", 6)) {
      return sql_query_with_handler(query, result_handler, ctx);
   }

   if (!result_handler) {       /* no need of big_query without handler */
      return false;
   }

   db_lock(this);

   if (!in_transaction) {       /* CURSOR needs transaction */
      sql_query_without_handler("BEGIN");
   }

   Mmsg(buf_, "DECLARE _bac_cursor CURSOR FOR %s", query);

   if (!sql_query_without_handler(buf_)) {
      Mmsg(errmsg, _("Query failed: %s: ERR=%s\n"), buf_, sql_strerror());
      Dmsg0(50, "sql_query_without_handler failed\n");
      goto bail_out;
   }

   do {
      if (!sql_query_without_handler("FETCH 100 FROM _bac_cursor")) {
         goto bail_out;
      }
      while ((row = sql_fetch_row()) != NULL) {
         Dmsg1(500, "Fetching %d rows\n", num_rows_);
         if (result_handler(ctx, num_fields_, row))
            break;
      }
      PQclear(result_);
      result_ = NULL;

   } while (num_rows_ > 0);

   sql_query_without_handler("CLOSE _bac_cursor");

   Dmsg0(500, "big_sql_query finished\n");
   sql_free_result();
   retval = true;

bail_out:
   if (!in_transaction) {
      sql_query_without_handler("COMMIT");  /* end transaction */
   }

   db_unlock(this);
   return retval;
}

/**
 * Submit a general SQL command (cmd), and for each row returned,
 * the result_handler is called with the ctx.
 */
bool BareosDbPostgresql::sql_query_with_handler(const char *query, DB_RESULT_HANDLER *result_handler, void *ctx)
{
   SQL_ROW row;
   bool retval = true;

   Dmsg1(500, "sql_query_with_handler starts with '%s'\n", query);

   db_lock(this);
   if (!sql_query_without_handler(query, QF_STORE_RESULT)) {
      Mmsg(errmsg, _("Query failed: %s: ERR=%s\n"), query, sql_strerror());
      Dmsg0(500, "sql_query_with_handler failed\n");
      retval = false;
      goto bail_out;
   }

   Dmsg0(500, "sql_query_with_handler succeeded. checking handler\n");

   if (result_handler != NULL) {
      Dmsg0(500, "sql_query_with_handler invoking handler\n");
      while ((row = sql_fetch_row()) != NULL) {
         Dmsg0(500, "sql_query_with_handler sql_fetch_row worked\n");
         if (result_handler(ctx, num_fields_, row))
            break;
      }
      sql_free_result();
   }

   Dmsg0(500, "sql_query_with_handler finished\n");

bail_out:
   db_unlock(this);
   return retval;
}

/**
 * Note, if this routine returns false (failure), BAREOS expects
 * that no result has been stored.
 * This is where QUERY_DB comes with Postgresql.
 *
 * Returns:  true  on success
 *           false on failure
 */
bool BareosDbPostgresql::sql_query_without_handler(const char *query, int flags)
{
   int i;
   bool retry = true;
   bool retval = false;

   Dmsg1(500, "sql_query_without_handler starts with '%s'\n", query);

   /*
    * We are starting a new query. reset everything.
    */
retry_query:
   num_rows_ = -1;
   row_number_ = -1;
   field_number_ = -1;

   if (result_) {
      PQclear(result_);  /* hmm, someone forgot to free?? */
      result_ = NULL;
   }

   for (i = 0; i < 10; i++) {
      result_ = PQexec(db_handle_, query);
      if (result_) {
         break;
      }
      bmicrosleep(5, 0);
   }

   status_ = PQresultStatus(result_);
   switch (status_) {
   case PGRES_TUPLES_OK:
   case PGRES_COMMAND_OK:
      Dmsg0(500, "we have a result\n");

      /*
       * How many fields in the set?
       */
      num_fields_ = (int)PQnfields(result_);
      Dmsg1(500, "we have %d fields\n", num_fields_);

      num_rows_ = PQntuples(result_);
      Dmsg1(500, "we have %d rows\n", num_rows_);

      row_number_ = 0;      /* we can start to fetch something */
      status_ = 0;          /* succeed */
      retval = true;
      break;
   case PGRES_FATAL_ERROR:
      Dmsg1(50, "Result status fatal: %s\n", query);
      if (exit_on_fatal_) {
         /*
          * Any fatal error should result in the daemon exiting.
          */
         Emsg0(M_FATAL, 0, "Fatal database error\n");
      }

      if (try_reconnect_ && !transaction_) {
         /*
          * Only try reconnecting when no transaction is pending.
          * Reconnecting within a transaction will lead to an aborted
          * transaction anyway so we better follow our old error path.
          */
         if (retry) {
            PQreset(db_handle_);

            /*
             * See if we got a working connection again.
             */
            if (PQstatus(db_handle_) == CONNECTION_OK) {
               /*
                * Reset the connection settings.
                */
               PQexec(db_handle_, "SET datestyle TO 'ISO, YMD'");
               PQexec(db_handle_, "SET cursor_tuple_fraction=1");
               result_ = PQexec(db_handle_, "SET standard_conforming_strings=on");

               switch (PQresultStatus(result_)) {
               case PGRES_COMMAND_OK:
                  retry = false;
                  goto retry_query;
               default:
                  break;
               }
            }
         }
      }
      goto bail_out;
   default:
      Dmsg1(50, "Result status failed: %s\n", query);
      goto bail_out;
   }

   Dmsg0(500, "sql_query_without_handler finishing\n");
   goto ok_out;

bail_out:
   Dmsg0(500, "we failed\n");
   PQclear(result_);
   result_ = NULL;
   status_ = 1;                   /* failed */

ok_out:
   return retval;
}

void BareosDbPostgresql::sql_free_result(void)
{
   db_lock(this);
   if (result_) {
      PQclear(result_);
      result_ = NULL;
   }
   if (rows_) {
      free(rows_);
      rows_ = NULL;
   }
   if (fields_) {
      free(fields_);
      fields_ = NULL;
   }
   num_rows_ = num_fields_ = 0;
   db_unlock(this);
}

SQL_ROW BareosDbPostgresql::sql_fetch_row(void)
{
   int j;
   SQL_ROW row = NULL; /* by default, return NULL */

   Dmsg0(500, "sql_fetch_row start\n");

   if (num_fields_ == 0) {     /* No field, no row */
      Dmsg0(500, "sql_fetch_row finishes returning NULL, no fields\n");
      return NULL;
   }

   if (!rows_ || rows_size_ < num_fields_) {
      if (rows_) {
         Dmsg0(500, "sql_fetch_row freeing space\n");
         free(rows_);
      }
      Dmsg1(500, "we need space for %d bytes\n", sizeof(char *) * num_fields_);
      rows_ = (SQL_ROW)malloc(sizeof(char *) * num_fields_);
      rows_size_ = num_fields_;

      /*
       * Now reset the row_number now that we have the space allocated
       */
      row_number_ = 0;
   }

   /*
    * If still within the result set
    */
   if (row_number_ >= 0 && row_number_ < num_rows_) {
      Dmsg2(500, "sql_fetch_row row number '%d' is acceptable (0..%d)\n", row_number_, num_rows_);
      /*
       * Get each value from this row
       */
      for (j = 0; j < num_fields_; j++) {
         rows_[j] = PQgetvalue(result_, row_number_, j);
         Dmsg2(500, "sql_fetch_row field '%d' has value '%s'\n", j, rows_[j]);
      }
      /*
       * Increment the row number for the next call
       */
      row_number_++;
      row = rows_;
   } else {
      Dmsg2(500, "sql_fetch_row row number '%d' is NOT acceptable (0..%d)\n", row_number_, num_rows_);
   }

   Dmsg1(500, "sql_fetch_row finishes returning %p\n", row);

   return row;
}

const char *BareosDbPostgresql::sql_strerror(void)
{
   return PQerrorMessage(db_handle_);
}

void BareosDbPostgresql::sql_data_seek(int row)
{
   /*
    * Set the row number to be returned on the next call to sql_fetch_row
    */
   row_number_ = row;
}

int BareosDbPostgresql::sql_affected_rows(void)
{
   return (unsigned) str_to_int32(PQcmdTuples(result_));
}

uint64_t BareosDbPostgresql::sql_insert_autokey_record(const char *query, const char *table_name)
{
   int i;
   uint64_t id = 0;
   char sequence[NAMEDATALEN-1];
   char getkeyval_query[NAMEDATALEN+50];
   PGresult *pg_result;

   /*
    * First execute the insert query and then retrieve the currval.
    */
   if (!sql_query_without_handler(query)) {
      return 0;
   }

   num_rows_ = sql_affected_rows();
   if (num_rows_ != 1) {
      return 0;
   }

   changes++;

   /*
    * Obtain the current value of the sequence that
    * provides the serial value for primary key of the table.
    *
    * currval is local to our session.  It is not affected by
    * other transactions.
    *
    * Determine the name of the sequence.
    * PostgreSQL automatically creates a sequence using
    * <table>_<column>_seq.
    * At the time of writing, all tables used this format for
    * for their primary key: <table>id
    * Except for basefiles which has a primary key on baseid.
    * Therefore, we need to special case that one table.
    *
    * everything else can use the PostgreSQL formula.
    */
   if (bstrcasecmp(table_name, "basefiles")) {
      bstrncpy(sequence, "basefiles_baseid", sizeof(sequence));
   } else {
      bstrncpy(sequence, table_name, sizeof(sequence));
      bstrncat(sequence, "_",        sizeof(sequence));
      bstrncat(sequence, table_name, sizeof(sequence));
      bstrncat(sequence, "id",       sizeof(sequence));
   }

   bstrncat(sequence, "_seq", sizeof(sequence));
   bsnprintf(getkeyval_query, sizeof(getkeyval_query), "SELECT currval('%s')", sequence);

   Dmsg1(500, "sql_insert_autokey_record executing query '%s'\n", getkeyval_query);
   for (i = 0; i < 10; i++) {
      pg_result = PQexec(db_handle_, getkeyval_query);
      if (pg_result) {
         break;
      }
      bmicrosleep(5, 0);
   }
   if (!pg_result) {
      Dmsg1(50, "Query failed: %s\n", getkeyval_query);
      goto bail_out;
   }

   Dmsg0(500, "exec done");

   if (PQresultStatus(pg_result) == PGRES_TUPLES_OK) {
      Dmsg0(500, "getting value");
      id = str_to_uint64(PQgetvalue(pg_result, 0, 0));
      Dmsg2(500, "got value '%s' which became %d\n", PQgetvalue(pg_result, 0, 0), id);
   } else {
      Dmsg1(50, "Result status failed: %s\n", getkeyval_query);
      Mmsg1(errmsg, _("error fetching currval: %s\n"), PQerrorMessage(db_handle_));
   }

bail_out:
   PQclear(pg_result);

   return id;
}

SQL_FIELD *BareosDbPostgresql::sql_fetch_field(void)
{
   int i, j;
   int max_length;
   int this_length;

   Dmsg0(500, "sql_fetch_field starts\n");

   if (!fields_ || fields_size_ < num_fields_) {
      if (fields_) {
         free(fields_);
         fields_ = NULL;
      }
      Dmsg1(500, "allocating space for %d fields\n", num_fields_);
      fields_ = (SQL_FIELD *)malloc(sizeof(SQL_FIELD) * num_fields_);
      fields_size_ = num_fields_;

      for (i = 0; i < num_fields_; i++) {
         Dmsg1(500, "filling field %d\n", i);
         fields_[i].name = PQfname(result_, i);
         fields_[i].type = PQftype(result_, i);
         fields_[i].flags = 0;

         /*
          * For a given column, find the max length.
          */
         max_length = 0;
         for (j = 0; j < num_rows_; j++) {
            if (PQgetisnull(result_, j, i)) {
                this_length = 4;        /* "NULL" */
            } else {
                this_length = cstrlen(PQgetvalue(result_, j, i));
            }

            if (max_length < this_length) {
               max_length = this_length;
            }
         }
         fields_[i].max_length = max_length;

         Dmsg4(500, "sql_fetch_field finds field '%s' has length='%d' type='%d' and IsNull=%d\n",
               fields_[i].name, fields_[i].max_length, fields_[i].type, fields_[i].flags);
      }
   }

   /*
    * Increment field number for the next time around
    */
   return &fields_[field_number_++];
}

bool BareosDbPostgresql::sql_field_is_not_null(int field_type)
{
   switch (field_type) {
   case 1:
      return true;
   default:
      return false;
   }
}

bool BareosDbPostgresql::sql_field_is_numeric(int field_type)
{
   /*
    * TEMP: the following is taken from select OID, typname from pg_type;
    */
   switch (field_type) {
   case 20:
   case 21:
   case 23:
   case 700:
   case 701:
      return true;
   default:
      return false;
   }
}

/**
 * Escape strings so that PostgreSQL is happy on COPY
 *
 *   NOTE! len is the length of the old string. Your new
 *         string must be long enough (max 2*old+1) to hold
 *         the escaped output.
 */
static char *pgsql_copy_escape(char *dest, char *src, size_t len)
{
   /* we have to escape \t, \n, \r, \ */
   char c = '\0' ;

   while (len > 0 && *src) {
      switch (*src) {
      case '\n':
         c = 'n';
         break;
      case '\\':
         c = '\\';
         break;
      case '\t':
         c = 't';
         break;
      case '\r':
         c = 'r';
         break;
      default:
         c = '\0' ;
      }

      if (c) {
         *dest = '\\';
         dest++;
         *dest = c;
      } else {
         *dest = *src;
      }

      len--;
      src++;
      dest++;
   }

   *dest = '\0';
   return dest;
}

bool BareosDbPostgresql::sql_batch_start(JobControlRecord *jcr)
{
   const char *query = "COPY batch FROM STDIN";

   Dmsg0(500, "sql_batch_start started\n");

   if (!sql_query_without_handler("CREATE TEMPORARY TABLE batch ("
                                  "FileIndex int,"
                                  "JobId int,"
                                  "Path varchar,"
                                  "Name varchar,"
                                  "LStat varchar,"
                                  "Md5 varchar,"
                                  "DeltaSeq smallint,"
                                  "Fhinfo NUMERIC(20),"
                                  "Fhnode NUMERIC(20))")) {
      Dmsg0(500, "sql_batch_start failed\n");
      return false;
   }

   /*
    * We are starting a new query.  reset everything.
    */
   num_rows_ = -1;
   row_number_ = -1;
   field_number_ = -1;

   sql_free_result();

   for (int i=0; i < 10; i++) {
      result_ = PQexec(db_handle_, query);
      if (result_) {
         break;
      }
      bmicrosleep(5, 0);
   }
   if (!result_) {
      Dmsg1(50, "Query failed: %s\n", query);
      goto bail_out;
   }

   status_ = PQresultStatus(result_);
   if (status_ == PGRES_COPY_IN) {
      /*
       * How many fields in the set?
       */
      num_fields_ = (int) PQnfields(result_);
      num_rows_ = 0;
      status_ = 1;
   } else {
      Dmsg1(50, "Result status failed: %s\n", query);
      goto bail_out;
   }

   Dmsg0(500, "sql_batch_start finishing\n");

   return true;

bail_out:
   Mmsg1(errmsg, _("error starting batch mode: %s"), PQerrorMessage(db_handle_));
   status_ = 0;
   PQclear(result_);
   result_ = NULL;
   return false;
}

/**
 * Set error to something to abort operation
 */
bool BareosDbPostgresql::sql_batch_end(JobControlRecord *jcr, const char *error)
{
   int res;
   int count=30;
   PGresult *pg_result;

   Dmsg0(500, "sql_batch_end started\n");

   do {
      res = PQputCopyEnd(db_handle_, error);
   } while (res == 0 && --count > 0);

   if (res == 1) {
      Dmsg0(500, "ok\n");
      status_ = 1;
   }

   if (res <= 0) {
      Dmsg0(500, "we failed\n");
      status_ = 0;
      Mmsg1(errmsg, _("error ending batch mode: %s"), PQerrorMessage(db_handle_));
      Dmsg1(500, "failure %s\n", errmsg);
   }

   /*
    * Check command status and return to normal libpq state
    */
   pg_result = PQgetResult(db_handle_);
   if (PQresultStatus(pg_result) != PGRES_COMMAND_OK) {
      Mmsg1(errmsg, _("error ending batch mode: %s"), PQerrorMessage(db_handle_));
      status_ = 0;
   }

   PQclear(pg_result);

   Dmsg0(500, "sql_batch_end finishing\n");

   return true;
}

bool BareosDbPostgresql::sql_batch_insert(JobControlRecord *jcr, AttributesDbRecord *ar)
{
   int res;
   int count=30;
   size_t len;
   const char *digest;
   char ed1[50], ed2[50], ed3[50];

   esc_name = check_pool_memory_size(esc_name, fnl*2+1);
   pgsql_copy_escape(esc_name, fname, fnl);

   esc_path = check_pool_memory_size(esc_path, pnl*2+1);
   pgsql_copy_escape(esc_path, path, pnl);

   if (ar->Digest == NULL || ar->Digest[0] == 0) {
      digest = "0";
   } else {
      digest = ar->Digest;
   }

   len = Mmsg(cmd, "%u\t%s\t%s\t%s\t%s\t%s\t%u\t%s\t%s\n",
              ar->FileIndex, edit_int64(ar->JobId, ed1), esc_path,
              esc_name, ar->attr, digest, ar->DeltaSeq,
              edit_uint64(ar->Fhinfo,ed2),
              edit_uint64(ar->Fhnode,ed3));

   do {
      res = PQputCopyData(db_handle_, cmd, len);
   } while (res == 0 && --count > 0);

   if (res == 1) {
      Dmsg0(500, "ok\n");
      changes++;
      status_ = 1;
   }

   if (res <= 0) {
      Dmsg0(500, "we failed\n");
      status_ = 0;
      Mmsg1(errmsg, _("error copying in batch mode: %s"), PQerrorMessage(db_handle_));
      Dmsg1(500, "failure %s\n", errmsg);
   }

   Dmsg0(500, "sql_batch_insert finishing\n");

   return true;
}

/**
 * Initialize database data structure. In principal this should
 * never have errors, or it is really fatal.
 */
#ifdef HAVE_DYNAMIC_CATS_BACKENDS
extern "C" BareosDb CATS_IMP_EXP *backend_instantiate(JobControlRecord *jcr,
                                                  const char *db_driver,
                                                  const char *db_name,
                                                  const char *db_user,
                                                  const char *db_password,
                                                  const char *db_address,
                                                  int db_port,
                                                  const char *db_socket,
                                                  bool mult_db_connections,
                                                  bool disable_batch_insert,
                                                  bool try_reconnect,
                                                  bool exit_on_fatal,
                                                  bool need_private)
#else
BareosDb *db_init_database(JobControlRecord *jcr,
                       const char *db_driver,
                       const char *db_name,
                       const char *db_user,
                       const char *db_password,
                       const char *db_address,
                       int db_port,
                       const char *db_socket,
                       bool mult_db_connections,
                       bool disable_batch_insert,
                       bool try_reconnect,
                       bool exit_on_fatal,
                       bool need_private)
#endif
{
   BareosDbPostgresql *mdb = NULL;

   if (!db_user) {
      Jmsg(jcr, M_FATAL, 0, _("A user name for PostgreSQL must be supplied.\n"));
      return NULL;
   }
   P(mutex);                          /* lock DB queue */

   /*
    * Look to see if DB already open
    */
   if (db_list && !mult_db_connections && !need_private) {
      foreach_dlist(mdb, db_list) {
         if (mdb->is_private()) {
            continue;
         }

         if (mdb->match_database(db_driver, db_name, db_address, db_port)) {
            Dmsg1(100, "DB REopen %s\n", db_name);
            mdb->increment_refcount();
            goto bail_out;
         }
      }
   }
   Dmsg0(100, "db_init_database first time\n");
   mdb = New(BareosDbPostgresql(jcr,
                             db_driver,
                             db_name,
                             db_user,
                             db_password,
                             db_address,
                             db_port,
                             db_socket,
                             mult_db_connections,
                             disable_batch_insert,
                             try_reconnect,
                             exit_on_fatal,
                             need_private
                             ));

bail_out:
   V(mutex);
   return mdb;
}

#ifdef HAVE_DYNAMIC_CATS_BACKENDS
extern "C" void CATS_IMP_EXP flush_backend(void)
#else
void db_flush_backends(void)
#endif
{
}

#endif /* HAVE_POSTGRESQL */
