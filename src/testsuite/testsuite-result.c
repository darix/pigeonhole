/* Copyright (c) 2002-2008 Dovecot Sieve authors, see the included COPYING file
 */

#include "sieve-common.h"
#include "sieve-error.h"
#include "sieve-actions.h"
#include "sieve-interpreter.h"
#include "sieve-result.h"

#include "testsuite-common.h"
#include "testsuite-result.h"

static struct sieve_result *_testsuite_result;

void testsuite_result_init(void)
{
	_testsuite_result = NULL;
}

void testsuite_result_deinit(void)
{
	if ( _testsuite_result != NULL ) {
		sieve_result_unref(&_testsuite_result);
	}
}

void testsuite_result_assign(struct sieve_result *result)
{
	if ( _testsuite_result != NULL ) {
		sieve_result_unref(&_testsuite_result);
	}

	_testsuite_result = result;
}

struct sieve_result_iterate_context *testsuite_result_iterate_init(void)
{
	if ( _testsuite_result == NULL )
		return NULL;

	return sieve_result_iterate_init(_testsuite_result);
}

bool testsuite_result_execute(const struct sieve_runtime_env *renv)
{
	struct sieve_script_env scriptenv;
	struct sieve_exec_status estatus;
	int ret;

	if ( _testsuite_result == NULL ) {
		sieve_runtime_error(renv, sieve_error_script_location(renv->script,0),
			"testsuite: no result evaluated yet");
		return FALSE;
	}

	testsuite_script_clear_messages();

	/* Compose script execution environment */
	memset(&scriptenv, 0, sizeof(scriptenv));
	scriptenv.default_mailbox = "INBOX";
	scriptenv.namespaces = NULL;
	scriptenv.username = "user";
	scriptenv.hostname = "host.example.com";
	scriptenv.postmaster_address = "postmaster@example.com";
	scriptenv.smtp_open = NULL;
	scriptenv.smtp_close = NULL;
	scriptenv.duplicate_mark = NULL;
	scriptenv.duplicate_check = NULL;
	
	/* Execute the result */	
	ret=sieve_result_execute
		(_testsuite_result, renv->msgdata, &scriptenv, &estatus);
	
	return ( ret > 0 );
}


