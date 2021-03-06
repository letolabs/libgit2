/*
 * Copyright (C) 2009-2011 the libgit2 contributors
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "git2/remote.h"
#include "git2/config.h"
#include "git2/types.h"

#include "config.h"
#include "repository.h"
#include "remote.h"
#include "fetch.h"
#include "refs.h"

static int refspec_parse(git_refspec *refspec, const char *str)
{
	char *delim;

	memset(refspec, 0x0, sizeof(git_refspec));

	if (*str == '+') {
		refspec->force = 1;
		str++;
	}

	delim = strchr(str, ':');
	if (delim == NULL)
		return git__throw(GIT_EOBJCORRUPTED, "Failed to parse refspec. No ':'");

	refspec->src = git__strndup(str, delim - str);
	if (refspec->src == NULL)
		return GIT_ENOMEM;

	refspec->dst = git__strdup(delim + 1);
	if (refspec->dst == NULL) {
		git__free(refspec->src);
		refspec->src = NULL;
		return GIT_ENOMEM;
	}

	return GIT_SUCCESS;
}

static int parse_remote_refspec(git_config *cfg, git_refspec *refspec, const char *var)
{
	const char *val;
	int error;

	error = git_config_get_string(cfg, var, &val);
	if (error < GIT_SUCCESS)
		return error;

	return refspec_parse(refspec, val);
}

int git_remote_new(git_remote **out, git_repository *repo, const char *url)
{
	git_remote *remote;

	remote = git__malloc(sizeof(git_remote));
	if (remote == NULL)
		return GIT_ENOMEM;

	memset(remote, 0x0, sizeof(git_remote));
	remote->repo = repo;
	remote->url = git__strdup(url);
	if (remote->url == NULL) {
		git__free(remote);
		return GIT_ENOMEM;
	}

	*out = remote;
	return GIT_SUCCESS;
}

int git_remote_get(git_remote **out, git_config *cfg, const char *name)
{
	git_remote *remote;
	char *buf = NULL;
	const char *val;
	int ret, error, buf_len;

	remote = git__malloc(sizeof(git_remote));
	if (remote == NULL)
		return GIT_ENOMEM;

	memset(remote, 0x0, sizeof(git_remote));
	remote->name = git__strdup(name);
	if (remote->name == NULL) {
		error = GIT_ENOMEM;
		goto cleanup;
	}

	/* "fetch" is the longest var name we're interested in */
	buf_len = strlen("remote.") + strlen(".fetch") + strlen(name) + 1;
	buf = git__malloc(buf_len);
	if (buf == NULL) {
		error = GIT_ENOMEM;
		goto cleanup;
	}

	ret = p_snprintf(buf, buf_len, "%s.%s.%s", "remote", name, "url");
	if (ret < 0) {
		error = git__throw(GIT_EOSERR, "Failed to build config var name");
		goto cleanup;
	}

	error = git_config_get_string(cfg, buf, &val);
	if (error < GIT_SUCCESS) {
		error = git__rethrow(error, "Remote's url doesn't exist");
		goto cleanup;
	}

	remote->repo = cfg->repo;
	remote->url = git__strdup(val);
	if (remote->url == NULL) {
		error = GIT_ENOMEM;
		goto cleanup;
	}

	ret = p_snprintf(buf, buf_len, "%s.%s.%s", "remote", name, "fetch");
	if (ret < 0) {
		error = git__throw(GIT_EOSERR, "Failed to build config var name");
		goto cleanup;
	}

	error = parse_remote_refspec(cfg, &remote->fetch, buf);
	if (error < GIT_SUCCESS) {
		error = git__rethrow(error, "Failed to get fetch refspec");
		goto cleanup;
	}

	ret = p_snprintf(buf, buf_len, "%s.%s.%s", "remote", name, "push");
	if (ret < 0) {
		error = git__throw(GIT_EOSERR, "Failed to build config var name");
		goto cleanup;
	}

	error = parse_remote_refspec(cfg, &remote->push, buf);
	/* Not finding push is fine */
	if (error == GIT_ENOTFOUND)
		error = GIT_SUCCESS;

	if (error < GIT_SUCCESS)
		goto cleanup;

	*out = remote;

cleanup:
	git__free(buf);
	if (error < GIT_SUCCESS)
		git_remote_free(remote);

	return error;
}

const char *git_remote_name(struct git_remote *remote)
{
	return remote->name;
}

const char *git_remote_url(struct git_remote *remote)
{
	return remote->url;
}

const git_refspec *git_remote_fetchspec(struct git_remote *remote)
{
	return &remote->fetch;
}

const git_refspec *git_remote_pushspec(struct git_remote *remote)
{
	return &remote->push;
}

int git_remote_connect(git_remote *remote, int direction)
{
	int error;
	git_transport *t;

	error = git_transport_new(&t, remote->url);
	if (error < GIT_SUCCESS)
		return git__rethrow(error, "Failed to create transport");

	error = t->connect(t, direction);
	if (error < GIT_SUCCESS) {
		error = git__rethrow(error, "Failed to connect the transport");
		goto cleanup;
	}

	remote->transport = t;

cleanup:
	if (error < GIT_SUCCESS)
		t->free(t);

	return error;
}

int git_remote_ls(git_remote *remote, git_headarray *refs)
{
	return remote->transport->ls(remote->transport, refs);
}

int git_remote_negotiate(git_remote *remote)
{
	return git_fetch_negotiate(remote);
}

int git_remote_download(char **filename, git_remote *remote)
{
	return git_fetch_download_pack(filename, remote);
}

int git_remote_update_tips(struct git_remote *remote)
{
	int error = GIT_SUCCESS;
	unsigned int i = 0;
	char refname[GIT_PATH_MAX];
	git_headarray *refs = &remote->refs;
	git_remote_head *head;
	git_reference *ref;
	struct git_refspec *spec = &remote->fetch;

	memset(refname, 0x0, sizeof(refname));

	if (refs->len == 0)
		return GIT_SUCCESS;

	/* HEAD is only allowed to be the first in the list */
	head = refs->heads[0];
	if (!strcmp(head->name, GIT_HEAD_FILE)) {
		error = git_reference_create_oid(&ref, remote->repo, GIT_FETCH_HEAD_FILE, &head->oid, 1);
		i = 1;
		if (error < GIT_SUCCESS)
			return git__rethrow(error, "Failed to update FETCH_HEAD");
	}

	for (; i < refs->len; ++i) {
		head = refs->heads[i];

		error = git_refspec_transform(refname, sizeof(refname), spec, head->name);
		if (error < GIT_SUCCESS)
			return error;

		error = git_reference_create_oid(&ref, remote->repo, refname, &head->oid, 1);
		if (error < GIT_SUCCESS)
			return error;
	}

	return GIT_SUCCESS;
}

void git_remote_free(git_remote *remote)
{
	if (remote == NULL)
		return;

	git__free(remote->fetch.src);
	git__free(remote->fetch.dst);
	git__free(remote->push.src);
	git__free(remote->push.dst);
	git__free(remote->url);
	git__free(remote->name);
	if (remote->transport != NULL) {
		if (remote->transport->connected)
			remote->transport->close(remote->transport);

		remote->transport->free(remote->transport);
	}
	git__free(remote);
}
