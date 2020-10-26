/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  Copyright (C) 2011       Sebastian Pipping <sebastian@pipping.org>

  This program can be distributed under the terms of the GNU GPL.
  See the file LICENSE.

*/

#define FUSE_USE_VERSION 39

#ifdef HAVE_CONFIG_H
#include "config.h"
#else
const char OUR_TOOL_VERSION[] = "Unknown version";
#endif

#ifdef linux
/* For pread()/pwrite()/utimensat() */
#define _XOPEN_SOURCE 700
#endif

#include <fuse.h>
#include <stdbool.h>
#include <stdio.h>
#ifdef HAVE_ATOI
#include <stdlib.h>
#endif
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#if defined(__cplusplus) && __cplusplus >= 201703L
	#define MAYBE_UNUSED [[maybe_unused]]
#else
	#if defined(__GNUC__) || defined(__clang__)
		#define MAYBE_UNUSED __attribute__ ((unused))
	#else
		#define MAYBE_UNUSED
	#endif
#endif


// DEBUG: FUSE calls
#define LOG(text) do{}while(0)// printf(text "\n")
#define LOGF(text,f) do{}while(0)//printf(text ": %s\n", f)

typedef uint8_t pstatus;
typedef uint8_t ptype;
typedef uint8_t chs[3];
typedef struct __attribute__((__packed__)) {
	pstatus status;
	chs firstSector;
	ptype type;
	chs lastSector;
	uint32_t firstSectorLBA;
	uint32_t sectorCount;
} pentry;

struct __attribute__((__packed__)) {
	unsigned char bootsector[446];
	pentry partitions[4];
	unsigned char signature[2];
} mbr;


typedef struct {
	uint8_t taken;
	int pIndex;
	int fd;
} pdescriptor;

int dev;
struct stat devStat;

#define MAX_FD 64
pdescriptor fds[MAX_FD];
uint64_t freefdindex = 0;
enum{
	blksize = 512
};

bool isroot(const char *path) {
	return path[0] == '/' && path[1] == '\0';
}

uint8_t pindex(const char *path) {
	int i = 0;
	if (path[i] == '/') { i++; }
	#ifdef HAVE_ATOI
	int res = atoi(path + i);
	#else
	int res = path[i]-'1';
	#endif
	if (res >= 0 && res < 4) { return (uint8_t) res; }
	return 0xFF;
}

struct chs_unpacked{
	uint8_t head;
	uint8_t sector;
	uint16_t cylinder;
};

struct chs_unpacked getchs(chs s) {
	struct chs_unpacked res = {
		s[0],
		(uint8_t)(s[1] >> 3),
		(uint16_t)((s[2] << 2) | (s[1] >> 6))
	};
	return res;
}

off_t chstolba(chs s) {
	struct chs_unpacked chs_unp = getchs(s);
	blksize_t blksize = devStat.st_blksize;
	return chs_unp.head*chs_unp.sector*chs_unp.cylinder*blksize;
}
off_t poffset(int pIndex) {
	pentry p = mbr.partitions[pIndex];
	return ((off_t)p.firstSectorLBA)*blksize;
	//return chstolba(p.firstSector);
}
ssize_t psize(int pIndex) {
	pentry p = mbr.partitions[pIndex];
	return ((ssize_t)p.sectorCount)*blksize;
	//return p.sectorCount*devStat.st_blksize;
}

int initstat(struct stat *st) {
	memset(st,0,sizeof(struct stat));
	st->st_mode = (devStat.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) | S_IFREG; //S_IFBLK;
	st->st_dev = 0x0505;
	st->st_uid = devStat.st_uid;
	st->st_gid = devStat.st_gid;
	st->st_blksize = devStat.st_blksize;
	st->st_atime = devStat.st_atime;
	st->st_mtime = devStat.st_mtime;
	st->st_ctime = devStat.st_ctime;
	st->st_blocks = devStat.st_blksize;
	return 0;
}
int plstat(uint8_t pi, struct stat *st) {
	st->st_rdev = 0x0100 | (pi + 1);
	st->st_ino = pi;
	st->st_size = psize(pi);
	st->st_blocks = st->st_size/512;
	return 0;
}


static int partfuse_getattr(const char *path, struct stat *st
#if FUSE_MAJOR_VERSION >= 3
, MAYBE_UNUSED struct fuse_file_info *fi
#endif
){
	LOGF("getattr",path);

	if (isroot(path)) { return stat(path,st); }

	uint8_t pi = pindex(path);
	if (pi == 0xFF) { return -ENOENT; }
	initstat(st);
	return plstat(pi, st);
}

static int partfuse_access(const char *path, MAYBE_UNUSED int mask)
{
	LOGF("access",path);
	int pi = pindex(path);
	if (pi == 0xFF) { return -ENOENT; }
	return 0;
}

static int partfuse_readlink(const char *path, char *buf, size_t size)
{
	ssize_t res;
	LOGF("readlink",path);

	res = readlink(path, buf, size - 1);
	if (res == -1)
		return -errno;

	buf[res] = '\0';
	return 0;
}

static int partfuse_readdir(MAYBE_UNUSED const char *path, void *buf, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi
#if FUSE_MAJOR_VERSION >= 3
	, MAYBE_UNUSED enum fuse_readdir_flags flags
#endif
)
{
	(void) offset;
	(void) fi;

	LOGF("readir",path);

	char name[2] = "1";
	struct stat st;
	initstat(&st);

	uint8_t pi;
	for(pi=0;pi<4;pi++) {
		pentry *entry = &mbr.partitions[pi];
		if (entry->type == 0) { continue; }

		plstat(pi,&st);
		name[0] = (char)('1' + pi);
		if (filler(buf, name, &st, 0
			#if FUSE_MAJOR_VERSION >= 3
			, FUSE_FILL_DIR_PLUS
			#endif
		))
			return 0;
	}
	return 0;
}

static int partfuse_mknod(MAYBE_UNUSED const char *path, MAYBE_UNUSED mode_t mode, MAYBE_UNUSED dev_t rdev)
{
	LOGF("mknod",path);

	return -EOPNOTSUPP;
}

static int partfuse_mkdir(MAYBE_UNUSED const char *path, MAYBE_UNUSED mode_t mode)
{
	LOGF("mkdir",path);
	return -EOPNOTSUPP;
}

static int partfuse_unlink(MAYBE_UNUSED const char *path)
{
	LOGF("unlink",path);
	return -EOPNOTSUPP;
}

static int partfuse_rmdir(MAYBE_UNUSED const char *path)
{
	LOGF("rmdir",path);
	return -EOPNOTSUPP;
}

static int partfuse_symlink(MAYBE_UNUSED const char *from, MAYBE_UNUSED const char *to)
{
	LOGF("symlink",from);
	return -EOPNOTSUPP;
}

static int partfuse_rename(MAYBE_UNUSED const char *from, MAYBE_UNUSED const char *to
#if FUSE_MAJOR_VERSION >= 3
, MAYBE_UNUSED unsigned int flags
#endif
)
{
	LOGF("rename",from);
	return -EOPNOTSUPP;
}

static int partfuse_link(MAYBE_UNUSED const char *from, MAYBE_UNUSED const char *to)
{
	LOGF("link",from);
	return -EOPNOTSUPP;
}

static int partfuse_chmod(MAYBE_UNUSED const char *path, MAYBE_UNUSED mode_t mode
#if FUSE_MAJOR_VERSION >= 3
, MAYBE_UNUSED struct fuse_file_info *fi
#endif
)
{
	LOGF("chmod",path);
	return -EOPNOTSUPP;
}

static int partfuse_chown(MAYBE_UNUSED const char *path, MAYBE_UNUSED uid_t uid, MAYBE_UNUSED gid_t gid
#if FUSE_MAJOR_VERSION >= 3
, MAYBE_UNUSED struct fuse_file_info *fi
#endif
)
{
	LOGF("chown",path);
	return -EOPNOTSUPP;
}

static int partfuse_truncate(MAYBE_UNUSED const char *path, MAYBE_UNUSED off_t size
#if FUSE_MAJOR_VERSION >= 3
, MAYBE_UNUSED struct fuse_file_info *fi
#endif
)
{
	LOGF("truncate",path);
	return -EOPNOTSUPP;
}

#ifdef HAVE_UTIMENSAT
static int partfuse_utimens(const char *path, const struct timespec ts[2]
#if FUSE_MAJOR_VERSION >= 3
	, MAYBE_UNUSED struct fuse_file_info *fi
#endif
)
{
	int res;

	/* don't use utime/utimes since they follow symlinks */
	res = utimensat(0, path, ts, AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		return -errno;

	return 0;
}
#endif

static int partfuse_open(const char *path, struct fuse_file_info *fi)
{
	LOGF("open",path);
	int pi = pindex(path);
	if (pi == 0xFF) { return -ENOENT; }
	uint64_t fdi = freefdindex;

	// Find new empty freefdindex
	while (fds[freefdindex].taken) {
		freefdindex = (freefdindex+1) % MAX_FD;
	}

	fds[fdi].taken = 0;
	fds[fdi].pIndex = pi;
	fds[fdi].fd = dup(dev);
	fi->fh = fdi;
	return 0;
}

static int partfuse_read(MAYBE_UNUSED const char *path, char *buf, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
	LOGF("read",path);
	pdescriptor *pd = &fds[fi->fh];
	off_t poff = poffset(pd->pIndex);
	lseek(pd->fd,poff+offset,SEEK_SET);
	return read(pd->fd,buf,size);
}

static int partfuse_write(MAYBE_UNUSED const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{
	LOGF("write",path);
	pdescriptor *pd = &fds[fi->fh];
	off_t poff = poffset(pd->pIndex);
	lseek(pd->fd,poff+offset,SEEK_SET);
	return write(pd->fd,buf,size);
}

static int partfuse_statfs(const char *path, struct statvfs *stbuf)
{
	int res;
	LOGF("statfs",path);
	res = statvfs(path, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

static int partfuse_release(const char *path, struct fuse_file_info *fi)
{
	/* Just a stub.	 This method is optional and can safely be left
	   unimplemented */
	LOGF("release",path);
	(void) path;
	(void) fi;
	pdescriptor *pd = &fds[fi->fh];
	close(pd->fd);
	memset(pd,0,sizeof(pdescriptor));
	return 0;
}

static int partfuse_fsync(const char *path, int isdatasync,
		     struct fuse_file_info *fi)
{
	/* Just a stub.	 This method is optional and can safely be left
	   unimplemented */
	LOGF("fsync",path);
	(void) path;
	(void) isdatasync;
	(void) fi;
	pdescriptor *pd = &fds[fi->fh];
	return fsync(pd->fd);
}

#ifdef HAVE_POSIX_FALLOCATE
static int partfuse_fallocate(MAYBE_UNUSED const char *path, MAYBE_UNUSED int mode,
			MAYBE_UNUSED off_t offset, MAYBE_UNUSED off_t length, struct fuse_file_info *fi)
{
	LOGF("fallocate",path);
	(void) fi;
	return -EOPNOTSUPP;
}
#endif

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
static int partfuse_setxattr(const char *path, const char *name, const char *value,
			size_t size, int flags)
{
	LOGF("setxattr",path);
	int res = lsetxattr(path, name, value, size, flags);
	if (res == -1)
		return -errno;
	return 0;
}

static int partfuse_getxattr(const char *path, const char *name, char *value,
			size_t size)
{
	LOGF("getxattr",path);
	ssize_t res = lgetxattr(path, name, value, size);
	if (res == -1)
		return -errno;
	return res;
}

static int partfuse_listxattr(const char *path, char *list, size_t size)
{
	LOGF("listxattr",path);
	ssize_t res = llistxattr(path, list, size);
	if (res == -1)
		return -errno;
	return res;
}

static int partfuse_removexattr(const char *path, const char *name)
{
	LOGF("removexattr",path);
	ssize_t res = lremovexattr(path, name);
	if (res == -1)
		return -errno;
	return 0;
}
#endif /* HAVE_SETXATTR */

static struct fuse_operations partfuse_oper = {
	.getattr	= partfuse_getattr,
	.readlink	= partfuse_readlink,
	.mknod		= partfuse_mknod,
	.mkdir		= partfuse_mkdir,
	.unlink		= partfuse_unlink,
	.rmdir		= partfuse_rmdir,
	.symlink	= partfuse_symlink,
	.rename		= partfuse_rename,
	.link		= partfuse_link,
	.chmod		= partfuse_chmod,
	.chown		= partfuse_chown,
	.truncate	= partfuse_truncate,
	.open		= partfuse_open,
	.read		= partfuse_read,
	.write		= partfuse_write,
	.statfs		= partfuse_statfs,
	.release	= partfuse_release,
	.fsync		= partfuse_fsync,
#ifdef HAVE_SETXATTR
	.setxattr	= partfuse_setxattr,
	.getxattr	= partfuse_getxattr,
	.listxattr	= partfuse_listxattr,
	.removexattr	= partfuse_removexattr,
#endif
	.readdir	= partfuse_readdir,
	.access		= partfuse_access,
#ifdef HAVE_UTIMENSAT
	.utimens	= partfuse_utimens,
#endif
#ifdef HAVE_POSIX_FALLOCATE
	.fallocate	= partfuse_fallocate,
#endif
};

void showUsage(char *argv[]) {
	printf("mbrfs %s built for fuse %d.%d.\nUsage: %s DEVICE MOUNTPOINT\n", OUR_TOOL_VERSION, FUSE_MAJOR_VERSION, FUSE_MINOR_VERSION, "partfuse");
	fuse_main(1, argv, &partfuse_oper, NULL);
}

int parseArgs(int *argc, char *argv[], char **device) {
	int i;
	
	if (*argc < 2) { return 1; }

	for(i=1;i<*argc;i++) {
		if (argv[i][0] != '-') {
			*device = argv[i];
			break;
		}
	}
	if (i == *argc) { return 1; }
	(*argc)--;
	for(;i<*argc;i++) {
		argv[i] = argv[i+1];
	}

	return 0;
}

int main(int argc, char *argv[])
{
	char *device;
	if (parseArgs(&argc, argv, &device) != 0) {
		showUsage(argv);
		return 1;
	}

	if (lstat(device,&devStat)) {
		fprintf(stderr, "File does not exist\n");
		return 1;
	}
	dev = open(device,O_RDWR,0);
	if (dev < 0) {
		fprintf(stderr, "Failed to open file\n");
		return 1;
	}
	if (read(dev,&mbr,sizeof(mbr)) != 512) {
		fprintf(stderr, "Failed to read MBR (file too small?)");
		return 1;
	}

	if (mbr.signature[0] != 0x55 || mbr.signature[1] != 0xaa) {
		fprintf(stderr, "Warning: MBR signature not found");
	}

	// DEBUG: partition table parsing
	// int pi;
	// for(pi=0;pi<4;pi++) {
	// 	pentry *entry = &mbr.partitions[pi];
	// 	printf("Partition %d\n", pi+1);
	// 	printf("  Status: %d\n", entry->status);
	// 	printf("  Type: %d\n", entry->type);
	// 	printf("  Offset: %d\n", entry->firstSectorLBA*512);
	// 	printf("  Size: %d\n", entry->sectorCount*512);
	// 	uint8_t head, sector;
	// 	uint16_t cylinder;
	// 	getchs(entry->firstSector,&head,&sector,&cylinder);
	// 	printf("  Head: %d\n", head);
	// 	printf("  Sector: %d\n", sector);
	// 	printf("  Cylinder: %d\n", cylinder);
	// }

	int res = fuse_main(argc, argv, &partfuse_oper, NULL);
	close(dev);
	return res;
}
