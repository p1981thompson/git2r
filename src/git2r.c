/*
 *  git2r, R bindings to the libgit2 library.
 *  Copyright (C) 2013-2014 The git2r contributors
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License, version 2,
 *  as published by the Free Software Foundation.
 *
 *  git2r is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/** @file git2r.c
 *  @brief R bindings to the libgit2 library
 *
 *  These functions are called from R with .Call to interface the
 *  libgit2 library from R.
 *
 */

#include <Rdefines.h>
#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>

#include "git2.h"

#include "git2r_clone.h"
#include "git2r_commit.h"
#include "git2r_error.h"
#include "git2r_repository.h"
#include "git2r_push.h"
#include "git2r_signature.h"
#include "git2r_status.h"

static size_t count_staged_changes(git_status_list *status_list);
static size_t count_unstaged_changes(git_status_list *status_list);
static void init_reference(git_reference *ref, SEXP reference);
static int number_of_branches(git_repository *repo, int flags, size_t *n);

/**
 * Add files to a repository
 *
 * @param repo S4 class git_repository
 * @param path
 * @return R_NilValue
 */
SEXP add(const SEXP repo, const SEXP path)
{
    int err;
    git_index *index = NULL;
    git_repository *repository = NULL;

    if (R_NilValue == path)
        error("'path' equals R_NilValue");
    if (!isString(path))
        error("'path' must be a string");

    repository= get_repository(repo);
    if (!repository)
        error(git2r_err_invalid_repository);

    err = git_repository_index(&index, repository);
    if (err < 0)
        goto cleanup;

    err = git_index_add_bypath(index, CHAR(STRING_ELT(path, 0)));
    if (err < 0)
        goto cleanup;

    err = git_index_write(index);
    if (err < 0)
        goto cleanup;

cleanup:
    if (index)
        git_index_free(index);

    if (repository)
        git_repository_free(repository);

    if (err < 0) {
        const git_error *e = giterr_last();
        error("Error %d/%d: %s\n", err, e->klass, e->message);
    }

    return R_NilValue;
}

/**
 * List branches in a repository
 *
 * @param repo S4 class git_repository
 * @return VECXSP with S4 objects of class git_branch
 */
SEXP branches(const SEXP repo, const SEXP flags)
{
    SEXP list;
    int err = 0;
    const char* err_msg = NULL;
    git_branch_iterator *iter = NULL;
    size_t i = 0, n = 0;
    size_t protected = 0;
    git_repository *repository = NULL;

    repository = get_repository(repo);
    if (!repository)
        error(git2r_err_invalid_repository);

    /* Count number of branches before creating the list */
    err = number_of_branches(repository, INTEGER(flags)[0], &n);
    if (err < 0)
        goto cleanup;

    PROTECT(list = allocVector(VECSXP, n));
    protected++;

    err = git_branch_iterator_new(&iter, repository,  INTEGER(flags)[0]);
    if (err < 0)
        goto cleanup;

    for (;;) {
        SEXP branch;
        git_branch_t type;
        git_reference *ref;
        const char *refname;

        err = git_branch_next(&ref, &type, iter);
        if (err < 0) {
            if (GIT_ITEROVER == err) {
                err = 0;
                break;
            }
            goto cleanup;
        }

        PROTECT(branch = NEW_OBJECT(MAKE_CLASS("git_branch")));
        protected++;

        refname = git_reference_name(ref);
        init_reference(ref, branch);

        switch (type) {
        case GIT_BRANCH_LOCAL:
            break;
        case GIT_BRANCH_REMOTE: {
            char *buf;
            size_t buf_size;
            git_remote *remote = NULL;

            buf_size = git_branch_remote_name(NULL, 0, repository, refname);
            buf = malloc(buf_size * sizeof(char));
            if (NULL == buf) {
                err = -1;
                err_msg = git2r_err_alloc_memory_buffer;
                goto cleanup;
            }

            git_branch_remote_name(buf, buf_size, repository, refname);
            SET_SLOT(branch, Rf_install("remote"), ScalarString(mkChar(buf)));

            err = git_remote_load(&remote, repository, buf);
            if (err < 0) {
                err = git_remote_create_inmemory(&remote, repository, NULL, buf);
                if (err < 0)
                    goto cleanup;
            }

            SET_SLOT(branch,
                     Rf_install("url"),
                     ScalarString(mkChar(git_remote_url(remote))));

            free(buf);
            git_remote_free(remote);
            break;
        }
        default:
            err = -1;
            err_msg = git2r_err_unexpected_type_of_branch;
            goto cleanup;
        }

        switch (git_branch_is_head(ref)) {
        case 0:
            SET_SLOT(branch, Rf_install("head"), ScalarLogical(0));
            break;
        case 1:
            SET_SLOT(branch, Rf_install("head"), ScalarLogical(1));
            break;
        default:
            err = -1;
            err_msg = git2r_err_unexpected_head_of_branch;
            goto cleanup;
        }

        git_reference_free(ref);
        SET_VECTOR_ELT(list, i, branch);
        UNPROTECT(1);
        protected--;
        i++;
    }

cleanup:
    if (iter)
        git_branch_iterator_free(iter);

    if (repository)
        git_repository_free(repository);

    if (protected)
        UNPROTECT(protected);

    if (err < 0) {
        if (err_msg) {
            error(err_msg);
        } else {
            const git_error *e = giterr_last();
            error("Error %d: %s\n", e->klass, e->message);
        }
    }

    return list;
}

/**
 * Checkout
 *
 * @param repo S4 class git_repository
 * @param treeish
 * @return R_NilValue
 */
SEXP checkout(SEXP repo, SEXP treeish)
{
    enum CHECKOUT_ACTION {
        CHECKOUT_COMMIT,
        CHECKOUT_TAG,
        CHECKOUT_TREE,
        CHECKOUT_HEAD
    } checkout_action;

    int err;
    git_repository *repository = NULL;
    git_checkout_opts checkout_opts = GIT_CHECKOUT_OPTS_INIT;
    checkout_opts.checkout_strategy = GIT_CHECKOUT_SAFE;

    /* Check arguments to checkout */
    if (R_NilValue == repo || R_NilValue == treeish)
        error(git2r_err_invalid_checkout_args);

    /* Determine checkout strategy */
    if(S4SXP == TYPEOF(treeish)) {
        const char *class_name = CHAR(STRING_ELT(getAttrib(treeish, R_ClassSymbol), 0));

        if (0 == strcmp(class_name, "git_commit"))
            checkout_action = CHECKOUT_COMMIT;
        else if(0 == strcmp(class_name, "git_tag"))
            checkout_action = CHECKOUT_TAG;
        else if(0 == strcmp(class_name, "git_tree"))
            checkout_action = CHECKOUT_TREE;
        else
            error(git2r_err_invalid_checkout_args);
    } else if (isString(treeish)
               && 1 == length(treeish)
               && 0 == strcmp(CHAR(STRING_ELT(treeish, 0)), "HEAD")) {
        checkout_action = CHECKOUT_HEAD;
    } else {
        error(git2r_err_invalid_checkout_args);
    }

    repository = get_repository(repo);
    if (!repository)
        error(git2r_err_invalid_repository);

    switch (checkout_action) {
    case CHECKOUT_COMMIT:
        /* :TODO:FIX: */
        break;
    case CHECKOUT_TAG:
        /* :TODO:FIX: */
        break;
    case CHECKOUT_TREE:
        /* :TODO:FIX: */
        break;
    case CHECKOUT_HEAD:
        /* :TODO:FIX: */
        break;
    default:
        break;
    }

cleanup:
    if (repository)
        git_repository_free(repository);

    if (err < 0) {
        const git_error *e = giterr_last();
        error("Error %d: %s\n", e->klass, e->message);
    }

    return R_NilValue;
}

/**
 * Config
 *
 * @param repo S4 class git_repository
 * @param variables
 * @return R_NilValue
 */
SEXP config(SEXP repo, SEXP variables)
{
    SEXP names;
    int err, i;
    git_config *cfg = NULL;
    git_repository *repository = NULL;

    if (R_NilValue == variables)
        error("'variables' equals R_NilValue.");
    if (!isNewList(variables))
        error("'variables' must be a list.");

    repository = get_repository(repo);
    if (!repository)
        error(git2r_err_invalid_repository);

    err = git_repository_config(&cfg, repository);
    if (err < 0)
        goto cleanup;

    names = getAttrib(variables, R_NamesSymbol);
    for (i = 0; i < length(variables); i++) {
        const char *key = CHAR(STRING_ELT(names, i));
        const char *value = CHAR(STRING_ELT(VECTOR_ELT(variables, i), 0));

        err = git_config_set_string(cfg, key, value);
        if (err < 0)
            goto cleanup;
    }

cleanup:
    if (config)
        git_config_free(cfg);

    if (repository)
        git_repository_free(repository);

    return R_NilValue;
}

/**
 * Fetch
 *
 * @param repo
 * @return R_NilValue
 */
SEXP fetch(const SEXP repo, const SEXP name)
{
    int err;
    git_remote *remote = NULL;
    git_repository *repository = NULL;

    if (R_NilValue == name
        || !isString(name)
        || 1 != length(name))
        error("Invalid arguments to fetch");

    repository = get_repository(repo);
    if (!repository)
        error(git2r_err_invalid_repository);

    err = git_remote_load(&remote, repository, CHAR(STRING_ELT(name, 0)));
    if (err < 0)
        goto cleanup;

    err = git_remote_fetch(remote);
    if (err < 0)
        goto cleanup;

cleanup:
    if (remote)
        git_remote_disconnect(remote);

    if (remote)
        git_remote_free(remote);

    if (repository)
        git_repository_free(repository);

    return R_NilValue;
}

/**
 * Get repo slot from S4 class git_repository
 *
 * @param repo S4 class git_repository
 * @return a git_repository pointer on success else NULL
 */
git_repository* get_repository(const SEXP repo)
{
    SEXP class_name;
    SEXP path;
    git_repository *r;

    if (R_NilValue == repo || S4SXP != TYPEOF(repo))
        return NULL;

    class_name = getAttrib(repo, R_ClassSymbol);
    if (0 != strcmp(CHAR(STRING_ELT(class_name, 0)), "git_repository"))
        return NULL;

    path = GET_SLOT(repo, Rf_install("path"));
    if (R_NilValue == path)
        return NULL;

    if (git_repository_open(&r, CHAR(STRING_ELT(path, 0))) < 0)
        return NULL;

    return r;
}

/**
 * Init a repository.
 *
 * @param path
 * @param bare
 * @return R_NilValue
 */
SEXP init(const SEXP path, const SEXP bare)
{
    int err;
    git_repository *repository = NULL;

    if (R_NilValue == path)
        error("'path' equals R_NilValue");
    if (!isString(path))
        error("'path' must be a string");
    if (R_NilValue == bare)
        error("'bare' equals R_NilValue");
    if (!isLogical(bare))
        error("'bare' must be a logical");

    err = git_repository_init(&repository,
                              CHAR(STRING_ELT(path, 0)),
                              LOGICAL(bare)[0]);
    if (err < 0)
        error("Unable to init repository");

    git_repository_free(repository);

    return R_NilValue;
}

/**
 * Init slots in S4 class git_reference.
 *
 * @param ref
 * @param reference
 * @return void
 */
static void init_reference(git_reference *ref, SEXP reference)
{
    char out[41];
    out[40] = '\0';

    SET_SLOT(reference,
             Rf_install("name"),
             ScalarString(mkChar(git_reference_name(ref))));

    SET_SLOT(reference,
             Rf_install("shorthand"),
             ScalarString(mkChar(git_reference_shorthand(ref))));

    switch (git_reference_type(ref)) {
    case GIT_REF_OID:
        SET_SLOT(reference, Rf_install("type"), ScalarInteger(GIT_REF_OID));
        git_oid_fmt(out, git_reference_target(ref));
        SET_SLOT(reference, Rf_install("hex"), ScalarString(mkChar(out)));
        break;
    case GIT_REF_SYMBOLIC:
        SET_SLOT(reference, Rf_install("type"), ScalarInteger(GIT_REF_SYMBOLIC));
        SET_SLOT(reference,
                 Rf_install("target"),
                 ScalarString(mkChar(git_reference_symbolic_target(ref))));
        break;
    default:
        error("Unexpected reference type");
    }
}

/**
 * Init slots in S4 class git_tag
 *
 * @param source a tag
 * @param dest S4 class git_tag to initialize
 * @return void
 */
static void init_tag(git_tag *source, SEXP dest)
{
    int err;
    const git_signature *tagger;
    const git_oid *oid;
    char target[GIT_OID_HEXSZ + 1];

    SET_SLOT(dest,
             Rf_install("message"),
             ScalarString(mkChar(git_tag_message(source))));

    SET_SLOT(dest,
             Rf_install("name"),
             ScalarString(mkChar(git_tag_name(source))));

    tagger = git_tag_tagger(source);
    if (tagger) {
        SEXP sexp_tagger;

        PROTECT(sexp_tagger = NEW_OBJECT(MAKE_CLASS("git_signature")));
        init_signature(tagger, sexp_tagger);
        SET_SLOT(dest, Rf_install("tagger"), sexp_tagger);
        UNPROTECT(1);
    }

    oid = git_tag_target_id(source);
    git_oid_tostr(target, sizeof(target), oid);;
    SET_SLOT(dest,
             Rf_install("target"),
             ScalarString(mkChar(target)));
}

/**
 * Check if repository is bare.
 *
 * @param repo S4 class git_repository
 * @return
 */
SEXP is_bare(const SEXP repo)
{
    SEXP result;
    git_repository *repository;

    repository= get_repository(repo);
    if (!repository)
        error(git2r_err_invalid_repository);

    if (git_repository_is_bare(repository))
        result = ScalarLogical(TRUE);
    else
        result = ScalarLogical(FALSE);

    git_repository_free(repository);

    return result;
}

/**
 * Check if repository is empty.
 *
 * @param repo S4 class git_repository
 * @return
 */
SEXP is_empty(const SEXP repo)
{
    SEXP result;
    git_repository *repository;

    repository= get_repository(repo);
    if (!repository)
        error(git2r_err_invalid_repository);

    if (git_repository_is_empty(repository))
        result = ScalarLogical(TRUE);
    else
        result = ScalarLogical(FALSE);

    git_repository_free(repository);

    return result;
}

/**
 * Check if valid repository.
 *
 * @param path
 * @return
 */
SEXP is_repository(const SEXP path)
{
    SEXP result;
    git_repository *repository;
    int err;

    if (R_NilValue == path)
        error("'path' equals R_NilValue");
    if (!isString(path))
        error("'path' must be a string");

    err = git_repository_open(&repository, CHAR(STRING_ELT(path, 0)));
    if (err < 0) {
        result = ScalarLogical(FALSE);
    } else {
        git_repository_free(repository);
        result = ScalarLogical(TRUE);
    }

    return result;
}

/**
 * Count number of branches.
 *
 * @param repo S4 class git_repository
 * @param flags
 * @return
 */
static int number_of_branches(git_repository *repo, int flags, size_t *n)
{
    int err;
    git_branch_iterator *iter;
    git_branch_t type;
    git_reference *ref;

    *n = 0;

    err = git_branch_iterator_new(&iter, repo, flags);
    if (err < 0)
        return err;

    for (;;) {
        err = git_branch_next(&ref, &type, iter);
        if (err < 0)
            break;
        git_reference_free(ref);
        (*n)++;
    }

    git_branch_iterator_free(iter);

    if (GIT_ITEROVER != err)
        return err;
    return 0;
}

/**
 * Count number of revisions.
 *
 * @param walker
 * @return
 */
static size_t number_of_revisions(git_revwalk *walker)
{
    size_t n = 0;
    git_oid oid;

    while (!git_revwalk_next(&oid, walker))
        n++;
    return n;
}

/**
 * Get all references that can be found in a repository.
 *
 * @param repo S4 class git_repository
 * @return VECXSP with S4 objects of class git_reference
 */
SEXP references(const SEXP repo)
{
    int i, err;
    git_strarray ref_list;
    SEXP list = R_NilValue;
    SEXP names = R_NilValue;
    git_reference *ref;
    git_repository *repository;

    repository = get_repository(repo);
    if (!repository)
        error(git2r_err_invalid_repository);

    err = git_reference_list(&ref_list, repository);
    if (err < 0)
        goto cleanup;

    PROTECT(list = allocVector(VECSXP, ref_list.count));
    PROTECT(names = allocVector(STRSXP, ref_list.count));

    for (i = 0; i < ref_list.count; i++) {
        SEXP reference;

        err = git_reference_lookup(&ref, repository, ref_list.strings[i]);
        if (err < 0)
            goto cleanup;

        PROTECT(reference = NEW_OBJECT(MAKE_CLASS("git_reference")));
        init_reference(ref, reference);
        SET_STRING_ELT(names, i, mkChar(ref_list.strings[i]));
        SET_VECTOR_ELT(list, i, reference);
        UNPROTECT(1);
    }

cleanup:
    git_strarray_free(&ref_list);

    if (repository)
        git_repository_free(repository);

    if (R_NilValue != list && R_NilValue != names) {
        setAttrib(list, R_NamesSymbol, names);
        UNPROTECT(2);
    }

    if (err < 0) {
        const git_error *e = giterr_last();
        error("Error %d/%d: %s\n", err, e->klass, e->message);
    }

    return list;
}

/**
 * Get the configured remotes for a repo
 *
 * @param repo S4 class git_repository
 * @return
 */
SEXP remotes(const SEXP repo)
{
    int i, err;
    git_strarray rem_list;
    SEXP list;
    git_repository *repository;

    repository = get_repository(repo);
    if (!repository)
        error(git2r_err_invalid_repository);

    err = git_remote_list(&rem_list, repository);
    if (err < 0)
        goto cleanup;

    PROTECT(list = allocVector(STRSXP, rem_list.count));
    for (i = 0; i < rem_list.count; i++)
        SET_STRING_ELT(list, i, mkChar(rem_list.strings[i]));
    UNPROTECT(1);

cleanup:
    git_strarray_free(&rem_list);

    if (repository)
        git_repository_free(repository);

    if (err < 0) {
        const git_error *e = giterr_last();
        error("Error %d/%d: %s\n", err, e->klass, e->message);
    }

    return list;
}

/**
 * Get the remote's url
 *
 * @param repo S4 class git_repository
 * @return
 */
SEXP remote_url(const SEXP repo, const SEXP remote)
{
    int err;
    SEXP url;
    size_t len = LENGTH(remote);
    size_t i = 0;
    git_repository *repository;

    repository = get_repository(repo);
    if (!repository)
        error(git2r_err_invalid_repository);

    PROTECT(url = allocVector(STRSXP, len));

    for (; i < len; i++) {
        git_remote *r;

        err = git_remote_load(&r, repository, CHAR(STRING_ELT(remote, i)));
        if (err < 0)
            goto cleanup;

        SET_STRING_ELT(url, i, mkChar(git_remote_url(r)));
        git_remote_free(r);
    }

cleanup:
    git_repository_free(repository);

    UNPROTECT(1);

    if (err < 0) {
        const git_error *e = giterr_last();
        error("Error %d/%d: %s\n", err, e->klass, e->message);
    }

    return url;
}

/**
 * List revisions
 *
 * @param repo S4 class git_repository
 * @return
 */
SEXP revisions(SEXP repo)
{
    int i=0;
    int err = 0;
    SEXP list;
    size_t n = 0;
    git_revwalk *walker = NULL;
    git_repository *repository;

    repository = get_repository(repo);
    if (!repository)
        error(git2r_err_invalid_repository);

    if (git_repository_is_empty(repository)) {
        /* No commits, create empty list */
        PROTECT(list = allocVector(VECSXP, 0));
        goto cleanup;
    }

    err = git_revwalk_new(&walker, repository);
    if (err < 0)
        goto cleanup;

    err = git_revwalk_push_head(walker);
    if (err < 0)
        goto cleanup;

    /* Count number of revisions before creating the list */
    n = number_of_revisions(walker);

    /* Create list to store result */
    PROTECT(list = allocVector(VECSXP, n));

    git_revwalk_reset(walker);
    err = git_revwalk_push_head(walker);
    if (err < 0)
        goto cleanup;

    for (;;) {
        git_commit *commit;
        SEXP sexp_commit;
        git_oid oid;

        err = git_revwalk_next(&oid, walker);
        if (err < 0) {
            if (GIT_ITEROVER == err)
                err = 0;
            goto cleanup;
        }

        err = git_commit_lookup(&commit, repository, &oid);
        if (err < 0)
            goto cleanup;

        PROTECT(sexp_commit = NEW_OBJECT(MAKE_CLASS("git_commit")));
        init_commit(commit, sexp_commit);
        SET_VECTOR_ELT(list, i, sexp_commit);
        UNPROTECT(1);
        i++;

        git_commit_free(commit);
    }

cleanup:
    if (walker)
        git_revwalk_free(walker);

    git_repository_free(repository);

    UNPROTECT(1);

    if (err < 0) {
        const git_error *e = giterr_last();
        error("Error %d/%d: %s\n", err, e->klass, e->message);
    }

    return list;
}

/**
 * Get all tags that can be found in a repository.
 *
 * @param repo S4 class git_repository
 * @return VECXSP with S4 objects of class git_tag
 */
SEXP tags(const SEXP repo)
{
    int err;
    SEXP list;
    size_t protected = 0;
    git_repository *repository;
    git_reference* reference = NULL;
    git_tag *tag = NULL;
    git_strarray tag_names = {0};
    size_t i;

    repository = get_repository(repo);
    if (!repository)
        error(git2r_err_invalid_repository);

    err = git_tag_list(&tag_names, repository);
    if (err < 0)
        goto cleanup;

    PROTECT(list = allocVector(VECSXP, tag_names.count));
    protected++;

    for(i = 0; i < tag_names.count; i++) {
        SEXP sexp_tag;
        const git_oid *oid;

        err = git_reference_dwim(&reference, repository, tag_names.strings[i]);
        if (err < 0)
            goto cleanup;

        oid = git_reference_target(reference);
        err = git_tag_lookup(&tag, repository, oid);
        if (err < 0)
            goto cleanup;

        PROTECT(sexp_tag = NEW_OBJECT(MAKE_CLASS("git_tag")));
        protected++;
        init_tag(tag, sexp_tag);

        SET_VECTOR_ELT(list, i, sexp_tag);
        UNPROTECT(1);
        protected--;

        git_tag_free(tag);
        tag = NULL;
        git_reference_free(reference);
        reference = NULL;
    }

cleanup:
    git_strarray_free(&tag_names);

    if (tag)
        git_tag_free(tag);

    if (reference)
        git_reference_free(reference);

    if (repository)
        git_repository_free(repository);

    if (protected)
        UNPROTECT(protected);

    if (err < 0) {
        const git_error *e = giterr_last();
        error("Error %d: %s\n", e->klass, e->message);
    }

    return list;
}

/**
 * Get workdir of repository.
 *
 * @param repo S4 class git_repository
 * @return
 */
SEXP workdir(const SEXP repo)
{
    SEXP result;
    git_repository *repository;

    repository = get_repository(repo);
    if (!repository)
        error(git2r_err_invalid_repository);

    result = ScalarString(mkChar(git_repository_workdir(repository)));

    git_repository_free(repository);

    return result;
}

static const R_CallMethodDef callMethods[] =
{
    {"add", (DL_FUNC)&add, 2},
    {"branches", (DL_FUNC)&branches, 2},
    {"checkout", (DL_FUNC)&checkout, 2},
    {"clone", (DL_FUNC)&clone, 2},
    {"commit", (DL_FUNC)&commit, 5},
    {"config", (DL_FUNC)&config, 2},
    {"default_signature", (DL_FUNC)&default_signature, 1},
    {"fetch", (DL_FUNC)&fetch, 2},
    {"init", (DL_FUNC)&init, 2},
    {"is_bare", (DL_FUNC)&is_bare, 1},
    {"is_empty", (DL_FUNC)&is_empty, 1},
    {"is_repository", (DL_FUNC)&is_repository, 1},
    {"push", (DL_FUNC)&push, 3},
    {"references", (DL_FUNC)&references, 1},
    {"remotes", (DL_FUNC)&remotes, 1},
    {"remote_url", (DL_FUNC)&remote_url, 2},
    {"revisions", (DL_FUNC)&revisions, 1},
    {"status", (DL_FUNC)&status, 5},
    {"tags", (DL_FUNC)&tags, 1},
    {"workdir", (DL_FUNC)&workdir, 1},
    {NULL, NULL, 0}
};

/**
 * Register routines to R.
 *
 * @param info Information about the DLL being loaded
 */
void
R_init_gitr(DllInfo *info)
{
    R_registerRoutines(info, NULL, callMethods, NULL, NULL);
}
