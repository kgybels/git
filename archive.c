#include "cache.h"
#include "commit.h"
#include "attr.h"
#include "archive.h"

static void format_subst(const struct commit *commit,
                         const char *src, size_t len,
                         struct strbuf *buf)
{
	char *to_free = NULL;
	struct strbuf fmt;

	if (src == buf->buf)
		to_free = strbuf_detach(buf, NULL);
	strbuf_init(&fmt, 0);
	for (;;) {
		const char *b, *c;

		b = memmem(src, len, "$Format:", 8);
		if (!b)
			break;
		c = memchr(b + 8, '$', (src + len) - b - 8);
		if (!c)
			break;

		strbuf_reset(&fmt);
		strbuf_add(&fmt, b + 8, c - b - 8);

		strbuf_add(buf, src, b - src);
		format_commit_message(commit, fmt.buf, buf);
		len -= c + 1 - src;
		src  = c + 1;
	}
	strbuf_add(buf, src, len);
	strbuf_release(&fmt);
	free(to_free);
}

static int convert_to_archive(const char *path,
                              const void *src, size_t len,
                              struct strbuf *buf,
                              const struct commit *commit)
{
	static struct git_attr *attr_export_subst;
	struct git_attr_check check[1];

	if (!commit)
		return 0;

	if (!attr_export_subst)
		attr_export_subst = git_attr("export-subst", 12);

	check[0].attr = attr_export_subst;
	if (git_checkattr(path, ARRAY_SIZE(check), check))
		return 0;
	if (!ATTR_TRUE(check[0].value))
		return 0;

	format_subst(commit, src, len, buf);
	return 1;
}

void *sha1_file_to_archive(const char *path, const unsigned char *sha1,
                           unsigned int mode, enum object_type *type,
                           unsigned long *sizep,
                           const struct commit *commit)
{
	void *buffer;

	buffer = read_sha1_file(sha1, type, sizep);
	if (buffer && S_ISREG(mode)) {
		struct strbuf buf;
		size_t size = 0;

		strbuf_init(&buf, 0);
		strbuf_attach(&buf, buffer, *sizep, *sizep + 1);
		convert_to_working_tree(path, buf.buf, buf.len, &buf);
		convert_to_archive(path, buf.buf, buf.len, &buf, commit);
		buffer = strbuf_detach(&buf, &size);
		*sizep = size;
	}

	return buffer;
}

int is_archive_path_ignored(const char *path)
{
	static struct git_attr *attr_export_ignore;
	struct git_attr_check check[1];

	if (!attr_export_ignore)
		attr_export_ignore = git_attr("export-ignore", 13);

	check[0].attr = attr_export_ignore;
	if (git_checkattr(path, ARRAY_SIZE(check), check))
		return 0;
	return ATTR_TRUE(check[0].value);
}

struct archiver_context {
	struct archiver_args *args;
	write_archive_entry_fn_t write_entry;
};

static int write_archive_entry(const unsigned char *sha1, const char *base,
		int baselen, const char *filename, unsigned mode, int stage,
		void *context)
{
	static struct strbuf path = STRBUF_INIT;
	struct archiver_context *c = context;
	struct archiver_args *args = c->args;
	write_archive_entry_fn_t write_entry = c->write_entry;
	int err;
	enum object_type type;
	unsigned long size;
	void *buffer;

	strbuf_reset(&path);
	strbuf_grow(&path, PATH_MAX);
	strbuf_add(&path, base, baselen);
	strbuf_addstr(&path, filename);

	if (is_archive_path_ignored(path.buf + args->baselen))
		return 0;

	if (S_ISDIR(mode) || S_ISGITLINK(mode)) {
		strbuf_addch(&path, '/');
		if (args->verbose)
			fprintf(stderr, "%.*s\n", (int)path.len, path.buf);
		err = write_entry(args, sha1, path.buf, path.len, mode, NULL, 0);
		if (err)
			return err;
		return READ_TREE_RECURSIVE;
	}

	buffer = sha1_file_to_archive(path.buf + args->baselen, sha1, mode,
			&type, &size, args->commit);
	if (!buffer)
		return error("cannot read %s", sha1_to_hex(sha1));
	if (args->verbose)
		fprintf(stderr, "%.*s\n", (int)path.len, path.buf);
	err = write_entry(args, sha1, path.buf, path.len, mode, buffer, size);
	free(buffer);
	return err;
}

int write_archive_entries(struct archiver_args *args,
		write_archive_entry_fn_t write_entry)
{
	struct archiver_context context;
	int err;

	if (args->baselen > 0 && args->base[args->baselen - 1] == '/') {
		size_t len = args->baselen;

		while (len > 1 && args->base[len - 2] == '/')
			len--;
		if (args->verbose)
			fprintf(stderr, "%.*s\n", (int)len, args->base);
		err = write_entry(args, args->tree->object.sha1, args->base,
				len, 040777, NULL, 0);
		if (err)
			return err;
	}

	context.args = args;
	context.write_entry = write_entry;

	err =  read_tree_recursive(args->tree, args->base, args->baselen, 0,
			args->pathspec, write_archive_entry, &context);
	if (err == READ_TREE_RECURSIVE)
		err = 0;
	return err;
}
