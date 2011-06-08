/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.

  gcc -Wall `pkg-config fuse --cflags --libs` hello.c -o hello
*/

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

static const char *hello_str = "Hello World!\n                                                                                          "
"                                                                                                                                       "
"                                                                                                                                       "
"                                                                                                                                       "
"                                                                                                                                       "
"                                                                                                                                       "
"                                                                                                                                       "
"                                                                                                                                       "
"                                                                                                                                       "
"                                                                                                                                       "
"                                                                                                                                       "
"                                                                                                                                       "
"                                                                                                                                       "
"                                                                                                                                       "
"                                                                                                                                       "
"                                                                                                                                       "
"                                                                                                                                54321\n";

static const char *hello_path = "/hello";

static int hello_getattr(const char *path, struct stat *stbuf)
{
	int res = 0;
	fprintf(stderr, "hello.c: hello_getattr `%s'\n", path);

	memset(stbuf, 0, sizeof(struct stat));
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else if (strcmp(path, hello_path) == 0) {
		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = strlen(hello_str);
	} else
		res = -ENOENT;

	return res;
}

static int hello_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	(void) offset;
	(void) fi;
	fprintf(stderr, "hello.c: hello_readdir `%s'\n", path);

	if (strcmp(path, "/") != 0)
		return -ENOENT;

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	filler(buf, hello_path + 1, NULL, 0);

	return 0;
}

static int hello_open(const char *path, struct fuse_file_info *fi)
{
	fprintf(stderr, "hello.c: hello_open `%s'\n", path);
	if (strcmp(path, hello_path) != 0) {
		fprintf(stderr, "hello.c: hello_open: (%d)`%s' != (%d)`%s'\n", strlen(path), path, strlen(hello_path), hello_path);
		return -ENOENT;
	}

	if ((fi->flags & 3) != O_RDONLY)
		return -EACCES;

	return 0;
}

static int hello_read(const char *path, char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
	size_t len;
	(void) fi;
	fprintf(stderr, "hello.c: hello_read `%s' (off: %ld, len: %ld)\n", path, offset, size);
	if(strcmp(path, hello_path) != 0)
		return -ENOENT;

	len = strlen(hello_str);
	if (offset < len) {
		if (offset + size > len)
			size = len - offset;
		memcpy(buf, hello_str + offset, size);
	} else
		size = 0;

	return size;
}

static struct fuse_operations hello_oper = {
	.getattr	= hello_getattr,
	.readdir	= hello_readdir,
	.open		= hello_open,
	.read		= hello_read,
};

int main(int argc, char *argv[])
{
	return fuse_main(argc, argv, &hello_oper, NULL);
}
