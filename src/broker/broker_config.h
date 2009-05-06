/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution.
 *
 *   This program is free software; you can redistribute it and/or modify 
 *   it under the terms of the GNU General Public License as published by 
 *   the Free Software Foundation; either version 2 of the License, or 
 *   (at your option) any later version. 
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License 
 *  along with this program; if not, write to the Free Software 
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA 
 *
 */


/*
 * broker_config.h - broker configuration utilities
 */

#ifndef	_BROKER_CONFIG_H_
#define	_BROKER_CONFIG_H_

#define	APPL_SERVER_CAS		0
#define	APPL_SERVER_CAS_ORACLE	1
#define	APPL_SERVER_CAS_MYSQL	2

#define MAX_BROKER_NUM          100

#define	CONF_LOG_FILE_LEN	128

#define	DEFAULT_AS_MIN_NUM	5
#define	DEFAULT_AS_MAX_NUM	40

#define	DEFAULT_SERVER_MAX_SIZE	20
#define	DEFAULT_TIME_TO_KILL	120	/* seconds */
#define SQL_LOG_TIME_MAX	-1

#define CONF_ERR_LOG_NONE       0x00
#define CONF_ERR_LOG_LOGFILE    0x01
#define CONF_ERR_LOG_BROWSER    0x02
#define CONF_ERR_LOG_BOTH       (CONF_ERR_LOG_LOGFILE | CONF_ERR_LOG_BROWSER)

#define SQL_LOG_MODE_OFF		0x00
#define SQL_LOG_MODE_ON			0x01
#define SQL_LOG_MODE_APPEND		0x02
#define SQL_LOG_MODE_BIND_VALUE		0x04

#define DEFAULT_SQL_LOG_MAX_SIZE	100000	/* 100M */
#define MAX_SQL_LOG_MAX_SIZE            2000000

#define BROKER_NAME_LEN		64
#define BROKER_LOG_MSG_SIZE	64

#define CONF_GET_VALUE_ON_OFF(CONF_FIELD, VALUE)	\
	CONF_FIELD = conf_get_value_table_on_off(VALUE)

#define CONF_GET_VALUE_POSITIVE_INT(CONF_FIELD, STR_VALUE, DEFAULT_VAL)	\
	do {							\
	  CONF_FIELD = atoi(STR_VALUE);				\
	  if ((CONF_FIELD) <= 0)				\
	    CONF_FIELD = DEFAULT_VAL;				\
	} while (0)

#define CONF_GET_VALUE_INT(CONF_FIELD, STR_VALUE)	\
	CONF_FIELD = atoi(STR_VALUE)

#define CONF_GET_VALUE_SQL_LOG(SQL_LOG_MODE, CONF_VALUE, SQL_LOG_MODE_VALUE)  \
	do {							\
	  char	_tmp_on_off;					\
	  CONF_GET_VALUE_ON_OFF(_tmp_on_off, CONF_VALUE);	\
	  if (_tmp_on_off == ON)				\
	    SQL_LOG_MODE |= SQL_LOG_MODE_VALUE;			\
	  else if (_tmp_on_off == OFF)				\
	    SQL_LOG_MODE &= ~SQL_LOG_MODE_VALUE;		\
	  else							\
	    SQL_LOG_MODE = -1;					\
	} while (0)

typedef enum t_keep_con_value T_KEEP_CON_VALUE;
enum t_keep_con_value
{
  KEEP_CON_OFF = 0,
  KEEP_CON_ON = 1,
  KEEP_CON_AUTO = 2,
  KEEP_CON_DEFAULT = KEEP_CON_AUTO
};

typedef struct t_broker_info T_BROKER_INFO;
struct t_broker_info
{
  char service_flag;
  char appl_server;
  char auto_add_appl_server;
  char log_backup;
  char access_log;
  char sql_log_mode;
  char stripped_column_name;
  char keep_connection;
  char cache_user_info;
  char sql_log2;
  char statement_pooling;
  char sql_log_single_line;
  char name[BROKER_NAME_LEN];
  int pid;
  int port;
  int appl_server_num;
  int appl_server_min_num;
  int appl_server_max_num;
#if defined (WINDOWS)
  int appl_server_port;
#endif
  int appl_server_shm_id;
  int appl_server_max_size;
  int session_timeout;
  int sql_log_time;
  int job_queue_size;
  int time_to_kill;
  int err_code;
  int os_err_code;
  int sql_log_max_size;
#if defined (WINDOWS)
  int pdh_workset;
  float pdh_pct_cpu;
  int cpu_time;
  int pdh_num_thr;
#endif
  int max_string_length;
  int num_busy_count;
  char log_dir[CONF_LOG_FILE_LEN];
  char err_log_dir[CONF_LOG_FILE_LEN];
  char access_log_file[CONF_LOG_FILE_LEN];
  char error_log_file[CONF_LOG_FILE_LEN];
  char source_env[CONF_LOG_FILE_LEN];
  char acl_file[CONF_LOG_FILE_LEN];

  char jdbc_cache;
  char jdbc_cache_only_hint;
  int jdbc_cache_life_time;
};

#if defined (_UC_ADMIN_SO_)
extern int broker_config_read (T_BROKER_INFO *, int *, int *, char *, char,
			       char *);
#else
extern int broker_config_read (T_BROKER_INFO *, int *, int *, char *);
#endif
int conf_get_value_table_on_off (const char *value);
int conf_get_value_keep_con (const char *value);


#endif /* _BROKER_CONFIG_H_ */
