/* Copyright (c) 2010, 2022, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   Without limiting anything contained in the foregoing, this file,
   which is part of C Driver for MySQL (Connector/C), is also subject to the
   Universal FOSS Exception, version 1.0, a copy of which can be found at
   http://oss.oracle.com/licenses/universal-foss-exception.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file

  Support code for the client side (libmysql) plugins

  Client plugins are somewhat different from server plugins, they are simpler.

  They do not need to be installed or in any way explicitly loaded on the
  client, they are loaded automatically on demand.
  One client plugin per shared object, soname *must* match the plugin name.

  There is no reference counting and no unloading either.
*/

#include "my_config.h"

#include <mysql/client_plugin.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/types.h>

#include "errmsg.h"
#include "m_ctype.h"
#include "m_string.h"
#include "my_alloc.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_io.h"
#include "my_macros.h"
#include "my_psi_config.h"
#include "my_sys.h"
#include "my_thread.h"
#include "mysql.h"
#include "mysql/psi/mysql_memory.h"
#include "mysql/psi/mysql_mutex.h"
#include "mysql/service_mysql_alloc.h"
#include "sql_common.h"
#include "template_utils.h"

#ifdef HAVE_DLFCN_H
#include <dlfcn.h>
#endif

#if defined(CLIENT_PROTOCOL_TRACING)
#include <mysql/plugin_trace.h>
#endif

PSI_memory_key key_memory_root;
PSI_memory_key key_memory_load_env_plugins;

PSI_mutex_key key_mutex_LOCK_load_client_plugin;

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_info all_client_plugin_mutexes[] = {
    {&key_mutex_LOCK_load_client_plugin, "LOCK_load_client_plugin",
     PSI_FLAG_SINGLETON, 0, PSI_DOCUMENT_ME}};

static PSI_memory_info all_client_plugin_memory[] = {
    {&key_memory_root, "root", PSI_FLAG_ONLY_GLOBAL_STAT, 0, PSI_DOCUMENT_ME},
    {&key_memory_load_env_plugins, "load_env_plugins",
     PSI_FLAG_ONLY_GLOBAL_STAT, 0, PSI_DOCUMENT_ME}};

static void init_client_plugin_psi_keys() {
  const char *category = "sql";
  int count;

  count = array_elements(all_client_plugin_mutexes);
  mysql_mutex_register(category, all_client_plugin_mutexes, count);

  count = array_elements(all_client_plugin_memory);
  mysql_memory_register(category, all_client_plugin_memory, count);
}
#endif /* HAVE_PSI_INTERFACE */

struct st_client_plugin_int {
  struct st_client_plugin_int *next;
  void *dlhandle;
  struct st_mysql_client_plugin *plugin;
};

static bool initialized = false;
static MEM_ROOT mem_root;

static const char *plugin_declarations_sym =
    "_mysql_client_plugin_declaration_";
static uint plugin_version[MYSQL_CLIENT_MAX_PLUGINS] = {
    0, /* these two are taken by Connector/C */
    0, /* these two are taken by Connector/C */
    MYSQL_CLIENT_AUTHENTICATION_PLUGIN_INTERFACE_VERSION,
    MYSQL_CLIENT_TRACE_PLUGIN_INTERFACE_VERSION,
};

/*
  Loaded plugins are stored in a linked list.
  The list is append-only, the elements are added to the head (like in a stack).
  The elements are added under a mutex, but the list can be read and traversed
  without any mutex because once an element is added to the list, it stays
  there. The main purpose of a mutex is to prevent two threads from
  loading the same plugin twice in parallel.
*/
struct st_client_plugin_int *plugin_list[MYSQL_CLIENT_MAX_PLUGINS];
static mysql_mutex_t LOCK_load_client_plugin;

static int is_not_initialized(MYSQL *mysql, const char *name) {
  if (initialized) return 0;

  set_mysql_extended_error(mysql, CR_AUTH_PLUGIN_CANNOT_LOAD, unknown_sqlstate,
                           ER_CLIENT(CR_AUTH_PLUGIN_CANNOT_LOAD), name,
                           "not initialized");
  return 1;
}

/**
  finds a plugin in the list

  @param name   plugin name to search for
  @param type   plugin type

  @note this does NOT necessarily need a mutex, take care!

  @retval a pointer to a found plugin or 0
*/
static struct st_mysql_client_plugin *find_plugin(const char *name, int type) {
  struct st_client_plugin_int *p;

  assert(initialized);
  assert(type >= 0 && type < MYSQL_CLIENT_MAX_PLUGINS);
  if (type < 0 || type >= MYSQL_CLIENT_MAX_PLUGINS) return nullptr;

  for (p = plugin_list[type]; p; p = p->next) {
    if (strcmp(p->plugin->name, name) == 0) return p->plugin;
  }
  return nullptr;
}

/**
  verifies the plugin and adds it to the list
  验证插件并将其添加到列表中

  @param mysql          MYSQL structure (for error reporting)
  @param plugin         plugin to install
  @param dlhandle       a handle to the shared object (returned by dlopen)
                        or 0 if the plugin was not dynamically loaded
  @param argc           number of arguments in the 'va_list args'
  @param args           arguments passed to the plugin initialization function
  @param mysql          MYSQL 结构体（用于错误报告）
  @param plugin         要安装的插件
  @param dlhandle       共享对象的句柄（由 dlopen 返回）
                        或 0 如果插件未动态加载
  @param argc           'va_list args' 中参数的数量
  @param args           传递给插件初始化函数的参数

  @retval a pointer to an installed plugin or 0
  @retval 返回已安装插件的指针或 0
*/
static struct st_mysql_client_plugin *do_add_plugin(
    MYSQL *mysql, struct st_mysql_client_plugin *plugin, void *dlhandle,
    int argc, va_list args) {
  const char *errmsg;  // 错误信息
  struct st_client_plugin_int plugin_int, *p;  // 插件结构体
  char errbuf[1024];  // 错误缓冲区

  assert(initialized);  // 确保已初始化

  plugin_int.plugin = plugin;  // 设置插件
  plugin_int.dlhandle = dlhandle;  // 设置动态加载句柄

  if (plugin->type >= MYSQL_CLIENT_MAX_PLUGINS) {
    errmsg = "Unknown client plugin type";  // 插件类型未知
    goto err1;  // 跳转到错误处理
  }

  if (plugin->interface_version < plugin_version[plugin->type] ||
      (plugin->interface_version >> 8) > (plugin_version[plugin->type] >> 8)) {
    errmsg = "Incompatible client plugin interface";  // 插件接口不兼容
    goto err1;  // 跳转到错误处理
  }

#if defined(CLIENT_PROTOCOL_TRACING) && !defined(MYSQL_SERVER)
  /*
    If we try to load a protocol trace plugin but one is already
    loaded (global trace_plugin pointer is not NULL) then we ignore
    the new trace plugin and give error. This is done before the
    new plugin gets initialized.
    如果我们尝试加载一个协议跟踪插件，但已经加载了一个（全局 trace_plugin 指针不为 NULL），
    那么我们将忽略新的跟踪插件并给出错误。这是在新插件初始化之前完成的。
  */
  if (plugin->type == MYSQL_CLIENT_TRACE_PLUGIN && nullptr != trace_plugin) {
    errmsg = "Can not load another trace plugin while one is already loaded";  // 不能同时加载多个跟踪插件
    goto err1;  // 跳转到错误处理
  }
#endif

  /* Call the plugin initialization function, if any */
  /* 调用插件初始化函数（如果有） */
  if (plugin->init && plugin->init(errbuf, sizeof(errbuf), argc, args)) {
    errmsg = errbuf;  // 获取错误信息
    goto err1;  // 跳转到错误处理
  }

  p = (struct st_client_plugin_int *)memdup_root(&mem_root, &plugin_int,
                                                 sizeof(plugin_int));  // 复制插件信息

  if (!p) {
    errmsg = "Out of memory";  // 内存不足
    goto err2;  // 跳转到错误处理
  }

  mysql_mutex_assert_owner(&LOCK_load_client_plugin);  // 确保当前线程拥有锁

  p->next = plugin_list[plugin->type];  // 将新插件添加到列表
  plugin_list[plugin->type] = p;  // 更新插件列表
  net_clear_error(&mysql->net);  // 清除网络错误

#if defined(CLIENT_PROTOCOL_TRACING) && !defined(MYSQL_SERVER)
  /*
    If loaded plugin is a protocol trace one, then set the global
    trace_plugin pointer to point at it. When trace_plugin is not NULL,
    each new connection will be traced using the plugin pointed by it
    (see MYSQL_TRACE_STAGE() macro in libmysql/mysql_trace.h).
    如果加载的插件是协议跟踪插件，则将全局 trace_plugin 指针指向它。
    当 trace_plugin 不为 NULL 时，每个新连接将使用指向它的插件进行跟踪
    （参见 libmysql/mysql_trace.h 中的 MYSQL_TRACE_STAGE() 宏）。
  */
  if (plugin->type == MYSQL_CLIENT_TRACE_PLUGIN) {
    trace_plugin = (struct st_mysql_client_plugin_TRACE *)plugin;  // 设置跟踪插件
  }
#endif

  return plugin;  // 返回已安装的插件

err2:
  if (plugin->deinit) plugin->deinit();  // 调用去初始化函数（如果有）
err1:
  set_mysql_extended_error(mysql, CR_AUTH_PLUGIN_CANNOT_LOAD, unknown_sqlstate,
                           ER_CLIENT(CR_AUTH_PLUGIN_CANNOT_LOAD), plugin->name,
                           errmsg);  // 设置扩展错误信息
  if (dlhandle) dlclose(dlhandle);  // 关闭动态加载句柄
  return nullptr;  // 返回空指针
}


/**
  Adds a plugin without arguments.

  @param mysql          MYSQL structure (for error reporting)
  @param plugin         plugin to install
  @param dlhandle       a handle to the shared object (returned by dlopen)
                        or 0 if the plugin was not dynamically loaded
  @param argc           number of arguments in the 'va_list args'
  @param ...            variable arguments passed to the plugin initialization function

  @retval a pointer to an installed plugin or 0
*/
static struct st_mysql_client_plugin *add_plugin_noargs(
    MYSQL *mysql, struct st_mysql_client_plugin *plugin, void *dlhandle,
    int argc, ...) {
  struct st_mysql_client_plugin *retval = nullptr;  // 声明一个指向客户端插件的指针，初始化为nullptr
  va_list ap;  // 声明一个可变参数列表
  va_start(ap, argc);  // 初始化可变参数列表，argc是最后一个固定参数
  retval = do_add_plugin(mysql, plugin, dlhandle, argc, ap);  // 调用do_add_plugin函数添加插件
  va_end(ap);  // 结束可变参数列表的处理
  return retval;  // 返回添加的插件指针
}

static struct st_mysql_client_plugin *add_plugin_withargs(
    MYSQL *mysql, struct st_mysql_client_plugin *plugin, void *dlhandle,
    int argc, va_list args) {
  return do_add_plugin(mysql, plugin, dlhandle, argc, args);
}

/**
  Loads plugins which are specified in the environment variable
  LIBMYSQL_PLUGINS.

  Multiple plugins must be separated by semicolon. This function doesn't
  return or log an error.

  The function is be called by mysql_client_plugin_init

  @todo
  Support extended syntax, passing parameters to plugins, for example
  LIBMYSQL_PLUGINS="plugin1(param1,param2);plugin2;..."
  or
  LIBMYSQL_PLUGINS="plugin1=int:param1,str:param2;plugin2;..."
*/
static void load_env_plugins(MYSQL *mysql) {
  char *plugs, *free_env, *s = getenv("LIBMYSQL_PLUGINS");  // 获取环境变量LIBMYSQL_PLUGINS的值
  char *enable_cleartext_plugin = getenv("LIBMYSQL_ENABLE_CLEARTEXT_PLUGIN");  // 获取环境变量LIBMYSQL_ENABLE_CLEARTEXT_PLUGIN的值

  // 如果启用明文插件的环境变量存在且值为"1"、"Y"或"y"，则设置libmysql_cleartext_plugin_enabled为true
  if (enable_cleartext_plugin && strchr("1Yy", enable_cleartext_plugin[0]))
    libmysql_cleartext_plugin_enabled = true;

  /* no plugins to load */
  if (!s) return;  // 如果没有插件需要加载，则返回

  // 复制环境变量字符串，以便后续处理
  free_env = plugs = my_strdup(key_memory_load_env_plugins, s, MYF(MY_WME));

  do {
    s = strchr(plugs, ';');  // 查找下一个分号
    if (s != nullptr) *s = '\0';  // 如果找到分号，将其替换为字符串结束符
    mysql_load_plugin(mysql, plugs, -1, 0);  // 加载插件
    if (s != nullptr) plugs = s + 1;  // 更新plugs指针，指向下一个插件名称
  } while (s != nullptr);  // 继续循环，直到没有更多的分号

  my_free(free_env);  // 释放复制的环境变量字符串
}

/********** extern functions to be used by libmysql *********************/

/**
  Initializes the client plugin layer. 
  初始化客户端插件层。

  This function must be called before any other client plugin function. 
  此函数必须在任何其他客户端插件函数之前调用。

  @retval 0    successful 
  @retval != 0 error occurred 
*/
int mysql_client_plugin_init() {
  MYSQL mysql;  // 声明一个MYSQL结构体，用于存储MySQL连接信息
  struct st_mysql_client_plugin **builtin;  // 声明一个指向客户端插件结构体的指针

  if (initialized) return 0;  // 如果已经初始化，直接返回0

#ifdef HAVE_PSI_INTERFACE
  init_client_plugin_psi_keys();  // 初始化PSI键
#endif /* HAVE_PSI_INTERFACE */

  memset(&mysql, 0, sizeof(mysql)); /* dummy mysql for set_mysql_extended_error */
  // 将mysql结构体的内存清零，以便用于设置MySQL扩展错误

  mysql_mutex_init(key_mutex_LOCK_load_client_plugin, &LOCK_load_client_plugin,
                   MY_MUTEX_INIT_SLOW);  // 初始化加载客户端插件的互斥锁
  ::new ((void *)&mem_root) MEM_ROOT(key_memory_root, 128);  // 初始化内存根

  memset(&plugin_list, 0, sizeof(plugin_list));  // 清空插件列表

  initialized = true;  // 设置初始化标志为true

  mysql_mutex_lock(&LOCK_load_client_plugin);  // 锁定加载客户端插件的互斥锁

  for (builtin = mysql_client_builtins; *builtin; builtin++)  // 遍历内置插件
    add_plugin_noargs(&mysql, *builtin, nullptr, 0);  // 添加插件，不带参数

  mysql_mutex_unlock(&LOCK_load_client_plugin);  // 解锁互斥锁

  load_env_plugins(&mysql);  // 加载环境变量指定的插件

  mysql_close_free(&mysql);  // 关闭并释放MYSQL结构体

  return 0;  // 返回0表示成功
}


/**
  Deinitializes the client plugin layer.

  Unloades all client plugins and frees any associated resources.
*/
void mysql_client_plugin_deinit() {
  int i;
  struct st_client_plugin_int *p;

  if (!initialized) return;

  for (i = 0; i < MYSQL_CLIENT_MAX_PLUGINS; i++)
    for (p = plugin_list[i]; p; p = p->next) {
      if (p->plugin->deinit) p->plugin->deinit();
      if (p->dlhandle) dlclose(p->dlhandle);
    }

  memset(&plugin_list, 0, sizeof(plugin_list));
  initialized = false;
  mem_root.Clear();
  mysql_mutex_destroy(&LOCK_load_client_plugin);
}

/************* public facing functions, for client consumption *********/

/* see <mysql/client_plugin.h> for a full description */
struct st_mysql_client_plugin *mysql_client_register_plugin(
    MYSQL *mysql, struct st_mysql_client_plugin *plugin) {
  if (is_not_initialized(mysql, plugin->name)) return nullptr;

  mysql_mutex_lock(&LOCK_load_client_plugin);

  /* make sure the plugin wasn't loaded meanwhile */
  if (find_plugin(plugin->name, plugin->type)) {
    set_mysql_extended_error(mysql, CR_AUTH_PLUGIN_CANNOT_LOAD,
                             unknown_sqlstate,
                             ER_CLIENT(CR_AUTH_PLUGIN_CANNOT_LOAD),
                             plugin->name, "it is already loaded");
    plugin = nullptr;
  } else
    plugin = add_plugin_noargs(mysql, plugin, nullptr, 0);

  mysql_mutex_unlock(&LOCK_load_client_plugin);
  return plugin;
}

/**
  see <mysql/client_plugin.h> for a full description 
  参见 <mysql/client_plugin.h> 以获取完整描述
*/
struct st_mysql_client_plugin *mysql_load_plugin_v(MYSQL *mysql,
                                                   const char *name, int type,
                                                   int argc, va_list args) {
  const char *errmsg;  // 错误信息
  char dlpath[FN_REFLEN + 1];  // 动态库路径
  void *sym, *dlhandle;  // 动态库符号和句柄
  struct st_mysql_client_plugin *plugin;  // 客户端插件结构体
  const char *plugindir;  // 插件目录
  const CHARSET_INFO *cs = nullptr;  // 字符集信息
  size_t len = (name ? strlen(name) : 0);  // 插件名称长度
  int well_formed_error;  // 是否格式正确的标志
  size_t res = 0;  // 结果长度
#ifdef _WIN32
  char win_errormsg[2048];  // Windows错误信息
#endif

  DBUG_TRACE;  // 调试跟踪
  DBUG_PRINT("entry", ("name=%s type=%d int argc=%d", name, type, argc));  // 打印进入函数的信息
  if (is_not_initialized(mysql, name)) {  // 检查是否已初始化
    DBUG_PRINT("leave", ("mysql not initialized"));  // 打印未初始化信息
    return nullptr;  // 返回空指针
  }

  mysql_mutex_lock(&LOCK_load_client_plugin);  // 锁定加载客户端插件的互斥锁

  /* make sure the plugin wasn't loaded meanwhile */
  if (type >= 0 && find_plugin(name, type)) {  // 确保插件没有被加载
    errmsg = "it is already loaded";  // 错误信息
    goto err;  // 跳转到错误处理
  }

  if (mysql->options.extension && mysql->options.extension->plugin_dir) {  // 检查插件目录
    plugindir = mysql->options.extension->plugin_dir;  // 获取插件目录
  } else {
    plugindir = getenv("LIBMYSQL_PLUGIN_DIR");  // 从环境变量获取插件目录
    if (!plugindir) {
      plugindir = PLUGINDIR;  // 默认插件目录
    }
  }
  if (mysql && mysql->charset)  // 检查字符集
    cs = mysql->charset;  // 获取字符集
  else
    cs = &my_charset_utf8mb4_bin;  // 默认字符集

  /* check if plugin name does not have any directory separator character */
  if ((my_strcspn(cs, name, name + len, FN_DIRSEP, strlen(FN_DIRSEP))) < len) {  // 检查插件名称是否包含目录分隔符
    errmsg = "No paths allowed for shared library";  // 错误信息
    goto err;  // 跳转到错误处理
  }
  
  /* check if plugin name does not exceed its maximum length */
  res = cs->cset->well_formed_len(cs, name, name + len, NAME_CHAR_LEN,
                                  &well_formed_error);  // 检查插件名称长度

  if (well_formed_error || len != res) {  // 检查格式是否正确
    errmsg = "Invalid plugin name";  // 错误信息
    goto err;  // 跳转到错误处理
  }
  
  /*
    check if length of(plugin_dir + plugin name) does not exceed its maximum
    length
  */
  if ((strlen(plugindir) + len + 1) >= FN_REFLEN) {  // 检查路径长度
    errmsg = "Invalid path";  // 错误信息
    goto err;  // 跳转到错误处理
  }

  /* Compile dll path */
  strxnmov(dlpath, sizeof(dlpath) - 1, plugindir, "/", name, SO_EXT, NullS);  // 生成动态库路径

  DBUG_PRINT("info", ("dlopeninig %s", dlpath));  // 打印动态库路径
  /* Open new dll handle */
#if defined(HAVE_ASAN) || defined(HAVE_LSAN)
  // Do not unload the shared object during dlclose().
  // LeakSanitizer needs this in order to match entries in lsan.supp
  if (!(dlhandle = dlopen(dlpath, RTLD_NOW | RTLD_NODELETE)))  // 打开动态库
#else
  if (!(dlhandle = dlopen(dlpath, RTLD_NOW)))  // 打开动态库
#endif
  {
#if defined(__APPLE__)
    /* Apple supports plugins with .so also, so try this as well */
    strxnmov(dlpath, sizeof(dlpath) - 1, plugindir, "/", name, ".so", NullS);  // 尝试以.so后缀打开
    if ((dlhandle = dlopen(dlpath, RTLD_NOW))) goto have_plugin;  // 如果成功，跳转到have_plugin
#endif

#ifdef _WIN32
    /* There should be no win32 calls between failed dlopen() and GetLastError()
     */
    if (FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, 0, GetLastError(), 0,
                      win_errormsg, 2048, NULL))  // 获取Windows错误信息
      errmsg = win_errormsg;  // 设置错误信息
    else
      errmsg = "";  // 设置为空
#else
    errmsg = dlerror();  // 获取错误信息
#endif
    DBUG_PRINT("info", ("failed to dlopen"));  // 打印打开失败信息
    goto err;  // 跳转到错误处理
  }

#if defined(__APPLE__)
have_plugin:
#endif
  if (!(sym = dlsym(dlhandle, plugin_declarations_sym))) {  // 获取插件符号
    errmsg = "not a plugin";  // 错误信息
    dlclose(dlhandle);  // 关闭动态库句柄
    goto err;  // 跳转到错误处理
  }

  plugin = (struct st_mysql_client_plugin *)sym;  // 转换为插件结构体

  if (type >= 0 && type != plugin->type) {  // 检查类型是否匹配
    errmsg = "type mismatch";  // 错误信息
    goto err;  // 跳转到错误处理
  }

  if (strcmp(name, plugin->name)) {  // 检查名称是否匹配
    errmsg = "name mismatch";  // 错误信息
    goto err;  // 跳转到错误处理
  }

  if (type < 0 && find_plugin(name, plugin->type)) {  // 检查插件是否已加载
    errmsg = "it is already loaded";  // 错误信息
    goto err;  // 跳转到错误处理
  }

  plugin = add_plugin_withargs(mysql, plugin, dlhandle, argc, args);  // 添加插件

  mysql_mutex_unlock(&LOCK_load_client_plugin);  // 解锁互斥锁

  DBUG_PRINT("leave", ("plugin loaded ok"));  // 打印加载成功信息
  return plugin;  // 返回插件指针

err:
  mysql_mutex_unlock(&LOCK_load_client_plugin);  // 解锁互斥锁
  DBUG_PRINT("leave", ("plugin load error : %s", errmsg));  // 打印加载错误信息
  set_mysql_extended_error(mysql, CR_AUTH_PLUGIN_CANNOT_LOAD, unknown_sqlstate,
                           ER_CLIENT(CR_AUTH_PLUGIN_CANNOT_LOAD), name, errmsg);  // 设置扩展错误信息
  return nullptr;  // 返回空指针
}

/**
  see <mysql/client_plugin.h> for a full description 
  参见 <mysql/client_plugin.h> 以获取完整描述
*/
struct st_mysql_client_plugin *mysql_load_plugin(MYSQL *mysql, const char *name,
                                                 int type, int argc, ...) {
  struct st_mysql_client_plugin *p;  // 声明一个指向客户端插件的指针
  va_list args;  // 声明一个可变参数列表
  va_start(args, argc);  // 初始化可变参数列表，argc是最后一个固定参数
  p = mysql_load_plugin_v(mysql, name, type, argc, args);  // 调用mysql_load_plugin_v函数加载插件
  va_end(args);  // 结束可变参数列表的处理
  return p;  // 返回加载的插件指针
}

/* see <mysql/client_plugin.h> for a full description */
struct st_mysql_client_plugin *mysql_client_find_plugin(MYSQL *mysql,
                                                        const char *name,
                                                        int type) {
  struct st_mysql_client_plugin *p;

  DBUG_TRACE;
  DBUG_PRINT("entry", ("name=%s, type=%d", name, type));
  if (is_not_initialized(mysql, name)) return nullptr;

  if (type < 0 || type >= MYSQL_CLIENT_MAX_PLUGINS) {
    set_mysql_extended_error(
        mysql, CR_AUTH_PLUGIN_CANNOT_LOAD, unknown_sqlstate,
        ER_CLIENT(CR_AUTH_PLUGIN_CANNOT_LOAD), name, "invalid type");
  }

  if ((p = find_plugin(name, type))) {
    DBUG_PRINT("leave", ("found %p", p));
    return p;
  }

  /* not found, load it */
  p = mysql_load_plugin(mysql, name, type, 0);
  DBUG_PRINT("leave", ("loaded %p", p));
  return p;
}

/* see <mysql/client_plugin.h> for a full description */
int mysql_plugin_options(struct st_mysql_client_plugin *plugin,
                         const char *option, const void *value) {
  DBUG_TRACE;
  /* does the plugin support options call? */
  if (!plugin || !plugin->options) return 1;
  return plugin->options(option, value);
}

/* see <mysql/client_plugin.h> for a full description */
int mysql_plugin_get_option(struct st_mysql_client_plugin *plugin,
                            const char *option, void *value) {
  DBUG_TRACE;
  /* does the plugin support options call? */
  if (!plugin || !plugin->get_options) return 1;
  return plugin->get_options(option, value);
}
