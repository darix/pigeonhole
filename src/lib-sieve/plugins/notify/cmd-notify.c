/* Copyright (c) 2002-2018 Pigeonhole authors, see the included COPYING file
 */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "ioloop.h"
#include "str-sanitize.h"
#include "ostream.h"
#include "message-date.h"
#include "mail-storage.h"

#include "rfc2822.h"

#include "sieve-common.h"
#include "sieve-stringlist.h"
#include "sieve-code.h"
#include "sieve-extensions.h"
#include "sieve-commands.h"
#include "sieve-actions.h"
#include "sieve-validator.h"
#include "sieve-generator.h"
#include "sieve-interpreter.h"
#include "sieve-dump.h"
#include "sieve-result.h"
#include "sieve-address.h"
#include "sieve-message.h"
#include "sieve-smtp.h"

#include "ext-notify-common.h"
#include "ext-notify-limits.h"

#include <ctype.h>

/* Notify command (DEPRECATED)
 *
 * Syntax:
 *   notify [":method" string] [":id" string] [":options" string-list]
 *          [<":low" / ":normal" / ":high">] ["message:" string]
 *
 */

static bool cmd_notify_registered
	(struct sieve_validator *valdtr, const struct sieve_extension *ext,
		struct sieve_command_registration *cmd_reg);
static bool cmd_notify_pre_validate
	(struct sieve_validator *valdtr, struct sieve_command *cmd);
static bool cmd_notify_validate
	(struct sieve_validator *valdtr, struct sieve_command *cmd);
static bool cmd_notify_generate
	(const struct sieve_codegen_env *cgenv,
		struct sieve_command *ctx);

const struct sieve_command_def cmd_notify_old = {
	.identifier = "notify",
	.type = SCT_COMMAND,
	.positional_args = 0,
	.subtests = 0,
	.block_allowed = FALSE,
	.block_required = FALSE,
	.registered = cmd_notify_registered,
	.pre_validate = cmd_notify_pre_validate,
	.validate = cmd_notify_validate,
	.generate = cmd_notify_generate
};

/*
 * Tagged arguments
 */

/* Forward declarations */

static bool cmd_notify_validate_string_tag
	(struct sieve_validator *valdtr, struct sieve_ast_argument **arg,
		struct sieve_command *cmd);
static bool cmd_notify_validate_stringlist_tag
	(struct sieve_validator *valdtr, struct sieve_ast_argument **arg,
		struct sieve_command *cmd);

/* Argument objects */

static const struct sieve_argument_def notify_method_tag = {
	.identifier = "method",
	.validate = cmd_notify_validate_string_tag
};

static const struct sieve_argument_def notify_options_tag = {
	.identifier = "options",
	.validate = cmd_notify_validate_stringlist_tag
};

static const struct sieve_argument_def notify_id_tag = {
	.identifier = "id",
	.validate = cmd_notify_validate_string_tag
};

static const struct sieve_argument_def notify_message_tag = {
	.identifier = "message",
	.validate = cmd_notify_validate_string_tag
};

/*
 * Notify operation
 */

static bool cmd_notify_operation_dump
	(const struct sieve_dumptime_env *denv, sieve_size_t *address);
static int cmd_notify_operation_execute
	(const struct sieve_runtime_env *renv, sieve_size_t *address);

const struct sieve_operation_def notify_old_operation = {
	.mnemonic = "NOTIFY",
	.ext_def = &notify_extension,
	.code = EXT_NOTIFY_OPERATION_NOTIFY,
	.dump = cmd_notify_operation_dump,
	.execute = cmd_notify_operation_execute
};

/* Codes for optional operands */

enum cmd_notify_optional {
  OPT_END,
  OPT_MESSAGE,
  OPT_IMPORTANCE,
  OPT_OPTIONS,
  OPT_ID
};

/*
 * Notify action
 */

/* Forward declarations */

static int act_notify_check_duplicate
	(const struct sieve_runtime_env *renv,
		const struct sieve_action *act,
		const struct sieve_action *act_other);
static void act_notify_print
	(const struct sieve_action *action, const struct sieve_result_print_env *rpenv,
		bool *keep);
static int act_notify_commit
	(const struct sieve_action *action,	const struct sieve_action_exec_env *aenv,
		void *tr_context, bool *keep);

/* Action object */

const struct sieve_action_def act_notify_old = {
	.name = "notify",
	.check_duplicate = act_notify_check_duplicate,
	.print = act_notify_print,
	.commit = act_notify_commit
};

/*
 * Command validation context
 */

struct cmd_notify_context_data {
	struct sieve_ast_argument *id;
	struct sieve_ast_argument *method;
	struct sieve_ast_argument *options;
	struct sieve_ast_argument *message;
};

/*
 * Tag validation
 */

static bool cmd_notify_validate_string_tag
(struct sieve_validator *valdtr, struct sieve_ast_argument **arg,
    struct sieve_command *cmd)
{
	struct sieve_ast_argument *tag = *arg;
	struct cmd_notify_context_data *ctx_data =
		(struct cmd_notify_context_data *) cmd->data;

	/* Detach the tag itself */
	*arg = sieve_ast_arguments_detach(*arg, 1);

	/* Check syntax:
	 *   :id <string>
	 *   :method <string>
	 *   :message <string>
	 */
	if ( !sieve_validate_tag_parameter
		(valdtr, cmd, tag, *arg, NULL, 0, SAAT_STRING, FALSE) )
		return FALSE;

	if ( sieve_argument_is(tag, notify_method_tag) ) {
		ctx_data->method = *arg;

		/* Removed */
		*arg = sieve_ast_arguments_detach(*arg, 1);

	} else if ( sieve_argument_is(tag, notify_id_tag) ) {
		ctx_data->id = *arg;

		/* Skip parameter */
		*arg = sieve_ast_argument_next(*arg);

	} else if ( sieve_argument_is(tag, notify_message_tag) ) {
		ctx_data->message = *arg;

		/* Skip parameter */
		*arg = sieve_ast_argument_next(*arg);
	}

	return TRUE;
}

static bool cmd_notify_validate_stringlist_tag
(struct sieve_validator *valdtr, struct sieve_ast_argument **arg,
	struct sieve_command *cmd)
{
	struct sieve_ast_argument *tag = *arg;
	struct cmd_notify_context_data *ctx_data =
		(struct cmd_notify_context_data *) cmd->data;

	/* Detach the tag itself */
	*arg = sieve_ast_arguments_detach(*arg,1);

	/* Check syntax:
	 *   :options string-list
	 */
	if ( !sieve_validate_tag_parameter
		(valdtr, cmd, tag, *arg, NULL, 0, SAAT_STRING_LIST, FALSE) )
		return FALSE;

	/* Assign context */
	ctx_data->options = *arg;

	/* Skip parameter */
	*arg = sieve_ast_argument_next(*arg);

	return TRUE;
}

/*
 * Command registration
 */

static bool cmd_notify_registered
(struct sieve_validator *valdtr, const struct sieve_extension *ext,
	struct sieve_command_registration *cmd_reg)
{
	sieve_validator_register_tag
		(valdtr, cmd_reg, ext, &notify_method_tag, 0);
	sieve_validator_register_tag
		(valdtr, cmd_reg, ext, &notify_id_tag, OPT_ID);
	sieve_validator_register_tag
		(valdtr, cmd_reg, ext, &notify_message_tag, OPT_MESSAGE);
	sieve_validator_register_tag
		(valdtr, cmd_reg, ext, &notify_options_tag, OPT_OPTIONS);

	ext_notify_register_importance_tags(valdtr, cmd_reg, ext, OPT_IMPORTANCE);

	return TRUE;
}

/*
 * Command validation
 */

static bool cmd_notify_pre_validate
(struct sieve_validator *valdtr ATTR_UNUSED,
	struct sieve_command *cmd)
{
	struct cmd_notify_context_data *ctx_data;

	/* Create context */
	ctx_data = p_new(sieve_command_pool(cmd),	struct cmd_notify_context_data, 1);
	cmd->data = ctx_data;

	return TRUE;
}

static int cmd_notify_address_validate
(void *context, struct sieve_ast_argument *arg)
{
	struct sieve_validator *valdtr = (struct sieve_validator *) context;

	if ( sieve_argument_is_string_literal(arg) ) {
		string_t *address = sieve_ast_argument_str(arg);
		const char *error;
		int result;

		T_BEGIN {
			result = ( sieve_address_validate_str(address, &error) ? 1 : -1 );

			if ( result <= 0 ) {
				sieve_argument_validate_error(valdtr, arg,
					"specified :options address '%s' is invalid for "
					"the mailto notify method: %s",
					str_sanitize(str_c(address), 128), error);
			}
		} T_END;

		return result;
	}

	return 1;
}

static bool cmd_notify_validate
(struct sieve_validator *valdtr, struct sieve_command *cmd)
{
	struct cmd_notify_context_data *ctx_data =
		(struct cmd_notify_context_data *) cmd->data;

	/* Check :method argument */
	if ( ctx_data->method != NULL )	{
		const char *method = sieve_ast_argument_strc(ctx_data->method);

		if ( strcasecmp(method, "mailto") != 0 ) {
			sieve_command_validate_error(valdtr, cmd,
				"the notify command of the deprecated notify extension "
				"only supports the 'mailto' notification method");
			return FALSE;
		}
	}

	/* Check :options argument */
	if ( ctx_data->options != NULL ) {
		struct sieve_ast_argument *option = ctx_data->options;

		/* Parse and check options */
		if ( sieve_ast_stringlist_map
			(&option, (void *) valdtr, cmd_notify_address_validate) <= 0 ) {
			return FALSE;
		}
	} else {
		sieve_command_validate_warning(valdtr, cmd,
			"no :options (and hence recipients) specified for the notify command");
	}

	return TRUE;
}

/*
 * Code generation
 */

static bool cmd_notify_generate
(const struct sieve_codegen_env *cgenv, struct sieve_command *cmd)
{
	sieve_operation_emit(cgenv->sblock, cmd->ext, &notify_old_operation);

	/* Generate arguments */
	return sieve_generate_arguments(cgenv, cmd, NULL);
}

/*
 * Code dump
 */

static bool cmd_notify_operation_dump
(const struct sieve_dumptime_env *denv, sieve_size_t *address)
{
	int opt_code = 0;

	sieve_code_dumpf(denv, "NOTIFY");
	sieve_code_descend(denv);

	/* Dump optional operands */
	for (;;) {
		int opt;
		bool opok = TRUE;

		if ( (opt=sieve_opr_optional_dump(denv, address, &opt_code)) < 0 )
			return FALSE;

		if ( opt == 0 ) break;

		switch ( opt_code ) {
		case OPT_IMPORTANCE:
			opok = sieve_opr_number_dump(denv, address, "importance");
			break;
		case OPT_ID:
			opok = sieve_opr_string_dump(denv, address, "id");
			break;
		case OPT_OPTIONS:
			opok = sieve_opr_stringlist_dump(denv, address, "options");
			break;
		case OPT_MESSAGE:
			opok = sieve_opr_string_dump(denv, address, "message");
			break;
		default:
			return FALSE;
		}

		if ( !opok ) return FALSE;
	}

	return TRUE;
}

/*
 * Code execution
 */


static int cmd_notify_operation_execute
(const struct sieve_runtime_env *renv, sieve_size_t *address)
{
	const struct sieve_extension *this_ext = renv->oprtn->ext;
	struct ext_notify_action *act;
	pool_t pool;
	int opt_code = 0;
	sieve_number_t importance = 1;
	struct sieve_stringlist *options = NULL;
	string_t *message = NULL, *id = NULL;
	int ret = 0;

	/*
	 * Read operands
	 */

	/* Optional operands */

	for (;;) {
		int opt;

		if ( (opt=sieve_opr_optional_read(renv, address, &opt_code)) < 0 )
			return SIEVE_EXEC_BIN_CORRUPT;

		if ( opt == 0 ) break;

		switch ( opt_code ) {
		case OPT_IMPORTANCE:
			ret = sieve_opr_number_read(renv, address, "importance", &importance);
			break;
		case OPT_ID:
			ret = sieve_opr_string_read(renv, address, "id", &id);
			break;
		case OPT_MESSAGE:
			ret = sieve_opr_string_read(renv, address, "from", &message);
			break;
		case OPT_OPTIONS:
			ret = sieve_opr_stringlist_read(renv, address, "options", &options);
			break;
		default:
			sieve_runtime_trace_error(renv, "unknown optional operand");
			return SIEVE_EXEC_BIN_CORRUPT;
		}

		if ( ret <= 0 ) return ret;
	}

	/*
	 * Perform operation
	 */

	/* Enforce 0 < importance < 4 (just to be sure) */

	if ( importance < 1 )
		importance = 1;
	else if ( importance > 3 )
		importance = 3;

	/* Trace */

	sieve_runtime_trace(renv, SIEVE_TRLVL_ACTIONS, "notify action");

	/* Compose action */
	if ( options != NULL ) {
		string_t *raw_address;
		string_t *out_message;

		pool = sieve_result_pool(renv->result);
		act = p_new(pool, struct ext_notify_action, 1);
		if ( id != NULL )
				act->id = p_strdup(pool, str_c(id));
		act->importance = importance;

		/* Process message */

		out_message = t_str_new(1024);
		if ( (ret=ext_notify_construct_message
			(renv, (message == NULL ? NULL : str_c(message)), out_message)) <= 0 )
			return ret;
		act->message = p_strdup(pool, str_c(out_message));

		/* Normalize and verify all :options addresses */

		sieve_stringlist_reset(options);

		p_array_init(&act->recipients, pool, 4);

		raw_address = NULL;
		while ( (ret=sieve_stringlist_next_item(options, &raw_address)) > 0 ) {
			const char *error = NULL;
			const struct smtp_address *address;

			/* Add if valid address */
			address = sieve_address_parse_str(raw_address, &error);
			if ( address != NULL ) {
				const struct ext_notify_recipient *rcpts;
				unsigned int rcpt_count, i;

				/* Prevent duplicates */
				rcpts = array_get(&act->recipients, &rcpt_count);

				for ( i = 0; i < rcpt_count; i++ ) {
					if ( smtp_address_equals(rcpts[i].address, address) )
						break;
				}

				/* Add only if unique */
				if ( i != rcpt_count ) {
					sieve_runtime_warning(renv, NULL,
						"duplicate recipient '%s' specified in the :options argument of "
						"the deprecated notify command",
						str_sanitize(str_c(raw_address), 128));

				}	else if
					( array_count(&act->recipients) >= EXT_NOTIFY_MAX_RECIPIENTS ) {
					sieve_runtime_warning(renv, NULL,
						"more than the maximum %u recipients are specified "
						"for the deprecated notify command; "
						"the rest is discarded", EXT_NOTIFY_MAX_RECIPIENTS);
					break;

				} else {
					struct ext_notify_recipient recipient;

					recipient.full = p_strdup(pool, str_c(raw_address));
					recipient.address = smtp_address_clone(pool, address);

					array_append(&act->recipients, &recipient, 1);
				}
			} else {
				sieve_runtime_error(renv, NULL,
					"specified :options address '%s' is invalid for "
					"the deprecated notify command: %s",
					str_sanitize(str_c(raw_address), 128), error);
				return SIEVE_EXEC_FAILURE;
			}
		}

		if ( ret < 0 ) {
			sieve_runtime_trace_error(renv, "invalid options stringlist");
			return SIEVE_EXEC_BIN_CORRUPT;
		}

		if ( sieve_result_add_action
			(renv, this_ext, &act_notify_old, NULL, (void *) act, 0, FALSE) < 0 )
			return SIEVE_EXEC_FAILURE;
	}

	return SIEVE_EXEC_OK;
}

/*
 * Action
 */

/* Runtime verification */

static int act_notify_check_duplicate
(const struct sieve_runtime_env *renv ATTR_UNUSED,
	const struct sieve_action *act ATTR_UNUSED,
	const struct sieve_action *act_other ATTR_UNUSED)
{
	struct ext_notify_action *new_nact, *old_nact;
	const struct ext_notify_recipient *new_rcpts;
	const struct ext_notify_recipient *old_rcpts;
	unsigned int new_count, old_count, i, j;
	unsigned int del_start = 0, del_len = 0;

	if ( act->context == NULL || act_other->context == NULL )
		return 0;

	new_nact = (struct ext_notify_action *) act->context;
	old_nact = (struct ext_notify_action *) act_other->context;

	new_rcpts = array_get(&new_nact->recipients, &new_count);
	old_rcpts = array_get(&old_nact->recipients, &old_count);

	for ( i = 0; i < new_count; i++ ) {
		for ( j = 0; j < old_count; j++ ) {
			if ( smtp_address_equals
				(new_rcpts[i].address, old_rcpts[j].address) )
				break;
		}

		if ( j == old_count ) {
			/* Not duplicate */
			if ( del_len > 0 ) {
				/* Perform pending deletion */
				array_delete(&new_nact->recipients, del_start, del_len);

				/* Make sure the loop integrity is maintained */
				i -= del_len;
				new_rcpts = array_get(&new_nact->recipients, &new_count);
			}

			del_len = 0;
		} else {
			/* Mark deletion */
			if ( del_len == 0 )
				del_start = i;
			del_len++;
		}
	}

	/* Perform pending deletion */
	if ( del_len > 0 ) {
		array_delete(&new_nact->recipients, del_start, del_len);
	}

	return ( array_count(&new_nact->recipients) > 0 ? 0 : 1 );
}

/* Result printing */

static void act_notify_print
(const struct sieve_action *action,	const struct sieve_result_print_env *rpenv,
	bool *keep ATTR_UNUSED)
{
	const struct ext_notify_action *act =
		(const struct ext_notify_action *) action->context;
	const struct ext_notify_recipient *recipients;
	unsigned int count, i;

	sieve_result_action_printf
		( rpenv, "send (deprecated) notification with method 'mailto':");

	/* Print main method parameters */

	sieve_result_printf
		( rpenv, "    => importance    : %llu\n",
			(unsigned long long)act->importance);

	if ( act->message != NULL )
		sieve_result_printf
			( rpenv, "    => message       : %s\n", act->message);

	if ( act->id != NULL )
		sieve_result_printf
			( rpenv, "    => id            : %s \n", act->id);

	/* Print mailto: recipients */

	sieve_result_printf
		( rpenv, "    => recipients    :\n" );

	recipients = array_get(&act->recipients, &count);
	if ( count == 0 ) {
		sieve_result_printf(rpenv, "       NONE, action has no effect\n");
	} else {
		for ( i = 0; i < count; i++ ) {
			sieve_result_printf
				( rpenv, "       + To: %s\n", recipients[i].full);
		}
	}

	/* Finish output with an empty line */

	sieve_result_printf(rpenv, "\n");
}

/* Result execution */

static bool contains_8bit(const char *msg)
{
	const unsigned char *s = (const unsigned char *)msg;

	for (; *s != '\0'; s++) {
		if ((*s & 0x80) != 0)
			return TRUE;
	}
	return FALSE;
}

static bool act_notify_send
(const struct sieve_action_exec_env *aenv,
	const struct ext_notify_action *act)
{
	const struct sieve_script_env *senv = aenv->scriptenv;
	const struct ext_notify_recipient *recipients;
	struct sieve_smtp_context *sctx;
	unsigned int count, i;
	struct ostream *output;
	string_t *msg, *to, *all;
	const char *outmsgid, *error;
	int ret;

	/* Get recipients */
	recipients = array_get(&act->recipients, &count);
	if ( count == 0  ) {
		sieve_result_warning(aenv,
			"notify action specifies no recipients; action has no effect");
		return TRUE;
	}

	/* Just to be sure */
	if ( !sieve_smtp_available(senv) ) {
		sieve_result_global_warning(aenv,
			"notify action has no means to send mail");
		return TRUE;
	}

	/* Compose common headers */
	msg = t_str_new(512);
	rfc2822_header_write(msg, "X-Sieve", SIEVE_IMPLEMENTATION);
	rfc2822_header_write(msg, "Date", message_date_create(ioloop_time));

	/* Set importance */
	switch ( act->importance ) {
	case 1:
		rfc2822_header_write(msg, "X-Priority", "1 (Highest)");
		rfc2822_header_write(msg, "Importance", "High");
		break;
	case 3:
		rfc2822_header_write(msg, "X-Priority", "5 (Lowest)");
		rfc2822_header_write(msg, "Importance", "Low");
		break;
	case 2:
	default:
		rfc2822_header_write(msg, "X-Priority", "3 (Normal)");
		rfc2822_header_write(msg, "Importance", "Normal");
		break;
	}

	rfc2822_header_write(msg, "From",
		sieve_get_postmaster_address(senv));

	rfc2822_header_write(msg, "Subject", "[SIEVE] New mail notification");

	rfc2822_header_write(msg, "Auto-Submitted", "auto-generated (notify)");
	rfc2822_header_write(msg, "Precedence", "bulk");

	rfc2822_header_write(msg, "MIME-Version", "1.0");
	if (contains_8bit(act->message)) {
		rfc2822_header_write(msg,
			"Content-Type", "text/plain; charset=utf-8");
		rfc2822_header_write(msg, "Content-Transfer-Encoding", "8bit");
	} else {
		rfc2822_header_write(msg,
			"Content-Type", "text/plain; charset=us-ascii");
		rfc2822_header_write(msg, "Content-Transfer-Encoding", "7bit");
	}

	outmsgid = sieve_message_get_new_id(aenv->svinst);
	rfc2822_header_write(msg, "Message-ID", outmsgid);

	if ( (aenv->flags & SIEVE_EXECUTE_FLAG_NO_ENVELOPE) == 0 &&
		sieve_message_get_sender(aenv->msgctx) != NULL ) {
		sctx = sieve_smtp_start(senv, sieve_get_postmaster_smtp(senv));
	} else {
		sctx = sieve_smtp_start(senv, NULL);
	}

	/* Add all recipients (and compose To header field) */
	to = t_str_new(128);
	all = t_str_new(256);
	for ( i = 0; i < count; i++ ) {
		sieve_smtp_add_rcpt(sctx, recipients[i].address);
		if ( i > 0 )
			str_append(to, ", ");
		str_append(to, recipients[i].full);
		if ( i < 3) {
			if ( i > 0 )
				str_append(all, ", ");
			str_append(all, smtp_address_encode_path(recipients[i].address));
		} else if (i == 3) {
			str_printfa(all, ", ... (%u total)", count);
		}
	}

	rfc2822_header_write_address(msg, "To", str_c(to));

	/* Generate message body */
	str_printfa(msg, "\r\n%s\r\n", act->message);

	output = sieve_smtp_send(sctx);
	o_stream_nsend(output, str_data(msg), str_len(msg));

	if ( (ret=sieve_smtp_finish(sctx, &error)) <= 0 ) {
		if (ret < 0) {
			sieve_result_global_error(aenv,
				"failed to send mail notification to %s: %s (temporary failure)",
				str_c(all),	str_sanitize(error, 512));
		} else {
			sieve_result_global_log_error(aenv,
				"failed to send mail notification to %s: %s (permanent failure)",
				str_c(all),	str_sanitize(error, 512));
		}
	} else {
		sieve_result_global_log(aenv,
			"sent mail notification to %s", str_c(all));
	}

	return TRUE;
}

static int act_notify_commit
(const struct sieve_action *action, const struct sieve_action_exec_env *aenv,
	void *tr_context ATTR_UNUSED, bool *keep ATTR_UNUSED)
{
	struct mail *mail = aenv->msgdata->mail;
	const struct ext_notify_action *act =
		(const struct ext_notify_action *) action->context;
	const char *const *hdsp;
	bool result;
	int ret;

	/* Is the message an automatic reply ? */
	if ( (ret=mail_get_headers(mail, "auto-submitted", &hdsp)) < 0 ) {
		return sieve_result_mail_error(aenv, mail,
			"notify action: "
			"failed to read `auto-submitted' header field");
	}

	/* Theoretically multiple headers could exist, so lets make sure */
	if (ret > 0) {
		while ( *hdsp != NULL ) {
			if ( strcasecmp(*hdsp, "no") != 0 ) {
				const struct smtp_address *sender = NULL;
				const char *from;

				if ( (aenv->flags & SIEVE_EXECUTE_FLAG_NO_ENVELOPE) == 0 )
					sender = sieve_message_get_sender(aenv->msgctx);
				from = (sender == NULL ? "" : t_strdup_printf
					(" from <%s>", smtp_address_encode(sender)));

				sieve_result_global_log(aenv,
					"not sending notification for auto-submitted message%s",
					from);
				return SIEVE_EXEC_OK;
			}
			hdsp++;
		}
	}

	T_BEGIN {
		result = act_notify_send(aenv, act);
	} T_END;

	return ( result ? SIEVE_EXEC_OK : SIEVE_EXEC_FAILURE );
}







