
/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  Copyright (C) 2011       Sebastian Pipping <sebastian@pipping.org>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

/** @file
 *
 * This file system mirrors the existing file system hierarchy of the
 * system, starting at the root file system. This is implemented by
 * just "passing through" all requests to the corresponding user-space
 * libc functions. Its performance is terrible.
 *
 * Compile with
 *
 *     gcc -Wall passthrough.c `pkg-config fuse3 --cflags --libs` -o passthrough
 *
 * ## Source code ##
 * \include passthrough.c
 */


#define FUSE_USE_VERSION 31

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef linux
/* For pread()/pwrite()/utimensat() */
#define _XOPEN_SOURCE 700
#endif

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
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

int key_count;
char ** key_list;
int key_check[100];
char *pd;
int init_over;

static int remove_dir(const char *dir)
{
    char cur_dir[] = ".";
    char up_dir[] = "..";
    char dir_name[128];
    DIR *dirp;
    struct dirent *dp;
    struct stat dir_stat;

    if ( 0 != access(dir, F_OK) ) {
        return 0;
    }

    if ( 0 > lstat(dir, &dir_stat) ) {
        perror("get directory stat error");
        return -1;
    }

    if ( S_ISREG(dir_stat.st_mode) ) {  
        remove(dir);
    } else if ( S_ISDIR(dir_stat.st_mode) ) {   
        dirp = opendir(dir);
        while ( (dp=readdir(dirp)) != NULL ) {
            if ( (0 == strcmp(cur_dir, dp->d_name)) || (0 == strcmp(up_dir, dp->d_name)) ) {
                continue;
            }

            sprintf(dir_name, "%s/%s", dir, dp->d_name);
            remove_dir(dir_name);  
        }
        closedir(dirp);

        rmdir(dir);     
    } else {
        perror("unknow file type!");    
    }
    return 0;
}

static void getnewpath(char *tmp, const char *path, char *pd) {
	int a1 = strlen(path);
	int a2 = strlen(pd);
	memset(tmp , 0, a1+a2+1);
	for(int i =0; i<a1+a2; i++)
		if (i < a2) tmp[i] = pd[i];
		else tmp[i] = path[i-a2];
}

static int is_accessible(const char *path)
{
	if (!init_over)	return 1;
	int i, j, k, l;
	l = strlen(path);
	if (l == 1)	return 1;
	char *tmp = malloc(sizeof(char) * (l + 1));
	for (i = 0, j = 0; i < l; i++, j++)
	{
		if (i == 0 && path[i] == '/')	
		{
			j--;
			continue;
		}
		if (path[i] == '/')	break;
		else	tmp[j] = path[i];
	}
	tmp[j] = '\0';
	for (k = 0, i = 0; i < key_count; i++)
		if (strcmp(key_list[i], tmp) == 0)
		{
			k = 1;
			break;
		}
	return k;
}

static int is_key_directory(const char *path)
{
	if (!init_over)	return 1;
	int i, k, l;
	l = strlen(path);
	for (i = 1, k = 0; i < l; i++)
	{
		if (path[i] == '/') k ++;
		if (k >= 1 && path[i] != '\0')	return 1;
	}
	return 0;
}

static void *xmp_init(struct fuse_conn_info *conn,
		      struct fuse_config *cfg)
{
	(void) conn;
	cfg->use_ino = 1;

	/* Pick up changes from lower filesystem right away. This is
	   also necessary for better hardlink support. When the kernel
	   calls the unlink() handler, it does not know the inode of
	   the to-be-removed entry and can therefore not invalidate
	   the cache of the associated inode - resulting in an
	   incorrect st_nlink value being reported for any remaining
	   hardlinks to this inode. */
	cfg->entry_timeout = 0;
	cfg->attr_timeout = 0;
	cfg->negative_timeout = 0;

	return NULL;
}

static int xmp_getattr(const char *path, struct stat *stbuf,
		       struct fuse_file_info *fi)
{
	(void) fi;
	int res;

	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	res = lstat(tmp, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_access(const char *path, int mask)
{
	int res;

	if (!is_accessible(path))
		return -EACCES;
	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	res = access(tmp, mask);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_readlink(const char *path, char *buf, size_t size)
{
	int res;

	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	res = readlink(tmp, buf, size - 1);
	if (res == -1)
		return -errno;

	buf[res] = '\0';
	return 0;
}


static int xmp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi,
		       enum fuse_readdir_flags flags)
{
	DIR *dp;
	struct dirent *de;

	(void) offset;
	(void) fi;
	(void) flags;

	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	dp = opendir(tmp);
	if (dp == NULL)
		return -errno;

	while ((de = readdir(dp)) != NULL) {
		struct stat st;
		memset(&st, 0, sizeof(st));
		st.st_ino = de->d_ino;
		st.st_mode = de->d_type << 12;
		if (filler(buf, de->d_name, &st, 0, 0))
			break;
	}

	closedir(dp);
	return 0;
}

static int xmp_mknod(const char *path, mode_t mode, dev_t rdev)
{
	int res;
	
	if (!is_accessible(path))
		return -EACCES;
	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);


	/* On Linux this could just be 'mknod(path, mode, rdev)' but this
	   is more portable */
	if (S_ISREG(mode)) {
		res = open(tmp, O_CREAT | O_EXCL | O_WRONLY, mode);
		if (res >= 0)
			res = close(res);
	} else if (S_ISFIFO(mode))
		res = mkfifo(tmp, mode);
	else
		res = mknod(tmp, mode, rdev);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_mkdir(const char *path, mode_t mode)
{
	int res;

	if (!is_accessible(path))
		return -EACCES;
	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	res = mkdir(tmp, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_unlink(const char *path)
{
	int res;

	if (!is_accessible(path))
		return -EACCES;
	if (!is_key_directory(path))
		return -EACCES;
	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	res = unlink(tmp);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_rmdir(const char *path)
{
	int res;

	if (!is_accessible(path))
		return -EACCES;
	if (!is_key_directory(path))
		return -EACCES;
	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	res = rmdir(tmp);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_symlink(const char *from, const char *to)
{
	int res;

	if (!is_accessible(from) || !is_accessible(to))
		return -EACCES;
	if (!is_key_directory(from))
		return -EACCES;
	char *tmp;
	int len_from = strlen(from);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_from + 1);
	getnewpath(tmp, from, pd);

	char *tmp1;
	int len_to = strlen(to);
	tmp1 = malloc(len_pd + len_to + 1);
	getnewpath(tmp1, to, pd);

	res = symlink(tmp, tmp1);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_rename(const char *from, const char *to, unsigned int flags)
{
	int res;

	if (!is_accessible(from) || !is_accessible(to))
		return -EACCES;
	if (!is_key_directory(from) || !is_key_directory(to))
		return -EACCES;
	char *tmp;
	int len_from = strlen(from);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_from + 1);
	getnewpath(tmp, from, pd);

	char *tmp1;
	int len_to = strlen(to);
	tmp1 = malloc(len_pd + len_to + 1);
	getnewpath(tmp1, to, pd);

	if (flags)
		return -EINVAL;

	res = rename(tmp, tmp1);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_link(const char *from, const char *to)
{
	int res;

	if (!is_accessible(from) || !is_accessible(to))
		return -EACCES;
	if (!is_key_directory(from))
		return -EACCES;
	char *tmp;
	int len_from = strlen(from);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_from + 1);
	getnewpath(tmp, from, pd);

	char *tmp1;
	int len_to = strlen(to);
	tmp1 = malloc(len_pd + len_to + 1);
	getnewpath(tmp1, to, pd);

	res = link(tmp, tmp1);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chmod(const char *path, mode_t mode,
		     struct fuse_file_info *fi)
{
	(void) fi;
	int res;

	if (!is_accessible(path))
		return -EACCES;
	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	res = chmod(tmp, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chown(const char *path, uid_t uid, gid_t gid,
		     struct fuse_file_info *fi)
{
	(void) fi;
	int res;

	if (!is_accessible(path))
		return -EACCES;
	if (!is_key_directory(path))
		return -EACCES;
	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	res = lchown(tmp, uid, gid);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_truncate(const char *path, off_t size,
			struct fuse_file_info *fi)
{
	int res;

	if (!is_accessible(path))
		return -EACCES;
	if (!is_key_directory(path))
		return -EACCES;
	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	if (fi != NULL)
		res = ftruncate(fi->fh, size);
	else
		res = truncate(tmp, size);
	if (res == -1)
		return -errno;

	return 0;
}

#ifdef HAVE_UTIMENSAT
static int xmp_utimens(const char *path, const struct timespec ts[2],
		       struct fuse_file_info *fi)
{
	(void) fi;
	int res;

	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	/* don't use utime/utimes since they follow symlinks */
	res = utimensat(0, tmp, ts, AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		return -errno;

	return 0;
}
#endif

static int xmp_create(const char *path, mode_t mode,
		      struct fuse_file_info *fi)
{
	int res;

	if (!is_accessible(path))
		return -EACCES;
	if (!is_key_directory(path))
		return -EACCES;
	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	res = open(tmp, fi->flags, mode);
	if (res == -1)
		return -errno;

	fi->fh = res;
	return 0;
}

static int xmp_open(const char *path, struct fuse_file_info *fi)
{
	int res;

	if (!is_accessible(path))
		return -EACCES;
	if (!is_key_directory(path))
		return -EACCES;
	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	res = open(tmp, fi->flags);
	if (res == -1)
		return -errno;

	fi->fh = res;
	return 0;
}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
	int fd;
	int res;

	if (!is_accessible(path))
		return -EACCES;
	if (!is_key_directory(path))
		return -EACCES;
	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	if(fi == NULL)
		fd = open(tmp, O_RDONLY);
	else
		fd = fi->fh;
	
	if (fd == -1)
		return -errno;

	res = pread(fd, buf, size, offset);
	if (res == -1)
		res = -errno;

	if(fi == NULL)
		close(fd);
	return res;
}

static int xmp_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{
	int fd;
	int res;

	if (!is_accessible(path))
		return -EACCES;
	if (!is_key_directory(path))
		return -EACCES;
	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	(void) fi;
	if(fi == NULL)
		fd = open(tmp, O_WRONLY);
	else
		fd = fi->fh;
	
	if (fd == -1)
		return -errno;

	res = pwrite(fd, buf, size, offset);
	if (res == -1)
		res = -errno;

	if(fi == NULL)
		close(fd);
	return res;
}

static int xmp_statfs(const char *path, struct statvfs *stbuf)
{
	int res;

	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	res = statvfs(tmp, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_release(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	close(fi->fh);
	return 0;
}

static int xmp_fsync(const char *path, int isdatasync,
		     struct fuse_file_info *fi)
{
	/* Just a stub.	 This method is optional and can safely be left
	   unimplemented */

	(void) path;
	(void) isdatasync;
	(void) fi;
	return 0;
}

#ifdef HAVE_POSIX_FALLOCATE
static int xmp_fallocate(const char *path, int mode,
			off_t offset, off_t length, struct fuse_file_info *fi)
{
	int fd;
	int res;

	(void) fi;

	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	if (mode)
		return -EOPNOTSUPP;

	if(fi == NULL)
		fd = open(tmp, O_WRONLY);
	else
		fd = fi->fh;
	
	if (fd == -1)
		return -errno;

	res = -posix_fallocate(fd, offset, length);

	if(fi == NULL)
		close(fd);
	return res;
}
#endif

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
static int xmp_setxattr(const char *path, const char *name, const char *value,
			size_t size, int flags)
{
	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	int res = lsetxattr(tmp, name, value, size, flags);
	if (res == -1)
		return -errno;
	return 0;
}

static int xmp_getxattr(const char *path, const char *name, char *value,
			size_t size)
{
	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	int res = lgetxattr(tmp, name, value, size);
	if (res == -1)
		return -errno;
	return res;
}

static int xmp_listxattr(const char *path, char *list, size_t size)
{
	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	int res = llistxattr(tmp, list, size);
	if (res == -1)
		return -errno;
	return res;
}

static int xmp_removexattr(const char *path, const char *name)
{
	char *tmp;
	int len_path = strlen(path);
	int len_pd = strlen(pd);
	tmp = malloc(len_pd + len_path + 1);
	getnewpath(tmp, path, pd);

	int res = lremovexattr(tmp, name);
	if (res == -1)
		return -errno;
	return 0;
}
#endif /* HAVE_SETXATTR */

static struct fuse_operations xmp_oper = {
	.init           = xmp_init,
	.getattr	= xmp_getattr, // key
	.access		= xmp_access,
	.readlink	= xmp_readlink,
	.readdir	= xmp_readdir, 
	.mknod		= xmp_mknod, 
	.mkdir		= xmp_mkdir,
	.symlink	= xmp_symlink,
	.unlink		= xmp_unlink,
	.rmdir		= xmp_rmdir,
	.rename		= xmp_rename,
	.link		= xmp_link,
	.chmod		= xmp_chmod,
	.chown		= xmp_chown,
	.truncate	= xmp_truncate,
#ifdef HAVE_UTIMENSAT
	.utimens	= xmp_utimens,
#endif
	.open		= xmp_open,
	.create 	= xmp_create,
	.read		= xmp_read,
	.write		= xmp_write,
	.statfs		= xmp_statfs,
	.release	= xmp_release,
	.fsync		= xmp_fsync,
#ifdef HAVE_POSIX_FALLOCATE
	.fallocate	= xmp_fallocate,
#endif
#ifdef HAVE_SETXATTR
	.setxattr	= xmp_setxattr,
	.getxattr	= xmp_getxattr,
	.listxattr	= xmp_listxattr,
	.removexattr	= xmp_removexattr,
#endif
};

int main(int argc, char *argv[])
{
	/*
		argc = 2 + 1 + keycount
		argv:
			./passthrought.c
			mountpoint
			/theplace_real
			key1
			key2
			......
			key_keycount

	*/
	init_over = 0;

	memset(key_check, 0 ,sizeof(key_check));

	int length;
	key_count = argc - 3;
	key_list = malloc(sizeof(int) * key_count + 1);
	for (int i = 3; i < argc; i++)
	{
		length = strlen(argv[i]) + 1;
		key_list[i-3] = malloc(length);
		memset(key_list[i-3], 0, length);
		memcpy(key_list[i-3], argv[i], length-1);
		printf("%s\n", key_list[i-3]);
	}


	length = strlen(argv[2]) + 1;
	pd = malloc(length);
	memset(pd , 0, length);
	memcpy(pd , argv[2], length-1);
	printf("%s\n", pd);

	struct dirent* ent = NULL;
	DIR *pDir;
	pDir = opendir(pd);
	char tmppath[512];
	struct stat s;
	int flag;

	while (NULL != (ent = readdir(pDir)))
	{
		if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
			continue;

		sprintf(tmppath, "%s/%s", pd, ent->d_name);
		printf("%s\n", tmppath);
		lstat(tmppath, &s);

		if (S_ISDIR(s.st_mode)) {
			flag = 0;
			for(int i = 0; i<key_count; i++)
				if (!strcmp(ent->d_name, key_list[i]))
					flag = i+1;

			if (!flag)
				remove_dir(tmppath);
			else
				key_check[flag-1] = 1;
		}
		else 
			continue;
	}

	for(int i = 0; i<key_count; i++)
		if (!key_check[i]) {
			sprintf(tmppath, "%s/%s", pd, key_list[i]);
			printf("%s\n", tmppath);
			mkdir(tmppath, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH); 
		}

	umask(0);
	init_over = 1;

	return fuse_main(2, argv, &xmp_oper, NULL);
}
