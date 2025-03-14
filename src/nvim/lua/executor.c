// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include "luv/luv.h"
#include "nvim/api/private/defs.h"
#include "nvim/api/private/helpers.h"
#include "nvim/api/vim.h"
#include "nvim/ascii.h"
#include "nvim/assert.h"
#include "nvim/buffer_defs.h"
#include "nvim/change.h"
#include "nvim/cursor.h"
#include "nvim/eval/userfunc.h"
#include "nvim/event/loop.h"
#include "nvim/event/time.h"
#include "nvim/ex_cmds2.h"
#include "nvim/ex_docmd.h"
#include "nvim/ex_getln.h"
#include "nvim/extmark.h"
#include "nvim/func_attr.h"
#include "nvim/garray.h"
#include "nvim/getchar.h"
#include "nvim/lua/converter.h"
#include "nvim/lua/executor.h"
#include "nvim/lua/stdlib.h"
#include "nvim/lua/treesitter.h"
#include "nvim/macros.h"
#include "nvim/map.h"
#include "nvim/memline.h"
#include "nvim/message.h"
#include "nvim/msgpack_rpc/channel.h"
#include "nvim/os/os.h"
#include "nvim/screen.h"
#include "nvim/undo.h"
#include "nvim/version.h"
#include "nvim/vim.h"

static int in_fast_callback = 0;

// Initialized in nlua_init().
static lua_State *global_lstate = NULL;

typedef struct {
  Error err;
  String lua_err_str;
} LuaError;

#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "lua/executor.c.generated.h"
# include "lua/vim_module.generated.h"
#endif

#define PUSH_ALL_TYPVALS(lstate, args, argcount, special) \
  for (int i = 0; i < argcount; i++) { \
    if (args[i].v_type == VAR_UNKNOWN) { \
      lua_pushnil(lstate); \
    } else { \
      nlua_push_typval(lstate, &args[i], special); \
    } \
  }

#if __has_feature(address_sanitizer)
static PMap(handle_T) nlua_ref_markers = MAP_INIT;
static bool nlua_track_refs = false;
# define NLUA_TRACK_REFS
#endif

/// Convert lua error into a Vim error message
///
/// @param  lstate  Lua interpreter state.
/// @param[in]  msg  Message base, must contain one `%s`.
static void nlua_error(lua_State *const lstate, const char *const msg)
  FUNC_ATTR_NONNULL_ALL
{
  size_t len;
  const char *const str = lua_tolstring(lstate, -1, &len);

  msg_ext_set_kind("lua_error");
  semsg_multiline(msg, (int)len, str);

  lua_pop(lstate, 1);
}

/// Like lua_pcall, but use debug.traceback as errfunc.
///
/// @param lstate Lua interpreter state
/// @param[in] nargs Number of arguments expected by the function being called.
/// @param[in] nresults Number of results the function returns.
static int nlua_pcall(lua_State *lstate, int nargs, int nresults)
{
  lua_getglobal(lstate, "debug");
  lua_getfield(lstate, -1, "traceback");
  lua_remove(lstate, -2);
  lua_insert(lstate, -2 - nargs);
  int status = lua_pcall(lstate, nargs, nresults, -2 - nargs);
  if (status) {
    lua_remove(lstate, -2);
  } else {
    lua_remove(lstate, -1 - nresults);
  }
  return status;
}


/// Gets the version of the current Nvim build.
///
/// @param  lstate  Lua interpreter state.
static int nlua_nvim_version(lua_State *const lstate) FUNC_ATTR_NONNULL_ALL
{
  Dictionary version = version_dict();
  nlua_push_Dictionary(lstate, version, true);
  api_free_dictionary(version);
  return 1;
}


static void nlua_luv_error_event(void **argv)
{
  char *error = (char *)argv[0];
  msg_ext_set_kind("lua_error");
  semsg_multiline("Error executing luv callback:\n%s", error);
  xfree(error);
}

static int nlua_luv_cfpcall(lua_State *lstate, int nargs, int nresult, int flags)
  FUNC_ATTR_NONNULL_ALL
{
  int retval;

  // luv callbacks might be executed at any os_breakcheck/line_breakcheck
  // call, so using the API directly here is not safe.
  in_fast_callback++;

  int top = lua_gettop(lstate);
  int status = nlua_pcall(lstate, nargs, nresult);
  if (status) {
    if (status == LUA_ERRMEM && !(flags & LUVF_CALLBACK_NOEXIT)) {
      // consider out of memory errors unrecoverable, just like xmalloc()
      mch_errmsg(e_outofmem);
      mch_errmsg("\n");
      preserve_exit();
    }
    const char *error = lua_tostring(lstate, -1);

    multiqueue_put(main_loop.events, nlua_luv_error_event,
                   1, xstrdup(error));
    lua_pop(lstate, 1);  // error message
    retval = -status;
  } else {  // LUA_OK
    if (nresult == LUA_MULTRET) {
      nresult = lua_gettop(lstate) - top + nargs + 1;
    }
    retval = nresult;
  }

  in_fast_callback--;
  return retval;
}

static void nlua_schedule_event(void **argv)
{
  LuaRef cb = (LuaRef)(ptrdiff_t)argv[0];
  lua_State *const lstate = global_lstate;
  nlua_pushref(lstate, cb);
  nlua_unref(lstate, cb);
  if (nlua_pcall(lstate, 0, 0)) {
    nlua_error(lstate, _("Error executing vim.schedule lua callback: %.*s"));
  }
}

/// Schedule Lua callback on main loop's event queue
///
/// @param  lstate  Lua interpreter state.
static int nlua_schedule(lua_State *const lstate)
  FUNC_ATTR_NONNULL_ALL
{
  if (lua_type(lstate, 1) != LUA_TFUNCTION) {
    lua_pushliteral(lstate, "vim.schedule: expected function");
    return lua_error(lstate);
  }

  LuaRef cb = nlua_ref(lstate, 1);

  multiqueue_put(main_loop.events, nlua_schedule_event,
                 1, (void *)(ptrdiff_t)cb);
  return 0;
}

// Dummy timer callback. Used by f_wait().
static void dummy_timer_due_cb(TimeWatcher *tw, void *data)
{
}

// Dummy timer close callback. Used by f_wait().
static void dummy_timer_close_cb(TimeWatcher *tw, void *data)
{
  xfree(tw);
}

static bool nlua_wait_condition(lua_State *lstate, int *status, bool *callback_result)
{
  lua_pushvalue(lstate, 2);
  *status = nlua_pcall(lstate, 0, 1);
  if (*status) {
    return true;  // break on error, but keep error on stack
  }
  *callback_result = lua_toboolean(lstate, -1);
  lua_pop(lstate, 1);
  return *callback_result;  // break if true
}

/// "vim.wait(timeout, condition[, interval])" function
static int nlua_wait(lua_State *lstate)
  FUNC_ATTR_NONNULL_ALL
{
  intptr_t timeout = luaL_checkinteger(lstate, 1);
  if (timeout < 0) {
    return luaL_error(lstate, "timeout must be > 0");
  }

  int lua_top = lua_gettop(lstate);

  // Check if condition can be called.
  bool is_function = false;
  if (lua_top >= 2 && !lua_isnil(lstate, 2)) {
    is_function = (lua_type(lstate, 2) == LUA_TFUNCTION);

    // Check if condition is callable table
    if (!is_function && luaL_getmetafield(lstate, 2, "__call") != 0) {
      is_function = (lua_type(lstate, -1) == LUA_TFUNCTION);
      lua_pop(lstate, 1);
    }

    if (!is_function) {
      lua_pushliteral(lstate,
                      "vim.wait: if passed, condition must be a function");
      return lua_error(lstate);
    }
  }

  intptr_t interval = 200;
  if (lua_top >= 3 && !lua_isnil(lstate, 3)) {
    interval = luaL_checkinteger(lstate, 3);
    if (interval < 0) {
      return luaL_error(lstate, "interval must be > 0");
    }
  }

  bool fast_only = false;
  if (lua_top >= 4) {
    fast_only =  lua_toboolean(lstate, 4);
  }

  MultiQueue *loop_events = fast_only || in_fast_callback > 0
    ? main_loop.fast_events : main_loop.events;

  TimeWatcher *tw = xmalloc(sizeof(TimeWatcher));

  // Start dummy timer.
  time_watcher_init(&main_loop, tw, NULL);
  tw->events = loop_events;
  tw->blockable = true;
  time_watcher_start(tw,
                     dummy_timer_due_cb,
                     (uint64_t)interval,
                     (uint64_t)interval);

  int pcall_status = 0;
  bool callback_result = false;

  LOOP_PROCESS_EVENTS_UNTIL(&main_loop,
                            loop_events,
                            (int)timeout,
                            is_function ? nlua_wait_condition(lstate,
                                                              &pcall_status,
                                                              &callback_result) : false || got_int);

  // Stop dummy timer
  time_watcher_stop(tw);
  time_watcher_close(tw, dummy_timer_close_cb);

  if (pcall_status) {
    return lua_error(lstate);
  } else if (callback_result) {
    lua_pushboolean(lstate, 1);
    lua_pushnil(lstate);
  } else if (got_int) {
    got_int = false;
    vgetc();
    lua_pushboolean(lstate, 0);
    lua_pushinteger(lstate, -2);
  } else {
    lua_pushboolean(lstate, 0);
    lua_pushinteger(lstate, -1);
  }

  return 2;
}

/// Initialize lua interpreter state
///
/// Called by lua interpreter itself to initialize state.
static int nlua_state_init(lua_State *const lstate) FUNC_ATTR_NONNULL_ALL
{
  // print
  lua_pushcfunction(lstate, &nlua_print);
  lua_setglobal(lstate, "print");

  // debug.debug
  lua_getglobal(lstate, "debug");
  lua_pushcfunction(lstate, &nlua_debug);
  lua_setfield(lstate, -2, "debug");
  lua_pop(lstate, 1);

#ifdef WIN32
  // os.getenv
  lua_getglobal(lstate, "os");
  lua_pushcfunction(lstate, &nlua_getenv);
  lua_setfield(lstate, -2, "getenv");
  lua_pop(lstate, 1);
#endif

  // vim
  lua_newtable(lstate);

  // vim.api
  nlua_add_api_functions(lstate);

  // vim.types, vim.type_idx, vim.val_idx
  nlua_init_types(lstate);

  // neovim version
  lua_pushcfunction(lstate, &nlua_nvim_version);
  lua_setfield(lstate, -2, "version");

  // schedule
  lua_pushcfunction(lstate, &nlua_schedule);
  lua_setfield(lstate, -2, "schedule");

  // in_fast_event
  lua_pushcfunction(lstate, &nlua_in_fast_event);
  lua_setfield(lstate, -2, "in_fast_event");

  // call
  lua_pushcfunction(lstate, &nlua_call);
  lua_setfield(lstate, -2, "call");

  // rpcrequest
  lua_pushcfunction(lstate, &nlua_rpcrequest);
  lua_setfield(lstate, -2, "rpcrequest");

  // rpcnotify
  lua_pushcfunction(lstate, &nlua_rpcnotify);
  lua_setfield(lstate, -2, "rpcnotify");

  // wait
  lua_pushcfunction(lstate, &nlua_wait);
  lua_setfield(lstate, -2, "wait");

  // vim.NIL
  lua_newuserdata(lstate, 0);
  lua_createtable(lstate, 0, 0);
  lua_pushcfunction(lstate, &nlua_nil_tostring);
  lua_setfield(lstate, -2, "__tostring");
  lua_setmetatable(lstate, -2);
  nlua_nil_ref = nlua_ref(lstate, -1);
  lua_pushvalue(lstate, -1);
  lua_setfield(lstate, LUA_REGISTRYINDEX, "mpack.NIL");
  lua_setfield(lstate, -2, "NIL");

  // vim._empty_dict_mt
  lua_createtable(lstate, 0, 0);
  lua_pushcfunction(lstate, &nlua_empty_dict_tostring);
  lua_setfield(lstate, -2, "__tostring");
  nlua_empty_dict_ref = nlua_ref(lstate, -1);
  lua_pushvalue(lstate, -1);
  lua_setfield(lstate, LUA_REGISTRYINDEX, "mpack.empty_dict");
  lua_setfield(lstate, -2, "_empty_dict_mt");

  // internal vim._treesitter... API
  nlua_add_treesitter(lstate);

  // vim.loop
  luv_set_loop(lstate, &main_loop.uv);
  luv_set_callback(lstate, nlua_luv_cfpcall);
  luaopen_luv(lstate);
  lua_pushvalue(lstate, -1);
  lua_setfield(lstate, -3, "loop");

  // package.loaded.luv = vim.loop
  // otherwise luv will be reinitialized when require'luv'
  lua_getglobal(lstate, "package");
  lua_getfield(lstate, -1, "loaded");
  lua_pushvalue(lstate, -3);
  lua_setfield(lstate, -2, "luv");
  lua_pop(lstate, 3);

  nlua_state_add_stdlib(lstate);

  lua_setglobal(lstate, "vim");

  {
    const char *code = (char *)&shared_module[0];
    if (luaL_loadbuffer(lstate, code, sizeof(shared_module) - 1, "@vim/shared.lua")
        || nlua_pcall(lstate, 0, 0)) {
      nlua_error(lstate, _("E5106: Error while creating shared module: %.*s\n"));
      return 1;
    }
  }

  {
    lua_getglobal(lstate, "package");  // [package]
    lua_getfield(lstate, -1, "loaded");  // [package, loaded]

    const char *code = (char *)&inspect_module[0];
    if (luaL_loadbuffer(lstate, code, sizeof(inspect_module) - 1, "@vim/inspect.lua")
        || nlua_pcall(lstate, 0, 1)) {
      nlua_error(lstate, _("E5106: Error while creating inspect module: %.*s\n"));
      return 1;
    }
    // [package, loaded, inspect]
    lua_setfield(lstate, -2, "vim.inspect");  // [package, loaded]

    code = (char *)&lua_F_module[0];
    if (luaL_loadbuffer(lstate, code, sizeof(lua_F_module) - 1, "@vim/F.lua")
        || nlua_pcall(lstate, 0, 1)) {
      nlua_error(lstate, _("E5106: Error while creating vim.F module: %.*s\n"));
      return 1;
    }
    // [package, loaded, module]
    lua_setfield(lstate, -2, "vim.F");  // [package, loaded]

    lua_pop(lstate, 2);  // []
  }

  {
    const char *code = (char *)&vim_module[0];
    if (luaL_loadbuffer(lstate, code, sizeof(vim_module) - 1, "@vim.lua")
        || nlua_pcall(lstate, 0, 0)) {
      nlua_error(lstate, _("E5106: Error while creating vim module: %.*s\n"));
      return 1;
    }
  }

  {
    lua_getglobal(lstate, "package");  // [package]
    lua_getfield(lstate, -1, "loaded");  // [package, loaded]

    const char *code = (char *)&lua_meta_module[0];
    if (luaL_loadbuffer(lstate, code, sizeof(lua_meta_module) - 1, "@vim/_meta.lua")
        || nlua_pcall(lstate, 0, 1)) {
      nlua_error(lstate, _("E5106: Error while creating vim._meta module: %.*s\n"));
      return 1;
    }
    // [package, loaded, module]
    lua_setfield(lstate, -2, "vim._meta");  // [package, loaded]

    lua_pop(lstate, 2);  // []
  }

  return 0;
}

/// Initialize global lua interpreter
///
/// Crashes Nvim if initialization fails.
void nlua_init(void)
{
#ifdef NLUA_TRACK_REFS
  const char *env = os_getenv("NVIM_LUA_NOTRACK");
  if (!env || !*env) {
    nlua_track_refs = true;
  }
#endif

  lua_State *lstate = luaL_newstate();
  if (lstate == NULL) {
    emsg(_("E970: Failed to initialize lua interpreter"));
    preserve_exit();
  }
  luaL_openlibs(lstate);
  nlua_state_init(lstate);

  global_lstate = lstate;
}


void nlua_free_all_mem(void)
{
  if (!global_lstate) {
    return;
  }
  lua_State *lstate = global_lstate;

  nlua_unref(lstate, nlua_nil_ref);
  nlua_unref(lstate, nlua_empty_dict_ref);

#ifdef NLUA_TRACK_REFS
  if (nlua_refcount) {
    fprintf(stderr, "%d lua references were leaked!", nlua_refcount);
  }

  if (nlua_track_refs) {
    // in case there are leaked luarefs, leak the associated memory
    // to get LeakSanitizer stacktraces on exit
    pmap_destroy(handle_T)(&nlua_ref_markers);
  }
#endif

  nlua_refcount = 0;
  lua_close(lstate);
}

static void nlua_print_event(void **argv)
{
  char *str = argv[0];
  const size_t len = (size_t)(intptr_t)argv[1]-1;  // exclude final NUL

  for (size_t i = 0; i < len;) {
    if (got_int) {
      break;
    }
    const size_t start = i;
    while (i < len) {
      switch (str[i]) {
      case NUL:
        str[i] = NL;
        i++;
        continue;
      case NL:
        // TODO(bfredl): use proper multiline msg? Probably should implement
        // print() in lua in terms of nvim_message(), when it is available.
        str[i] = NUL;
        i++;
        break;
      default:
        i++;
        continue;
      }
      break;
    }
    msg(str + start);
  }
  if (len && str[len - 1] == NUL) {  // Last was newline
    msg("");
  }
  xfree(str);
}

/// Print as a Vim message
///
/// @param  lstate  Lua interpreter state.
static int nlua_print(lua_State *const lstate)
  FUNC_ATTR_NONNULL_ALL
{
#define PRINT_ERROR(msg) \
  do { \
    errmsg = msg; \
    errmsg_len = sizeof(msg) - 1; \
    goto nlua_print_error; \
  } while (0)
  const int nargs = lua_gettop(lstate);
  lua_getglobal(lstate, "tostring");
  const char *errmsg = NULL;
  size_t errmsg_len = 0;
  garray_T msg_ga;
  ga_init(&msg_ga, 1, 80);
  int curargidx = 1;
  for (; curargidx <= nargs; curargidx++) {
    lua_pushvalue(lstate, -1);  // tostring
    lua_pushvalue(lstate, curargidx);  // arg
    // Do not use nlua_pcall here to avoid duplicate stack trace information
    if (lua_pcall(lstate, 1, 1, 0)) {
      errmsg = lua_tolstring(lstate, -1, &errmsg_len);
      goto nlua_print_error;
    }
    size_t len;
    const char *const s = lua_tolstring(lstate, -1, &len);
    if (s == NULL) {
      PRINT_ERROR("<Unknown error: lua_tolstring returned NULL for tostring result>");
    }
    ga_concat_len(&msg_ga, s, len);
    if (curargidx < nargs) {
      ga_append(&msg_ga, ' ');
    }
    lua_pop(lstate, 1);
  }
#undef PRINT_ERROR
  ga_append(&msg_ga, NUL);

  if (in_fast_callback) {
    multiqueue_put(main_loop.events, nlua_print_event,
                   2, msg_ga.ga_data, msg_ga.ga_len);
  } else {
    nlua_print_event((void *[]){ msg_ga.ga_data,
                                 (void *)(intptr_t)msg_ga.ga_len });
  }
  return 0;

nlua_print_error:
  ga_clear(&msg_ga);
  const char *fmt = _("E5114: Error while converting print argument #%i: %.*s");
  size_t len = (size_t)vim_snprintf((char *)IObuff, IOSIZE, fmt, curargidx,
                                    (int)errmsg_len, errmsg);
  lua_pushlstring(lstate, (char *)IObuff, len);
  return lua_error(lstate);
}

/// debug.debug: interaction with user while debugging.
///
/// @param  lstate  Lua interpreter state.
static int nlua_debug(lua_State *lstate)
  FUNC_ATTR_NONNULL_ALL
{
  const typval_T input_args[] = {
    {
      .v_lock = VAR_FIXED,
      .v_type = VAR_STRING,
      .vval.v_string = (char_u *)"lua_debug> ",
    },
    {
      .v_type = VAR_UNKNOWN,
    },
  };
  for (;;) {
    lua_settop(lstate, 0);
    typval_T input;
    get_user_input(input_args, &input, false, false);
    msg_putchar('\n');  // Avoid outputting on input line.
    if (input.v_type != VAR_STRING
        || input.vval.v_string == NULL
        || *input.vval.v_string == NUL
        || STRCMP(input.vval.v_string, "cont") == 0) {
      tv_clear(&input);
      return 0;
    }
    if (luaL_loadbuffer(lstate, (const char *)input.vval.v_string,
                        STRLEN(input.vval.v_string), "=(debug command)")) {
      nlua_error(lstate, _("E5115: Error while loading debug string: %.*s"));
    } else if (nlua_pcall(lstate, 0, 0)) {
      nlua_error(lstate, _("E5116: Error while calling debug string: %.*s"));
    }
    tv_clear(&input);
  }
  return 0;
}

int nlua_in_fast_event(lua_State *lstate)
{
  lua_pushboolean(lstate, in_fast_callback > 0);
  return 1;
}

int nlua_call(lua_State *lstate)
{
  Error err = ERROR_INIT;
  size_t name_len;
  const char_u *name = (const char_u *)luaL_checklstring(lstate, 1, &name_len);
  if (!nlua_is_deferred_safe()) {
    return luaL_error(lstate, e_luv_api_disabled, "vimL function");
  }

  int nargs = lua_gettop(lstate)-1;
  if (nargs > MAX_FUNC_ARGS) {
    return luaL_error(lstate, "Function called with too many arguments");
  }

  typval_T vim_args[MAX_FUNC_ARGS + 1];
  int i = 0;  // also used for freeing the variables
  for (; i < nargs; i++) {
    lua_pushvalue(lstate, (int)i+2);
    if (!nlua_pop_typval(lstate, &vim_args[i])) {
      api_set_error(&err, kErrorTypeException,
                    "error converting argument %d", i+1);
      goto free_vim_args;
    }
  }

  TRY_WRAP({
    // TODO(bfredl): this should be simplified in error handling refactor
    force_abort = false;
    suppress_errthrow = false;
    current_exception = NULL;
    did_emsg = false;

    try_start();
    typval_T rettv;
    funcexe_T funcexe = FUNCEXE_INIT;
    funcexe.firstline = curwin->w_cursor.lnum;
    funcexe.lastline = curwin->w_cursor.lnum;
    funcexe.evaluate = true;
    // call_func() retval is deceptive, ignore it.  Instead we set `msg_list`
    // (TRY_WRAP) to capture abort-causing non-exception errors.
    (void)call_func(name, (int)name_len, &rettv, nargs, vim_args, &funcexe);
    if (!try_end(&err)) {
      nlua_push_typval(lstate, &rettv, false);
    }
    tv_clear(&rettv);
  });

free_vim_args:
  while (i > 0) {
    tv_clear(&vim_args[--i]);
  }
  if (ERROR_SET(&err)) {
    lua_pushstring(lstate, err.msg);
    api_clear_error(&err);
    return lua_error(lstate);
  }
  return 1;
}

static int nlua_rpcrequest(lua_State *lstate)
{
  if (!nlua_is_deferred_safe()) {
    return luaL_error(lstate, e_luv_api_disabled, "rpcrequest");
  }
  return nlua_rpc(lstate, true);
}

static int nlua_rpcnotify(lua_State *lstate)
{
  return nlua_rpc(lstate, false);
}

static int nlua_rpc(lua_State *lstate, bool request)
{
  size_t name_len;
  uint64_t chan_id = (uint64_t)luaL_checkinteger(lstate, 1);
  const char *name = luaL_checklstring(lstate, 2, &name_len);
  int nargs = lua_gettop(lstate)-2;
  Error err = ERROR_INIT;
  Array args = ARRAY_DICT_INIT;

  for (int i = 0; i < nargs; i++) {
    lua_pushvalue(lstate, (int)i+3);
    ADD(args, nlua_pop_Object(lstate, false, &err));
    if (ERROR_SET(&err)) {
      api_free_array(args);
      goto check_err;
    }
  }

  if (request) {
    Object result = rpc_send_call(chan_id, name, args, &err);
    if (!ERROR_SET(&err)) {
      nlua_push_Object(lstate, result, false);
      api_free_object(result);
    }
  } else {
    if (!rpc_send_event(chan_id, name, args)) {
      api_set_error(&err, kErrorTypeValidation,
                    "Invalid channel: %" PRIu64, chan_id);
    }
  }

check_err:
  if (ERROR_SET(&err)) {
    lua_pushstring(lstate, err.msg);
    api_clear_error(&err);
    return lua_error(lstate);
  }

  return request ? 1 : 0;
}

static int nlua_nil_tostring(lua_State *lstate)
{
  lua_pushstring(lstate, "vim.NIL");
  return 1;
}

static int nlua_empty_dict_tostring(lua_State *lstate)
{
  lua_pushstring(lstate, "vim.empty_dict()");
  return 1;
}


#ifdef WIN32
/// os.getenv: override os.getenv to maintain coherency. #9681
///
/// uv_os_setenv uses SetEnvironmentVariableW which does not update _environ.
///
/// @param  lstate  Lua interpreter state.
static int nlua_getenv(lua_State *lstate)
{
  lua_pushstring(lstate, os_getenv(luaL_checkstring(lstate, 1)));
  return 1;
}
#endif


/// add the value to the registry
LuaRef nlua_ref(lua_State *lstate, int index)
{
  lua_pushvalue(lstate, index);
  LuaRef ref = luaL_ref(lstate, LUA_REGISTRYINDEX);
  if (ref > 0) {
    nlua_refcount++;
#ifdef NLUA_TRACK_REFS
    if (nlua_track_refs) {
      // dummy allocation to make LeakSanitizer track our luarefs
      pmap_put(handle_T)(&nlua_ref_markers, ref, xmalloc(3));
    }
#endif
  }
  return ref;
}

/// remove the value from the registry
void nlua_unref(lua_State *lstate, LuaRef ref)
{
  if (ref > 0) {
    nlua_refcount--;
#ifdef NLUA_TRACK_REFS
    // NB: don't remove entry from map to track double-unref
    if (nlua_track_refs) {
      xfree(pmap_get(handle_T)(&nlua_ref_markers, ref));
    }
#endif
    luaL_unref(lstate, LUA_REGISTRYINDEX, ref);
  }
}

void api_free_luaref(LuaRef ref)
{
  nlua_unref(global_lstate, ref);
}

/// push a value referenced in the registry
void nlua_pushref(lua_State *lstate, LuaRef ref)
{
  lua_rawgeti(lstate, LUA_REGISTRYINDEX, ref);
}


/// Gets a new reference to an object stored at original_ref
///
/// NOTE: It does not copy the value, it creates a new ref to the lua object.
///       Leaves the stack unchanged.
LuaRef api_new_luaref(LuaRef original_ref)
{
  if (original_ref == LUA_NOREF) {
    return LUA_NOREF;
  }

  lua_State *const lstate = global_lstate;
  nlua_pushref(lstate, original_ref);
  LuaRef new_ref = nlua_ref(lstate, -1);
  lua_pop(lstate, 1);
  return new_ref;
}


/// Evaluate lua string
///
/// Used for luaeval().
///
/// @param[in]  str  String to execute.
/// @param[in]  arg  Second argument to `luaeval()`.
/// @param[out]  ret_tv  Location where result will be saved.
///
/// @return Result of the execution.
void nlua_typval_eval(const String str, typval_T *const arg, typval_T *const ret_tv)
  FUNC_ATTR_NONNULL_ALL
{
#define EVALHEADER "local _A=select(1,...) return ("
  const size_t lcmd_len = sizeof(EVALHEADER) - 1 + str.size + 1;
  char *lcmd;
  if (lcmd_len < IOSIZE) {
    lcmd = (char *)IObuff;
  } else {
    lcmd = xmalloc(lcmd_len);
  }
  memcpy(lcmd, EVALHEADER, sizeof(EVALHEADER) - 1);
  memcpy(lcmd + sizeof(EVALHEADER) - 1, str.data, str.size);
  lcmd[lcmd_len - 1] = ')';
#undef EVALHEADER
  nlua_typval_exec(lcmd, lcmd_len, "luaeval()", arg, 1, true, ret_tv);

  if (lcmd != (char *)IObuff) {
    xfree(lcmd);
  }
}

void nlua_typval_call(const char *str, size_t len, typval_T *const args, int argcount,
                      typval_T *ret_tv)
  FUNC_ATTR_NONNULL_ALL
{
#define CALLHEADER "return "
#define CALLSUFFIX "(...)"
  const size_t lcmd_len = sizeof(CALLHEADER) - 1 + len + sizeof(CALLSUFFIX) - 1;
  char *lcmd;
  if (lcmd_len < IOSIZE) {
    lcmd = (char *)IObuff;
  } else {
    lcmd = xmalloc(lcmd_len);
  }
  memcpy(lcmd, CALLHEADER, sizeof(CALLHEADER) - 1);
  memcpy(lcmd + sizeof(CALLHEADER) - 1, str, len);
  memcpy(lcmd + sizeof(CALLHEADER) - 1 + len, CALLSUFFIX,
         sizeof(CALLSUFFIX) - 1);
#undef CALLHEADER
#undef CALLSUFFIX

  nlua_typval_exec(lcmd, lcmd_len, "v:lua", args, argcount, false, ret_tv);

  if (lcmd != (char *)IObuff) {
    xfree(lcmd);
  }
}

void nlua_call_user_expand_func(expand_T *xp, typval_T *ret_tv)
  FUNC_ATTR_NONNULL_ALL
{
  lua_State *const lstate = global_lstate;

  nlua_pushref(lstate, xp->xp_luaref);
  lua_pushstring(lstate, (char *)xp->xp_pattern);
  lua_pushstring(lstate, (char *)xp->xp_line);
  lua_pushinteger(lstate, xp->xp_col);

  if (nlua_pcall(lstate, 3, 1)) {
    nlua_error(lstate, _("E5108: Error executing Lua function: %.*s"));
    return;
  }

  nlua_pop_typval(lstate, ret_tv);
}

static void nlua_typval_exec(const char *lcmd, size_t lcmd_len, const char *name,
                             typval_T *const args, int argcount, bool special, typval_T *ret_tv)
{
  if (check_secure()) {
    if (ret_tv) {
      ret_tv->v_type = VAR_NUMBER;
      ret_tv->vval.v_number = 0;
    }
    return;
  }

  lua_State *const lstate = global_lstate;
  if (luaL_loadbuffer(lstate, lcmd, lcmd_len, name)) {
    nlua_error(lstate, _("E5107: Error loading lua %.*s"));
    return;
  }

  PUSH_ALL_TYPVALS(lstate, args, argcount, special);

  if (nlua_pcall(lstate, argcount, ret_tv ? 1 : 0)) {
    nlua_error(lstate, _("E5108: Error executing lua %.*s"));
    return;
  }

  if (ret_tv) {
    nlua_pop_typval(lstate, ret_tv);
  }
}

int nlua_source_using_linegetter(LineGetter fgetline, void *cookie, char *name)
{
  const linenr_T save_sourcing_lnum = sourcing_lnum;
  const sctx_T save_current_sctx = current_sctx;
  current_sctx.sc_sid = SID_STR;
  current_sctx.sc_seq = 0;
  current_sctx.sc_lnum = 0;
  sourcing_lnum = 0;

  garray_T ga;
  char_u *line = NULL;

  ga_init(&ga, (int)sizeof(char_u *), 10);
  while ((line = fgetline(0, cookie, 0, false)) != NULL) {
    GA_APPEND(char_u *, &ga, line);
  }
  char *code = ga_concat_strings_sep(&ga, "\n");
  size_t len = strlen(code);
  nlua_typval_exec(code, len, name, NULL, 0, false, NULL);

  sourcing_lnum = save_sourcing_lnum;
  current_sctx = save_current_sctx;
  ga_clear_strings(&ga);
  xfree(code);
  return OK;
}

/// Call a LuaCallable given some typvals
///
/// Used to call any lua callable passed from Lua into VimL
///
/// @param[in]  lstate Lua State
/// @param[in]  lua_cb Lua Callable
/// @param[in]  argcount Count of typval arguments
/// @param[in]  argvars Typval Arguments
/// @param[out] rettv The return value from the called function.
int typval_exec_lua_callable(lua_State *lstate, LuaCallable lua_cb, int argcount, typval_T *argvars,
                             typval_T *rettv)
{
  LuaRef cb = lua_cb.func_ref;

  nlua_pushref(lstate, cb);

  PUSH_ALL_TYPVALS(lstate, argvars, argcount, false);

  if (nlua_pcall(lstate, argcount, 1)) {
    nlua_print(lstate);
    return ERROR_OTHER;
  }

  nlua_pop_typval(lstate, rettv);

  return ERROR_NONE;
}

/// Execute Lua string
///
/// Used for nvim_exec_lua() and internally to execute a lua string.
///
/// @param[in]  str  String to execute.
/// @param[in]  args array of ... args
/// @param[out]  err  Location where error will be saved.
///
/// @return Return value of the execution.
Object nlua_exec(const String str, const Array args, Error *err)
{
  lua_State *const lstate = global_lstate;

  if (luaL_loadbuffer(lstate, str.data, str.size, "<nvim>")) {
    size_t len;
    const char *errstr = lua_tolstring(lstate, -1, &len);
    api_set_error(err, kErrorTypeValidation,
                  "Error loading lua: %.*s", (int)len, errstr);
    return NIL;
  }

  for (size_t i = 0; i < args.size; i++) {
    nlua_push_Object(lstate, args.items[i], false);
  }

  if (nlua_pcall(lstate, (int)args.size, 1)) {
    size_t len;
    const char *errstr = lua_tolstring(lstate, -1, &len);
    api_set_error(err, kErrorTypeException,
                  "Error executing lua: %.*s", (int)len, errstr);
    return NIL;
  }

  return nlua_pop_Object(lstate, false, err);
}

/// call a LuaRef as a function (or table with __call metamethod)
///
/// @param ref     the reference to call (not consumed)
/// @param name    if non-NULL, sent to callback as first arg
///                if NULL, only args are used
/// @param retval  if true, convert return value to Object
///                if false, discard return value
/// @param err     Error details, if any (if NULL, errors are echoed)
/// @return        Return value of function, if retval was set. Otherwise NIL.
Object nlua_call_ref(LuaRef ref, const char *name, Array args, bool retval, Error *err)
{
  lua_State *const lstate = global_lstate;
  nlua_pushref(lstate, ref);
  int nargs = (int)args.size;
  if (name != NULL) {
    lua_pushstring(lstate, name);
    nargs++;
  }
  for (size_t i = 0; i < args.size; i++) {
    nlua_push_Object(lstate, args.items[i], false);
  }

  if (nlua_pcall(lstate, nargs, retval ? 1 : 0)) {
    // if err is passed, the caller will deal with the error.
    if (err) {
      size_t len;
      const char *errstr = lua_tolstring(lstate, -1, &len);
      api_set_error(err, kErrorTypeException,
                    "Error executing lua: %.*s", (int)len, errstr);
    } else {
      nlua_error(lstate, _("Error executing lua callback: %.*s"));
    }
    return NIL;
  }

  if (retval) {
    Error dummy = ERROR_INIT;
    if (err == NULL) {
      err = &dummy;
    }
    return nlua_pop_Object(lstate, false, err);
  } else {
    return NIL;
  }
}

/// check if the current execution context is safe for calling deferred API
/// methods. Luv callbacks are unsafe as they are called inside the uv loop.
bool nlua_is_deferred_safe(void)
{
  return in_fast_callback == 0;
}

/// Run lua string
///
/// Used for :lua.
///
/// @param  eap  VimL command being run.
void ex_lua(exarg_T *const eap)
  FUNC_ATTR_NONNULL_ALL
{
  size_t len;
  char *const code = script_get(eap, &len);
  if (eap->skip) {
    xfree(code);
    return;
  }
  nlua_typval_exec(code, len, ":lua", NULL, 0, false, NULL);

  xfree(code);
}

/// Run lua string for each line in range
///
/// Used for :luado.
///
/// @param  eap  VimL command being run.
void ex_luado(exarg_T *const eap)
  FUNC_ATTR_NONNULL_ALL
{
  if (u_save(eap->line1 - 1, eap->line2 + 1) == FAIL) {
    emsg(_("cannot save undo information"));
    return;
  }
  const char *const cmd = (const char *)eap->arg;
  const size_t cmd_len = strlen(cmd);

  lua_State *const lstate = global_lstate;

#define DOSTART "return function(line, linenr) "
#define DOEND " end"
  const size_t lcmd_len = (cmd_len
                           + (sizeof(DOSTART) - 1)
                           + (sizeof(DOEND) - 1));
  char *lcmd;
  if (lcmd_len < IOSIZE) {
    lcmd = (char *)IObuff;
  } else {
    lcmd = xmalloc(lcmd_len + 1);
  }
  memcpy(lcmd, DOSTART, sizeof(DOSTART) - 1);
  memcpy(lcmd + sizeof(DOSTART) - 1, cmd, cmd_len);
  memcpy(lcmd + sizeof(DOSTART) - 1 + cmd_len, DOEND, sizeof(DOEND) - 1);
#undef DOSTART
#undef DOEND

  if (luaL_loadbuffer(lstate, lcmd, lcmd_len, ":luado")) {
    nlua_error(lstate, _("E5109: Error loading lua: %.*s"));
    if (lcmd_len >= IOSIZE) {
      xfree(lcmd);
    }
    return;
  }
  if (lcmd_len >= IOSIZE) {
    xfree(lcmd);
  }
  if (nlua_pcall(lstate, 0, 1)) {
    nlua_error(lstate, _("E5110: Error executing lua: %.*s"));
    return;
  }
  for (linenr_T l = eap->line1; l <= eap->line2; l++) {
    if (l > curbuf->b_ml.ml_line_count) {
      break;
    }
    lua_pushvalue(lstate, -1);
    const char *old_line = (const char *)ml_get_buf(curbuf, l, false);
    lua_pushstring(lstate, old_line);
    lua_pushnumber(lstate, (lua_Number)l);
    if (nlua_pcall(lstate, 2, 1)) {
      nlua_error(lstate, _("E5111: Error calling lua: %.*s"));
      break;
    }
    if (lua_isstring(lstate, -1)) {
      size_t old_line_len = STRLEN(old_line);

      size_t new_line_len;
      const char *const new_line = lua_tolstring(lstate, -1, &new_line_len);
      char *const new_line_transformed = xmemdupz(new_line, new_line_len);
      for (size_t i = 0; i < new_line_len; i++) {
        if (new_line_transformed[i] == NUL) {
          new_line_transformed[i] = '\n';
        }
      }
      ml_replace(l, (char_u *)new_line_transformed, false);
      inserted_bytes(l, 0, (int)old_line_len, (int)new_line_len);
    }
    lua_pop(lstate, 1);
  }
  lua_pop(lstate, 1);
  check_cursor();
  update_screen(NOT_VALID);
}

/// Run lua file
///
/// Used for :luafile.
///
/// @param  eap  VimL command being run.
void ex_luafile(exarg_T *const eap)
  FUNC_ATTR_NONNULL_ALL
{
  nlua_exec_file((const char *)eap->arg);
}

/// execute lua code from a file.
///
/// @param  path  path of the file
///
/// @return  true if everything ok, false if there was an error (echoed)
bool nlua_exec_file(const char *path)
  FUNC_ATTR_NONNULL_ALL
{
  lua_State *const lstate = global_lstate;

  if (luaL_loadfile(lstate, path)) {
    nlua_error(lstate, _("E5112: Error while creating lua chunk: %.*s"));
    return false;
  }

  if (nlua_pcall(lstate, 0, 0)) {
    nlua_error(lstate, _("E5113: Error while calling lua chunk: %.*s"));
    return false;
  }

  return true;
}

int tslua_get_language_version(lua_State *L)
{
  lua_pushnumber(L, TREE_SITTER_LANGUAGE_VERSION);
  return 1;
}

static void nlua_add_treesitter(lua_State *const lstate) FUNC_ATTR_NONNULL_ALL
{
  tslua_init(lstate);

  lua_pushcfunction(lstate, tslua_push_parser);
  lua_setfield(lstate, -2, "_create_ts_parser");

  lua_pushcfunction(lstate, tslua_add_language);
  lua_setfield(lstate, -2, "_ts_add_language");

  lua_pushcfunction(lstate, tslua_has_language);
  lua_setfield(lstate, -2, "_ts_has_language");

  lua_pushcfunction(lstate, tslua_inspect_lang);
  lua_setfield(lstate, -2, "_ts_inspect_language");

  lua_pushcfunction(lstate, tslua_parse_query);
  lua_setfield(lstate, -2, "_ts_parse_query");

  lua_pushcfunction(lstate, tslua_get_language_version);
  lua_setfield(lstate, -2, "_ts_get_language_version");
}

int nlua_expand_pat(expand_T *xp, char_u *pat, int *num_results, char_u ***results)
{
  lua_State *const lstate = global_lstate;
  int ret = OK;

  // [ vim ]
  lua_getglobal(lstate, "vim");

  // [ vim, vim._expand_pat ]
  lua_getfield(lstate, -1, "_expand_pat");
  luaL_checktype(lstate, -1, LUA_TFUNCTION);

  // [ vim, vim._on_key, buf ]
  lua_pushlstring(lstate, (const char *)pat, STRLEN(pat));

  if (nlua_pcall(lstate, 1, 2) != 0) {
    nlua_error(lstate,
               _("Error executing vim._expand_pat: %.*s"));
    return FAIL;
  }

  Error err = ERROR_INIT;

  *num_results = 0;
  *results = NULL;

  int prefix_len = (int)nlua_pop_Integer(lstate, &err);
  if (ERROR_SET(&err)) {
    ret = FAIL;
    goto cleanup;
  }

  Array completions = nlua_pop_Array(lstate, &err);
  if (ERROR_SET(&err)) {
    ret = FAIL;
    goto cleanup_array;
  }

  garray_T result_array;
  ga_init(&result_array, (int)sizeof(char *), 80);
  for (size_t i = 0; i < completions.size; i++) {
    Object v = completions.items[i];

    if (v.type != kObjectTypeString) {
      ret = FAIL;
      goto cleanup_array;
    }

    GA_APPEND(char_u *,
              &result_array,
              vim_strsave((char_u *)v.data.string.data));
  }

  xp->xp_pattern += prefix_len;
  *results = result_array.ga_data;
  *num_results = result_array.ga_len;

cleanup_array:
  api_free_array(completions);

cleanup:

  if (ret == FAIL) {
    ga_clear(&result_array);
  }

  return ret;
}

// Required functions for lua c functions as VimL callbacks

int nlua_CFunction_func_call(int argcount, typval_T *argvars, typval_T *rettv, void *state)
{
  lua_State *const lstate = global_lstate;
  LuaCFunctionState *funcstate = (LuaCFunctionState *)state;

  return typval_exec_lua_callable(lstate, funcstate->lua_callable,
                                  argcount, argvars, rettv);
}

void nlua_CFunction_func_free(void *state)
{
  lua_State *const lstate = global_lstate;
  LuaCFunctionState *funcstate = (LuaCFunctionState *)state;

  nlua_unref(lstate, funcstate->lua_callable.func_ref);
  xfree(funcstate);
}

bool nlua_is_table_from_lua(typval_T *const arg)
{
  if (arg->v_type == VAR_DICT) {
    return arg->vval.v_dict->lua_table_ref != LUA_NOREF;
  } else if (arg->v_type == VAR_LIST) {
    return arg->vval.v_list->lua_table_ref != LUA_NOREF;
  } else {
    return false;
  }
}

char_u *nlua_register_table_as_callable(typval_T *const arg)
{
  LuaRef table_ref = LUA_NOREF;
  if (arg->v_type == VAR_DICT) {
    table_ref = arg->vval.v_dict->lua_table_ref;
  } else if (arg->v_type == VAR_LIST) {
    table_ref = arg->vval.v_list->lua_table_ref;
  }

  if (table_ref == LUA_NOREF) {
    return NULL;
  }

  lua_State *const lstate = global_lstate;

#ifndef NDEBUG
  int top = lua_gettop(lstate);
#endif

  nlua_pushref(lstate, table_ref);  // [table]
  if (!lua_getmetatable(lstate, -1)) {
    lua_pop(lstate, 1);
    assert(top == lua_gettop(lstate));
    return NULL;
  }  // [table, mt]

  lua_getfield(lstate, -1, "__call");  // [table, mt, mt.__call]
  if (!lua_isfunction(lstate, -1)) {
    lua_pop(lstate, 3);
    assert(top == lua_gettop(lstate));
    return NULL;
  }
  lua_pop(lstate, 2);  // [table]

  LuaCFunctionState *state = xmalloc(sizeof(LuaCFunctionState));
  state->lua_callable.func_ref = nlua_ref(lstate, -1);

  char_u *name = register_cfunc(&nlua_CFunction_func_call,
                                &nlua_CFunction_func_free, state);


  lua_pop(lstate, 1);  // []
  assert(top == lua_gettop(lstate));

  return name;
}

void nlua_execute_on_key(int c)
{
  char_u buf[NUMBUFLEN];
  size_t buf_len = special_to_buf(c, mod_mask, false, buf);

  lua_State *const lstate = global_lstate;

#ifndef NDEBUG
  int top = lua_gettop(lstate);
#endif

  // [ vim ]
  lua_getglobal(lstate, "vim");

  // [ vim, vim._on_key]
  lua_getfield(lstate, -1, "_on_key");
  luaL_checktype(lstate, -1, LUA_TFUNCTION);

  // [ vim, vim._on_key, buf ]
  lua_pushlstring(lstate, (const char *)buf, buf_len);

  if (nlua_pcall(lstate, 1, 0)) {
    nlua_error(lstate,
               _("Error executing  vim.on_key Lua callback: %.*s"));
  }

  // [ vim ]
  lua_pop(lstate, 1);

#ifndef NDEBUG
  // [ ]
  assert(top == lua_gettop(lstate));
#endif
}

void nlua_do_ucmd(ucmd_T *cmd, exarg_T *eap)
{
  lua_State *const lstate = global_lstate;

  nlua_pushref(lstate, cmd->uc_luaref);

  lua_newtable(lstate);
  lua_pushboolean(lstate, eap->forceit == 1);
  lua_setfield(lstate, -2, "bang");

  lua_pushinteger(lstate, eap->line1);
  lua_setfield(lstate, -2, "line1");

  lua_pushinteger(lstate, eap->line2);
  lua_setfield(lstate, -2, "line2");

  lua_pushstring(lstate, (const char *)eap->arg);
  lua_setfield(lstate, -2, "args");

  lua_pushstring(lstate, (const char *)&eap->regname);
  lua_setfield(lstate, -2, "reg");

  lua_pushinteger(lstate, eap->addr_count);
  lua_setfield(lstate, -2, "range");

  if (eap->addr_count > 0) {
    lua_pushinteger(lstate, eap->line2);
  } else {
    lua_pushinteger(lstate, cmd->uc_def);
  }
  lua_setfield(lstate, -2, "count");

  // The size of this buffer is chosen empirically to be large enough to hold
  // every possible modifier (with room to spare). If the list of possible
  // modifiers grows this may need to be updated.
  char buf[200] = { 0 };
  (void)uc_mods(buf);
  lua_pushstring(lstate, buf);
  lua_setfield(lstate, -2, "mods");

  if (nlua_pcall(lstate, 1, 0)) {
    nlua_error(lstate, _("Error executing Lua callback: %.*s"));
  }
}

