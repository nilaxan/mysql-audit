/*
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#include "mysql_inc.h"
#include "hot_patch.h"
#include <stdlib.h>
#include <ctype.h>

#include "audit_handler.h"
#include <string.h>
#include <sys/mman.h>
#if MYSQL_VERSION_ID >= 50600
//in 5.6 md5 implementation changed and we include our own
#include "md5.h"
#endif

/*
 Disable __attribute__() on non-gcc compilers.

 #if !defined(__attribute__) && !defined(__GNUC__)
 #define __attribute__(A)
 #endif
 */

static const char * log_prefix = AUDIT_LOG_PREFIX;

//possible audit handlers
static Audit_file_handler json_file_handler;
static Audit_socket_handler json_socket_handler;

//formatters
static Audit_json_formatter json_formatter;

//flags to hold if audit handlers are enabled
static my_bool json_file_handler_enable = FALSE;
static my_bool force_record_logins_enable = FALSE;
static my_bool json_file_handler_flush = FALSE;
static my_bool json_socket_handler_enable = FALSE;
static my_bool uninstall_plugin_enable = FALSE;
static my_bool validate_checksum_enable = FALSE;
static my_bool offsets_by_version_enable = FALSE;
static my_bool validate_offsets_extended_enable = FALSE;
static char * offsets_string = NULL;
static char * checksum_string = NULL;
static int delay_ms_val =0;
static char *delay_cmds_string = NULL;
static char delay_cmds_buff[4096] = {0};
static char *whitelist_cmds_string = NULL;
static char whitelist_cmds_buff[4096] = {0};
static char *record_cmds_string = NULL;
static char record_cmds_buff[4096] = {0};
static char *password_masking_cmds_string = NULL;
static char password_masking_cmds_buff[4096] = {0};
static char *record_objs_string = NULL;
static char record_objs_buff[4096] = {0};
static char *whitelist_users_string = NULL;
static char whitelist_users_buff[4096] = {0};

static char delay_cmds_array [SQLCOM_END + 2][MAX_COMMAND_CHAR_NUMBERS] = {{0}};
static char whitelist_cmds_array [SQLCOM_END + 2][MAX_COMMAND_CHAR_NUMBERS] = {{0}};
static char record_cmds_array [SQLCOM_END + 2][MAX_COMMAND_CHAR_NUMBERS] = {{0}};
static char password_masking_cmds_array [SQLCOM_END + 2][MAX_COMMAND_CHAR_NUMBERS] = {{0}};
static char record_objs_array [MAX_NUM_OBJECT_ELEM + 2][MAX_OBJECT_CHAR_NUMBERS] = {{0}};
static char whitelist_users_array [MAX_NUM_USER_ELEM + 2][MAX_USER_CHAR_NUMBERS] = {{0}};
static bool record_empty_objs_set = true;
static int num_delay_cmds = 0;
static int num_whitelist_cmds = 0;
static int num_record_cmds = 0;
static int num_password_masking_cmds = 0;
static int num_record_objs = 0;
static int num_whitelist_users = 0;
static SHOW_VAR com_status_vars_array [MAX_COM_STATUS_VARS_RECORDS] = {{0}};

//regex stuff
static char password_masking_regex_check_buff[4096] = {0};
static char * password_masking_regex_string = NULL;
static char password_masking_regex_buff[4096] = {0};

//socket name
static char json_socket_name_buff[1024] = {0};


/**
 * The trampoline functions we use. Will be set to point to allocated mem.
 */
static int (*trampoline_mysql_execute_command)(THD *thd) = NULL;
static unsigned int trampoline_mysql_execute_size =0;

#if MYSQL_VERSION_ID < 50600
static void (*trampoline_log_slow_statement)(THD *thd) = NULL;
static unsigned int trampoline_log_slow_statement_size =0;
#endif

#if MYSQL_VERSION_ID < 50505
static int (*trampoline_check_user)(THD *thd, enum enum_server_command command, const char *passwd, uint passwd_len, const char *db, bool check_count) = NULL;
static unsigned int trampoline_check_user_size =0;
#elif MYSQL_VERSION_ID < 50600
static bool (*trampoline_acl_authenticate)(THD *thd, uint connect_errors, uint com_change_user_pkt_len) = NULL;
static unsigned int trampoline_acl_authenticate_size =0;
#endif

static MYSQL_THDVAR_ULONG(is_thd_printed_list,
	PLUGIN_VAR_READONLY | PLUGIN_VAR_NOSYSVAR | PLUGIN_VAR_NOCMDOPT, 	"avoid duplicate printing",
NULL, NULL,0,0,
#ifdef __x86_64__
0xffffffffffffff,
#else
0xffffffff,
#endif 
1);

static MYSQL_THDVAR_ULONG(query_cache_table_list,
    PLUGIN_VAR_READONLY | PLUGIN_VAR_NOSYSVAR | PLUGIN_VAR_NOCMDOPT,    "Pointer to query cache table list.",
NULL, NULL,0,0,
#ifdef __x86_64__
0xffffffffffffff,
#else
0xffffffff,
#endif
1);

THDPRINTED * GetThdPrintedList (THD *thd)
{
    THDPRINTED * pThdPrintedList= (THDPRINTED*)THDVAR(thd,is_thd_printed_list);
    if (pThdPrintedList)
    {
        return pThdPrintedList;
    }
    THDVAR(thd,is_thd_printed_list) =0;
    return NULL;
 }

static int check_array(const char *cmds[],const char *array, int length) {
  for (int k=0; array[k * length] !='\0';k++) {
    for (int q = 0; cmds[q] != NULL; q++) {
      const char *cmd = cmds[q];
      int j = 0;
      while (array[k * length + j] != '\0' && cmd[j] != '\0'
              && array[k * length + j] == tolower(cmd[j])) {
        j++;
      }
      if (array[k * length + j] == '\0' && j != 0) {
        return 1;
      }
    }
  }
  return 0;
}

//utility method checks if the passed db object is part of record_objs_array
static bool check_db_obj(const char * db, const char * name)
{
    char db_obj[MAX_OBJECT_CHAR_NUMBERS] = {0};
    char wildcard_obj[MAX_OBJECT_CHAR_NUMBERS] = {0};
    char db_wildcard[MAX_OBJECT_CHAR_NUMBERS] = {0};
    if(db && name &&
      ((strlen(db) + strlen(name)) < MAX_OBJECT_CHAR_NUMBERS - 2))
    {
        strcpy(db_obj, db);
        strcat(db_obj, ".");
        strcat(db_obj, name);
        strcpy(wildcard_obj, "*.");
        strcat(wildcard_obj, name);
        strcpy(db_wildcard, db);
        strcat(db_wildcard, ".*");
        const char *objects[4];
        objects[0] = db_obj;
        objects[1] = wildcard_obj;
        objects[2] = db_wildcard;
        objects[3] = NULL;
        return check_array(objects, (char *) record_objs_array, MAX_OBJECT_CHAR_NUMBERS);
    }
    return false;
}

//callback function returns if password masking is required according to cmd type
static my_bool check_do_password_masking(const char * cmd)
{
	if(num_password_masking_cmds > 0)
	{
		const char *cmds[2];
		cmds[0] = cmd;
		cmds[1] = NULL;
		return check_array(cmds, (const char *) password_masking_cmds_array, sizeof(password_masking_cmds_array[0]));
	}
	return false;
}

//utility compiles the regex. if fails zero outs password_masking_regex_string
static void password_masking_regex_compile()
{
	int res = json_formatter.compile_password_masking_regex(password_masking_regex_string);
	if(res)
	{	password_masking_regex_buff[0] = '\0';
		password_masking_regex_string = password_masking_regex_buff;
	}
	sql_print_information("%s Compile password_masking_regex  res: [%d]", log_prefix, res);	
}


static void audit(ThdSesData *pThdData)
{
    THDPRINTED *pThdPrintedList = GetThdPrintedList (pThdData->getTHD());
  if (num_whitelist_cmds > 0) {
      const char * cmd = pThdData->getCmdName();
      const char *cmds[2];
      cmds[0] = cmd;
      cmds[1] = NULL;
      if (check_array(cmds, (char *) whitelist_cmds_array, MAX_COMMAND_CHAR_NUMBERS)) {
	return;
      }
  }
  if (num_whitelist_users > 0) {
      const char * user = pThdData->getUserName(); //If name is present, then no need to log the query
      const char *users[2];
	  if(NULL == user || '\0' == user[0]) //empty user use special symbol: "{}"
	  {
		user = "{}";
	  }
      users[0] = user;
      users[1] = NULL;
      if (check_array(users, (char *) whitelist_users_array, MAX_USER_CHAR_NUMBERS)) {
	return;
      }
  }    
  bool do_objs_cmds_check = true;
  if (force_record_logins_enable) {
      const char * cmd = pThdData->getCmdName();      
      if (!strcasecmp(cmd, "Connect") || !strcasecmp(cmd, "Quit") || !strcasecmp(cmd, "Failed Login")) {
        do_objs_cmds_check = false;
      }
  }
  if (num_record_cmds > 0 && do_objs_cmds_check) {
      const char * cmd = pThdData->getCmdName();
      const char *cmds[2];
      cmds[0] = cmd;
      cmds[1] = NULL;
      if (!check_array(cmds, (char *) record_cmds_array, MAX_COMMAND_CHAR_NUMBERS)) {
	return;
      }
  }
  if (num_record_objs > 0 && do_objs_cmds_check) {
	bool matched = false;
	if(pThdData->startGetObjects())
    {
        const char * db_name = NULL;
        const char * obj_name = NULL;        
        while(!matched && pThdData->getNextObject(&db_name, &obj_name, NULL))
        {
            matched = check_db_obj(db_name, obj_name);
        }        
    }
	else //no objects
	{
		matched = record_empty_objs_set;
	}    
    if (!matched) {
		return;
    }
  }    
    if (pThdPrintedList && pThdPrintedList->cur_index  < MAX_NUM_QUEUE_ELEM)
    {
		//audit the event if we haven't done so yet or in the case of prepare_sql we audit as the test "test select" doesn't go through mysql_execute_command
        if (pThdPrintedList->is_thd_printed_queue[pThdPrintedList->cur_index] == 0 || strcmp(pThdData->getCmdName(), "prepare_sql") == 0)
        {
            Audit_handler::log_audit_all(pThdData);
            pThdPrintedList->is_thd_printed_queue[pThdPrintedList->cur_index] = 1;
        }
		else //duplicate no need to audit then simply return
		{
			return;
		}
    }
    else 
    {
        Audit_handler::log_audit_all(pThdData);    
	}
	if (delay_ms_val > 0) 
	{
		const char * cmd = pThdData->getCmdName();
      const char *cmds[2];
      cmds[0] = cmd;
      cmds[1] = NULL;
      int delay = check_array(cmds, (char *) delay_cmds_array, MAX_COMMAND_CHAR_NUMBERS);
      if (delay)
			{
				//Audit_file_handler::print_sleep(thd,delay_ms_val);
				my_sleep (delay_ms_val *1000);
			}
		}
}


static int  (*trampoline_send_result_to_client)(Query_cache *pthis, THD *thd, char *sql, uint query_length) = NULL;

#if MYSQL_VERSION_ID > 50505
static bool (*trampoline_open_tables)(THD *thd, TABLE_LIST **start, uint *counter, uint flags,
                Prelocking_strategy *prelocking_strategy) = NULL;
#else
static int (*trampoline_open_tables)(THD *thd, TABLE_LIST **start, uint *counter, uint flags) = NULL;
#endif


QueryTableInf * Audit_formatter::getQueryCacheTableList1 (THD *thd)
{

	return (QueryTableInf*)	THDVAR(thd, query_cache_table_list);
}

static bool (*trampoline_check_table_access)(THD *thd, ulong want_access,TABLE_LIST *tables,  uint number, bool no_errors) = NULL;

static bool audit_check_table_access(THD *thd, ulong want_access,TABLE_LIST *tables,
	uint number, bool no_errors)
{
	TABLE_LIST *pTables;
	bool res = trampoline_check_table_access (thd, want_access, tables, number, no_errors);
	if (!res &&  tables)
	{
		pTables = tables;
		QueryTableInf * pQueryTableInf =(QueryTableInf*) THDVAR (thd,query_cache_table_list);
		if (pQueryTableInf)
		{
			while (pTables)
			{
				if (pQueryTableInf->num_of_elem < MAX_NUM_QUERY_TABLE_ELEM && pQueryTableInf->num_of_elem>=0)
				{
					pQueryTableInf->db[pQueryTableInf->num_of_elem] = (char*) thd_alloc (thd, strlen(Audit_formatter::table_get_db_name(pTables))+1);
					strcpy (pQueryTableInf->db[pQueryTableInf->num_of_elem],Audit_formatter::table_get_db_name(pTables));
					pQueryTableInf->table_name[pQueryTableInf->num_of_elem] = (char*) thd_alloc (thd, strlen(Audit_formatter::table_get_name(pTables)) +1);
					strcpy (pQueryTableInf->table_name[pQueryTableInf->num_of_elem],Audit_formatter::table_get_name(pTables));
					pQueryTableInf->object_type[pQueryTableInf->num_of_elem] = Audit_formatter::retrieve_object_type ( pTables);					
					pQueryTableInf->num_of_elem ++;
				}
				pTables = pTables->next_global;
			}

		}	
	}
	return res;
}

static unsigned int trampoline_check_table_access_size = 0;

static int  audit_send_result_to_client(Query_cache *pthis, THD *thd, char *sql, uint query_length)
{
	 int res;
	 void *pList = thd_alloc (thd, sizeof (QueryTableInf));
	 

	if (pList)
	 {
		  memset (pList,0,sizeof (QueryTableInf));
		  THDVAR(thd, query_cache_table_list) =(ulong)pList;
	 }	 
	 res = trampoline_send_result_to_client (pthis,thd, sql, query_length);
	 if (res)
	 {
         ThdSesData thd_data (thd);
		 audit (&thd_data);
	 }
	 THDVAR(thd, query_cache_table_list) = 0;
	 return res;
}
static unsigned int trampoline_send_result_to_client_size =0;

#if MYSQL_VERSION_ID > 50505
static bool audit_open_tables(THD *thd, TABLE_LIST **start, uint *counter, uint flags,
                Prelocking_strategy *prelocking_strategy)
{

    bool res;
    res = trampoline_open_tables (thd, start, counter, flags, prelocking_strategy);
    //only log if thread id or query id is non 0 (otherwise this is comming from startup activity)
    if(Audit_formatter::thd_inst_thread_id(thd) || Audit_formatter::thd_inst_query_id(thd))
    {
        ThdSesData thd_data (thd);
        audit(&thd_data);
    }
    return res;
}
#else
static int audit_open_tables(THD *thd, TABLE_LIST **start, uint *counter, uint flags)
{
    bool res;
    res = trampoline_open_tables (thd, start, counter, flags);
    //only log if thread id or query id is non 0 (otherwise this is comming from startup activity)
    if(Audit_formatter::thd_inst_thread_id(thd) || Audit_formatter::thd_inst_query_id(thd))
    {         
        ThdSesData thd_data (thd);
        audit(&thd_data);
    }
    return res;
}
#endif
static unsigned int trampoline_open_tables_size =0;



//called by log_slow_statement and general audit event caught by audit interface
static void audit_post_execute(THD * thd)
{
    //only audit non query events
    //query events are audited by mysql execute command
    if (Audit_formatter::thd_inst_command(thd) != COM_QUERY)
    {
        ThdSesData ThdData (thd);
        if (strcasestr (ThdData.getCmdName(), "show_fields")==NULL)
        {
            audit(&ThdData);
        }
    }
}



/*
 Plugin descriptor
 */
//in 5.6 we use the AUDIT plugin interface. In 5.1/5.5 we just use the general DAEMON plugin

#if MYSQL_VERSION_ID < 50600

static int plugin_type = MYSQL_DAEMON_PLUGIN;
static struct st_mysql_daemon audit_plugin =
{ MYSQL_DAEMON_INTERFACE_VERSION };

#else

static void audit_notify(THD *thd, unsigned int event_class,
        const void * event)
{
    if (MYSQL_AUDIT_GENERAL_CLASS == event_class)
    {
        const struct mysql_event_general *event_general =
                (const struct mysql_event_general *) event;
        if(MYSQL_AUDIT_GENERAL_STATUS == event_general->event_subclass)
        {
            audit_post_execute(thd);
        }
    }
    else if (MYSQL_AUDIT_CONNECTION_CLASS == event_class)
    {
        const struct mysql_event_connection *event_connection =
                (const struct mysql_event_connection *) event;
        //only audit for connect and change_user. disconnect is caught by general event
        if(event_connection->event_subclass != MYSQL_AUDIT_CONNECTION_DISCONNECT)
        {
            ThdSesData ThdData (thd);
            audit (&ThdData);
        }
    }
}

static int plugin_type = MYSQL_AUDIT_PLUGIN;
static struct st_mysql_audit audit_plugin=
{
  MYSQL_AUDIT_INTERFACE_VERSION,                    /* interface version    */
  NULL,                                             /* release_thd function */
  audit_notify,                                /* notify function      */
  { (unsigned long) MYSQL_AUDIT_GENERAL_CLASSMASK |
                    MYSQL_AUDIT_CONNECTION_CLASSMASK } /* class mask           */
};

#endif


//some extern definitions which are not in include files
extern void log_slow_statement(THD *thd);
extern int mysql_execute_command(THD *thd);
#if MYSQL_VERSION_ID >= 50505
//in 5.5 builtins is named differently
#define mysqld_builtins mysql_mandatory_plugins
#endif
extern struct st_mysql_plugin *mysqld_builtins[];


void remove_hot_functions ()
{
    void * target_function = NULL;
#if MYSQL_VERSION_ID < 50600
	target_function = (void *) log_slow_statement;
	remove_hot_patch_function(target_function,
	(void*) trampoline_log_slow_statement, trampoline_log_slow_statement_size, true);
	trampoline_log_slow_statement_size=0;
#endif
#if MYSQL_VERSION_ID < 50505
	target_function = (void *) check_user;
	remove_hot_patch_function(target_function,
	(void*) trampoline_check_user, trampoline_check_user_size, true);
	trampoline_check_user_size=0;
#elif MYSQL_VERSION_ID < 50600
    target_function = (void *) acl_authenticate;
	remove_hot_patch_function(target_function,
	(void*) trampoline_acl_authenticate, trampoline_acl_authenticate_size, true);
	trampoline_acl_authenticate_size=0;
#endif	

#if MYSQL_VERSION_ID > 50505
	target_function = (void *)*(bool (*)(THD *thd, TABLE_LIST **start, uint *counter, uint flags,
                Prelocking_strategy *prelocking_strategy)) &open_tables;
	remove_hot_patch_function(target_function,
	(void*) trampoline_open_tables, trampoline_open_tables_size, true);
	trampoline_open_tables_size=0;
#else
	target_function = (void *)*(int (*)(THD *thd, TABLE_LIST **start, uint *counter, uint flags)) &open_tables;
	remove_hot_patch_function(target_function,
	(void*) trampoline_open_tables, trampoline_open_tables_size, true);
	trampoline_open_tables_size=0;
#endif

	int (Query_cache::*pf_send_result_to_client)(THD *,char *, uint) = &Query_cache::send_result_to_client;
	target_function = *(void **) &pf_send_result_to_client;
	remove_hot_patch_function(target_function,
	(void*) trampoline_send_result_to_client, trampoline_send_result_to_client_size, true);		
	trampoline_send_result_to_client_size=0;

	remove_hot_patch_function((void*) check_table_access,
		(void*) trampoline_check_table_access,
		trampoline_check_table_access_size, true);	
	trampoline_check_table_access_size=0;
	remove_hot_patch_function((void*)mysql_execute_command,
		(void*) trampoline_mysql_execute_command, 
		trampoline_mysql_execute_size, true);
	trampoline_mysql_execute_size=0;
}

int is_remove_patches (ThdSesData *pThdData)
	{	

		static bool called_once = false;
		const char *cmd = pThdData->getCmdName();
        const char *sUninstallPlugin = "uninstall_plugin";
		LEX *pLex = Audit_formatter::thd_lex(pThdData->getTHD());
		if (pThdData->getTHD() && pLex!=NULL && strncasecmp (cmd,sUninstallPlugin ,strlen (sUninstallPlugin))==0  ) 
		{
			LEX_STRING Lex_comment = *(LEX_STRING*)(((unsigned char *) pLex) + Audit_formatter::thd_offsets.lex_comment);
			if (strncasecmp(Lex_comment.str, "AUDIT", 5) == 0)
			{
				if (!uninstall_plugin_enable)
				{
		
                   my_message (ER_NOT_ALLOWED_COMMAND,"Uninstall AUDIT plugin disabled",MYF(0));
                   return 2;
				}
				Audit_handler::stop_all();
				remove_hot_functions ();
				if(!called_once)
				{
					called_once = true;
					my_message (WARN_PLUGIN_BUSY,"Uninstall AUDIT plugin must be called again to complete",MYF(0));
					return 2;
				}
				return 1;
			}
		}
	return 0;
}

/*
 * Over ride functions for hot patch + audit. We call our audit function
 * after the execute command so all tables are resolved.
 */
static int audit_mysql_execute_command(THD *thd) 
{
    bool firstTime = false;
     THDPRINTED *pThdPrintedList = GetThdPrintedList (thd);
     if (pThdPrintedList)
     {
        if (pThdPrintedList->cur_index < (MAX_NUM_QUEUE_ELEM -1)  )
        {
            pThdPrintedList->cur_index ++;
            pThdPrintedList->is_thd_printed_queue[pThdPrintedList->cur_index] =0;
        }
     }
     else
     {
         firstTime = true;
       pThdPrintedList = (THDPRINTED *) thd_alloc (thd, sizeof (THDPRINTED));
       if (pThdPrintedList) 
       {
           memset (pThdPrintedList, 0, sizeof (THDPRINTED));
           //pThdPrintedList->cur_index = 0;
           THDVAR(thd,is_thd_printed_list) = (ulong) pThdPrintedList;
       }
     }
    ThdSesData thd_data (thd);
    const char *cmd = thd_data.getCmdName();
    if (strcasestr (cmd,"alter") !=NULL ||  strcasestr (cmd,"drop") !=NULL || strcasestr (cmd, "create") !=NULL ||  strcasestr (cmd, "truncate") !=NULL ||  strcasestr (cmd, "rename") !=NULL)
    {
        audit(&thd_data);
    }
	int res;
#if  defined(MARIADB_BASE_VERSION)
    if(Audit_formatter::thd_killed(thd) >= KILL_CONNECTION)
#else
    if(Audit_formatter::thd_killed(thd) == THD::KILL_CONNECTION)
#endif	
	{
	    res = 1;
	}
	else
	{
        switch (is_remove_patches(&thd_data))
        {
        case 1:
            //hot patch function were removed and we call the real execute (restored)
            res = mysql_execute_command(thd);
            break;
        case 2:
            //denied uninstall  plugin
            res = 1;
            break;
        default:
            //everything else
            res = trampoline_mysql_execute_command(thd);
        }
	}
    audit(&thd_data);
    if (pThdPrintedList && pThdPrintedList->cur_index >0)
    {
        pThdPrintedList->cur_index --;
    }
    if(firstTime)
    {
        THDVAR(thd,is_thd_printed_list) = 0;
    }
    return res;

}


#if MYSQL_VERSION_ID < 50600
static void audit_log_slow_statement(THD * thd)
{
    trampoline_log_slow_statement(thd);
    audit_post_execute(thd);
}
#endif

#if MYSQL_VERSION_ID < 50505
static int audit_check_user(THD *thd, enum enum_server_command command,
	       const char *passwd, uint passwd_len, const char *db,
	       bool check_count)
{
	int res = trampoline_check_user (thd, command, passwd, passwd_len, db, check_count);
	ThdSesData ThdData (thd);
    audit (&ThdData);

	return (res);
}
#elif MYSQL_VERSION_ID < 50600
//only for 5.5
//in 5.6: we use audit plugin event to get the login event
static bool audit_acl_authenticate(THD *thd, uint connect_errors, uint com_change_user_pkt_len)
{
    bool res = trampoline_acl_authenticate (thd, connect_errors, com_change_user_pkt_len);
    ThdSesData ThdData (thd);
    audit (&ThdData);
	return (res);
}
#endif

static bool parse_thd_offsets_string (char *poffsets_string)
{
		
    char  offset_str [2048] = {0};
	char *poffset_str = offset_str;
	strncpy (poffset_str,poffsets_string,2048);
	char * comma_delimiter = strchr (poffset_str,',');
	size_t i =0;
	OFFSET *pOffset;
	size_t len = strlen (poffset_str);

	for (size_t j=0;j<len;j++)
	{
		if (!((poffset_str[j] >= '0' && poffset_str[j] <='9') || poffset_str[j] == ' ' || poffset_str[j] == ','))
			return false;
	}
	while (comma_delimiter !=NULL)
	{
		*comma_delimiter = '\0';
		pOffset = (OFFSET*)&Audit_formatter::thd_offsets.query_id + i;
		if ((size_t)pOffset- (size_t)&Audit_formatter::thd_offsets < sizeof (Audit_formatter::thd_offsets))
		{
			sscanf (poffset_str, "%zu", pOffset);
		}
		else 
		{
			return false;
		}
		i++;
		poffset_str = comma_delimiter + 1;
		comma_delimiter = strchr (poffset_str,',');
	}
	if (poffset_str !=NULL)
	{
		pOffset = &Audit_formatter::thd_offsets.query_id + i;
		if ((size_t)pOffset- (size_t)&Audit_formatter::thd_offsets < sizeof (Audit_formatter::thd_offsets))
		{
			sscanf (poffset_str, "%zu", pOffset);
		}
		else
		{
			return false;
		}
	}
	return true;
}

static bool validate_offsets(const ThdOffsets * offset)
{
	//check that offsets are actually correct. We use a buff of memory as a dummy THD (32K is high enough)
	char buf[32*1024] = {0};
	THD * thd = (THD *)buf;
	//sanity check that offsets match
	
	//we set the thread id to a value using the offset and then check that the value matches what thd_get_thread_id returns	
	const my_thread_id thread_id_test_val = 123456;
	(*(my_thread_id *) (((char *) thd)+ offset->thread_id)) = thread_id_test_val;
	my_thread_id res= thd_get_thread_id(thd);
	if (res != thread_id_test_val)
	{
		sql_print_error(
			"%s Offsets: %s (%s) match thread validation check fails with value: %lu. Skipping offest.",
			log_prefix, offset->version, offset->md5digest, res);
		return false;
	}
	//extended validation via security_context method
	//can be disabled via: audit_validate_offsets_extended=OFF
	if(validate_offsets_extended_enable)
	{
	    const query_id_t query_id_test_val = 789;
	    (*(query_id_t *) (((char *) thd)+ offset->query_id)) = query_id_test_val;
	    Security_context * sctx = (Security_context *) (((unsigned char *) thd) + offset->main_security_ctx);
	    char user_test_val[] = "aud_tusr";
		if(!offset->sec_ctx_user) //use compiled header
		{
			sctx->user = user_test_val;
		}
	    else
		{
			(*(const char **) (((unsigned char *) sctx) + offset->sec_ctx_user)) = user_test_val;
		}
	    char buffer[2048] = {0};
	    thd_security_context(thd, buffer, 2048, 1000);
		
	    //verfiy our buffer contains query id
	    if(strstr(buffer, " 789") == NULL || strstr(buffer, user_test_val) == NULL)
	    {
	        sql_print_error(
                "%s Offsets: %s (%s) sec context validation check fails with value: %s. Skipping offest.",
                log_prefix, offset->version, offset->md5digest, buffer);
	        return false;
	    }
        sql_print_information(
            "%s extended offsets validate res: %s", log_prefix, buffer);
	}
	return true;
}

/**
 * Calculate md5 sum of a file.
 *
 * @file_name: file to calc md5 for
 * @digest_str: string to fill with digest result should be big enought to hold 32 chars
 *
 * @return true on success.
 */
static bool calc_file_md5(const char * file_name, char * digest_str)
{
    File fd;
    unsigned char digest[16] = {0};
    bool ret = false;
    if ((fd = my_open(file_name, O_RDONLY, MYF(MY_WME))) < 0)
    {
        sql_print_error("%s Failed file open: [%s], errno: %d.",
                            log_prefix, file_name, errno);
        return false;
    }

    my_MD5Context context;
    my_MD5Init(&context);
    const size_t buff_size = 16384;
    unsigned char file_buff[buff_size] = {0};

    ssize_t res;
    do
    {
        res = read(fd, file_buff, buff_size);
        if(res > 0)
        {
            my_MD5Update(&context, file_buff, res);
        }
    }
    while(res > 0);
    if(res == 0) //reached end of file
    {
        my_MD5Final(digest, &context);
        ret = true;
    }
    else
    {
        sql_print_error("%s Failed program read. res: %zd, errno: %d.",
                log_prefix, res, errno);
    }
    (void) my_close(fd, MYF(0));
    if(ret) //we got the digest
    {
        for (int j = 0; j < 16; j++)
        {
            sprintf(&(digest_str[j * 2]), "%02x", digest[j]);
        }
    }
    return ret;
}

/**
 * Setup the offsets needs to extract data from THD.
 *
 * return 0 on success otherwise 1
 */
static int setup_offsets()
{
    DBUG_ENTER("setup_offsets");
	sql_print_information ("%s setup_offsets audit_offsets: %s validate_checksum: %d offsets_by_version: %d",
	        log_prefix, offsets_string, validate_checksum_enable, offsets_by_version_enable);

	char digest_str [128] = {0};
	const ThdOffsets * offset;

    //setup digest_str to contain the md5sum in hex
	calc_file_md5(my_progname, digest_str);

    sql_print_information(
        "%s mysqld: %s (%s) ", log_prefix, my_progname, digest_str);

    //if present in my.cnf
    //[mysqld]
    //audit_validate_checksum=1
    // or if
    //audit_checksum=0f4d7e3b17eb36f17aafe4360993a769
    //if (validate_checksum_enable || (checksum_string != NULL && strlen(checksum_string) > 0))
    //{

    //if present the offset_string specified in my.cnf 
    //[mysqld]
    //audit_offsets=6200, 6264, 3672, 3944, 88, 2048

	if (offsets_string != NULL)
    {
        if (checksum_string != NULL && strlen(checksum_string) > 0)
        {
            if (strncasecmp(checksum_string, digest_str, 32))
            {
                sql_print_information(
                        "%s checksum check failed for %s, but found %s",
                        log_prefix, checksum_string, digest_str);
                DBUG_RETURN(1);
            }
        }
		if (parse_thd_offsets_string (offsets_string)) 
		{
			sql_print_information ("%s setup_offsets Audit_formatter::thd_offsets values: %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu", log_prefix,
				Audit_formatter::thd_offsets.query_id,
				Audit_formatter::thd_offsets.thread_id,
				Audit_formatter::thd_offsets.main_security_ctx, 
				Audit_formatter::thd_offsets.command,
				Audit_formatter::thd_offsets.lex,
				Audit_formatter::thd_offsets.lex_comment,
				Audit_formatter::thd_offsets.sec_ctx_user,
				Audit_formatter::thd_offsets.sec_ctx_host,
				Audit_formatter::thd_offsets.sec_ctx_ip,
				Audit_formatter::thd_offsets.sec_ctx_priv_user);

			if (!validate_offsets(&Audit_formatter::thd_offsets))
			{
				sql_print_error("%s Offsets set didn't pass validation. audit_offsets: %s .", log_prefix, offsets_string);
				DBUG_RETURN(1);
			}
		}
		else
		{
			sql_print_error("%s Failed parsing audit_offsets: %s", log_prefix, offsets_string);
			DBUG_RETURN(1);
		}
		sql_print_information ("%s Validation passed. Using offsets from audit_offsets: %s",log_prefix, offsets_string);
		DBUG_RETURN(0);
        //exit from function 
	}
	
    size_t arr_size =  thd_offsets_arr_size;
    //iterate and search for the first offset which matches our checksum
    if(validate_checksum_enable && strlen(digest_str) > 0)
    {
        for(size_t i=0; i < arr_size; i++)
        {
            offset = thd_offsets_arr + i;
            if (strlen(offset->md5digest) >0)
            {
                if (!strncasecmp(digest_str, offset->md5digest, 32))
                {
                    sql_print_information("%s Checksum verified. Using offsets from offset version: %s (%s)", log_prefix, offset->version, digest_str);
                    Audit_formatter::thd_offsets = *offset;
                    DBUG_RETURN(0);
                    //return
                }
            }
        }
    }
    if(offsets_by_version_enable)
    {
        bool server_is_ndb = strstr(server_version, "ndb") != NULL;
        for(size_t i=0; i < arr_size; i++)
        {
            offset = thd_offsets_arr + i;
            const char * version = offset->version;
            bool version_is_ndb = strstr(offset->version, "ndb") != NULL;
            const char * dash = strchr(version, '-');
            char version_stripped[16] = {0};
            if(dash) //we use the version string up to the '-'
            {
                size_t tocopy = dash - version;
                if(tocopy > 15) tocopy = 15; //sanity
                strncpy(version_stripped, version, tocopy);
                version = version_stripped;
            }
            if(strstr(server_version, version))
            {
                if(server_is_ndb == version_is_ndb)
                {
                    if (validate_offsets(offset))
                    {
                        sql_print_information("%s Using offsets from offset version: %s (%s)", log_prefix, offset->version, offset->md5digest);
                        Audit_formatter::thd_offsets = *offset;
                        DBUG_RETURN(0);
                    }
                    else
                    {
                        //try doing 24 byte decrement on THD offsets. Seen that on Ubuntu/Debian this is valid. On 5.6 this is 16 bytes.
						#if MYSQL_VERSION_ID < 50600
							OFFSET dec = 24;
						#else
							OFFSET dec = 16;
						#endif
                        ThdOffsets decoffsets = *offset;
                        decoffsets.query_id -= dec;
                        decoffsets.thread_id -= dec;
                        decoffsets.main_security_ctx -= dec;
                        decoffsets.command -= dec;
                        if (validate_offsets(&decoffsets))
                        {
                            Audit_formatter::thd_offsets = decoffsets;
                            sql_print_information("%s Using decrement (%zu) offsets from offset version: %s (%s) values: %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu",
                                log_prefix, dec, offset->version, offset->md5digest,
                                Audit_formatter::thd_offsets.query_id,
                                Audit_formatter::thd_offsets.thread_id,
                                Audit_formatter::thd_offsets.main_security_ctx,
                                Audit_formatter::thd_offsets.command,
                                Audit_formatter::thd_offsets.lex,
                                Audit_formatter::thd_offsets.lex_comment,
								Audit_formatter::thd_offsets.sec_ctx_user,
								Audit_formatter::thd_offsets.sec_ctx_host,
								Audit_formatter::thd_offsets.sec_ctx_ip,
								Audit_formatter::thd_offsets.sec_ctx_priv_user);

                            DBUG_RETURN(0);
                        }
                    }
                }//ndb check
#if defined(__x86_64__) && MYSQL_VERSION_ID > 50505
                else if(server_is_ndb)
                {
                    //in 64bit 5.5 we've seen ndb has an offset of 32 on first 2 values
                    OFFSET inc = 32;
                    ThdOffsets incoffsets = *offset;
                    incoffsets.query_id += inc;
                    incoffsets.thread_id += inc;
                    if (validate_offsets(&incoffsets))
                    {
                        Audit_formatter::thd_offsets = incoffsets;
                        sql_print_information("%s Using increment (%zu) offsets from offset version: %s (%s) values: %zu %zu %zu %zu %zu %zu %zu %zu %zu %zu",
                            log_prefix, inc, offset->version, offset->md5digest,
                            Audit_formatter::thd_offsets.query_id,
                            Audit_formatter::thd_offsets.thread_id,
                            Audit_formatter::thd_offsets.main_security_ctx,
                            Audit_formatter::thd_offsets.command,
                            Audit_formatter::thd_offsets.lex,
                            Audit_formatter::thd_offsets.lex_comment,
							Audit_formatter::thd_offsets.sec_ctx_user,
							Audit_formatter::thd_offsets.sec_ctx_host,
							Audit_formatter::thd_offsets.sec_ctx_ip,
							Audit_formatter::thd_offsets.sec_ctx_priv_user);
                        DBUG_RETURN(0);
                    }
                }
#endif
            }
        }
    }
	
    sql_print_information("%s Couldn't find proper THD offsets for: %s", log_prefix, server_version);
    DBUG_RETURN(1);
}


const char * retrieve_command (THD * thd, bool & is_sql_cmd)
{
    const char *cmd = NULL;
	is_sql_cmd = false;
    int command = Audit_formatter::thd_inst_command(thd);
    if (command < 0 || command > COM_END)
    {
        command = COM_END;
    }
    //check if from query cache. If so set to select and return
    if(THDVAR(thd, query_cache_table_list) != 0)
    {
        return "select";
    }
    const int sql_command = thd_sql_command(thd);
    if (sql_command >=0 && sql_command < MAX_COM_STATUS_VARS_RECORDS )
    {
		is_sql_cmd = true;
        cmd = com_status_vars_array[sql_command].name;
    }
    if(!cmd)
    {
        cmd = command_name[command].str;
    }
	const char * user = Audit_formatter::thd_inst_main_security_ctx_user(thd);
	const char * priv_user = Audit_formatter::thd_inst_main_security_ctx_priv_user(thd);
    if (strcmp (cmd, "Connect") ==0 && ((user && strcmp(user, "event_scheduler") != 0) && (priv_user == NULL || *priv_user == 0x0)))
    {
        cmd = "Failed Login";
    }
    return cmd;
}

static int set_com_status_vars_array ()
{
    DBUG_ENTER("set_com_status_vars_array");
    SHOW_VAR *com_status_vars;
    int sv_idx =0;
    while (strcmp (status_vars[sv_idx].name,"Com") !=0 && status_vars[sv_idx].name != NullS)
    {
        sv_idx ++;
    }
    if (strcmp (status_vars[sv_idx].name,"Com")==0)
    {
        com_status_vars = (SHOW_VAR*)status_vars[sv_idx].value;
        int status_vars_index =0;
        //we use "select" as 0 offset (SQLCOM_SELECT=0)

        while(strcmp(com_status_vars[status_vars_index].name,"select") !=0 && com_status_vars[status_vars_index].name != NullS)
        {
            status_vars_index ++;
        }
        if(strcmp(com_status_vars[status_vars_index].name,"select") !=0)
        {
            sql_print_error("%s Failed finding 'select' index in com_status_vars: [%p]. Plugin Init failed.",
                                   log_prefix, com_status_vars);
            DBUG_RETURN (1);
        }
        size_t initial_offset = (size_t) com_status_vars[status_vars_index].value;
        status_vars_index =0;
        while  (com_status_vars[status_vars_index].name != NullS)
        {
            int sql_command_idx = (com_status_vars[status_vars_index].value - (char*) (initial_offset)) / sizeof (ulong);
            if (sql_command_idx >=0 && sql_command_idx < MAX_COM_STATUS_VARS_RECORDS)
            {
                com_status_vars_array [sql_command_idx].name = com_status_vars[status_vars_index].name;
                com_status_vars_array [sql_command_idx].type = com_status_vars[status_vars_index].type;
                com_status_vars_array [sql_command_idx].value = com_status_vars[status_vars_index].value;
            }
            status_vars_index ++;
        }
        sql_print_information("%s Done initializing sql command names. status_vars_index: [%d], com_status_vars: [%p].",
                                               log_prefix, status_vars_index, com_status_vars);
    }
    else
    {
        sql_print_error("%s Failed looking up 'Com' entry in status_vars. Plugin Init failed.",
                       log_prefix);
        DBUG_RETURN (1);
    }
    DBUG_RETURN (0);
}
static int string_to_array(const void *save, void *array,
        int rows, int length)
{
    const char* save_string;
    save_string = *static_cast<const char* const *> (save);
    char* string_array;
    string_array = (char *) array;
    int r = 0;
    if (save_string != NULL)
    {
        int p = 0;
		int last_char=0; //points to the index after last non-whitespace char (used to trim whitespace at end)
        for (int i = 0; save_string[i] != '\0'; i++)
        {
			if (0 == p && isspace(save_string[i]))
			{
				//leading white space. trim it out
				continue;
			}
            if (save_string[i] == ',')
            {
                if (p > 0)
                {
                    string_array[r * length + last_char] = '\0';
					last_char = 0;
                    p = 0;
                    r++;
                    if (r == (rows - 1))
                    {
                        break;
                    }
                }
            }
            else
            {								
                string_array[r * length + p] = tolower(save_string[i]);
                p++;
				if(!isspace(save_string[i]))
				{
					last_char = p;
				}
            }
        }
        if (last_char > 0)
        {
            string_array[r * length + last_char] = '\0';
            r++;
        }
        string_array[r * length + 0] = '\0';
    }
    return r;
}

__attribute__ ((noinline)) static void trampoline_dummy_func_for_mem()
{
    TRAMPOLINE_NOP_DEF
}
//holds memory used for trampoline
static void * trampoline_mem = NULL;
//pointer to current free mem
static void * trampoline_mem_free = NULL;

/**
 * Utility method for hot patching 
 */
static int do_hot_patch(void ** trampoline_func_pp, unsigned int * trampoline_size,  
	void* target_function, void* audit_function,  const char * func_name)
{
	//16 byte align the pointer
	DATATYPE_ADDRESS addrs = (DATATYPE_ADDRESS)trampoline_mem_free + 15;
	*trampoline_func_pp = (void*)(addrs & ~0x0F);		
    //hot patch functions 
    
    int res = hot_patch_function(target_function, audit_function,
            *trampoline_func_pp, trampoline_size, true);
    if (res != 0)
    {
        //hot patch failed.
        sql_print_error("%s unable to hot patch %s (%p). res: %d.",
                log_prefix, func_name, target_function, res);
        return 1;
    }
    sql_print_information(
            "%s hot patch for: %s (%p) complete. Audit func: %p, Trampoline address: %p size: %u.",
            log_prefix, func_name, target_function, audit_function, *trampoline_func_pp, *trampoline_size);
	trampoline_mem_free = (void *)(((DATATYPE_ADDRESS)*trampoline_func_pp) + *trampoline_size + jump_size());
	return 0;
}

#define DECLARE_STRING_ARR_UPDATE_FUNC(NAME) \
static void NAME ## _string_update(THD *thd, struct st_mysql_sys_var *var, void *tgt, const void *save)\
{\
	num_ ## NAME = string_to_array(save, NAME ## _array, array_elements( NAME ## _array), sizeof( NAME ## _array[0]));\
	strncpy( NAME ##_buff , *static_cast<char*const*>(save), array_elements( NAME ## _buff) - 1);\
	NAME ## _string = NAME ##_buff;\
    sql_print_information("%s Set " #NAME " num: %d, value: %s", log_prefix, num_ ## NAME, NAME ## _string);\
}

DECLARE_STRING_ARR_UPDATE_FUNC(delay_cmds)
DECLARE_STRING_ARR_UPDATE_FUNC(whitelist_cmds)
DECLARE_STRING_ARR_UPDATE_FUNC(record_cmds)
DECLARE_STRING_ARR_UPDATE_FUNC(password_masking_cmds)
DECLARE_STRING_ARR_UPDATE_FUNC(whitelist_users)
DECLARE_STRING_ARR_UPDATE_FUNC(record_objs)

static void password_masking_regex_string_update(THD *thd, struct st_mysql_sys_var *var, void *tgt, const void *save)
{
	const char * str_val = *static_cast<char*const*>(save);
	const char * str = str_val;	
	//copy str to buffer only if str is not pointing to buff
	if(str != password_masking_regex_buff)
	{
		strncpy( password_masking_regex_buff , str, array_elements( password_masking_regex_buff) - 1);
	}	
	password_masking_regex_string = password_masking_regex_buff;		
	password_masking_regex_compile();			
	sql_print_information("%s Set password_masking_regex  value: [%s]", log_prefix, str_val);
}

static void replace_char(char * str, const char tofind, const char rplc)
{
	size_t n = strlen(str);
	for(size_t i = 0; i< n; i++)
	{
		if(tofind == str[i])
		{
			str[i] = rplc;
		}
	}
}

static void json_socket_name_update(THD *thd, struct st_mysql_sys_var *var, void *tgt, const void *save)
{
	const char * str_val = *static_cast<char*const*>(save);
	const char * str = str_val;
	const size_t buff_len = array_elements( json_socket_name_buff) -1;
	//copy str to buffer only if str is not pointing to buff	
	if(NULL == str)
	{
		json_socket_name_buff[0] = '\0';
	}
	else if(str != json_socket_name_buff)
	{
		strncpy( json_socket_name_buff , str, buff_len);
	}
	if(strlen(json_socket_name_buff) == 0 && (mysqld_port > 0 || mysqld_unix_port)) //set default
	{
		const char * name_prefix = "/tmp/mysql.audit_";
		
		size_t indx = strlen(name_prefix); //count how much to move forward the buff
		strncpy( json_socket_name_buff, name_prefix, buff_len);
		char cwd_buff[512] = {0};
		my_getwd(cwd_buff, array_elements(cwd_buff) - 1, 0);
		replace_char(cwd_buff, '/', '_');
		size_t cwd_len = strlen(cwd_buff);
		if(cwd_len > 0 && '_' != cwd_buff[cwd_len-1]) //add _ to end
		{
			strncpy(cwd_buff + cwd_len, "_", array_elements(cwd_buff) - 1  - cwd_len);
		}
		strncpy(json_socket_name_buff + indx, cwd_buff, buff_len  - indx);
		indx += cwd_len;
		if(indx < buff_len)
		{
			if(mysqld_port > 0)
			{
				snprintf(json_socket_name_buff + indx, buff_len  - indx, "%u", mysqld_port);
			}
			else
			{
				strncpy(json_socket_name_buff + indx,  mysqld_unix_port, buff_len  - indx);
				replace_char(json_socket_name_buff + indx, '/', '_');
			}
		}
		else //should never happen
		{
			sql_print_error("%s json_socket_name_buff not big enough to set default name. buff: %s",
                log_prefix, json_socket_name_buff);
		}		
	}
	json_socket_handler.m_io_dest = json_socket_name_buff;			
	sql_print_information("%s Set json_socket_name str: [%s] value: [%s]", log_prefix, str, json_socket_handler.m_io_dest);
}

//check that the regex compiles. Return 0 on success.
static int password_masking_regex_check(THD* thd, struct st_mysql_sys_var * var, void* save, st_mysql_value* value)
{
	int length= array_elements(password_masking_regex_check_buff);
	const char * str = value->val_str(value, password_masking_regex_check_buff, &length);
	*(const char**)save= str;	
	if(NULL == str || str[0] == '\0') //empty string is fine (means disabled)
	{
		return 0;
	}
	int res = 1;
	pcre * preg = Audit_json_formatter::regex_compile(str);
	if(preg)
	{
		res = 0;
	}
	pcre_free(preg);
	return res;
}

//extended method to set also record_empty_objs_set
static void record_objs_string_update_extended(THD *thd, struct st_mysql_sys_var *var, void *tgt, const void *save)
{
	record_objs_string_update(thd, var, tgt, save);
	if(num_record_objs > 0) //check if to record also the empty set of objects
	{
		const char *objects[] = {"{}", NULL};
		record_empty_objs_set = check_array(objects, (const char *) record_objs_array, sizeof(record_objs_array[0]));
	}
	else
	{
			record_empty_objs_set = true;
	}
	sql_print_information("%s Set record_empty_objs: %d", log_prefix, record_empty_objs_set);
}


/*
 Initialize the plugin installation.

 SYNOPSIS
 audit_plugin_init()

 RETURN VALUE
 0                    success
 1                    failure
 */
 static int audit_plugin_init(void *p)
{

    DBUG_ENTER("audit_plugin_init");
	
	#ifdef __x86_64__
		const char * arch = "64bit";
	#else
		const char * arch = "32bit";
	#endif
	
	int interface_ver = audit_plugin.interface_version ;
#if MYSQL_VERSION_ID < 50600
	interface_ver = interface_ver >> 8;	
#endif
    sql_print_information(
            "%s starting up. Version: %s , Revision: %s (%s). AUDIT plugin interface version: %d (0x%x). MySQL Server version: %s.",
            log_prefix, MYSQL_AUDIT_PLUGIN_VERSION,
            MYSQL_AUDIT_PLUGIN_REVISION, arch, interface_ver, interface_ver,
            server_version);
    //setup our offsets.

    if(setup_offsets() != 0)
    {
        DBUG_RETURN(1);
    }
  if (delay_cmds_string != NULL) {
	delay_cmds_string_update(NULL, NULL, NULL, &delay_cmds_string);    
  }
  if (whitelist_cmds_string != NULL) {
	whitelist_cmds_string_update(NULL, NULL, NULL, &whitelist_cmds_string);    
  }
  if (record_cmds_string != NULL) {
	record_cmds_string_update(NULL, NULL, NULL, &record_cmds_string);    
  }  
  if (whitelist_users_string != NULL) {
    whitelist_users_string_update(NULL, NULL, NULL, &whitelist_users_string);
  }
  if (record_objs_string != NULL) {
	record_objs_string_update_extended(NULL, NULL, NULL, &record_objs_string);
  }
  if (NULL != password_masking_cmds_string) {
	password_masking_cmds_string_update(NULL, NULL, NULL, &password_masking_cmds_string);    
  }
  if (NULL != password_masking_regex_string) {
	password_masking_regex_string_update(NULL, NULL, NULL, &password_masking_regex_string);
  }
  //update to generate the default if needed
  json_socket_name_update(NULL, NULL, NULL, &(json_socket_handler.m_io_dest));
  
  //set the password masking callback for json formatters
  json_formatter.m_perform_password_masking = check_do_password_masking;
   
    //setup audit handlers (initially disabled)
    int res = json_file_handler.init(&json_formatter);
    if (res != 0)
    {
        sql_print_error(
                "%s unable to init json file handler. res: %d. Aborting.",
                log_prefix, res);
        DBUG_RETURN(1);
    }
    res = json_socket_handler.init(&json_formatter);
    if (res != 0)
    {
        sql_print_error(
                "%s unable to init json socket handler. res: %d. Aborting.",
                log_prefix, res);
        DBUG_RETURN(1);
    }
    //enable according to what we have in *file_handler_enable (this is set accordingly by sysvar functionality)
    json_file_handler.set_enable(json_file_handler_enable);
    json_socket_handler.set_enable(json_socket_handler_enable);
    Audit_handler::m_audit_handler_list[Audit_handler::JSON_FILE_HANDLER]
            = &json_file_handler;
    Audit_handler::m_audit_handler_list[Audit_handler::JSON_SOCKET_HANDLER]
            = &json_socket_handler;
	
	//align our trampoline mem on its own page
	const unsigned long page_size = GETPAGESIZE();
	const unsigned long std_page_size = 4096;
	if(page_size <= std_page_size)
	{
		//use static executable memory we alocated via trampoline_dummy_func_for_mem
		DATATYPE_ADDRESS addrs = (DATATYPE_ADDRESS)trampoline_dummy_func_for_mem + (page_size - 1);	
		trampoline_mem = (void*)(addrs & ~(page_size - 1));
		sql_print_information(
				"%s mem func addr: %p mem start addr: %p page size: %ld",
				log_prefix, trampoline_dummy_func_for_mem, trampoline_mem, page_size);
	}
	else //big pages for some reason. allocate mem using mmap
	{	
		trampoline_mem = mmap(NULL, page_size, PROT_READ|PROT_EXEC,  MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
		if(MAP_FAILED == trampoline_mem)
		{
			sql_print_error("%s unable to mmap memory size: %lu, errno: %d. Aborting.",
					log_prefix, page_size, errno);
			DBUG_RETURN(1);
		}
		else
		{
			sql_print_information(
				"%s mem via mmap: %p page size: %ld", log_prefix, trampoline_mem, page_size);
		}
	}
	trampoline_mem_free = trampoline_mem;
	//hot patch stuff
	void * target_function = NULL;
	
	if(do_hot_patch((void **)&trampoline_mysql_execute_command, &trampoline_mysql_execute_size,  
		(void *)mysql_execute_command, (void *)audit_mysql_execute_command,  "mysql_execute_command"))
	{
		DBUG_RETURN(1);
	}
	
#if MYSQL_VERSION_ID < 50600
	if(do_hot_patch((void **)&trampoline_log_slow_statement, &trampoline_log_slow_statement_size,  
		(void *)log_slow_statement, (void *)audit_log_slow_statement,  "log_slow_statement"))
	{
		sql_print_error("%s Failed hot patch. Continuing as non-critical.",
                log_prefix);		
				
	}
#endif
	    
 
#if MYSQL_VERSION_ID < 50505				
	if(do_hot_patch((void **)&trampoline_check_user, &trampoline_check_user_size,  
		(void *)check_user, (void *)audit_check_user,  "check_user"))
	{
		DBUG_RETURN(1);
	}	
#elif MYSQL_VERSION_ID < 50600
	if(do_hot_patch((void **)&trampoline_acl_authenticate, &trampoline_acl_authenticate_size,  
		(void *)acl_authenticate, (void *)audit_acl_authenticate,  "acl_authenticate"))
	{
		DBUG_RETURN(1);
	}
#endif
	int (Query_cache::*pf_send_result_to_client)(THD *,char *, uint) = &Query_cache::send_result_to_client;	
	target_function = *(void **)  &pf_send_result_to_client;
	if(do_hot_patch((void **)&trampoline_send_result_to_client, &trampoline_send_result_to_client_size,  
		(void *)target_function, (void *)audit_send_result_to_client,  "send_result_to_client"))
	{
		DBUG_RETURN(1);
	}
	
    if(do_hot_patch((void **)&trampoline_check_table_access, &trampoline_check_table_access_size,  
		(void *)check_table_access, (void *)audit_check_table_access,  "check_table_access"))
	{
		DBUG_RETURN(1);
	}
		
#if MYSQL_VERSION_ID > 50505				
	target_function = (void *)*(bool (*)(THD *thd, TABLE_LIST **start, uint *counter, uint flags,
                Prelocking_strategy *prelocking_strategy)) &open_tables;
	if(do_hot_patch((void **)&trampoline_open_tables, &trampoline_open_tables_size,  
		(void *)target_function, (void *)audit_open_tables,  "open_tables"))
	{
		DBUG_RETURN(1);
	}		    
#else
    target_function = (void *)*(int (*)(THD *thd, TABLE_LIST **start, uint *counter, uint flags)) &open_tables;
	if(do_hot_patch((void **)&trampoline_open_tables, &trampoline_open_tables_size,  
		(void *)target_function, (void *)audit_open_tables,  "open_tables"))
	{
		DBUG_RETURN(1);
	}    
#endif
    if (set_com_status_vars_array () !=0)
    {
        DBUG_RETURN(1);
    }
	sql_print_information("%s Init completed successfully.", log_prefix);
    DBUG_RETURN(0);
}

/*
 plugin deinstallation.

 SYNOPSIS
 audit_plugin_deinit()
 Does nothing.

 RETURN VALUE
 0                    success
 1                    failure (cannot happen)

 */

static int audit_plugin_deinit(void *p)
{	
    DBUG_ENTER("audit_plugin_deinit");	
	sql_print_information("%s deinit", log_prefix);
	remove_hot_functions();	
	DBUG_RETURN(0);    
}

/*
 Plugin status variables for SHOW STATUS
 */

static struct st_mysql_show_var audit_status[] =
{
{ "Audit_version",
        (char *) MYSQL_AUDIT_PLUGIN_VERSION "-" MYSQL_AUDIT_PLUGIN_REVISION,
        SHOW_CHAR },
{ "Audit_protocol_version",
		(char *) AUDIT_PROTOCOL_VERSION,
		SHOW_CHAR },
//{"called",     (char *)&number_of_calls, SHOW_LONG},
        { 0, 0, (enum_mysql_show_type) 0 } };



static void json_log_file_enable(THD *thd, struct st_mysql_sys_var *var,
        void *tgt, const void *save)
{
    json_file_handler_enable = *(my_bool *) save ? TRUE : FALSE;
    if(json_file_handler.is_init())
    {
        json_file_handler.set_enable(json_file_handler_enable);
    }
}

static void json_log_file_flush(THD *thd, struct st_mysql_sys_var *var,
        void *tgt, const void *save)
{
	//always set to false. as we just flush if set to true and leave at 0
    json_file_handler_flush = FALSE;
	my_bool val = *(my_bool *) save ? TRUE : FALSE;
    if(val && json_file_handler.is_init())
    {
        json_file_handler.flush();
    }
}




static void json_log_socket_enable(THD *thd, struct st_mysql_sys_var *var,
        void *tgt, const void *save)
{
    json_socket_handler_enable = *(my_bool *) save ? TRUE : FALSE;
    if(json_socket_handler.is_init())
    {
        json_socket_handler.set_enable(json_socket_handler_enable);
    }
}


//setup sysvars which update directly the relevant plugins

static MYSQL_SYSVAR_BOOL(header_msg, json_formatter.m_write_start_msg,
             PLUGIN_VAR_RQCMDARG,
        "AUDIT write header message at start of logging or file flush Enable|Disable. Default enabled.", NULL, NULL, 1);

static MYSQL_SYSVAR_BOOL(force_record_logins, force_record_logins_enable,
             PLUGIN_VAR_RQCMDARG,
        "AUDIT force record Connect, Quit and Failed Login commands, regardless of the settings in audit_record_cmds and audit_record_objs  Enable|Disable. Default disabled.", NULL, NULL, 0);

static MYSQL_SYSVAR_STR(json_log_file, json_file_handler.m_io_dest,
        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
        "AUDIT plugin json log file name",
        NULL, NULL, "mysql-audit.json");

static MYSQL_SYSVAR_UINT(json_file_sync, json_file_handler.m_sync_period,
        PLUGIN_VAR_RQCMDARG,
        "AUDIT plugin json log file sync period. If the value of this variable is greater than 0, audit log will sync to disk after every audit_json_file_sync writes.",
        NULL, NULL, 0, 0, UINT_MAX32, 0);

static MYSQL_SYSVAR_UINT(json_file_retry, json_file_handler.m_retry_interval,
        PLUGIN_VAR_RQCMDARG,
        "AUDIT plugin json log file retry interval. If the plugin fails to open/write to the json log file, will retry to open every specified interval in seconds. Set for 0 to disable retrying. Default 60 seconds.",
        NULL, NULL, 60, 0, UINT_MAX32, 0);

static MYSQL_SYSVAR_UINT(json_socket_retry, json_socket_handler.m_retry_interval,
        PLUGIN_VAR_RQCMDARG,
        "AUDIT plugin json socket connect interval. If the plugin fails to connect/write to the json audit socket, will retry to connect every specified interval in seconds. Set for 0 to disable retrying. Default 10 seconds.",
        NULL, NULL, 10, 0, UINT_MAX32, 0);

static MYSQL_SYSVAR_BOOL(json_file, json_file_handler_enable,
			 PLUGIN_VAR_RQCMDARG,
        "AUDIT plugin json log file Enable|Disable", NULL, json_log_file_enable, 0);
		
static MYSQL_SYSVAR_BOOL(json_file_flush, json_file_handler_flush,
			 PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_NOCMDOPT,
        "AUDIT plugin json log file flush. Set to ON to perform a flush of the log.", NULL, json_log_file_flush, 0);


static MYSQL_SYSVAR_STR(json_socket_name, json_socket_handler.m_io_dest,
        PLUGIN_VAR_RQCMDARG,
        "AUDIT plugin json log unix socket name",
        NULL, json_socket_name_update, "");

static MYSQL_SYSVAR_STR(offsets, offsets_string,
        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY  | PLUGIN_VAR_MEMALLOC,
        "AUDIT plugin offsets. Comma separated list of offsets to use for extracting data",
        NULL, NULL, NULL);

static MYSQL_SYSVAR_STR(checksum, checksum_string,
			PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY  | PLUGIN_VAR_MEMALLOC,
			"AUDIT plugin checksum. Checksum for mysqld corresponding to offsets",
			NULL, NULL, "");

#define _COMMENT_SPACE_ "(?:/\\*.*?\\*/|\\s)*?"
#define _QUOTED_PSW_ "[\'|\"](?<psw>.*?)(?<!\\\\)[\'|\"]"
			
static MYSQL_SYSVAR_STR(password_masking_regex, password_masking_regex_string,
			PLUGIN_VAR_RQCMDARG ,
			"AUDIT plugin regex to use for password masking",
			password_masking_regex_check, password_masking_regex_string_update, 
			//identified by [password] '***'
			"identified"_COMMENT_SPACE_"by"_COMMENT_SPACE_"(?:password)?"_COMMENT_SPACE_ _QUOTED_PSW_
			//password function
			"|password"_COMMENT_SPACE_"\\("_COMMENT_SPACE_ _QUOTED_PSW_ _COMMENT_SPACE_"\\)"
			//Used at: CHANGE MASTER TO MASTER_PASSWORD='new3cret', SET PASSWORD [FOR user] = 'hash', password 'user_pass';
			"|password"_COMMENT_SPACE_"(?:for"_COMMENT_SPACE_"\\S+?)?"_COMMENT_SPACE_"="_COMMENT_SPACE_ _QUOTED_PSW_
			"|password"_COMMENT_SPACE_ _QUOTED_PSW_
			//federated engine create table with connection. See: http://dev.mysql.com/doc/refman/5.5/en/federated-create-connection.html
			//commented out as federated engine is disabled by default 
			//"|ENGINE"_COMMENT_SPACE_"="_COMMENT_SPACE_"FEDERATED"_COMMENT_SPACE_".*CONNECTION"_COMMENT_SPACE_"="_COMMENT_SPACE_"[\'|\"]\\S+?://\\S+?:(?<psw>.*)@\\S+[\'|\"]"
			);			
			
static MYSQL_SYSVAR_BOOL(uninstall_plugin, uninstall_plugin_enable,
        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY ,
        "AUDIT uninstall plugin Enable|Disable. Default disabled. If disabled attempts to uninstall the AUDIT plugin via the sql UNINSTALL command will fail.", NULL, NULL, 0);


static MYSQL_SYSVAR_BOOL(offsets_by_version, offsets_by_version_enable,
        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY ,
        "AUDIT plugin search offsets by version. If checksum validation doesn't pass will attempt to load and validate offsets according to version. Enable|Disable", NULL, NULL, 1);

static MYSQL_SYSVAR_BOOL(validate_checksum, validate_checksum_enable,
        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY ,
        "AUDIT plugin binary checksum validation Enable|Disable", NULL, NULL, 1);


static MYSQL_SYSVAR_BOOL(validate_offsets_extended, validate_offsets_extended_enable,
        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY ,
        "AUDIT plugin offset extended validation Enable|Disable", NULL, NULL, 1);

static MYSQL_SYSVAR_BOOL(json_socket, json_socket_handler_enable,
			 PLUGIN_VAR_RQCMDARG,
        "AUDIT plugin json log unix socket Enable|Disable", NULL, json_log_socket_enable, 0);

static MYSQL_SYSVAR_INT(delay_ms, delay_ms_val,
        PLUGIN_VAR_RQCMDARG,
        "AUDIT plugin delay in miliseconds. Delay amount injection. If 0 or negative then delay is disabled.",
        NULL, NULL, 0, 0, INT_MAX32, 0);

static MYSQL_SYSVAR_STR(delay_cmds, delay_cmds_string,
        PLUGIN_VAR_RQCMDARG,
        "AUDIT plugin delay commands to match against comma separated. If empty then delay is disabled.",
			NULL, delay_cmds_string_update, NULL);
static MYSQL_SYSVAR_STR(whitelist_cmds, whitelist_cmds_string,
			PLUGIN_VAR_RQCMDARG,
			"AUDIT plugin commands to skip, comma separated",
			NULL, whitelist_cmds_string_update, NULL);
static MYSQL_SYSVAR_STR(record_cmds, record_cmds_string,
			PLUGIN_VAR_RQCMDARG,
			"AUDIT plugin whitelisted commands for which queries are not recorded, comma separated",
			NULL, record_cmds_string_update, NULL);
static MYSQL_SYSVAR_STR(password_masking_cmds, password_masking_cmds_string,
			PLUGIN_VAR_RQCMDARG,
			"AUDIT plugin commands to apply password masking regex to, comma separated",
			NULL, password_masking_cmds_string_update,
			//set passowrd is recoreded as set_option
			"CREATE_USER,GRANT,SET_OPTION,SLAVE_START,CREATE_SERVER,ALTER_SERVER,CHANGE_MASTER");			
static MYSQL_SYSVAR_STR(whitelist_users, whitelist_users_string,
			PLUGIN_VAR_RQCMDARG,
			"AUDIT plugin whitelisted users whose queries are not recorded, comma separated",
			NULL, whitelist_users_string_update, NULL);

static MYSQL_SYSVAR_STR(record_objs, record_objs_string,
			PLUGIN_VAR_RQCMDARG,
			"AUDIT plugin objects to record, comma separated",
			NULL, record_objs_string_update_extended, NULL);

/*
 * Plugin system vars
 */
static struct st_mysql_sys_var* audit_system_variables[] =
{
	MYSQL_SYSVAR(header_msg),
	MYSQL_SYSVAR(force_record_logins),
	MYSQL_SYSVAR(json_log_file),
	MYSQL_SYSVAR(json_file_sync),
	MYSQL_SYSVAR(json_file_retry),
	MYSQL_SYSVAR(json_socket_retry),
	MYSQL_SYSVAR(json_file),
	MYSQL_SYSVAR(json_file_flush),
	MYSQL_SYSVAR(uninstall_plugin),
	MYSQL_SYSVAR(validate_checksum),
	MYSQL_SYSVAR(offsets_by_version),
	MYSQL_SYSVAR(validate_offsets_extended),
	MYSQL_SYSVAR(json_socket_name),
	MYSQL_SYSVAR(offsets),
	MYSQL_SYSVAR(json_socket),
	MYSQL_SYSVAR(query_cache_table_list),
	MYSQL_SYSVAR(is_thd_printed_list),
	MYSQL_SYSVAR(delay_ms),
	MYSQL_SYSVAR(delay_cmds),
        MYSQL_SYSVAR(whitelist_cmds),
        MYSQL_SYSVAR(record_cmds),
	MYSQL_SYSVAR(password_masking_cmds),
        MYSQL_SYSVAR(whitelist_users),
        MYSQL_SYSVAR(record_objs),
        MYSQL_SYSVAR(checksum),
	MYSQL_SYSVAR(password_masking_regex),
	
    NULL 
};

//declare our plugin
mysql_declare_plugin(audit_plugin)
{
    plugin_type,
    &audit_plugin,
    "AUDIT",
    "McAfee Inc",
    "AUDIT plugin, creates a file mysql-audit.log to log activity",
    PLUGIN_LICENSE_GPL,
    audit_plugin_init, /* Plugin Init */
    audit_plugin_deinit, /* Plugin Deinit */
    0x0100 /* 1.0 */,
    audit_status, /* status variables                */
    audit_system_variables, /* system variables                */
    NULL /* config options                  */
}
mysql_declare_plugin_end;

#if MYSQL_VERSION_ID < 50505
/**
 * DLL constructor method.
 * We set here the audit plugin version to the same as the first built in plugin.
 * This is so we can have a single lib for all versions (needed in 5.1)
 */
extern "C" void __attribute__ ((constructor)) audit_plugin_so_init(void)
{
    if (mysqld_builtins && mysqld_builtins[0])
    {
        audit_plugin.interface_version = *(int *) mysqld_builtins[0]->info;
        sql_print_information("%s Set interface version to: %d (%d)",
                log_prefix, audit_plugin.interface_version,
                audit_plugin.interface_version >> 8);
    }
    else
    {
        sql_print_error(
                "%s mysqld_builtins are null. Plugin will not load unless the mysql version is: %d. \n",
                log_prefix, audit_plugin.interface_version >> 8);
    }
}
#elif MYSQL_VERSION_ID < 50600
extern struct st_mysql_plugin *mysql_mandatory_plugins[];
extern "C"  void __attribute__ ((constructor)) audit_plugin_so_init(void)
{

	
	audit_plugin.interface_version = *(int *) mysql_mandatory_plugins[0]->info;
    sql_print_information("%s Set interface version to: %d (%d)",
              log_prefix, audit_plugin.interface_version,
               audit_plugin.interface_version >> 8);

}
#elif !defined(MARIADB_BASE_VERSION)
//interface version for MySQL 5.6 changed in 5.6.14 
extern "C"  void __attribute__ ((constructor)) audit_plugin_so_init(void)
{
	const char * ver_5_6_13 = "5.6.13";
	if(strncmp(server_version, ver_5_6_13, strlen(ver_5_6_13)) <= 0)
	{
		audit_plugin.interface_version = 0x0300;
	}
	else
	{
		audit_plugin.interface_version = 0x0301;
	}
}
#endif

/*
 Pure virtual handler. Needed when running in mysql compiled with a newer version of gcc.
 Versions of mysql for RH 6 and Percona this function is defined local in mysqld. 
 So we define our own implementation.
*/
extern "C" int __cxa_pure_virtual (void)
{
	sql_print_error(
		"%s __cxa_pure_virtual called. Fatal condition. ",
		log_prefix);
	return 0;
}

/*
 * Variable to hold version
 */
MYSQL_AUDIT_PLUGIN_SYMBOL_VERSION() = '\0';





