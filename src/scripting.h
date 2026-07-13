#pragma once

#include <stdbool.h>
#include <Python.h>

#include "game.h"

/* scripting_finalize — call once at true program exit to run Py_Finalize().
 * Unlike scripting_shutdown() this is NOT called between matches. */
void scripting_finalize(void);

/* One-time init/shutdown — wraps Py_Initialize / Py_Finalize.
 * scripting_init() must be called before any other scripting_* function.
 * scripting_shutdown() must be called before program exit. */
void scripting_init(void);
void scripting_shutdown(void);

/* Load a Python bot script into a fresh namespace dict.
 * Returns the namespace dict (a new reference) on success, or NULL on error.
 * On error the message can be retrieved with scripting_get_last_error(). */
PyObject *scripting_load(const char *path);

/* Return the last load error string, or NULL if none. */
const char *scripting_get_last_error(void);

/* Call init() in the given namespace, parse the returned dict, and fill *out.
 * Also exports per-bot globals (self_locomotion, self_body, etc.) into the namespace. */
void scripting_call_init(PyObject *ns, BotConfig *out);

/* Set the bot index that the API callbacks (move/fire/scan) will act on.
 * Must be called immediately before invoking think() on a bot's namespace. */
void scripting_set_current_bot(int idx);

/* Shared per-team blackboard. Returns a borrowed reference to a persistent
 * Python dict that is the SAME object for every bot with the given script_id,
 * letting a team coordinate (focus-fire target, rally point, roles, ...).
 * The dict is created lazily on first use. Returns NULL only on allocation
 * failure. Must be called while holding the GIL. */
PyObject *scripting_team_mem(int script_id);

/* Clear all per-team blackboards. Call between matches so shared state does
 * not leak from one match into the next. */
void scripting_reset_team_mem(void);

/* Check Python syntax of a script file without executing it.
 * Returns true if syntax is OK; false and writes error into err_buf otherwise. */
bool scripting_check_syntax_file(const char *path, char *err_buf, int err_size);

/* scripting_weapon_fire_interval — used by llama_bot.c for prompt stats */
float scripting_weapon_fire_interval(WeaponType t);
