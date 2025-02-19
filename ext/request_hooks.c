#include "request_hooks.h"

#include <Zend/zend.h>
#include <Zend/zend_compile.h>
#include <exceptions/exceptions.h>
#include <php_main.h>
#include <string.h>

#include <ext/standard/php_filestat.h>

#include "ddtrace.h"
#include "engine_hooks.h"
#include "logging.h"

ZEND_EXTERN_MODULE_GLOBALS(ddtrace);

int dd_execute_php_file(const char *filename) {
    int filename_len = strlen(filename);
    if (filename_len == 0) {
        return FAILURE;
    }
    zval dummy;
    zend_file_handle file_handle;
    zend_op_array *new_op_array;
    zval result;
    int ret, rv = false;

    ddtrace_error_handling eh_stream;
    // Using an EH_THROW here causes a non-recoverable zend_bailout()
    ddtrace_backup_error_handling(&eh_stream, EH_NORMAL);
    zend_bool _original_cg_multibyte = CG(multibyte);
    CG(multibyte) = false;

#if PHP_VERSION_ID < 80100
    ret = php_stream_open_for_zend_ex(filename, &file_handle, USE_PATH | STREAM_OPEN_FOR_INCLUDE);
#else
    zend_string *fn = zend_string_init(filename, filename_len, 0);
    zend_stream_init_filename_ex(&file_handle, fn);
    ret = php_stream_open_for_zend_ex(&file_handle, USE_PATH | STREAM_OPEN_FOR_INCLUDE);
    zend_string_release(fn);
#endif

    if (get_DD_TRACE_DEBUG() && PG(last_error_message) && eh_stream.message != PG(last_error_message)) {
#if PHP_VERSION_ID < 80000
        char *error = PG(last_error_message);
#else
        char *error = ZSTR_VAL(PG(last_error_message));
#endif
        ddtrace_log_errf("Error raised while opening request-init-hook stream: %s in %s on line %d", error,
                         PG(last_error_file), PG(last_error_lineno));
    }

    ddtrace_restore_error_handling(&eh_stream);

    if (!EG(exception) && ret == SUCCESS) {
        zend_string *opened_path;
        if (!file_handle.opened_path) {
            file_handle.opened_path = zend_string_init(filename, filename_len, 0);
        }
        opened_path = zend_string_copy(file_handle.opened_path);
        ZVAL_NULL(&dummy);

        if (zend_hash_add(&EG(included_files), opened_path, &dummy)) {
            new_op_array = zend_compile_file(&file_handle, ZEND_REQUIRE);
            zend_destroy_file_handle(&file_handle);
        } else {
            new_op_array = NULL;
#if PHP_VERSION_ID < 80100
            zend_file_handle_dtor(&file_handle);
#else
            zend_destroy_file_handle(&file_handle);
#endif
        }

        zend_string_release(opened_path);
        if (new_op_array) {
            ZVAL_UNDEF(&result);

            ddtrace_error_handling eh;
            ddtrace_backup_error_handling(&eh, EH_THROW);

            zend_execute(new_op_array, &result);

            if (get_DD_TRACE_DEBUG() && PG(last_error_message) && eh.message != PG(last_error_message)) {
#if PHP_VERSION_ID < 80000
                char *error = PG(last_error_message);
#else
                char *error = ZSTR_VAL(PG(last_error_message));
#endif
#if PHP_VERSION_ID < 80100
                char *error_filename = PG(last_error_file);
#else
                char *error_filename = ZSTR_VAL(PG(last_error_file));
#endif
                ddtrace_log_errf("Error raised in request init hook: %s in %s on line %d", error, error_filename,
                                 PG(last_error_lineno));
            }

            ddtrace_restore_error_handling(&eh);

            destroy_op_array(new_op_array);
            efree(new_op_array);
            if (!EG(exception)) {
                zval_ptr_dtor(&result);
            } else if (get_DD_TRACE_DEBUG()) {
                zend_object *ex = EG(exception);

                const char *type = ex->ce->name->val;
                zend_string *msg = zai_exception_message(ex);
                ddtrace_log_errf("%s thrown in request init hook: %s", type, ZSTR_VAL(msg));
            }
            ddtrace_maybe_clear_exception();
            rv = true;
        }
    } else {
        ddtrace_maybe_clear_exception();
        ddtrace_log_debugf("Error opening request init hook: %s", filename);
#if PHP_VERSION_ID >= 80100
        zend_destroy_file_handle(&file_handle);
#endif
    }
    CG(multibyte) = _original_cg_multibyte;

    return rv;
}

int dd_execute_auto_prepend_file(char *auto_prepend_file) {
    zend_file_handle prepend_file;
    // We must emulate being at the root of the stack so that exception handling sees a root frame and reports the error rather than swallowing it.
    zend_execute_data *ex = EG(current_execute_data);
    EG(current_execute_data) = NULL;
#if PHP_VERSION_ID < 80100
    memset(&prepend_file, 0, sizeof(zend_file_handle));
    prepend_file.type = ZEND_HANDLE_FILENAME;
    prepend_file.filename = auto_prepend_file;
    int ret = zend_execute_scripts(ZEND_REQUIRE, NULL, 1, &prepend_file) == SUCCESS;
#else
    zend_stream_init_filename(&prepend_file, auto_prepend_file);
    int ret = zend_execute_scripts(ZEND_REQUIRE, NULL, 1, &prepend_file) == SUCCESS;
    zend_destroy_file_handle(&prepend_file);
#endif
    EG(current_execute_data) = ex;
#if PHP_VERSION_ID >= 80000
    // Exit no longer calls zend_bailout in PHP 8, so we need to "rethrow" the exit
    if (ret == 0) {
        zend_throw_unwind_exit();
    }
#endif
    return ret;
}

void dd_request_init_hook_rinit(void) {
    DDTRACE_G(auto_prepend_file) = PG(auto_prepend_file);
    zend_string *hook_path = get_DD_TRACE_REQUEST_INIT_HOOK();
    if (php_check_open_basedir_ex(ZSTR_VAL(hook_path), 0) == -1) {
        ddtrace_log_debugf("open_basedir restriction in effect; cannot open request init hook: '%s'",
                           ZSTR_VAL(hook_path));
        return;
    }

    zval exists_flag;
#if PHP_VERSION_ID < 80100
    php_stat(ZSTR_VAL(hook_path), ZSTR_LEN(hook_path), FS_EXISTS, &exists_flag);
#else
    php_stat(hook_path, FS_EXISTS, &exists_flag);
#endif
    if (Z_TYPE(exists_flag) == IS_FALSE) {
        ddtrace_log_debugf("Cannot open request init hook; file does not exist: '%s'", ZSTR_VAL(hook_path));
        return;
    }

    PG(auto_prepend_file) = ZSTR_VAL(hook_path);
    if (DDTRACE_G(auto_prepend_file) && DDTRACE_G(auto_prepend_file)[0]) {
        ddtrace_log_debugf("Backing up auto_prepend_file '%s'", DDTRACE_G(auto_prepend_file));
    }
}

void dd_request_init_hook_rshutdown(void) { PG(auto_prepend_file) = DDTRACE_G(auto_prepend_file); }
