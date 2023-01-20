#include "config_ini.h"

#include <SAPI.h>
#include <assert.h>
#include <main/php.h>
#include <stdbool.h>

#define UNUSED(x) (void)(x)

static void (*env_to_ini_name)(zai_string_view env_name, zai_config_name *ini_name);
static bool is_fpm = false;

static bool zai_config_generate_ini_name(zai_string_view name, zai_config_name *ini_name) {
    ini_name->len = 0;
    *ini_name->ptr = '\0';

    env_to_ini_name(name, ini_name);

    return *ini_name->ptr;
}

#if ZTS
// we need to prevent race conditions between copying the inis and setting the global inis during first rinit
static pthread_rwlock_t lock_ini_init_rw = PTHREAD_RWLOCK_INITIALIZER;
static tsrm_thread_end_func_t original_thread_end_handler;

static void zai_config_lock_ini_copying(THREAD_T thread_id) {
    pthread_rwlock_rdlock(&lock_ini_init_rw);
    original_thread_end_handler(thread_id);
    pthread_rwlock_unlock(&lock_ini_init_rw);
}
#endif

// values retrieved here are assumed to be valid
int16_t zai_config_initialize_ini_value(zend_ini_entry **entries, int16_t ini_count, zai_string_view *buf,
                                        zai_string_view default_value, zai_config_id entry_id) {
    if (!env_to_ini_name) return -1;

#if ZTS
    pthread_rwlock_wrlock(&lock_ini_init_rw);
#endif

    int16_t name_index = -1;
    zend_string *runtime_value = NULL;
    zend_string *parsed_ini_value = NULL;

    if (is_fpm) {
        for (int16_t i = 0; i < ini_count; ++i) {
            // Unconditional assignment of inis, bypassing any APIs for random ini values is very much not nice
            // Try working around ...
            zend_string *ini_str = entries[i]->modified ? entries[i]->orig_value : entries[i]->value;
            if (ZSTR_LEN(ini_str) != default_value.len || strcmp(ZSTR_VAL(ini_str), default_value.ptr) != 0) {
                parsed_ini_value = zend_string_copy(ini_str);
                name_index = i;
                break;
            }
        }
    }

    // On post-minit, i.e. at runtime, we do not want to take care of runtime values, only system values
    bool is_minit = !php_get_module_initialized();

    for (int16_t i = 0; i < ini_count; ++i) {
        if (entries[i]->modified && !runtime_value && (is_minit || entries[i]->modifiable == PHP_INI_SYSTEM)) {
            runtime_value = zend_string_copy(entries[i]->value);
        }
        zval *inizv = cfg_get_entry(ZSTR_VAL(entries[i]->name), ZSTR_LEN(entries[i]->name));
        if (inizv != NULL && !parsed_ini_value) {
            parsed_ini_value = zend_string_copy(Z_STR_P(inizv));
            name_index = i;
        }
    }

    if (!zai_config_memoized_entries[entry_id].original_on_modify) {
        for (int16_t i = 0; i < ini_count; ++i) {
            bool duplicate = false;
            for (int j = i + 1; j < ini_count; ++j) {
                if (entries[i] == entries[j]) {
                    duplicate = true;
                }
            }
            if (duplicate) {
                continue;
            }

            zend_string **target = entries[i]->modified ? &entries[i]->orig_value : &entries[i]->value;
            if (i > 0) {
                zend_string_release(*target);
                *target = zend_string_copy(entries[0]->modified ? entries[0]->orig_value : entries[0]->value);
            } else if (buf->ptr != NULL) {
                zend_string_release(*target);
                *target = zend_string_init(buf->ptr, buf->len, 1);
            } else if (parsed_ini_value != NULL) {
                zend_string_release(*target);
                *target = zend_string_copy(parsed_ini_value);
            }

            if (runtime_value) {
                if (entries[i]->modified) {
                    if (entries[i]->value != entries[i]->orig_value) {
                        zend_string_release(entries[i]->value);
                    }
                } else {
                    entries[i]->orig_value = entries[i]->value;
                    entries[i]->modified = true;
                    entries[i]->orig_modifiable = entries[i]->modifiable;
                    zend_hash_add_ptr(EG(modified_ini_directives), entries[i]->name, entries[i]);
                }
                entries[i]->value = zend_string_copy(runtime_value);
            }
        }

        if (runtime_value) {
            buf->ptr = ZSTR_VAL(runtime_value);
            buf->len = ZSTR_LEN(runtime_value);
            zend_string_release(runtime_value);
        } else if (parsed_ini_value && buf->ptr == NULL) {
            buf->ptr = ZSTR_VAL(parsed_ini_value);
            buf->len = ZSTR_LEN(parsed_ini_value);
        }
    }

    if (parsed_ini_value) {
        zend_string_release(parsed_ini_value);
    }

#if ZTS
    pthread_rwlock_unlock(&lock_ini_init_rw);
#endif

    return name_index;
}

bool zai_config_is_initialized(void);
static ZEND_INI_MH(ZaiConfigOnUpdateIni) {
    // ensure validity at any stage
    zai_config_id id;
    zai_string_view name = {.len = ZSTR_LEN(entry->name), .ptr = ZSTR_VAL(entry->name)};
    zai_string_view value_view = {.len = ZSTR_LEN(new_value), .ptr = ZSTR_VAL(new_value)};

    if (!zai_config_get_id_by_name(name, &id)) {
        // TODO Log cannot find ID
        return FAILURE;
    }

    zai_config_memoized_entry *memoized = &zai_config_memoized_entries[id];

    if (memoized->original_on_modify && memoized->original_on_modify(entry, new_value, mh_arg1, mh_arg2, mh_arg3, stage) == FAILURE) {
        return FAILURE;
    }

    zval new_zv;
    ZVAL_UNDEF(&new_zv);

    if (!zai_config_decode_value(value_view, memoized->type, memoized->parser, &new_zv, /* persistent */ stage != PHP_INI_STAGE_RUNTIME)) {
        // TODO Log decoding error

        return FAILURE;
    }

    /* Ignore calls that happen before runtime (e.g. the default INI values on MINIT). System values are obtained on
     * first-time RINIT. */
    if (stage != PHP_INI_STAGE_RUNTIME) {
        zai_config_dtor_pzval(&new_zv);
        return SUCCESS;
    }

    if (!zai_config_is_initialized()) {
        zval_dtor(&new_zv);
        return SUCCESS;
    }

    if (memoized->ini_change && !memoized->ini_change(zai_config_get_value(id), &new_zv)) {
        zval_dtor(&new_zv);
        return FAILURE;
    }

    bool is_reset = zend_string_equals(new_value, entry->modified ? entry->orig_value : entry->value);
    for (int i = 0; i < memoized->names_count; ++i) {
        zend_ini_entry *alias = memoized->ini_entries[i];
#if ZTS
        alias = zend_hash_find_ptr(EG(ini_directives), alias->name);  // alias initially contains the global ini
#endif
        if (alias != entry) {  // otherwise we leak memory, entry->modified is cached in zend_alter_ini_entry_ex...
            if (alias->modified) {
                zend_string_release(alias->value);
            } else {
                alias->modified = true;
                alias->orig_value = alias->value;
                alias->orig_modifiable = alias->modifiable;
                zend_hash_add_ptr(EG(modified_ini_directives), alias->name, alias);
            }
            if (is_reset) {
                alias->value = entry->orig_value;
                alias->modified = false;
                alias->orig_value = NULL;
            } else {
                alias->value = zend_string_copy(new_value);
            }
        }
    }

    zai_config_replace_runtime_config(id, &new_zv);
    zval_dtor(&new_zv);
    return SUCCESS;
}

static void zai_config_add_ini_entry(zai_config_memoized_entry *memoized, zai_string_view name,
                                     zai_config_name *ini_name, int module_number, zai_config_id id) {
    if (!zai_config_generate_ini_name(name, ini_name)) {
        assert(false && "Invalid INI name conversion");
        return;
    }

    zai_config_id duplicate;
    if (zai_config_get_id_by_name((zai_string_view){.ptr = ini_name->ptr, .len = ini_name->len}, &duplicate)) {
        return;
    }

    zai_config_register_config_id(ini_name, id);

    zend_ini_entry *existing;
    if ((existing = zend_hash_str_find_ptr(EG(ini_directives), ini_name->ptr, ini_name->len))) {
        memoized->original_on_modify = existing->on_modify;
        zai_string_view current_value = memoized->default_encoded_value;
        if (ZSTR_LEN(existing->value) != current_value.len || memcmp(current_value.ptr, ZSTR_VAL(existing->value), ZSTR_LEN(existing->value)) != 0) {
            zai_string_view value_view = (zai_string_view){ .ptr = existing->value->val, .len = existing->value->len };
            zval decoded;
            // This should never fail, ideally, as all usages should validate the same way, but at least not crash, just don't accept the value then
            if (zai_config_decode_value(value_view, memoized->type, memoized->parser, &decoded, 1)) {
                zai_config_dtor_pzval(&memoized->decoded_value);
                ZVAL_COPY_VALUE(&memoized->decoded_value, &decoded);
            }
        }
        existing->on_modify = ZaiConfigOnUpdateIni;

        return;
    }

    /* ZEND_INI_END() adds a null terminating entry */
    zend_ini_entry_def entry_defs[1 + /* terminator entry */ 1] = {{0}, {0}};
    zend_ini_entry_def *entry = &entry_defs[0];

    entry->name = ini_name->ptr;
    entry->name_length = ini_name->len;
    entry->value = memoized->default_encoded_value.ptr;
    entry->value_length = memoized->default_encoded_value.len;
    entry->on_modify = ZaiConfigOnUpdateIni;
    entry->modifiable = memoized->ini_change == zai_config_system_ini_change ? PHP_INI_SYSTEM : PHP_INI_ALL;
    if (memoized->type == ZAI_CONFIG_TYPE_BOOL) {
        entry->displayer = php_ini_boolean_displayer_cb;
    }

    if (zend_register_ini_entries(entry_defs, module_number) == FAILURE) {
        // This is not really recoverable ...
        assert(0 && "All our ini entries have been removed due to a single duplicate :-(");
    }
}

// PHP 5 expects 'static storage duration for ini entry names
zai_config_name ini_names[ZAI_CONFIG_ENTRIES_COUNT_MAX * ZAI_CONFIG_NAMES_COUNT_MAX];

void zai_config_ini_minit(zai_config_env_to_ini_name env_to_ini, int module_number) {
    env_to_ini_name = env_to_ini;

    is_fpm = strlen(sapi_module.name) == sizeof("fpm-fcgi") - 1 && !strcmp(sapi_module.name, "fpm-fcgi");

    if (!env_to_ini_name) return;

    for (zai_config_id i = 0; i < zai_config_memoized_entries_count; ++i) {
        zai_config_memoized_entry *memoized = &zai_config_memoized_entries[i];
        for (uint8_t n = 0; n < memoized->names_count; ++n) {
            zai_config_name *ini_name = &ini_names[i * ZAI_CONFIG_NAMES_COUNT_MAX + n];
            zai_string_view name = {.len = memoized->names[n].len, .ptr = memoized->names[n].ptr};
            zai_config_add_ini_entry(memoized, name, ini_name, module_number, i);
            // We need to cache ini directives here, at least for ZTS in order to access the global inis
            memoized->ini_entries[n] = zend_hash_str_find_ptr(EG(ini_directives), ini_name->ptr, ini_name->len);
            assert(memoized->ini_entries[n] != NULL);
        }
    }

#if ZTS
    original_thread_end_handler = tsrm_set_new_thread_end_handler(zai_config_lock_ini_copying);
#endif
}

void zai_config_ini_rinit(void) {
    // we have to cover two cases here:
    // a) update ini tables to take changes during first-time rinit into account on ZTS
    // b) read current env variables
    // c) apply and verify fpm&apache config/user.ini/htaccess settings

    // effectively, also including preloading
    // in_startup = true, if zend_post_startup() hasn't been executed yet
#if PHP_VERSION_ID < 70400
    bool in_startup = !php_get_module_initialized();
#elif PHP_VERSION_ID < 80000
    // Sadly not precisely, observable on PHP 7.4, so we'll just explicitly support the preload case only
    bool in_startup = (CG(compiler_options) & ZEND_COMPILE_PRELOAD) != 0;
#else
    bool in_startup = php_during_module_startup();
#endif

#if ZTS
    // Skip during preloading, in that case EG(ini_directives) is the actual source of truth (NTS-like)
    if (env_to_ini_name && !in_startup) {
        for (uint8_t i = 0; i < zai_config_memoized_entries_count; ++i) {
            zai_config_memoized_entry *memoized = &zai_config_memoized_entries[i];
            if (!memoized->original_on_modify) {
                bool applied_update = false;
                for (uint8_t n = 0; n < memoized->names_count; ++n) {
                    zend_ini_entry *source = memoized->ini_entries[n],
                                   *ini = zend_hash_find_ptr(EG(ini_directives), source->name);
                    if (ini->modified) {
                        if (ini->orig_value == ini->value) {
                            ini->value = source->value;
                        }
                        zend_string_release(ini->orig_value);
                        ini->orig_value = zend_string_copy(source->value);

                        if (!applied_update) {
                            if (ZaiConfigOnUpdateIni(ini, ini->value, NULL, NULL, NULL, PHP_INI_STAGE_RUNTIME) == SUCCESS) {
                                // first encountered name has highest priority
                                applied_update = true;
                            } else {
                                zend_string_release(ini->value);
                                ini->value = ini->orig_value;
                                ini->modified = false;
                                ini->orig_value = NULL;
                            }
                        }
                    } else {
                        zend_string_release(ini->value);
                        ini->value = zend_string_copy(source->value);
                    }
                }
            }
        }
    }
#endif

    ZAI_ENV_BUFFER_INIT(buf, ZAI_ENV_MAX_BUFSIZ);

    for (uint8_t i = 0; i < zai_config_memoized_entries_count; ++i) {
        zai_config_memoized_entry *memoized = &zai_config_memoized_entries[i];
        if (memoized->ini_change == zai_config_system_ini_change || memoized->original_on_modify) {
            continue;
        }

        for (uint8_t name_index = 0; name_index < memoized->names_count; name_index++) {
            zai_string_view name = {.len = memoized->names[name_index].len, .ptr = memoized->names[name_index].ptr};
            zai_env_result result = zai_getenv_ex(name, buf, false);

            if (result == ZAI_ENV_SUCCESS) {
                /*
                 * we unconditionally decode the value because we do not store the in-use encoded value
                 * so we cannot compare the current environment value to the current configuration value
                 * for the purposes of short circuiting decode
                 */
                if (env_to_ini_name) {
                    zend_string *str = zend_string_init(buf.ptr, strlen(buf.ptr), in_startup);

                    zend_ini_entry *ini = memoized->ini_entries[name_index];
                    if (zend_alter_ini_entry_ex(ini->name, str, PHP_INI_USER, PHP_INI_STAGE_RUNTIME, 0) == SUCCESS) {
                        zend_string_release(str);
                        goto next_entry;
                    }
                    zend_string_release(str);
                } else {
                    zai_string_view rte_value = {.len = strlen(buf.ptr), .ptr = buf.ptr};

                    zval new_zv;
                    ZVAL_UNDEF(&new_zv);
                    if (zai_config_decode_value(rte_value, memoized->type, memoized->parser, &new_zv, /* persistent */ false)) {
                        zai_config_replace_runtime_config(i, &new_zv);
                        zval_ptr_dtor(&new_zv);
                    }
                }
            }
        }

        if (env_to_ini_name) {
            for (uint8_t name_index = 0; name_index < memoized->names_count; name_index++) {
                zend_ini_entry *ini = memoized->ini_entries[name_index];
#if ZTS
                ini = zend_hash_find_ptr(EG(ini_directives), ini->name);
#endif
                if (ini->modified) {
                    if (ZaiConfigOnUpdateIni(ini, ini->value, NULL, NULL, NULL, PHP_INI_STAGE_RUNTIME) == SUCCESS) {
                        goto next_entry;
                    }
                }
            }
        }

    next_entry:;
    }
}

void zai_config_ini_mshutdown(void) {}

bool zai_config_is_modified(zai_config_id entry_id) {
    zai_config_memoized_entry *entry = &zai_config_memoized_entries[entry_id];
    if (entry->name_index >= 0) {
        return true;
    }

    zend_ini_entry *ini = entry->ini_entries[0];
#if ZTS
    ini = zend_hash_find_ptr(EG(ini_directives), ini->name);
#endif

    return ini->modified;
}
