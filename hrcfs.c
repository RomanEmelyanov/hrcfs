/*
 * HTTP Range Cache FS (HRCFS)
 *
 * Copyright (C) 2010 Fedya Mosalov <fedya.mosalov@gmail.com>
 * Copyright (C) 2014 Roman Emelyanov <ers@ers.msk.ru>
 * This program can be distributed under the terms of the GNU GPL.

Make:
 cc -D_FILE_OFFSET_BITS=64 -Wall hrcfs.c -lfuse -lcurl -o hrcfs

Run:
fusermount -uz /var/cache/hrcfs/media
sudo -u www-data ./hrcfs /var/cache/hrcfs/media \
	-o cache_url=.googlevideo.com/videoplayback\? \
        -o cache_meta_dir=/var/cache/hrcfs/metainfo \
        -o cache_data_dir=/var/cache/hrcfs/data
 */

#define FUSE_USE_VERSION  26
#define PACKAGE_VERSION "0.6"
#define PATHLEN 4096
#define DELIMETR "#"

#include <stddef.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/file.h>
#include <unistd.h>
#include <stdlib.h>
#include <syslog.h>
#include <curl/curl.h>
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

struct buffer {
	size_t          size;
	char           *data;
};

struct cache_metainfo {
	size_t          size;
	time_t          mt;
	size_t          blocks;
	size_t          blocks_cached;
	char           *blocks_info;
};

struct hrcfs_config {
	int             block_size;
	int             http_timeout;
	char           *cache_metainfo_dir;
	char           *cache_data_dir;
	char           *complete_link_dir;
	char           *cache_url;
};

struct hrcfs_config conf = {
	131072,						// set block_size in bytes
	10,						// set http_timeout in seconds
	NULL,
	NULL,
	NULL,
	NULL
};

struct cache_metainfo *load_cache_metainfo(const char *path);
void save_cache_metainfo(const char *path, struct cache_metainfo *cmi);
void delete_cache_metainfo(struct cache_metainfo *cmi);

size_t
http_write_data(void *ptr, size_t size, size_t nmemb,
				struct buffer *buffer)
{
	size_t          offset = buffer->size;

	buffer->size = buffer->size + size * nmemb;
	buffer->data = realloc(buffer->data, buffer->size);
	memcpy(buffer->data + offset, ptr, size * nmemb);

	return size * nmemb;
}

void get_url(char *url, const char *path){
 char *hn, *ar;
 char path2[PATHLEN];
 strcpy(path2, path);
 hn = strtok (path2, DELIMETR); // skip cache key
 hn = strtok (NULL, DELIMETR); // get hostname
 ar = strtok (NULL, DELIMETR); // get arguments
 snprintf(url, PATHLEN, "http://%s%s%s", hn, conf.cache_url, ar);
}
void get_ck(char *ck, const char *path){ // get cache key
 char *cktmp;
 char path2[PATHLEN];
 strcpy(path2, path);
 cktmp = strtok (path2, DELIMETR);
 snprintf(ck, PATHLEN, "%s", cktmp);
}

size_t
http_get_size(const char *path)
{
	CURL           *easyhandle;
	int             success;
	char            url[PATHLEN];
	struct buffer   buffer = { 0, NULL };
	double          content_length = -1;
	long            response_code;
	char            curl_error[CURL_ERROR_SIZE];
	char		*ptr;

	easyhandle = curl_easy_init();
	get_url(url, path);
	ptr = strstr(url, "clen=");
	if(ptr){
		sscanf(ptr, "clen=%lf", &content_length);
		syslog(LOG_DAEMON, "CURL_SIZE_START_CLEN: %lf", content_length);
		return content_length;
	}	
	syslog(LOG_DAEMON, "CURL_SIZE_START: %s", url);
	curl_easy_setopt(easyhandle, CURLOPT_URL, url);
	curl_easy_setopt(easyhandle, CURLOPT_CUSTOMREQUEST, "HEAD");
	curl_easy_setopt(easyhandle, CURLOPT_HEADER, 1);
	curl_easy_setopt(easyhandle, CURLOPT_NOBODY, 1);
	curl_easy_setopt(easyhandle, CURLOPT_ERRORBUFFER, curl_error);
	curl_easy_setopt(easyhandle, CURLOPT_WRITEFUNCTION, http_write_data);
	curl_easy_setopt(easyhandle, CURLOPT_WRITEDATA, &buffer);
	curl_easy_setopt(easyhandle, CURLOPT_TIMEOUT, conf.http_timeout);
	curl_easy_setopt(easyhandle, CURLOPT_CONNECTTIMEOUT,
					 conf.http_timeout);
	curl_easy_setopt(easyhandle, CURLOPT_NOSIGNAL, 1);

	success = curl_easy_perform(easyhandle);

	if (success == 0) {
		curl_easy_getinfo(easyhandle, CURLINFO_RESPONSE_CODE,
						  &response_code);
		if (response_code == 200)
			curl_easy_getinfo(easyhandle, CURLINFO_CONTENT_LENGTH_DOWNLOAD,
							  &content_length);
		else
			syslog(LOG_DAEMON, "http_get_size: %s: %ld", url,
				   response_code);
	} else
		syslog(LOG_DAEMON, "http_get_size: %s: %s", url, curl_error);

	curl_easy_cleanup(easyhandle);
	free(buffer.data);
	
	syslog(LOG_DAEMON, "CURL_SIZE_STOP: %d, %f", success, content_length);

	return content_length;
}

size_t
http_get(const char *path, char *buf, size_t size, size_t offset)
{
	CURL           *easyhandle;
	int             success;
	struct curl_slist *headers = NULL;
	char            header_range[PATHLEN];
	char            url[PATHLEN];
	struct buffer   buffer = { 0, NULL };
	char            curl_error[CURL_ERROR_SIZE];
	size_t          result = 0;

	easyhandle = curl_easy_init();

	get_url(url, path);
	syslog(LOG_DAEMON, "CURL_START: url: %s", url);
	syslog(LOG_DAEMON, "CURL_START: %zd-%zd", offset, offset + size - 1);
	curl_easy_setopt(easyhandle, CURLOPT_URL, url);
	curl_easy_setopt(easyhandle, CURLOPT_WRITEFUNCTION, http_write_data);
	curl_easy_setopt(easyhandle, CURLOPT_WRITEDATA, &buffer);
	curl_easy_setopt(easyhandle, CURLOPT_ERRORBUFFER, curl_error);
	curl_easy_setopt(easyhandle, CURLOPT_TIMEOUT, conf.http_timeout);
	curl_easy_setopt(easyhandle, CURLOPT_CONNECTTIMEOUT,
					 conf.http_timeout);
	curl_easy_setopt(easyhandle, CURLOPT_NOSIGNAL, 1);

	snprintf(header_range, sizeof(header_range), "Range: bytes=%zd-%zd",
			 offset, offset + size - 1);
	headers = curl_slist_append(headers, header_range);
	curl_easy_setopt(easyhandle, CURLOPT_HTTPHEADER, headers);

	success = curl_easy_perform(easyhandle);

syslog(LOG_DAEMON, "CURL_STOP: %d %zd", success, buffer.size);
 
	/* TODO: erro handling */

	if (success == 0) {
		memcpy(buf, buffer.data, buffer.size);
		result = buffer.size;
	} else
		syslog(LOG_DAEMON, "http_get: %s: %s", url, curl_error);

	/*
	 * curl_easy_getinfo(easyhandle, CURLINFO_RESPONSE_CODE,
	 * &response_code); if (response_code != 200) syslog(LOG_DAEMON,
	 * "http_get: %s: %ld", url, response_code); 
	 */

	curl_slist_free_all(headers);
	curl_easy_cleanup(easyhandle);
	free(buffer.data);

	return result;
}

size_t
get_size(const char *path)
{
	struct cache_metainfo *cmi;
	size_t          size;

	cmi = load_cache_metainfo(path);

	if (cmi) {
		size = cmi->size;
		delete_cache_metainfo(cmi);
		return size;
	}

	return -1;
}

static int
hrcfs_getattr(const char *path, struct stat *stbuf)
{
	int             res = 0;

	memset(stbuf, 0, sizeof(struct stat));
	if (strcmp(path, "/") == 0) {
		res = lstat(conf.cache_data_dir, stbuf);
		if (res == -1)
			return -errno;
		/*
		 * stbuf->st_mode = S_IFDIR | 0775; stbuf->st_nlink = 2; 
		 */
	} else if (strstr(path, ".H.") == NULL) { // all filenames must include ".H."
		// looks like a directory, returns a default dir data
		res = lstat(conf.cache_data_dir, stbuf);
		if (res == -1)
			return -errno;
	} else {
		stbuf->st_mode = S_IFREG | 0664;
		stbuf->st_nlink = 1;
		stbuf->st_size = get_size(path);
		if (stbuf->st_size == -1)
			res = -ENOENT;
	}

	return res;
}

static int
hrcfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			  off_t offset, struct fuse_file_info *fi)
{
	(void) offset;
	(void) fi;

	if (strcmp(path, "/") != 0)
		return -ENOENT;

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	// filler(buf, hrcfs_path + 1, NULL, 0);

	return 0;
}

static int
hrcfs_open(const char *path, struct fuse_file_info *fi)
{
	int             fd;
	char            filename[PATHLEN];
	struct cache_metainfo *cmi;
	int             rc = -1;
	struct stat     st;
	char		ck[PATHLEN];

	cmi = load_cache_metainfo(path);
	if (!cmi)
		return -1;

	if (cmi->size) {
		get_ck(ck, path);
		snprintf(filename, PATHLEN, "%s%s", conf.cache_data_dir, ck);

		/* create file if not exist */
		if (stat(filename, &st) != 0)
			close(open(filename, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR));
		/* open in read only for work */
		fd = open(filename, O_RDONLY);
		if (fd == -1) {
			syslog(LOG_DAEMON, "open: %s: %s", filename, strerror(errno));
			rc = -1;
		} else {
			fi->fh = fd;
			rc = 0;
		}
	}

	delete_cache_metainfo(cmi);

	return rc;
}

void
delete_cache_metainfo(struct cache_metainfo *cmi)
{
	free((void *) cmi->blocks_info);
	free((void *) cmi);
}

struct cache_metainfo *
create_cache_metainfo(const char *path)
{
	struct cache_metainfo *cmi;
	size_t          size;
	size_t          blocks;

	syslog(LOG_DAEMON, "create_cache_metainfo: %s", path);

	size = http_get_size(path);

	if (size == -1)
		return NULL;

	blocks = size / conf.block_size + 1;

	cmi = malloc(sizeof(struct cache_metainfo));
	memset(cmi, 0, sizeof(struct cache_metainfo));
	cmi->size = size;
	cmi->blocks = blocks;
	cmi->blocks_info = malloc(blocks);
	memset(cmi->blocks_info, 0, blocks);

	return cmi;
}

struct cache_metainfo *
load_cache_metainfo(const char *path)
{
	int             fd;
	char            filename[PATHLEN];
	struct cache_metainfo *cmi;
	char		ck[PATHLEN];

	get_ck(ck, path);
	snprintf(filename, PATHLEN, "%s%s", conf.cache_metainfo_dir, ck);

	fd = open(filename, O_RDONLY);
	if (fd == -1) {
		cmi = create_cache_metainfo(path);
		if (cmi)
			save_cache_metainfo(path, cmi);
		return cmi;
	}

	cmi = malloc(sizeof(struct cache_metainfo));
	memset(cmi, 0, sizeof(struct cache_metainfo));

	read(fd, &(cmi->size), sizeof(size_t));
	read(fd, &(cmi->mt), sizeof(time_t));
	read(fd, &(cmi->blocks), sizeof(size_t));
	read(fd, &(cmi->blocks_cached), sizeof(size_t));

	cmi->blocks_info = malloc(cmi->blocks);
	read(fd, cmi->blocks_info, cmi->blocks);
	close(fd);

	return cmi;
}

void
save_cache_metainfo(const char *path, struct cache_metainfo *cmi)
{
	int             fd;
	char            filename[PATHLEN];
	char		ck[PATHLEN];

	get_ck(ck, path);	
	snprintf(filename, sizeof(filename), "%s%s", conf.cache_metainfo_dir, ck);
	fd = open(filename, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
	if (fd == -1) {
		syslog(LOG_DAEMON, "open: %s: %s", filename, strerror(errno));
		return;
	}
	if (flock(fd, LOCK_EX) != 0) {
		syslog(LOG_DAEMON, "flock: %s: %s", filename, strerror(errno));
		return;
	}
	write(fd, &(cmi->size), sizeof(size_t));
	write(fd, &(cmi->mt), sizeof(time_t));
	write(fd, &(cmi->blocks), sizeof(size_t));
	write(fd, &(cmi->blocks_cached), sizeof(size_t));
	write(fd, cmi->blocks_info, cmi->blocks);
	close(fd);
}

size_t
download_block(const char *path, size_t i)
{
	char            buf[conf.block_size];
	size_t          size;
	int             fd;
	char            filename[PATHLEN];
	char		ck[PATHLEN];

	// syslog(LOG_DAEMON, "download_block_start: %s", path);

	size = http_get(path, buf, conf.block_size, conf.block_size * i);

	if (size > 0) {
		get_ck(ck, path);
		snprintf(filename, sizeof(filename), "%s%s", conf.cache_data_dir, ck);
		fd = open(filename, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
		if (fd == -1) {
			syslog(LOG_DAEMON, "open: %s: %s", filename, strerror(errno));
			return 0;
		}
		if (flock(fd, LOCK_EX) != 0) {
			syslog(LOG_DAEMON, "flock: %s: %s", filename, strerror(errno));
			return 0;
		}
		lseek(fd, conf.block_size * i, SEEK_SET);
		write(fd, buf, size);
		close(fd);
	}

	return size;
}

/* check block in metainfo files */
size_t
cache_metainfo_has_block(const char *path, size_t i)
{
	struct cache_metainfo *cmi;
	int             rc;

	cmi = load_cache_metainfo(path);
	if (!cmi)
		return 0;

	/* programm able to get more data via read(), but return only that available */

	if (i >= cmi->blocks)
		return 1;

	rc = cmi->blocks_info[i];

	delete_cache_metainfo(cmi);

	return rc;
}

void
complete_link(const char *path)
{
	char            data_filename[PATHLEN];
	char            complete_link_filename[PATHLEN];

	if (conf.complete_link_dir == NULL)
		return;

	snprintf(data_filename, sizeof(data_filename), "%s%s",
			 conf.cache_data_dir, path);
	snprintf(complete_link_filename, sizeof(complete_link_filename),
			 "%s%s", conf.complete_link_dir, path);

	if (link(data_filename, complete_link_filename) != 0) {
		syslog(LOG_DAEMON, "link: %s to %s: %s", data_filename,
			   complete_link_filename, strerror(errno));
	}
}

void
cache_metainfo_add_block(const char *path, size_t i)
{
	struct cache_metainfo *cmi;

	cmi = load_cache_metainfo(path);
	if (!cmi)
		return;

	cmi->blocks_cached++;
	cmi->blocks_info[i] = 1;

	save_cache_metainfo(path, cmi);

	if (cmi->blocks_cached == cmi->blocks)
		complete_link(path);

	delete_cache_metainfo(cmi);
}

void
block_lock_filename(char *filename, const char *path, size_t i)
{
	char	ck[PATHLEN];

	get_ck(ck, path);
	snprintf(filename, PATHLEN, "%s%s-%zd.lock",
			 conf.cache_data_dir, ck, i);
}

void
block_lock_release(const char *path, size_t i, int fd)
{
	char            filename[PATHLEN];

	close(fd);

	block_lock_filename(filename, path, i);
	unlink(filename);
}

size_t
block_lock_acquire(const char *path, size_t i)
{
	char            filename[PATHLEN];
	int             fd;
	time_t          time_failed = time(NULL) + conf.http_timeout * 2;

	block_lock_filename(filename, path, i);

	fd = open(filename, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
	if (fd == -1) {
		syslog(LOG_DAEMON, "block_lock_acquire: open: %s: %s", filename,
			   strerror(errno));
		return 0;
	}

	while (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		usleep(250 * 1000);		// 0.25 seconds
		if (time(NULL) > time_failed) {
			syslog(LOG_DAEMON, "block_lock_acquire: flock: %s: %s",
				   filename, strerror(errno));
			block_lock_release(path, i, fd);
			return 0;
		}
	}

	return fd;
}

size_t
check_block(const char *path, size_t i)
{
	int             lock_fd;

// syslog(LOG_DAEMON, "check_block_start: %s", path);

	if (cache_metainfo_has_block(path, i))
		return 1;

	if ((lock_fd = block_lock_acquire(path, i))) {
		if (!cache_metainfo_has_block(path, i)) {
			if (download_block(path, i)) {
				cache_metainfo_add_block(path, i);
			}
		}
		block_lock_release(path, i, lock_fd);
	}

	return cache_metainfo_has_block(path, i);
}

/* return 1 - if all blocks available, overwise zero */ 

size_t
check_blocks(const char *path, size_t size, off_t offset)
{
	size_t          first_block = offset / conf.block_size;
	size_t          last_block = (offset + size) / conf.block_size;
	size_t          i;

	for (i = first_block; i <= last_block; ++i) {
		if (!check_block(path, i))
			return 0;
	}

	return 1;
}

static int
hrcfs_release(const char *path, struct fuse_file_info *fi)
{
	return close(fi->fh);
}

static int
hrcfs_read(const char *path, char *buf, size_t size, off_t offset,
		   struct fuse_file_info *fi)
{
	size_t          len;
	int             fd;

// syslog(LOG_DAEMON, "hrcfs_read_start: %s", path);

	if (!check_blocks(path, size, offset)) {
		syslog(LOG_DAEMON, "hrcfs_read: %s: size=%ld, offset=%ld: EIO",
			   path, size, offset);
		return -EIO;
	}

	fd = fi->fh;
	lseek(fd, offset, SEEK_SET);
	len = read(fd, buf, size);
	return len;
}

static struct fuse_operations hrcfs_oper = {
	.getattr = hrcfs_getattr,
	.readdir = hrcfs_readdir,
	.open = hrcfs_open,
	.read = hrcfs_read,
	.release = hrcfs_release,
};

enum {
	KEY_HELP,
	KEY_VERSION,
};

#define HRCFS_OPT(t, p, v) { t, offsetof(struct hrcfs_config, p), v }

static struct fuse_opt hrcfs_opts[] = {
	HRCFS_OPT("bs=%d", block_size, 0),
	HRCFS_OPT("http_timeout=%d", http_timeout, 0),
	HRCFS_OPT("cache_url=%s", cache_url, 0),
	HRCFS_OPT("cache_meta_dir=%s", cache_metainfo_dir, 0),
	HRCFS_OPT("cache_data_dir=%s", cache_data_dir, 0),
	HRCFS_OPT("complete_link_dir=%s", complete_link_dir, 0),

	FUSE_OPT_KEY("-V", KEY_VERSION),
	FUSE_OPT_KEY("--version", KEY_VERSION),
	FUSE_OPT_KEY("-h", KEY_HELP),
	FUSE_OPT_KEY("--help", KEY_HELP),
	FUSE_OPT_END
};

void
usage(struct fuse_args *args)
{
	fprintf(stderr,
			"usage: %s mountpoint -o cache_url=URL -o cachemetadir=DIR -o cachedatadir=DIR [options]\n"
			"\n"
			"general options:\n"
			"    -o opt,[opt...]  mount options\n"
			"    -h   --help      print help\n"
			"    -V   --version   print version\n"
			"\n"
			"hrcfs options:\n"
			"    -o bs=BLOCKSIZE\n"
			"    -o http_timeout=SECONDS\n"
			"    -o cache_url=URL\n"
			"    -o cache_meta_dir=DIR\n"
			"    -o cache_data_dir=DIR\n"
			"    -o complete_link_dir=DIR\n" "\n", args->argv[0]);
};

static int
hrcfs_opt_proc(void *data, const char *arg, int key,
			   struct fuse_args *outargs)
{
	switch (key) {
	case KEY_HELP:
		usage(outargs);
		fuse_opt_add_arg(outargs, "-ho");
		fuse_main(outargs->argc, outargs->argv, &hrcfs_oper, NULL);
		exit(1);

	case KEY_VERSION:
		fprintf(stderr, "hrcfs version %s\n", PACKAGE_VERSION);
		fuse_opt_add_arg(outargs, "--version");
		fuse_main(outargs->argc, outargs->argv, &hrcfs_oper, NULL);
		exit(0);
	}
	return 1;
}

int
main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	fuse_opt_parse(&args, &conf, hrcfs_opts, hrcfs_opt_proc);

	if (!conf.cache_metainfo_dir || !conf.cache_data_dir
		|| !conf.cache_url) {
		usage(&args);
		exit(1);
	}

	return fuse_main(args.argc, args.argv, &hrcfs_oper, NULL);
}
