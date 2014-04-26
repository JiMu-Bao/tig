/* Copyright (c) 2006-2014 Jonas Fonseca <jonas.fonseca@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "tig/tig.h"
#include "tig/types.h"
#include "tig/argv.h"
#include "tig/io.h"
#include "tig/repo.h"
#include "tig/refdb.h"
#include "tig/options.h"
#include "tig/request.h"
#include "tig/line.h"
#include "tig/keys.h"
#include "tig/view.h"

/*
 * Option variables.
 */

#define DEFINE_OPTION_VARIABLES(name, type, flags) type opt_##name;
OPTION_INFO(DEFINE_OPTION_VARIABLES);

static struct option_info option_info[] = {
#define DEFINE_OPTION_INFO(name, type, flags) { #name, STRING_SIZE(#name), #type, &opt_##name },
	OPTION_INFO(DEFINE_OPTION_INFO)
};

struct option_info *
find_option_info(struct option_info *option, size_t options, const char *name)
{
	size_t namelen = strlen(name);
	int i;

	for (i = 0; i < options; i++)
		if (enum_equals(option[i], name, namelen))
			return &option[i];

	return NULL;
}

static struct option_info *
find_option_info_by_value(void *value)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(option_info); i++)
		if (option_info[i].value == value)
			return &option_info[i];

	return NULL;
}

static void
mark_option_seen(void *value)
{
	struct option_info *option = find_option_info_by_value(value);

	if (option)
		option->seen = TRUE;
}

/*
 * State variables.
 */

iconv_t opt_iconv_out		= ICONV_NONE;
char opt_editor[SIZEOF_STR]	= "";
const char **opt_cmdline_argv	= NULL;
const char **opt_rev_argv	= NULL;
const char **opt_file_argv	= NULL;
char opt_env_lines[64]		= "";
char opt_env_columns[64]	= "";
char *opt_env[]			= { opt_env_lines, opt_env_columns, NULL };

/*
 * Mapping between options and command argument mapping.
 */

const char *
diff_context_arg()
{
	static char opt_diff_context_arg[9]	= "";

	if (opt_diff_context < 0 ||
	    !string_format(opt_diff_context_arg, "-U%u", opt_diff_context))
		return "";

	return opt_diff_context_arg;
}


#define ENUM_ARG(enum_name, arg_string) ENUM_MAP_ENTRY(arg_string, enum_name)

static const struct enum_map_entry ignore_space_arg_map[] = {
	ENUM_ARG(IGNORE_SPACE_NO,	""),
	ENUM_ARG(IGNORE_SPACE_ALL,	"--ignore-all-space"),
	ENUM_ARG(IGNORE_SPACE_SOME,	"--ignore-space-change"),
	ENUM_ARG(IGNORE_SPACE_AT_EOL,	"--ignore-space-at-eol"),
};

const char *
ignore_space_arg()
{
	return ignore_space_arg_map[opt_ignore_space].name;
}

static const struct enum_map_entry commit_order_arg_map[] = {
	ENUM_ARG(COMMIT_ORDER_DEFAULT,		""),
	ENUM_ARG(COMMIT_ORDER_TOPO,		"--topo-order"),
	ENUM_ARG(COMMIT_ORDER_DATE,		"--date-order"),
	ENUM_ARG(COMMIT_ORDER_AUTHOR_DATE,	"--author-date-order"),
	ENUM_ARG(COMMIT_ORDER_REVERSE,		"--reverse"),
};

const char *
commit_order_arg()
{
	return commit_order_arg_map[opt_commit_order].name;
}

/* Use --show-notes to support Git >= 1.7.6 */
#define NOTES_ARG	"--show-notes"
#define NOTES_EQ_ARG	NOTES_ARG "="

static char opt_notes_arg[SIZEOF_STR] = NOTES_ARG;

const char *
show_notes_arg()
{
	if (opt_show_notes)
		return opt_notes_arg;
	/* Notes are disabled by default when passing --pretty args. */
	return "";
}

void
update_options_from_argv(const char *argv[])
{
	int next, flags_pos;

	for (next = flags_pos = 0; argv[next]; next++) {
		const char *flag = argv[next];
		int value = -1;

		if (map_enum(&value, commit_order_arg_map, flag)) {
			opt_commit_order = value;
			mark_option_seen(&opt_commit_order);
			continue;
		}

		if (map_enum(&value, ignore_space_arg_map, flag)) {
			opt_ignore_space = value;
			mark_option_seen(&opt_ignore_space);
			continue;
		}

		if (!strcmp(flag, "--no-notes")) {
			opt_show_notes = FALSE;
			mark_option_seen(&opt_show_notes);
			continue;
		}

		if (!prefixcmp(flag, "--show-notes") ||
		    !prefixcmp(flag, "--notes")) {
			opt_show_notes = TRUE;
			string_ncopy(opt_notes_arg, flag, strlen(flag));
			mark_option_seen(&opt_show_notes);
			continue;
		}

		if (!prefixcmp(flag, "-U")
		    && parse_int(&value, flag + 2, 0, 999999) == SUCCESS) {
			opt_diff_context = value;
			mark_option_seen(&opt_diff_context);
			continue;
		}

		argv[flags_pos++] = flag;
	}

	argv[flags_pos] = NULL;
}

/*
 * User config file handling.
 */

static const struct enum_map_entry color_map[] = {
#define COLOR_MAP(name) ENUM_MAP_ENTRY(#name, COLOR_##name)
	COLOR_MAP(DEFAULT),
	COLOR_MAP(BLACK),
	COLOR_MAP(BLUE),
	COLOR_MAP(CYAN),
	COLOR_MAP(GREEN),
	COLOR_MAP(MAGENTA),
	COLOR_MAP(RED),
	COLOR_MAP(WHITE),
	COLOR_MAP(YELLOW),
};

static const struct enum_map_entry attr_map[] = {
#define ATTR_MAP(name) ENUM_MAP_ENTRY(#name, A_##name)
	ATTR_MAP(NORMAL),
	ATTR_MAP(BLINK),
	ATTR_MAP(BOLD),
	ATTR_MAP(DIM),
	ATTR_MAP(REVERSE),
	ATTR_MAP(STANDOUT),
	ATTR_MAP(UNDERLINE),
};

#define set_attribute(attr, name)	map_enum(attr, attr_map, name)

enum status_code
parse_step(double *opt, const char *arg)
{
	*opt = atoi(arg);
	if (!strchr(arg, '%'))
		return SUCCESS;

	/* "Shift down" so 100% and 1 does not conflict. */
	*opt = (*opt - 1) / 100;
	if (*opt >= 1.0) {
		*opt = 0.99;
		return error("Percentage is larger than 100%%");
	}
	if (*opt < 0.0) {
		*opt = 1;
		return error("Percentage is less than 0%%");
	}
	return SUCCESS;
}

enum status_code
parse_int(int *opt, const char *arg, int min, int max)
{
	int value = atoi(arg);

	if (min <= value && value <= max) {
		*opt = value;
		return SUCCESS;
	}

	return error("Value must be between %d and %d", min, max);
}

static bool
set_color(int *color, const char *name)
{
	if (map_enum(color, color_map, name))
		return TRUE;
	if (!prefixcmp(name, "color"))
		return parse_int(color, name + 5, 0, 255) == SUCCESS;
	/* Used when reading git colors. Git expects a plain int w/o prefix.  */
	return parse_int(color, name, 0, 255) == SUCCESS;
}

#define is_quoted(c)	((c) == '"' || (c) == '\'')

static enum status_code
parse_color_name(const char *color, struct line_rule *rule, const char **prefix_ptr)
{
	const char *prefixend = is_quoted(*color) ? NULL : strchr(color, '.');

	if (prefixend) {
		struct keymap *keymap = get_keymap(color, prefixend - color);

		if (!keymap)
			return error("Unknown key map: %.*s", (int) (prefixend - color), color);
		if (prefix_ptr)
			*prefix_ptr = keymap->name;
		color = prefixend + 1;
	}

	memset(rule, 0, sizeof(*rule));
	if (is_quoted(*color)) {
		rule->line = color + 1;
		rule->linelen = strlen(color) - 2;
	} else {
		rule->name = color;
		rule->namelen = strlen(color);
	}

	return SUCCESS;
}

static int
find_remapped(const char *remapped[][2], size_t remapped_size, const char *arg)
{
	size_t arglen = strlen(arg);
	int i;

	for (i = 0; i < remapped_size; i++) {
		const char *name = remapped[i][0];
		size_t namelen = strlen(name);

		if (arglen == namelen &&
		    !string_enum_compare(arg, name, namelen))
			return i;
	}

	return -1;
}

/* Wants: object fgcolor bgcolor [attribute] */
static enum status_code
option_color_command(int argc, const char *argv[])
{
	struct line_rule rule = {};
	const char *prefix = NULL;
	struct line_info *info;
	enum status_code code;

	if (argc < 3)
		return error("Invalid color mapping: color area fgcolor bgcolor [attrs]");

	code = parse_color_name(argv[0], &rule, &prefix);
	if (code != SUCCESS)
		return code;

	info = add_line_rule(prefix, &rule);
	if (!info) {
		static const char *obsolete[][2] = {
			{ "acked",			"'    Acked-by'" },
			{ "diff-copy-from",		"'copy from '" },
			{ "diff-copy-to",		"'copy to '" },
			{ "diff-deleted-file-mode",	"'deleted file mode '" },
			{ "diff-dissimilarity",		"'dissimilarity '" },
			{ "diff-rename-from",		"'rename from '" },
			{ "diff-rename-to",		"'rename to '" },
			{ "diff-tree",			"'diff-tree '" },
			{ "filename",			"file" },
			{ "help-keymap",		"help.section" },
			{ "pp-adate",			"'AuthorDate: '" },
			{ "pp-author",			"'Author: '" },
			{ "pp-cdate",			"'CommitDate: '" },
			{ "pp-commit",			"'Commit: '" },
			{ "pp-date",			"'Date: '" },
			{ "reviewed",			"'    Reviewed-by'" },
			{ "signoff",			"'    Signed-off-by'" },
			{ "stat-head",			"status.header" },
			{ "stat-section",		"status.section" },
			{ "tested",			"'    Tested-by'" },
			{ "tree-dir",			"tree.directory" },
			{ "tree-file",			"tree.file" },
			{ "tree-head",			"tree.header" },
		};
		int index;

		index = find_remapped(obsolete, ARRAY_SIZE(obsolete), rule.name);
		if (index != -1) {
			/* Keep the initial prefix if defined. */
			code = parse_color_name(obsolete[index][1], &rule, prefix ? NULL : &prefix);
			if (code != SUCCESS)
				return code;
			info = add_line_rule(prefix, &rule);
		}

		if (!info)
			return error("Unknown color name: %s", argv[0]);

		code = error("%s has been replaced by %s",
			     obsolete[index][0], obsolete[index][1]);
	}

	if (!set_color(&info->fg, argv[1]))
		return error("Unknown color: %s", argv[1]);

	if (!set_color(&info->bg, argv[2]))
		return error("Unknown color: %s", argv[2]);

	info->attr = 0;
	while (argc-- > 3) {
		int attr;

		if (!set_attribute(&attr, argv[argc]))
			return error("Unknown color attribute: %s", argv[argc]);
		info->attr |= attr;
	}

	return code;
}

static enum status_code
parse_bool(bool *opt, const char *arg)
{
	*opt = (!strcmp(arg, "1") || !strcmp(arg, "true") || !strcmp(arg, "yes"))
		? TRUE : FALSE;
	if (*opt || !strcmp(arg, "0") || !strcmp(arg, "false") || !strcmp(arg, "no"))
		return SUCCESS;
	return error("Non-boolean value treated as false: %s", arg);
}

static enum status_code
parse_enum(unsigned int *opt, const char *arg, const struct enum_map *map)
{
	bool is_true;

	assert(map->size > 1);

	if (map_enum_do(map->entries, map->size, (int *) opt, arg))
		return SUCCESS;

	parse_bool(&is_true, arg);
	*opt = is_true ? map->entries[1].value : map->entries[0].value;
	return SUCCESS;
}

static enum status_code
parse_string(char *opt, const char *arg, size_t optsize)
{
	int arglen = strlen(arg);

	switch (arg[0]) {
	case '\"':
	case '\'':
		if (arglen == 1 || arg[arglen - 1] != arg[0])
			return ERROR_UNMATCHED_QUOTATION;
		arg += 1; arglen -= 2;
	default:
		string_ncopy_do(opt, optsize, arg, arglen);
		return SUCCESS;
	}
}

static enum status_code
parse_encoding(struct encoding **encoding_ref, const char *arg, bool priority)
{
	char buf[SIZEOF_STR];
	enum status_code code = parse_string(buf, arg, sizeof(buf));

	if (code == SUCCESS) {
		struct encoding *encoding = *encoding_ref;

		if (encoding && !priority)
			return code;
		encoding = encoding_open(buf);
		if (encoding)
			*encoding_ref = encoding;
	}

	return code;
}

static enum status_code
parse_args(const char ***args, const char *argv[])
{
	if (!argv_copy(args, argv))
		return ERROR_OUT_OF_MEMORY;
	return SUCCESS;
}

enum status_code
parse_option(struct option_info *option, const char *prefix, const char *arg)
{
	char name[SIZEOF_STR];

	if (!enum_name_prefixed(name, sizeof(name), prefix, option->name))
		return error("Failed to parse option");

	if (!strcmp("show-notes", name)) {
		bool *value = option->value;
		enum status_code res;

		if (parse_bool(option->value, arg) == SUCCESS)
			return SUCCESS;

		*value = TRUE;
		string_copy(opt_notes_arg, NOTES_EQ_ARG);
		res = parse_string(opt_notes_arg + STRING_SIZE(NOTES_EQ_ARG), arg,
				   sizeof(opt_notes_arg) - STRING_SIZE(NOTES_EQ_ARG));
		if (res == SUCCESS && !opt_notes_arg[STRING_SIZE(NOTES_EQ_ARG)])
			opt_notes_arg[STRING_SIZE(NOTES_ARG)] = 0;
		return res;
	}

	if (!strcmp(option->type, "bool"))
		return parse_bool(option->value, arg);

	if (!strcmp(option->type, "double"))
		return parse_step(option->value, arg);

	if (!strncmp(option->type, "enum", 4)) {
		const char *type = option->type + STRING_SIZE("enum ");
		const struct enum_map *map = find_enum_map(type);

		return parse_enum(option->value, arg, map);
	}

	if (!strcmp(option->type, "int")) {
		if (strstr(name, "title-overflow")) {
			bool enabled = FALSE;
			int *value = option->value;

			/* We try to parse it as a boolean (and set the
			 * value to 0 if fale), otherwise we parse it as
			 * an integer and use the given value. */
			if (parse_bool(&enabled, arg) == SUCCESS) {
				if (!enabled) {
					*value = 0;
					return SUCCESS;
				}
				arg = "50";
			}
		}

		if (!strcmp(name, "line-number-interval") ||
		    !strcmp(name, "tab-size"))
			return parse_int(option->value, arg, 1, 1024);
		else if (!strcmp(name, "id-width"))
			return parse_int(option->value, arg, 0, SIZEOF_REV - 1);
		else
			return parse_int(option->value, arg, 0, 1024);
	}

	return error("Unhandled option: %s", name);
}

struct view_config {
	const char *name;
	const char ***argv;
};

static struct view_config view_configs[] = {
	{ "blame-view", &opt_blame_view },
	{ "blob-view", &opt_blob_view },
	{ "diff-view", &opt_diff_view },
	{ "grep-view", &opt_grep_view },
	{ "log-view", &opt_log_view },
	{ "main-view", &opt_main_view },
	{ "pager-view", &opt_pager_view },
	{ "refs-view", &opt_refs_view },
	{ "stage-view", &opt_stage_view },
	{ "stash-view", &opt_stash_view },
	{ "status-view", &opt_status_view },
	{ "tree-view", &opt_tree_view },
};

static enum status_code
check_view_config(struct option_info *option, const char *argv[])
{
	const char *name = enum_name(option->name);
	int i;

	for (i = 0; i < ARRAY_SIZE(view_configs); i++)
		if (!strcmp(name, view_configs[i].name))
			return parse_view_config(name, argv);

	return SUCCESS;
}

/* Wants: name = value */
static enum status_code
option_set_command(int argc, const char *argv[])
{
	struct option_info *option;

	if (argc < 3)
		return error("Invalid set command: set option = value");

	if (strcmp(argv[1], "="))
		return error("No value assigned to %s", argv[0]);

	if (!strcmp(argv[0], "reference-format"))
		return parse_ref_formats(argv + 2);

	option = find_option_info(option_info, ARRAY_SIZE(option_info), argv[0]);
	if (option) {
		enum status_code code;

		if (option->seen)
			return SUCCESS;

		if (!strcmp(option->type, "const char **")) {
			code = check_view_config(option, argv + 2);
			if (code != SUCCESS)
				return code;
			return parse_args(option->value, argv + 2);
		}

		code = parse_option(option, "", argv[2]);
		if (code == SUCCESS && argc != 3)
			return error("Option %s only takes one value", argv[0]);

		return code;

	}

	{
		const char *obsolete[][2] = {
			{ "author-width",		"author" },
			{ "filename-width",		"file-name" },
			{ "line-number-interval",	"line-number" },
			{ "show-author",		"author" },
			{ "show-date",			"date" },
			{ "show-file-size",		"file-size" },
			{ "show-filename",		"file-name" },
			{ "show-id",			"id" },
			{ "show-line-numbers",		"line-number" },
			{ "show-refs",			"commit-title" },
			{ "show-rev-graph",		"commit-title" },
			{ "title-overflow",		"commit-title and text" },
		};
		int index = find_remapped(obsolete, ARRAY_SIZE(obsolete), argv[0]);

		if (index != -1)
			return error("%s is obsolete; use the %s view column options instead",
				     obsolete[index][0], obsolete[index][1]);
	}

	return error("Unknown option name: %s", argv[0]);
}

/* Wants: mode request key */
static enum status_code
option_bind_command(int argc, const char *argv[])
{
	struct key key[1];
	size_t keys = 0;
	enum request request;
	struct keymap *keymap;
	const char *key_arg;

	if (argc < 3)
		return error("Invalid key binding: bind keymap key action");

	if (!(keymap = get_keymap(argv[0], strlen(argv[0])))) {
		if (!strcmp(argv[0], "branch"))
			keymap = get_keymap("refs", strlen("refs"));
		if (!keymap)
			return error("Unknown key map: %s", argv[0]);
	}

	for (keys = 0, key_arg = argv[1]; *key_arg && keys < ARRAY_SIZE(key); keys++) {
		if (get_key_value(&key_arg, &key[keys]) == ERR)
			return error("Unknown key combo: %s", argv[1]);
	}

	if (*key_arg && keys == ARRAY_SIZE(key))
		return error("Max %zu keys are allowed in key combos: %s", ARRAY_SIZE(key), argv[1]);

	request = get_request(argv[2]);
	if (request == REQ_UNKNOWN) {
		static const char *obsolete[][2] = {
			{ "view-branch",		"view-refs" },
		};
		static const char *toggles[][2] = {
			{ "diff-context-down",		"diff-context" },
			{ "diff-context-up",		"diff-context" },
			{ "toggle-author",		"author-display" },
			{ "toggle-changes",		"show-changes" },
			{ "toggle-commit-order",	"show-commit-order" },
			{ "toggle-date",		"date-display" },
			{ "toggle-file-filter",		"file-filter" },
			{ "toggle-file-size",		"file-size-display" },
			{ "toggle-filename",		"filename-display" },
			{ "toggle-graphic",		"show-graphic" },
			{ "toggle-id",			"id-display" },
			{ "toggle-ignore-space",	"show-ignore-space" },
			{ "toggle-lineno",		"line-number-display" },
			{ "toggle-refs",		"commit-title-refs" },
			{ "toggle-rev-graph",		"commit-title-graph" },
			{ "toggle-sort-field",		"sort-field" },
			{ "toggle-sort-order",		"sort-order" },
			{ "toggle-title-overflow",	"commit-title-overflow" },
			{ "toggle-untracked-dirs",	"status-untracked-dirs" },
			{ "toggle-vertical-split",	"show-vertical-split" },
		};
		int alias;

		alias = find_remapped(obsolete, ARRAY_SIZE(obsolete), argv[2]);
		if (alias != -1) {
			const char *action = obsolete[alias][1];

			add_keybinding(keymap, get_request(action), key, keys);
			return error("%s has been renamed to %s",
				     obsolete[alias][0], action);
		}

		alias = find_remapped(toggles, ARRAY_SIZE(toggles), argv[2]);
		if (alias != -1) {
			const char *action = toggles[alias][0];
			const char *arg = prefixcmp(action, "diff-context-")
					? NULL : (strstr(action, "-down") ? "-1" : "+1");
			const char *toggle[] = { ":toggle", toggles[alias][1], arg, NULL};
			enum status_code code = add_run_request(keymap, key, keys, toggle);

			if (code == SUCCESS)
				code = error("%s has been replaced by `:toggle %s%s%s'",
					     action, toggles[alias][1],
					     arg ? " " : "", arg ? arg : "");
			return code;
		}
	}

	if (request == REQ_UNKNOWN)
		return add_run_request(keymap, key, keys, argv + 2);

	return add_keybinding(keymap, request, key, keys);
}


static enum status_code load_option_file(const char *path);

static enum status_code
option_source_command(int argc, const char *argv[])
{
	enum status_code code;

	if (argc < 1)
		return error("Invalid source command: source path");

	code = load_option_file(argv[0]);

	return code == ERROR_FILE_DOES_NOT_EXIST
		? error("File does not exist: %s", argv[0]) : code;
}

enum status_code
set_option(const char *opt, int argc, const char *argv[])
{
	if (!strcmp(opt, "color"))
		return option_color_command(argc, argv);

	if (!strcmp(opt, "set"))
		return option_set_command(argc, argv);

	if (!strcmp(opt, "bind"))
		return option_bind_command(argc, argv);

	if (!strcmp(opt, "source"))
		return option_source_command(argc, argv);

	return error("Unknown option command: %s", opt);
}

struct config_state {
	const char *path;
	int lineno;
	bool errors;
};

static int
read_option(char *opt, size_t optlen, char *value, size_t valuelen, void *data)
{
	struct config_state *config = data;
	enum status_code status = ERROR_NO_OPTION_VALUE;

	config->lineno++;

	/* Check for comment markers, since read_properties() will
	 * only ensure opt and value are split at first " \t". */
	optlen = strcspn(opt, "#");
	if (optlen == 0)
		return OK;

	if (opt[optlen] == 0) {
		/* Look for comment endings in the value. */
		size_t len = strcspn(value, "#");
		const char *argv[SIZEOF_ARG];
		int argc = 0;

		if (len < valuelen) {
			valuelen = len;
			value[valuelen] = 0;
		}

		if (!argv_from_string(argv, &argc, value))
			status = error("Too many option arguments for %s", opt);
		else
			status = set_option(opt, argc, argv);
	}

	if (status != SUCCESS) {
		warn("%s:%d: %s", config->path, config->lineno,
		     get_status_message(status));
		config->errors = TRUE;
	}

	/* Always keep going if errors are encountered. */
	return OK;
}

static enum status_code
load_option_file(const char *path)
{
	struct config_state config = { path, 0, FALSE };
	struct io io;
	char buf[SIZEOF_STR];

	/* Do not read configuration from stdin if set to "" */
	if (!path || !strlen(path))
		return SUCCESS;

	if (!prefixcmp(path, "~/")) {
		const char *home = getenv("HOME");

		if (!home || !string_format(buf, "%s/%s", home, path + 2))
			return error("Failed to expand ~ to user home directory");
		path = buf;
	}

	/* It's OK that the file doesn't exist. */
	if (!io_open(&io, "%s", path)) {
		/* XXX: Must return ERROR_FILE_DOES_NOT_EXIST so missing
		 * system tigrc is detected properly. */
		if (io_error(&io) == ENOENT)
			return ERROR_FILE_DOES_NOT_EXIST;
		return error("Error loading file %s: %s", path, strerror(io_error(&io)));
	}

	if (io_load(&io, " \t", read_option, &config) == ERR ||
	    config.errors == TRUE)
		warn("Errors while loading %s.", path);
	return SUCCESS;
}

extern const char *builtin_config;

int
load_options(void)
{
	const char *tigrc_user = getenv("TIGRC_USER");
	const char *tigrc_system = getenv("TIGRC_SYSTEM");
	const char *tig_diff_opts = getenv("TIG_DIFF_OPTS");
	const bool diff_opts_from_args = !!opt_diff_options;
	bool custom_tigrc_system = !!tigrc_system;

	opt_file_filter = TRUE;
	if (!find_option_info_by_value(&opt_diff_context)->seen)
		opt_diff_context = -3;

	if (!custom_tigrc_system)
		tigrc_system = SYSCONFDIR "/tigrc";

	if (load_option_file(tigrc_system) == ERROR_FILE_DOES_NOT_EXIST && !custom_tigrc_system) {
		struct config_state config = { "<built-in>", 0, FALSE };
		struct io io;

		if (!io_from_string(&io, builtin_config) ||
		    !io_load(&io, " \t", read_option, &config) == ERR ||
		    config.errors == TRUE)
			die("Error in built-in config");
	}

	if (!tigrc_user)
		tigrc_user = "~/.tigrc";
	load_option_file(tigrc_user);

	if (!diff_opts_from_args && tig_diff_opts && *tig_diff_opts) {
		static const char *diff_opts[SIZEOF_ARG] = { NULL };
		char buf[SIZEOF_STR];
		int argc = 0;

		if (!string_format(buf, "%s", tig_diff_opts) ||
		    !argv_from_string(diff_opts, &argc, buf))
			die("TIG_DIFF_OPTS contains too many arguments");
		else if (!argv_copy(&opt_diff_options, diff_opts))
			die("Failed to format TIG_DIFF_OPTS arguments");
	}

	return OK;
}

/*
 * Repository properties
 */

static void
set_remote_branch(const char *name, const char *value, size_t valuelen)
{
	if (!strcmp(name, ".remote")) {
		string_ncopy(repo.remote, value, valuelen);

	} else if (*repo.remote && !strcmp(name, ".merge")) {
		size_t from = strlen(repo.remote);

		if (!prefixcmp(value, "refs/heads/"))
			value += STRING_SIZE("refs/heads/");

		if (!string_format_from(repo.remote, &from, "/%s", value))
			repo.remote[0] = 0;
	}
}

static void
set_repo_config_option(char *name, char *value, enum status_code (*cmd)(int, const char **))
{
	const char *argv[SIZEOF_ARG] = { name, "=" };
	int argc = 1 + (cmd == option_set_command);
	enum status_code code;

	if (!argv_from_string(argv, &argc, value))
		code = error("Too many arguments");
	else
		code = cmd(argc, argv);

	if (code != SUCCESS)
		warn("Option 'tig.%s': %s", name, get_status_message(code));
}

static void
set_work_tree(const char *value)
{
	char cwd[SIZEOF_STR];

	if (!getcwd(cwd, sizeof(cwd)))
		die("Failed to get cwd path: %s", strerror(errno));
	if (chdir(cwd) < 0)
		die("Failed to chdir(%s): %s", cwd, strerror(errno));
	if (chdir(repo.git_dir) < 0)
		die("Failed to chdir(%s): %s", repo.git_dir, strerror(errno));
	if (!getcwd(repo.git_dir, sizeof(repo.git_dir)))
		die("Failed to get git path: %s", strerror(errno));
	if (chdir(value) < 0)
		die("Failed to chdir(%s): %s", value, strerror(errno));
	if (!getcwd(cwd, sizeof(cwd)))
		die("Failed to get cwd path: %s", strerror(errno));
	if (setenv("GIT_WORK_TREE", cwd, TRUE))
		die("Failed to set GIT_WORK_TREE to '%s'", cwd);
	if (setenv("GIT_DIR", repo.git_dir, TRUE))
		die("Failed to set GIT_DIR to '%s'", repo.git_dir);
	repo.is_inside_work_tree = TRUE;
}

static struct line_info *
parse_git_color_option(struct line_info *info, char *value)
{
	const char *argv[SIZEOF_ARG];
	int argc = 0;
	bool first_color = TRUE;
	int i;

	if (!argv_from_string(argv, &argc, value))
		return NULL;

	info->fg = COLOR_DEFAULT;
	info->bg = COLOR_DEFAULT;
	info->attr = 0;

	for (i = 0; i < argc; i++) {
		int attr = 0;

		if (set_attribute(&attr, argv[i])) {
			info->attr |= attr;

		} else if (set_color(&attr, argv[i])) {
			if (first_color)
				info->fg = attr;
			else
				info->bg = attr;
			first_color = FALSE;
		}
	}
	return info;
}

static void
set_git_color_option(const char *name, char *value)
{
	static const char *git_colors[][2] = {
		{ "branch.current", "main-head" },
		{ "branch.local", "main-ref" },
		{ "branch.plain", "main-ref" },
		{ "branch.remote", "main-remote" },

		{ "diff.meta", "diff-header" },
		{ "diff.meta", "diff-index" },
		{ "diff.meta", "diff-oldmode" },
		{ "diff.meta", "diff-newmode" },
		{ "diff.frag", "diff-chunk" },
		{ "diff.old", "diff-del" },
		{ "diff.new", "diff-add" },

		{ "grep.filename", "grep.file" },
		{ "grep.linenumber", "grep.line-number" },
		{ "grep.separator", "grep.delimiter" },

		{ "status.branch", "status.header" },
		{ "status.added", "stat-staged" },
		{ "status.updated", "stat-staged" },
		{ "status.changed", "stat-unstaged" },
		{ "status.untracked", "stat-untracked" },
	};
	struct line_info parsed = {};
	int i;

	if (!opt_read_git_colors)
		return;

	i = find_remapped(git_colors, ARRAY_SIZE(git_colors), name);
	if (i < 0 || !parse_git_color_option(&parsed, value))
		return;

	for (; i < ARRAY_SIZE(git_colors) && !strcasecmp(git_colors[i][0], name); i++) {
		struct line_rule rule = {};
		const char *prefix = NULL;
		struct line_info *info;

		if (parse_color_name(git_colors[i][1], &rule, &prefix) == SUCCESS &&
		    (info = add_line_rule(prefix, &rule))) {
			info->fg = parsed.fg;
			info->bg = parsed.bg;
			info->attr = parsed.attr;
		}
	}
}

static void
set_encoding(struct encoding **encoding_ref, const char *arg, bool priority)
{
	if (parse_encoding(encoding_ref, arg, priority) == SUCCESS)
		encoding_arg[0] = 0;
}

static int
read_repo_config_option(char *name, size_t namelen, char *value, size_t valuelen, void *data)
{
	if (!strcmp(name, "i18n.commitencoding"))
		set_encoding(&default_encoding, value, FALSE);

	else if (!strcmp(name, "gui.encoding"))
		set_encoding(&default_encoding, value, TRUE);

	else if (!strcmp(name, "core.editor"))
		string_ncopy(opt_editor, value, valuelen);

	else if (!strcmp(name, "core.worktree"))
		set_work_tree(value);

	else if (!strcmp(name, "core.abbrev"))
		parse_int(&opt_id_width, value, 0, SIZEOF_REV - 1);

	else if (!prefixcmp(name, "tig.color."))
		set_repo_config_option(name + 10, value, option_color_command);

	else if (!prefixcmp(name, "tig.bind."))
		set_repo_config_option(name + 9, value, option_bind_command);

	else if (!prefixcmp(name, "tig."))
		set_repo_config_option(name + 4, value, option_set_command);

	else if (!prefixcmp(name, "color."))
		set_git_color_option(name + STRING_SIZE("color."), value);

	else if (*repo.head && !prefixcmp(name, "branch.") &&
		 !strncmp(name + 7, repo.head, strlen(repo.head)))
		set_remote_branch(name + 7 + strlen(repo.head), value, valuelen);

	else if (!strcmp(name, "diff.context")) {
		if (!find_option_info_by_value(&opt_diff_context)->seen)
			opt_diff_context = -atoi(value);
	}

	return OK;
}

int
load_git_config(void)
{
	const char *config_list_argv[] = { "git", "config", "--list", NULL };

	return io_run_load(config_list_argv, "=", read_repo_config_option, NULL);
}

/* vim: set ts=8 sw=8 noexpandtab: */
