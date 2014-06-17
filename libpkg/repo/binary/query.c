/* Copyright (c) 2014, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *       * Redistributions of source code must retain the above copyright
 *         notice, this list of conditions and the following disclaimer.
 *       * Redistributions in binary form must reproduce the above copyright
 *         notice, this list of conditions and the following disclaimer in the
 *         documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <assert.h>
#include <errno.h>
#include <regex.h>
#include <grp.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>

#include <sqlite3.h>

#include "pkg.h"
#include "private/event.h"
#include "private/pkg.h"
#include "private/pkgdb.h"
#include "private/utils.h"
#include "binary.h"

static struct pkg_repo_it* pkg_repo_binary_it_new(struct pkg_repo *repo,
	sqlite3_stmt *s, short flags);
static int pkg_repo_binary_it_next(struct pkg_repo_it *it, struct pkg **pkg_p, unsigned flags);
static void pkg_repo_binary_it_free(struct pkg_repo_it *it);
static void pkg_repo_binary_it_reset(struct pkg_repo_it *it);

static struct pkg_repo_it_ops pkg_repo_binary_it_ops = {
	.next = pkg_repo_binary_it_next,
	.free = pkg_repo_binary_it_free,
	.reset = pkg_repo_binary_it_reset
};

static struct pkg_repo_it*
pkg_repo_binary_it_new(struct pkg_repo *repo, sqlite3_stmt *s, short flags)
{
	struct pkg_repo_it *it;
	struct pkgdb fakedb;

	it = malloc(sizeof(*it));
	if (it == NULL) {
		pkg_emit_errno("malloc", "pkg_repo_it");
		sqlite3_finalize(s);
		return (NULL);
	}

	it->ops = &pkg_repo_binary_it_ops;
	it->flags = flags;
	it->repo = repo;

	fakedb.sqlite = PRIV_GET(repo);
	it->data = pkgdb_it_new_sqlite(&fakedb, s, PKG_REMOTE, flags);

	if (it->data == NULL) {
		free(it);
		return (NULL);
	}

	return (it);
}

static int
pkg_repo_binary_it_next(struct pkg_repo_it *it, struct pkg **pkg_p, unsigned flags)
{
	return (pkgdb_it_next(it->data, pkg_p, flags));
}

static void
pkg_repo_binary_it_free(struct pkg_repo_it *it)
{
	pkgdb_it_free(it->data);
	free(it);
}

static void
pkg_repo_binary_it_reset(struct pkg_repo_it *it)
{
	pkgdb_it_reset(it->data);
}

struct pkg_repo_it *
pkg_repo_binary_query(struct pkg_repo *repo, const char *pattern, match_t match)
{
	sqlite3 *sqlite = PRIV_GET(repo);
	sqlite3_stmt	*stmt = NULL;
	struct sbuf	*sql = NULL;
	const char	*comp = NULL;
	int		 ret;
	char		 basesql[BUFSIZ] = ""
		"SELECT id, origin, name, name || '~' || origin as uniqueid, version, comment, "
		"prefix, desc, arch, maintainer, www, "
		"licenselogic, flatsize, pkgsize, "
		"cksum, manifestdigest, path AS repopath, '%1$s' AS dbname "
		"FROM packages";

	assert(match == MATCH_ALL || (pattern != NULL && pattern[0] != '\0'));

	sql = sbuf_new_auto();
	comp = pkgdb_get_pattern_query(pattern, match);
	if (comp && comp[0])
		strlcat(basesql, comp, sizeof(basesql));

	sbuf_printf(sql, basesql, repo->name);

	sbuf_cat(sql, " ORDER BY name;");
	sbuf_finish(sql);

	pkg_debug(4, "Pkgdb: running '%s' query for %s", sbuf_get(sql), pattern);
	ret = sqlite3_prepare_v2(sqlite, sbuf_get(sql), sbuf_size(sql), &stmt, NULL);
	if (ret != SQLITE_OK) {
		ERROR_SQLITE(sqlite, sbuf_get(sql));
		sbuf_delete(sql);
		return (NULL);
	}

	sbuf_delete(sql);

	if (match != MATCH_ALL && match != MATCH_CONDITION)
		sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);

	return (pkg_repo_binary_it_new(repo, stmt, PKGDB_IT_FLAG_ONCE));
}

struct pkg_repo_it *
pkg_repo_binary_shlib_provide(struct pkg_repo *repo, const char *require)
{
	sqlite3_stmt	*stmt;
	sqlite3 *sqlite = PRIV_GET(repo);
	struct sbuf	*sql = NULL;
	int		 ret;
	const char	 basesql[] = ""
			"SELECT p.id, p.origin, p.name, p.version, p.comment, "
			"p.name || '~' || p.origin as uniqueid, "
			"p.prefix, p.desc, p.arch, p.maintainer, p.www, "
			"p.licenselogic, p.flatsize, p.pkgsize, "
			"p.cksum, p.manifestdigest, p.path AS repopath, '%1$s' AS dbname "
			"FROM '%1$s'.packages AS p INNER JOIN '%1$s'.pkg_shlibs_provided AS ps ON "
			"p.id = ps.package_id "
			"WHERE ps.shlib_id IN (SELECT id FROM '%1$s'.shlibs WHERE "
			"name BETWEEN ?1 AND ?1 || '.9');";

	sbuf_printf(sql, basesql, repo->name);

	sbuf_finish(sql);

	pkg_debug(4, "Pkgdb: running '%s'", sbuf_get(sql));
	ret = sqlite3_prepare_v2(sqlite, sbuf_get(sql), -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ERROR_SQLITE(sqlite, sbuf_get(sql));
		sbuf_delete(sql);
		return (NULL);
	}

	sbuf_delete(sql);

	sqlite3_bind_text(stmt, 1, require, -1, SQLITE_TRANSIENT);

	return (pkg_repo_binary_it_new(repo, stmt, PKGDB_IT_FLAG_ONCE));
}

struct pkg_repo_it *
pkg_repo_binary_shlib_require(struct pkg_repo *repo, const char *provide)
{
	sqlite3_stmt	*stmt;
	sqlite3 *sqlite = PRIV_GET(repo);
	struct sbuf	*sql = NULL;
	int		 ret;
	const char	 basesql[] = ""
			"SELECT p.id, p.origin, p.name, p.version, p.comment, "
			"p.name || '~' || p.origin as uniqueid, "
			"p.prefix, p.desc, p.arch, p.maintainer, p.www, "
			"p.licenselogic, p.flatsize, p.pkgsize, "
			"p.cksum, p.manifestdigest, p.path AS repopath, '%1$s' AS dbname "
			"FROM '%1$s'.packages AS p INNER JOIN '%1$s'.pkg_shlibs_required AS ps ON "
			"p.id = ps.package_id "
			"WHERE ps.shlib_id = (SELECT id FROM '%1$s'.shlibs WHERE name=?1);";

	sbuf_printf(sql, basesql, repo->name);

	sbuf_finish(sql);

	pkg_debug(4, "Pkgdb: running '%s'", sbuf_get(sql));
	ret = sqlite3_prepare_v2(sqlite, sbuf_get(sql), -1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		ERROR_SQLITE(sqlite, sbuf_get(sql));
		sbuf_delete(sql);
		return (NULL);
	}

	sbuf_delete(sql);

	sqlite3_bind_text(stmt, 1, provide, -1, SQLITE_TRANSIENT);

	return (pkg_repo_binary_it_new(repo, stmt, PKGDB_IT_FLAG_ONCE));
}
