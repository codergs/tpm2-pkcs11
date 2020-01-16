/* SPDX-License-Identifier: BSD-2-Clause */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <fcntl.h>
#include <libgen.h>
#include <unistd.h>

#include <linux/limits.h>

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <sqlite3.h>

#include "config.h"
#include "db.h"
#include "emitter.h"
#include "log.h"
#include "mutex.h"
#include "object.h"
#include "parser.h"
#include "session_table.h"
#include "token.h"
#include "tpm.h"
#include "twist.h"
#include "utils.h"

#include <openssl/evp.h>

#ifndef TPM2_PKCS11_STORE_DIR
#define TPM2_PKCS11_STORE_DIR "/etc/tpm2_pkcs11"
#endif

#define DB_VERSION 2

#define goto_oom(x, l) if (!x) { LOGE("oom"); goto l; }
#define goto_error(x, l) if (x) { goto l; }

static struct {
    sqlite3 *db;
} global;

static int _get_blob(sqlite3_stmt *stmt, int i, bool can_be_null, twist *blob) {

    int size = sqlite3_column_bytes(stmt, i);
    if (size < 0) {
        return 1;
    }

    if (size == 0) {
        return can_be_null ? 0 : 1;
    }

    const void *data = sqlite3_column_blob(stmt, i);
    *blob = twistbin_new(data, size);
    if (!*blob) {
        LOGE("oom");
        return 1;
    }

    return 0;
}

static int get_blob_null(sqlite3_stmt *stmt, int i, twist *blob) {

    return _get_blob(stmt, i, true, blob);
}

static int get_blob(sqlite3_stmt *stmt, int i, twist *blob) {

    return _get_blob(stmt, i, false, blob);
}

typedef struct token_get_cb_ud token_get_cb_ud;
struct token_get_cb_ud {
    size_t offset;
    size_t len;
    token *tokens;
};

tobject *db_tobject_new(sqlite3_stmt *stmt) {

    tobject *tobj = tobject_new();
    if (!tobj) {
        LOGE("oom");
        return NULL;
    }

    int i;
    int col_count = sqlite3_data_count(stmt);
    for (i=0; i < col_count; i++) {
        const char *name = sqlite3_column_name(stmt, i);

        if (!strcmp(name, "id")) {
            tobj->id = sqlite3_column_int(stmt, i);

        } else if (!strcmp(name, "tokid")) {
            // Ignore sid we don't need it as token has that data.
        } else if (!strcmp(name, "attrs")) {

            int bytes = sqlite3_column_bytes(stmt, i);
            const unsigned char *attrs = sqlite3_column_text(stmt, i);
            if (!attrs || !bytes) {
                LOGE("tobject does not have attributes");
                goto error;
            }

            bool res = parse_attributes_from_string(attrs, bytes,
                    &tobj->attrs);
            if (!res) {
                LOGE("Could not parse DB attrs, got: \"%s\"", attrs);
                goto error;
            }
        } else {
            LOGE("Unknown row, got: %s", name);
            goto error;
        }
    }

    assert(tobj->id);

    CK_ATTRIBUTE_PTR a = attr_get_attribute_by_type(tobj->attrs, CKA_TPM2_OBJAUTH_ENC);
    if (a && a->pValue && a->ulValueLen) {
        tobj->objauth = twistbin_new(a->pValue, a->ulValueLen);
        if (!tobj->objauth) {
            LOGE("oom");
            goto error;
        }
    }

    a = attr_get_attribute_by_type(tobj->attrs, CKA_TPM2_PUB_BLOB);
    if (a && a->pValue && a->ulValueLen) {

        tobj->pub = twistbin_new(a->pValue, a->ulValueLen);
        if (!tobj->pub) {
            LOGE("oom");
            goto error;
        }
    }

    a = attr_get_attribute_by_type(tobj->attrs, CKA_TPM2_PRIV_BLOB);
    if (a && a->pValue && a->ulValueLen) {

        if (!tobj->pub) {
            LOGE("objects with CKA_TPM2_PUB_BLOB should have CKA_TPM2_PRIV_BLOB");
            goto error;
        }

        tobj->priv = twistbin_new(a->pValue, a->ulValueLen);
        if (!tobj->priv) {
            LOGE("oom");
            goto error;
        }
    }

    return tobj;

error:
    tobject_free(tobj);
    return NULL;
}

int init_tobjects(unsigned tokid, tobject **head) {

    const char *sql =
            "SELECT * FROM tobjects WHERE tokid=?";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(global.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOGE("Cannot prepare tobject query: %s\n", sqlite3_errmsg(global.db));
        return rc;
    }

    rc = sqlite3_bind_int(stmt, 1, tokid);
    if (rc != SQLITE_OK) {
        LOGE("Cannot bind tobject tokid: %s\n", sqlite3_errmsg(global.db));
        goto error;
    }

    list *cur = NULL;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {

        tobject *insert = db_tobject_new(stmt);
        if (!insert) {
            LOGE("Failed to initialize tobject from db");
            goto error;
        }

        if (!*head) {
            *head = insert;
            cur = &insert->l;
            continue;
        }

        assert(cur);
        assert(insert);
        cur->next = &insert->l;
        cur = cur->next;
    }

    rc = SQLITE_OK;

error:
    sqlite3_finalize(stmt);
    return rc;
}

int init_pobject(unsigned pid, pobject *pobj, tpm_ctx *tpm) {

    const char *sql =
            "SELECT handle,objauth FROM pobjects WHERE id=?";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(global.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOGE("Cannot prepare sobject query: %s\n", sqlite3_errmsg(global.db));
        return rc;
    }

    rc = sqlite3_bind_int(stmt, 1, pid);
    if (rc != SQLITE_OK) {
        LOGE("Cannot bind pobject id: %s\n", sqlite3_errmsg(global.db));
        goto error;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        LOGE("stepping in pobjects, got: %s\n", sqlite3_errstr(rc));
        goto error;
    }

    twist blob = NULL;
    rc = _get_blob(stmt, 0, false, &blob);
    if (rc != SQLITE_OK) {
        LOGE("Cannot get ESYS_TR handle blob %s\n", sqlite3_errmsg(global.db));
        goto error;
    }


    bool res = tpm_deserialize_handle(tpm, blob, &pobj->handle);
    twist_free(blob);
    if (!res) {
        /* just set a general error as rc could be success right now */
        rc = SQLITE_ERROR;
        goto error;
    }

    pobj->objauth = twist_new((char *)sqlite3_column_text(stmt, 1));
    goto_oom(pobj->objauth, error);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        LOGE("stepping in pobjects, got: %s\n", sqlite3_errstr(rc));
        goto error;
    }

    rc = SQLITE_OK;

error:
    sqlite3_finalize(stmt);

    return rc;
}

CK_RV db_init_pobject(unsigned pid, pobject *pobj, tpm_ctx *tpm) {
    int rc = init_pobject(pid, pobj, tpm);
    return rc == SQLITE_OK ? CKR_OK : CKR_GENERAL_ERROR;
}

int init_sealobjects(unsigned tokid, sealobject *sealobj) {

    const char *sql =
            "SELECT * FROM sealobjects WHERE tokid=?";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(global.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOGE("Cannot prepare sealobject query: %s\n", sqlite3_errmsg(global.db));
        return rc;
    }

    rc = sqlite3_bind_int(stmt, 1, tokid);
    if (rc != SQLITE_OK) {
        LOGE("Cannot bind tokid: %s\n", sqlite3_errmsg(global.db));
        goto error;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        LOGE("stepping in sealobjects, got: %s\n", sqlite3_errstr(rc));
        goto error;
    }

    int i;
    int col_count = sqlite3_data_count(stmt);
    for (i=0; i < col_count; i++) {
        const char *name = sqlite3_column_name(stmt, i);

        if (!strcmp(name, "id")) {
            sealobj->id = sqlite3_column_int(stmt, i);
        } else if (!strcmp(name, "userauthsalt")) {
            const char *x = (const char *)sqlite3_column_text(stmt, i);
            if (x) {
                sealobj->userauthsalt = twist_new(x);
                goto_oom(sealobj->userauthsalt, error);
            }
        } else if (!strcmp(name, "userpriv")) {
            goto_error(get_blob_null(stmt, i, &sealobj->userpriv), error);
        } else if (!strcmp(name, "userpub")) {
            goto_error(get_blob_null(stmt, i, &sealobj->userpub), error);
        } else if (!strcmp(name, "soauthsalt")) {
            sealobj->soauthsalt = twist_new((char *)sqlite3_column_text(stmt, i));
            goto_oom(sealobj->soauthsalt, error);
        } else if (!strcmp(name, "sopriv")) {
            goto_error(get_blob(stmt, i, &sealobj->sopriv), error);
        } else if (!strcmp(name, "sopub")) {
            goto_error(get_blob(stmt, i, &sealobj->sopub), error);
        } else if (!strcmp(name, "tokid")) {
            // pass
        } else {
            LOGE("Unknown token: %s", name);
            goto error;
        }
    }

    rc = SQLITE_OK;

error:
    sqlite3_finalize(stmt);

    return rc;
}

CK_RV db_get_tokens(token **tok, size_t *len) {

    size_t cnt = 0;

    token *tmp = calloc(MAX_TOKEN_CNT, sizeof(token));
    if (!tmp) {
        LOGE("oom");
        return CKR_HOST_MEMORY;
    }

    const char *sql =
            "SELECT * FROM tokens";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(global.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        free(tmp);
        LOGE("Cannot prepare tobject query: %s\n", sqlite3_errmsg(global.db));
        return rc;
    }

    bool has_uninit_token = false;
    size_t row = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {

        if (cnt >= MAX_TOKEN_CNT) {
            LOGE("Too many tokens, must have less than %d", MAX_TOKEN_CNT);
            goto error;
        }

        token *t = &tmp[row++];
        int col_count = sqlite3_data_count(stmt);

        int i;
        for (i=0; i < col_count; i++) {
            const char *name = sqlite3_column_name(stmt, i);

            if (!strcmp(name, "id")) {
                t->id = sqlite3_column_int(stmt, i);

            } else if(!strcmp(name, "pid")) {
                t->pid = sqlite3_column_int(stmt, i);

            } else if (!strcmp(name, "label")) {
                snprintf((char *)t->label, sizeof(t->label), "%s",
                        sqlite3_column_text(stmt, i));

            } else if (!strcmp(name, "config")) {
                int bytes = sqlite3_column_bytes(stmt, i);
                const unsigned char *config = sqlite3_column_text(stmt, i);
                if (!config || !i) {
                    LOGE("Expected token config to contain config data");
                    goto error;
                }
                bool result = parse_token_config_from_string(config, bytes, &t->config);
                if (!result) {
                    LOGE("Could not parse token config, got: \"%s\"", config);
                    goto error;
                }

            } else {
                LOGE("Unknown key: %s", name);
                goto error;
            }
        } /* done with sql key value search */

        CK_RV rv = token_min_init(t);
        if (rv != CKR_OK) {
            goto error;
        }

        /* tokens in the DB store already have an associated primary object */
        int rc = init_pobject(t->pid, &t->pobject, t->tctx);
        if (rc != SQLITE_OK) {
            goto error;
        }

        if (!t->config.is_initialized) {
            has_uninit_token = true;
            LOGV("skipping further initialization of token tid: %u", t->id);
            continue;
        }

        rc = init_sealobjects(t->id, &t->sealobject);
        if (rc != SQLITE_OK) {
            goto error;
        }

        rc = init_tobjects(t->id, &t->tobjects);
        if (rc != SQLITE_OK) {
            goto error;
        }

        /* token initialized, bump cnt */
        cnt++;
    }

    /* if their was no unitialized token in the db, add it */
    if (!has_uninit_token) {
        if (cnt >= MAX_TOKEN_CNT) {
            LOGE("Too many tokens, must have less than %d", MAX_TOKEN_CNT);
            goto error;
        }

        token *t = &tmp[cnt++];
        t->id = cnt;
        CK_RV rv = token_min_init(t);
        if (rv != CKR_OK) {
            goto error;
        }
    }

    *tok = tmp;
    *len = cnt;
    sqlite3_finalize(stmt);

    return CKR_OK;

error:
    token_free_list(tmp, cnt);
    sqlite3_finalize(stmt);
    return CKR_GENERAL_ERROR;

}

static int start(void) {
    int rc = sqlite3_exec(global.db, "BEGIN TRANSACTION", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        LOGE("%s", sqlite3_errmsg(global.db));
    }
    return rc;
}

static int commit(void) {
    return sqlite3_exec(global.db, "COMMIT", NULL, NULL, NULL);
}

static int rollback(void) {
    return sqlite3_exec(global.db, "ROLLBACK", NULL, NULL, NULL);
}

#define gotobinderror(rc, msg) if (rc) { LOGE("cannot bind "msg); goto error; }

CK_RV db_update_for_pinchange(
        token *tok,
        bool is_so,

        /* new seal object auth metadata */
        twist newauthsalthex,

        /* private and public blobs */
        twist newprivblob,
        twist newpubblob) {

    sqlite3_stmt *stmt = NULL;

    int rc = start();
    if (rc != SQLITE_OK) {
        return CKR_GENERAL_ERROR;
    }

    char *sql = NULL;
    /* so update statements */
    if (is_so) {
        if (newpubblob) {
            sql = "UPDATE sealobjects SET"
                     " soauthsalt=?,"           /* index: 1 */
                     " sopriv=?,"               /* index: 2 */
                     " sopub=?"                 /* index: 3 */
                     " WHERE tokid=?";          /* index: 4 */
        } else {
            sql = "UPDATE sealobjects SET"
                 " soauthsalt=?,"           /* index: 1 */
                 " sopriv=?"                /* index: 2 */
                 " WHERE tokid=?";          /* index: 3 */
        }
    /* user */
    } else {
        if (newpubblob) {
            sql = "UPDATE sealobjects SET"
                     " userauthsalt=?,"           /* index: 1 */
                     " userpriv=?,"               /* index: 2 */
                     " userpub=?"                 /* index: 3 */
                     " WHERE tokid=?" ;           /* index: 4 */
        } else {
            sql = "UPDATE sealobjects SET"
                 " userauthsalt=?,"           /* index: 1 */
                 " userpriv=?"                /* index: 2 */
                 " WHERE tokid=?";            /* index: 3 */
        }
    }

    /*
     * Prepare statements
     */
    rc = sqlite3_prepare(global.db, sql, -1, &stmt, NULL);
    if (rc) {
        LOGE("Could not prepare statement: \"%s\" error: \"%s\"",
        sql, sqlite3_errmsg(global.db));
        goto error;
    }

    /* bind values */
    /* sealobjects */

    int index = 1;
    rc = sqlite3_bind_text(stmt, index++, newauthsalthex, -1, SQLITE_STATIC);
    gotobinderror(rc, "newauthsalthex");

    rc = sqlite3_bind_blob(stmt, index++, newprivblob, twist_len(newprivblob), SQLITE_STATIC);
    gotobinderror(rc, "newprivblob");

    if (newpubblob) {
        rc = sqlite3_bind_blob(stmt, index++, newpubblob, twist_len(newpubblob), SQLITE_STATIC);
        gotobinderror(rc, "newpubblob");
    }

    rc = sqlite3_bind_int(stmt,  index++, tok->id);
    gotobinderror(rc, "tokid");

    /*
     * Everything is bound, fire off the sql statements
     */
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        LOGE("Could not execute stmt");
        goto error;
    }

    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        LOGE("Could not finalize stmt");
        goto error;
    }

    rc = commit();
    if (rc != SQLITE_OK) {
        goto error;
    }

    return CKR_OK;

error:

    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        LOGW("Could not finalize stmt");
    }

    rollback();
    return CKR_GENERAL_ERROR;
}

CK_RV generic_mech_type_handler(CK_MECHANISM_PTR mech, CK_ULONG index, void *userdat) {
    UNUSED(index);
    assert(userdat);

    twist *t = (twist *)(userdat);

    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%lu=\n", mech->mechanism);

    twist x = twist_append(*t, tmp);
    if (!x) {
        return CKR_HOST_MEMORY;
    }

    *t = x;

    return CKR_OK;
}

CK_RV oaep_mech_type_handler(CK_MECHANISM_PTR mech, CK_ULONG index, void *userdat) {
    UNUSED(index);
    assert(userdat);
    assert(mech->pParameter);
    assert(mech->ulParameterLen);

    twist *t = (twist *)(userdat);

    CK_RSA_PKCS_OAEP_PARAMS_PTR p = mech->pParameter;

    /* 9=hashalg=592,mgf=2 */
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%lu=hashalg=%lu,mgf=%lu\n",
            mech->mechanism, p->hashAlg, p->mgf);

    twist x = twist_append(*t, tmp);
    if (!x) {
        return CKR_HOST_MEMORY;
    }

    *t = x;

    return CKR_OK;
}

CK_RV db_add_new_object(token *tok, tobject *tobj) {

    CK_RV rv = CKR_GENERAL_ERROR;

    sqlite3_stmt *stmt = NULL;

    char *attrs = emit_attributes_to_string(tobj->attrs);
    if (!attrs) {
        return CKR_GENERAL_ERROR;
    }

    const char *sql =
          "INSERT INTO tobjects ("
            "tokid, "     // index: 1 type: INT
            "attrs"       // index: 2 type: TEXT (JSON)
          ") VALUES ("
            "?,?"
          ");";

    int rc = sqlite3_prepare_v2(global.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOGE("%s", sqlite3_errmsg(global.db));
        goto error;
    }

    rc = start();
    if (rc != SQLITE_OK) {
        goto error;
    }

    rc = sqlite3_bind_int(stmt, 1, tok->id);
    gotobinderror(rc, "tokid");

    rc = sqlite3_bind_text(stmt, 2, attrs, -1, SQLITE_STATIC);
    gotobinderror(rc, "attrs");

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        LOGE("step error: %s", sqlite3_errmsg(global.db));
        goto error;
    }

    sqlite3_int64 id = sqlite3_last_insert_rowid(global.db);
    if (id == 0) {
        LOGE("Could not get id: %s", sqlite3_errmsg(global.db));
        goto error;
    }

    if (id > UINT_MAX) {
        LOGE("id is larger than unsigned int, got: %lld", id);
        goto error;
    }

    tobject_set_id(tobj, (unsigned)id);

    rc = sqlite3_finalize(stmt);
    gotobinderror(rc, "finalize");

    rc = commit();
    gotobinderror(rc, "commit");

    rv = CKR_OK;

out:
    free(attrs);
    return rv;

error:
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        LOGW("Could not finalize stmt: %d", rc);
    }

    rollback();

    rv = CKR_GENERAL_ERROR;
    goto out;
}

CK_RV db_delete_object(tobject *tobj) {

    CK_RV rv = CKR_GENERAL_ERROR;

    sqlite3_stmt *stmt = NULL;

    static const char *sql =
      "DELETE FROM tobjects WHERE id=?;";

    int rc = sqlite3_prepare_v2(global.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOGE("%s", sqlite3_errmsg(global.db));
        goto error;
    }

    rc = start();
    if (rc != SQLITE_OK) {
        goto error;
    }

    rc = sqlite3_bind_int(stmt, 1, tobj->id);
    gotobinderror(rc, "id");

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        LOGE("step error: %s", sqlite3_errmsg(global.db));
        goto error;
    }

    rc = sqlite3_finalize(stmt);
    gotobinderror(rc, "finalize");

    rc = commit();
    gotobinderror(rc, "commit");

    rv = CKR_OK;

out:
    return rv;

error:
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        LOGW("Could not finalize stmt: %d", rc);
    }

    rollback();

    rv = CKR_GENERAL_ERROR;
    goto out;
}

CK_RV db_add_primary(twist blob, unsigned *pid) {
    assert(blob);
    assert(pid);

    CK_RV rv = CKR_GENERAL_ERROR;

    sqlite3_stmt *stmt = NULL;

    const char *sql =
          "INSERT INTO pobjects ("
            "hierarchy, "     // index: 1 type: TEXT
            "handle,"          // index: 2 type: BLOB
            "objauth"         // index: 3 type: TEXT
          ") VALUES ("
            "?,?,?"
          ");";

    int rc = sqlite3_prepare_v2(global.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOGE("%s", sqlite3_errmsg(global.db));
        goto error;
    }

    rc = start();
    if (rc != SQLITE_OK) {
        goto error;
    }

    rc = sqlite3_bind_text(stmt, 1, "o", -1, SQLITE_STATIC);
    gotobinderror(rc, "hierarchy");

    rc = sqlite3_bind_blob(stmt, 2, blob, twist_len(blob), SQLITE_STATIC);
    gotobinderror(rc, "handle");

    rc = sqlite3_bind_text(stmt, 3, "", -1, SQLITE_STATIC);
    gotobinderror(rc, "objauth");

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        LOGE("step error: %s", sqlite3_errmsg(global.db));
        goto error;
    }

    sqlite3_int64 id = sqlite3_last_insert_rowid(global.db);
    if (id == 0) {
        LOGE("Could not get id: %s", sqlite3_errmsg(global.db));
        goto error;
    }

    if (id > UINT_MAX) {
        LOGE("id is larger than unsigned int, got: %lld", id);
        goto error;
    }

    *pid = (unsigned)id;

    rc = sqlite3_finalize(stmt);
    gotobinderror(rc, "finalize");

    rc = commit();
    gotobinderror(rc, "commit");

    rv = CKR_OK;

out:
    return rv;

error:
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        LOGW("Could not finalize stmt: %d", rc);
    }

    rollback();

    rv = CKR_GENERAL_ERROR;
    goto out;
}

CK_RV db_add_token(token *tok) {
    assert(tok);
    assert(tok->id);

    CK_RV rv = CKR_GENERAL_ERROR;

    sqlite3_stmt *stmt = NULL;

    char *config = emit_config_to_string(tok);
    if (!config) {
        LOGE("Could not get token config");
        return CKR_GENERAL_ERROR;
    }

    /* strip trailing spaces */
    char label_buf[sizeof(tok->label) + 1] = { 0 };
    memcpy(label_buf, tok->label, sizeof(tok->label));

    size_t i;
    for (i=sizeof(tok->label); i > 0; i--) {
        char *p = &label_buf[i-1];
        if (*p != ' ') {
            break;
        }
        *p = '\0';
    }

    const char *sql =
          "INSERT INTO tokens ("
            "id,"         // index: 1 type: INT
            "pid, "       // index: 2 type: INT
            "label,"      // index: 3 type: TEXT
            "config"      // index: 4 type: TEXT (JSON)
          ") VALUES ("
            "?,?,?,?"
          ");";

    int rc = sqlite3_prepare_v2(global.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOGE("%s", sqlite3_errmsg(global.db));
        goto error;
    }

    rc = start();
    if (rc != SQLITE_OK) {
        goto error;
    }

    /*
     * we specify the id since we have an in-memory id that we need to use
     * This will also cause the constraint that primary key's are unique to
     * fail if someone comes in and initializes a token with this id.
     *
     * XXX
     * We should consider relaxing this:
     *   - https://github.com/tpm2-software/tpm2-pkcs11/issues/371
     */
    rc = sqlite3_bind_int(stmt, 1, tok->id);
    gotobinderror(rc, "pid");

    rc = sqlite3_bind_int(stmt, 2, tok->pid);
    gotobinderror(rc, "pid");

    rc = sqlite3_bind_text(stmt, 3, label_buf, -1, SQLITE_STATIC);
    gotobinderror(rc, "config");

    rc = sqlite3_bind_text(stmt, 4, config, -1, SQLITE_STATIC);
    gotobinderror(rc, "label");

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        LOGE("step error: %s", sqlite3_errmsg(global.db));
        goto error;
    }

    sqlite3_int64 id = sqlite3_last_insert_rowid(global.db);
    if (id == 0) {
        LOGE("Could not get id: %s", sqlite3_errmsg(global.db));
        goto error;
    }

    if (id > UINT_MAX) {
        LOGE("id is larger than unsigned int, got: %lld", id);
        goto error;
    }

    assert(tok->id == id);

    rc = sqlite3_finalize(stmt);
    gotobinderror(rc, "finalize");

    /* nothing more to add */
    if (!tok->config.is_initialized) {
        rc = commit();
        gotobinderror(rc, "commit");
        rv = CKR_OK;
        goto out;
    }

    /* add the sealobjects WITHIN the transaction */
    sql = "INSERT INTO sealobjects"
            "(tokid, soauthsalt, sopriv, sopub)"
            "VALUES(?,?,?,?)";

    rc = sqlite3_prepare_v2(global.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOGE("%s", sqlite3_errmsg(global.db));
        goto error;
    }

    rc = sqlite3_bind_int(stmt, 1, tok->id);
    gotobinderror(rc, "tokid");

    rc = sqlite3_bind_text(stmt, 2, tok->sealobject.soauthsalt, -1, SQLITE_STATIC);
    gotobinderror(rc, "soauthsalt");

    rc = sqlite3_bind_blob(stmt, 3, tok->sealobject.sopriv,
            twist_len(tok->sealobject.sopriv), SQLITE_STATIC);
    gotobinderror(rc, "sopriv");

    rc = sqlite3_bind_blob(stmt, 4, tok->sealobject.sopub,
            twist_len(tok->sealobject.sopub), SQLITE_STATIC);
    gotobinderror(rc, "sopub");

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        LOGE("step error: %s", sqlite3_errmsg(global.db));
        goto error;
    }

    rc = sqlite3_finalize(stmt);
    gotobinderror(rc, "finalize");

    rc = commit();
    gotobinderror(rc, "commit");

    rv = CKR_OK;

out:
    free(config);
    return rv;

error:
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        LOGW("Could not finalize stmt: %d", rc);
    }

    rollback();

    rv = CKR_GENERAL_ERROR;
    goto out;
}


CK_RV db_get_first_pid(unsigned *id) {
    assert(id);

    CK_RV rv = CKR_GENERAL_ERROR;

    const char *sql =
            "SELECT id FROM pobjects ORDER BY id ASC LIMIT 1";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(global.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOGE("Cannot prepare first pid query: %s\n", sqlite3_errmsg(global.db));
        return rv;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *id = sqlite3_column_int(stmt, 0);
    } else if (rc == SQLITE_DONE) {
        *id = 0;
    } else {
        LOGE("Cannot step query: %s\n", sqlite3_errmsg(global.db));
        goto error;
    }

    rv = CKR_OK;

error:
    sqlite3_finalize(stmt);
    return rv;
}


CK_RV db_init(void) {

    return db_new(&global.db);
}

CK_RV db_destroy(void) {
    return db_free(&global.db);
}

#define DB_NAME "tpm2_pkcs11.sqlite3"
#define PKCS11_STORE_ENV_VAR "TPM2_PKCS11_STORE"

static CK_RV handle_env_var(char *path, size_t len, bool *skip, bool *stat_is_no_token) {

    *skip = false;
    *stat_is_no_token = true;

    char *env_path = getenv(PKCS11_STORE_ENV_VAR);
    if (!env_path) {
        *skip = true;
        return CKR_OK;
    }

    unsigned l = snprintf(path, len, "%s/%s", env_path, DB_NAME);
    if (l >= len) {
        LOGE("Completed DB path was over-length, got %d expected less than %lu",
                l, len);
        return CKR_GENERAL_ERROR;
    }

    return CKR_OK;
}

static CK_RV handle_home(char *path, size_t len, bool *skip) {

    *skip = false;

    char *env_home = getenv("HOME");
    if (!env_home) {
        *skip = true;
        return CKR_OK;
    }

    unsigned l = snprintf(path, len, "%s/.tpm2_pkcs11/%s", env_home, DB_NAME);
    if (l >= len) {
        LOGE("Completed DB path was over-length, got %d expected less than %lu",
                l, len);
        return CKR_GENERAL_ERROR;
    }

    return CKR_OK;
}

static CK_RV handle_cwd(char *path, size_t len, bool *skip) {

    *skip = false;

    char *cwd_path = getcwd(NULL, 0);
    if (!cwd_path) {
        return errno == ENOMEM ? CKR_HOST_MEMORY : CKR_GENERAL_ERROR;
    }

    unsigned l = snprintf(path, len, "%s/%s", cwd_path, DB_NAME);
    free(cwd_path);
    if (l >= len) {
        LOGE("Completed DB path was over-length, got %d expected less than %lu",
                l, len);
        return CKR_GENERAL_ERROR;
    }

    return CKR_OK;
}

static CK_RV handle_path(char *path, size_t len, bool *skip) {

    *skip = false;

    unsigned l = snprintf(path, len, "%s/%s", TPM2_PKCS11_STORE_DIR, DB_NAME);
    if (l >= len) {
        LOGE("Completed DB path was over-length, got %d expected less than %lu",
                l, len);
        return CKR_GENERAL_ERROR;
    }

    return CKR_OK;
}

typedef CK_RV (*db_handler)(char *path, size_t len);

CK_RV db_for_path(char *path, size_t len, db_handler h) {

    /*
     * Search in the following order:
     * 1. ENV variable
     * 2. $HOME/.tpm2_pkcs11
     * 3. cwd
     * 4. TPM2_PKCS11_STORE_DIR
     */

    unsigned i;
    for (i=0; i < 4; i++) {

        CK_RV rv = CKR_GENERAL_ERROR;
        bool skip = false;
        bool stat_is_no_token = false;

        switch (i) {
        case 0:
            rv = handle_env_var(path, len, &skip, &stat_is_no_token);
            break;
        case 1:
            rv = handle_home(path, len, &skip);
            break;
        case 2:
            rv = handle_cwd(path, len, &skip);
            break;
        case 3:
            rv = handle_path(path, len, &skip);
            break;
            /* no default */
        }

        /* handler had fatal error, exit with return code */
        if (rv != CKR_OK) {
            return rv;
        }

        /* handler says skip, something must not be set */
        if (skip) {
            continue;
        }

        rv = h(path, len);
        if (rv != CKR_TOKEN_NOT_PRESENT) {
            return rv;
        }
    }

    return CKR_TOKEN_NOT_PRESENT;
}

CK_RV db_get_path_handler(char *path, size_t len) {
    UNUSED(len);

    struct stat sb;
    int rc = stat(path, &sb);
    if (rc) {
        LOGV("Could not stat db at path \"%s\", error: %s", path, strerror(errno));

        /* no db, keep looking */
        return CKR_TOKEN_NOT_PRESENT;
    }

    /*
     * made it all the way through and found an existing store,
     * done searching.
     */
    return CKR_OK;
}

CK_RV db_get_existing(char *path, size_t len) {

    return db_for_path(path, len, db_get_path_handler);
}

CK_RV db_create_handler(char *path, size_t len) {
    UNUSED(len);

    CK_RV rv = CKR_TOKEN_NOT_PRESENT;

    char *pathdup = strdup(path);
    if (!pathdup) {
        LOGE("oom");
        return CKR_HOST_MEMORY;
    }

    char *d = dirname(pathdup);
    if (strcmp(d, ".")) {

        struct stat sb;
        int rc = stat(d, &sb);
        if (rc) {
            LOGV("Could not stat db dir \"%s\", error: %s", d, strerror(errno));

            /* no db dir, keep looking */
            goto out;
        }
    }

    /*
     * made it all the way through and found a dir I can use,
     * done searching. Now use it to create the db.
     */
    rv = CKR_OK;

out:
    free(pathdup);
    return rv;
}

CK_RV db_get_version(unsigned *version) {

    CK_RV rv = CKR_GENERAL_ERROR;

    const char *sql = "SELECT schema_version FROM schema";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(global.db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOGW("Cannot prepare version query: %s\n", sqlite3_errmsg(global.db));
        *version = DB_VERSION;
        return CKR_OK;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *version = sqlite3_column_int(stmt, 0);
    } else if (rc == SQLITE_DONE) {
        *version = DB_VERSION;
    } else {
        LOGE("Cannot step query: %s\n", sqlite3_errmsg(global.db));
        goto error;
    }

    rv = CKR_OK;

error:
    sqlite3_finalize(stmt);
    return rv;
}

CK_RV dbup_handler_from_1_to_2(sqlite3 *db) {

    /* Between version 1 and 2 of the DB the following changes need to be made:
     *   The existing rows:
     *     - userpub BLOB NOT NULL,
     *     - userpriv BLOB NOT NULL,
     *     - userauthsalt TEXT NOT NULL,
     *   All have the "NOT NULL" constarint removed, like so:
     *       userpub BLOB,
     *       userpriv BLOB,
     *       userauthsalt TEXT
     * So we need to create a new table with this constraint removed,
     * copy the data and move the table back
     */

    /* Create a new table to copy data to that has the constraints removed */
    const char *s = ""
        "CREATE TABLE sealobjects_new2("
            "id INTEGER PRIMARY KEY,"
            "tokid INTEGER NOT NULL,"
            "userpub BLOB,"
            "userpriv BLOB,"
            "userauthsalt TEXT,"
            "sopub BLOB NOT NULL,"
            "sopriv BLOB NOT NULL,"
            "soauthsalt TEXT NOT NULL,"
            "FOREIGN KEY (tokid) REFERENCES tokens(id) ON DELETE CASCADE"
        ");";
    int rc = sqlite3_exec(db, s, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        LOGE("Cannot create temp table: %s", sqlite3_errmsg(db));
        return CKR_GENERAL_ERROR;
    }

    /* copy the data */
    s = "INSERT INTO sealobjects_new2\n"
        "SELECT * FROM sealobjects;";
    rc = sqlite3_exec(db, s, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        LOGE("Cannot copy data to the temp table: %s", sqlite3_errmsg(db));
        return CKR_GENERAL_ERROR;
    }

    /* Drop the old table */
    s = "DROP TABLE sealobjects;";
    rc = sqlite3_exec(db, s, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        LOGE("Cannot drop the temp table: %s", sqlite3_errmsg(db));
        return CKR_GENERAL_ERROR;
    }

    /* Rename the new table to the correct table name */
    s = "ALTER TABLE sealobjects_new2 RENAME TO sealobjects;";
    rc = sqlite3_exec(db, s, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        LOGE("Cannot rename the temp table back to the original table name: %s",
                sqlite3_errmsg(db));
        return CKR_GENERAL_ERROR;
    }

    return CKR_OK;
}

typedef CK_RV (*db_update_handlers)(sqlite3 *db);

CK_RV db_update(sqlite3 *db, unsigned old_version, unsigned new_version) {

    static const db_update_handlers updaters[] = {
            NULL,
            dbup_handler_from_1_to_2,
    };

    if (new_version > ARRAY_LEN(updaters)) {
        LOGE("db update code does not know how to update to version: %u",
                new_version);
        return CKR_GENERAL_ERROR;
    }

    if (old_version == 0) {
        LOGE("version 0 was never a valid db version");
        return CKR_GENERAL_ERROR;
    }

    size_t i;
    for(i=old_version; i < ARRAY_LEN(updaters) && i < new_version; i++) {
        CK_RV rv = updaters[i](db);
        if (rv != CKR_OK) {
            LOGE("Running updater index %u failed", i);
            return rv;
        }
    }

    return CKR_OK;
}

static CK_RV db_backup(sqlite3 *db, const char *dbpath, char **copypath) {

    CK_RV rv = CKR_GENERAL_ERROR;

    char temp[PATH_MAX];

    sqlite3 *copydb = NULL;

    const char *suffix = ".bak";

    sqlite3_backup *backup_conn = NULL;

    snprintf(temp, sizeof(temp), "%s%s", dbpath, suffix);

    LOGV("Performing DB backup at: \"%s\"", temp);

    struct stat sb;
    int rc = stat(temp, &sb);
    if (rc == 0) {
        LOGE("Backup DB exists at \"%s\" not overwriting. "
                "Refusing to run, see "
                "https://github.com/tpm2-software/tpm2-pkcs11/blob/master/docs/DB_UPGRADE.md.",
                temp);
        return CKR_GENERAL_ERROR;
    } else if (rc < 0 && errno != ENOENT) {
        LOGE("Failed to stat path \"%s\", error: %s",
                temp, strerror(errno));
        return CKR_GENERAL_ERROR;
    }

    rc = sqlite3_open(temp, &copydb);
    if (rc != SQLITE_OK) {
        LOGE("Cannot open database: %s\n", sqlite3_errmsg(copydb));
        goto out;
    }

    backup_conn = sqlite3_backup_init(copydb, "temp", db, "main");
    if (!backup_conn) {
        LOGE("Cannot backup init db: %s\n", sqlite3_errmsg(copydb));
        goto out;
    }

    rc = sqlite3_backup_step(backup_conn, -1);
    if (rc != SQLITE_DONE) {
        LOGE("Cannot step db backup: %s\n", sqlite3_errmsg(copydb));
        goto out;
    }

    *copypath = strdup(temp);
    if (!(*copypath)) {
        LOGE("oom");
        rv = CKR_HOST_MEMORY;
        goto out;
    }

    rv = CKR_OK;

out:
    if (backup_conn) {
        sqlite3_backup_finish(backup_conn);
    }
    if (copydb) {
        sqlite3_close(copydb);
    }
    return rv;
}

CK_RV db_create(char *path, size_t len) {

    return db_for_path(path, len, db_create_handler);
}

CK_RV do_db_upgrade_if_needed(sqlite3 *db) {

    unsigned old_version = 0;
    CK_RV rv = db_get_version(&old_version);
    if (rv != CKR_OK) {
        LOGE("Could not get DB version");
        return rv;
    }

    if (old_version == 0) {
        LOGE("Version of DB cannot be 0");
        return CKR_GENERAL_ERROR;
    }

    if (old_version == DB_VERSION) {
        LOGV("No DB upgrade needed");
        return CKR_OK;
    }

    rv = db_update(db, old_version, DB_VERSION);
    if (rv != CKR_OK) {
        LOGE("Could not perform db update");
        return rv;
    }

    return CKR_OK;
}

static FILE *take_lock(const char *path, char *lockpath) {

    snprintf(lockpath, PATH_MAX, "%s%s", path, ".lock");

    FILE *f = fopen(lockpath, "w+");
    if (!f) {
        LOGE("Could not open lock file \"%s\", error: %s",
                lockpath, strerror(errno));
        return NULL;
    }

    int rc = flock(fileno(f), LOCK_EX);
    if (rc < 0) {
        LOGE("Could not flock file \"%s\", error: %s",
                lockpath, strerror(errno));
        fclose(f);
        unlink(lockpath);
        return NULL;
    }

    return f;
}

static void release_lock(FILE *f, char *lockpath) {

    int rc = flock(fileno(f), LOCK_UN);
    if (rc < 0) {
        LOGE("Could not unlock file \"%s\", error: %s",
                lockpath, strerror(errno));
    }
    UNUSED(unlink(lockpath));
    UNUSED(fclose(f));
}

CK_RV db_setup(sqlite3 *db, const char *path) {

    char *dbbakpath = NULL;
    char lockpath[PATH_MAX];
    FILE *f = take_lock(path, lockpath);
    if (!f) {
        return CKR_GENERAL_ERROR;
    }

    CK_RV rv = db_backup(db, path, &dbbakpath);
    if (rv != CKR_OK) {
        LOGE("Could not make DB copy");
        goto out;
    }

    /* run specific upgrade code */
    rv = do_db_upgrade_if_needed(db);
    if (rv != CKR_OK) {
        goto out;
    }

    /* SQL that is always safe across updates */
    const char *sql[] = {
        "CREATE TABLE IF NOT EXISTS tokens("
            "id INTEGER PRIMARY KEY,"
            "pid INTEGER NOT NULL,"
            "label TEXT UNIQUE,"
            "config TEXT NOT NULL,"
            "FOREIGN KEY (pid) REFERENCES pobjects(id) ON DELETE CASCADE"
        ");",
        "CREATE TABLE IF NOT EXISTS sealobjects("
            "id INTEGER PRIMARY KEY,"
            "tokid INTEGER NOT NULL,"
            "userpub BLOB,"
            "userpriv BLOB,"
            "userauthsalt TEXT,"
            "sopub BLOB NOT NULL,"
            "sopriv BLOB NOT NULL,"
            "soauthsalt TEXT NOT NULL,"
            "FOREIGN KEY (tokid) REFERENCES tokens(id) ON DELETE CASCADE"
        ");",
        "CREATE TABLE IF NOT EXISTS pobjects("
            "id INTEGER PRIMARY KEY,"
            "hierarchy TEXT NOT NULL,"
            "handle BLOB NOT NULL,"
            "objauth TEXT NOT NULL"
        ");",
        "CREATE TABLE IF NOT EXISTS tobjects("
            "id INTEGER PRIMARY KEY,"
            "tokid INTEGER NOT NULL,"
            "attrs TEXT NOT NULL,"
            "FOREIGN KEY (tokid) REFERENCES tokens(id) ON DELETE CASCADE"
        ");",
        "CREATE TABLE IF NOT EXISTS schema("
            "id INTEGER PRIMARY KEY,"
            "schema_version INTEGER NOT NULL"
        ");",
        // NOTE: Update the DB Schema Version if the format above changes!
        // REPLACE updates the value if it exists, or inserts it if it doesn't
        "REPLACE INTO schema (id, schema_version) VALUES (1, "xstr(DB_VERSION) ");",
        "CREATE TRIGGER IF NOT EXISTS limit_tokens\n"
        "BEFORE INSERT ON tokens\n"
        "BEGIN\n"
        "    SELECT CASE WHEN\n"
        "        (SELECT COUNT (*) FROM tokens) >= 255\n"
        "    THEN\n"
        "        RAISE(FAIL, \"Maximum token count of 255 reached.\")\n"
        "    END;\n"
        "END;\n",
        "CREATE TRIGGER IF NOT EXISTS limit_tobjects\n"
        "BEFORE INSERT ON tobjects\n"
        "BEGIN\n"
        "    SELECT CASE WHEN\n"
        "        (SELECT COUNT (*) FROM tobjects) >= 16777215\n"
        "    THEN\n"
        "        RAISE(FAIL, \"Maximum object count of 16777215 reached.\")\n"
        "    END;\n"
        "END;\n"
    };

    bool fail = false;

    size_t i;
    for (i=0; i < ARRAY_LEN(sql) && !fail; i++) {
        const char *s = sql[i];

        int rc = sqlite3_exec(global.db, s, NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            LOGE("%s", sqlite3_errmsg(global.db));
            fail = true;
            break;
        }
    }

    if (fail) {
        LOGE("db creation failed");
        UNUSED(sqlite3_close(db));
        rv = CKR_GENERAL_ERROR;
        goto out;
    }
    rv = CKR_OK;

out:
    /* on success remove the backup */
    if (rv == CKR_OK && dbbakpath) {
        LOGV("Unlinking DB backup: \"%s\"", dbbakpath);
        unlink(dbbakpath);
    } else if (rv != CKR_OK) {
        LOGE("Error within db, leaving backup see: "
            "https://github.com/tpm2-software/tpm2-pkcs11/blob/master/docs/DB_UPGRADE.md.");
    }
    /* always free the backup copy path memory */
    free(dbbakpath);

    release_lock(f, lockpath);
    return rv;
}

CK_RV db_new(sqlite3 **db) {

    char path[PATH_MAX];
    CK_RV rv = db_get_existing(path, sizeof(path));
    if (rv == CKR_TOKEN_NOT_PRESENT) {
        rv = db_create(path, sizeof(path));
    }

    if (rv == CKR_TOKEN_NOT_PRESENT) {
        LOGV("Could not find pkcs11 store");
        LOGV("Consider exporting "PKCS11_STORE_ENV_VAR" to point to a valid store directory");
    }

    LOGV("Using sqlite3 DB: \"%s\"", path);

    int rc = sqlite3_open(path, db);
    if (rc != SQLITE_OK) {
        LOGE("Cannot open database: %s\n", sqlite3_errmsg(*db));
        return CKR_GENERAL_ERROR;
    }

    return db_setup(*db, path);
}

CK_RV db_free(sqlite3 **db) {

    int rc = sqlite3_close(*db);
    if (rc != SQLITE_OK) {
        LOGE("Cannot close database: %s\n", sqlite3_errmsg(*db));
        return CKR_GENERAL_ERROR;
    }

    *db = NULL;

    return CKR_OK;
}
