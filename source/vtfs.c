#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include "http.h"

#define MODULE_NAME "vtfs"
MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("VTFS - simple FS");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

struct dentry* vtfs_mount(
    struct file_system_type* fs_type, int flags, const char* token, void* data
);
void vtfs_kill_sb(struct super_block* sb);
int vtfs_fill_super(struct super_block* sb, void* data, int silent);
struct inode* vtfs_get_inode(struct super_block* sb, struct inode* dir, umode_t mode, int i_ino);
struct dentry* vtfs_lookup(
    struct inode* parent_inode, struct dentry* child_dentry, unsigned int flag
);
int vtfs_iterate(struct file* filp, struct dir_context* ctx);
int vtfs_create(
    struct mnt_idmap* idmap,
    struct inode* parent_inode,
    struct dentry* child_dentry,
    umode_t mode,
    bool excl
);
int vtfs_unlink(struct inode* parent_inode, struct dentry* child_dentry);
int vtfs_mkdir(struct mnt_idmap*, struct inode*, struct dentry*, umode_t);
int vtfs_rmdir(struct inode*, struct dentry*);
ssize_t vtfs_read(struct file* filp, char* buffer, size_t len, loff_t* offset);
ssize_t vtfs_write(struct file* filp, const char* buffer, size_t len, loff_t* offset);
int vtfs_link(struct dentry* old_dentry, struct inode* parent_dir, struct dentry* new_dentry);

struct inode_operations vtfs_inode_ops = {
    .lookup = vtfs_lookup,
    .create = vtfs_create,
    .unlink = vtfs_unlink,
    .mkdir = vtfs_mkdir,
    .rmdir = vtfs_rmdir,
    .link = vtfs_link
};

struct file_operations vtfs_dir_ops = {
    .iterate_shared = vtfs_iterate,
};

struct file_operations vtfs_file_ops = {.read = vtfs_read, .write = vtfs_write};

struct dentry* vtfs_mount(
    struct file_system_type* fs_type, int flags, const char* token, void* data
) {
  struct dentry* ret = mount_nodev(fs_type, flags, data, vtfs_fill_super);
  if (!ret) {
    printk(KERN_ERR "[vtfs] Can't mount file system\n");
  } else {
    printk(KERN_INFO "[vtfs] Mounted successfully\n");
  }
  return ret;
}

void vtfs_kill_sb(struct super_block* sb) {
  printk(KERN_INFO "[vtfs] Superblock destroyed, unmount ok\n");
}

struct inode* vtfs_get_inode(struct super_block* sb, struct inode* dir, umode_t mode, int i_ino) {
  struct inode* inode = new_inode(sb);
  if (!inode)
    return NULL;

  inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
  inode->i_ino = i_ino;

  return inode;
}

struct dentry* vtfs_lookup(struct inode* dir, struct dentry* dentry, unsigned int flags) {
  char response[1024];
  char parent_ino[16];
  snprintf(parent_ino, 16, "%lu", dir->i_ino);

  int64_t ret = vtfs_http_call(
      "token",
      "lookup",
      response,
      sizeof(response),
      2,
      "parent_ino",
      parent_ino,
      "name",
      dentry->d_name.name
  );

  if (ret < 0)
    return NULL;

  ino_t ino;
  char type[8];
  umode_t mode;
  sscanf(response, "%lu %s %ho", &ino, type, &mode);

  struct inode* inode =
      vtfs_get_inode(dir->i_sb, dir, (strcmp(type, "dir") == 0 ? S_IFDIR : S_IFREG) | mode, ino);

  inode->i_op = &vtfs_inode_ops;
  inode->i_fop = strcmp(type, "dir") == 0 ? &vtfs_dir_ops : &vtfs_file_ops;

  d_add(dentry, inode);
  return NULL;
}

int vtfs_create(
    struct mnt_idmap* idmap,
    struct inode* parent_inode,
    struct dentry* child_dentry,
    umode_t mode,
    bool excl
) {
  char response[256];
  char parent_ino[16];
  snprintf(parent_ino, 16, "%lu", parent_inode->i_ino);

  int64_t ret = vtfs_http_call(
      "token",
      "create",
      response,
      sizeof(response),
      3,
      "parent_ino",
      parent_ino,
      "name",
      child_dentry->d_name.name,
      "mode",
      "666"
  );

  if (ret < 0)
    return -EIO;

  ino_t ino;
  sscanf(response, "%lu", &ino);

  struct inode* inode = vtfs_get_inode(parent_inode->i_sb, parent_inode, S_IFREG | mode, ino);

  inode->i_op = &vtfs_inode_ops;
  inode->i_fop = &vtfs_file_ops;
  d_add(child_dentry, inode);
  return 0;
}

int vtfs_unlink(struct inode* parent_inode, struct dentry* child_dentry) {
  char ino[16];
  snprintf(ino, 16, "%lu", child_dentry->d_inode->i_ino);

  return vtfs_http_call("token", "unlink", NULL, 0, 1, "ino", ino) < 0 ? -EIO : 0;
}

int vtfs_mkdir(
    struct mnt_idmap*, struct inode* parent_inode, struct dentry* child_dentry, umode_t mode
) {
  char parent[16];
  snprintf(parent, 16, "%lu", parent_inode->i_ino);

  int64_t ret = vtfs_http_call(
      "token", "mkdir", NULL, 0, 2, "parent_ino", parent, "name", child_dentry->d_name.name
  );

  return ret < 0 ? -EIO : 0;
}

int vtfs_rmdir(struct inode* parent_inode, struct dentry* child_dentry) {
  char ino[16];
  snprintf(ino, 16, "%lu", child_dentry->d_inode->i_ino);

  return vtfs_http_call("token", "rmdir", NULL, 0, 1, "ino", ino) < 0 ? -EIO : 0;
}

ssize_t vtfs_read(struct file* filp, char* buffer, size_t len, loff_t* off) {
  char response[4096];
  char ino[16], offset[16], size[16];

  snprintf(ino, 16, "%lu", filp->f_inode->i_ino);
  snprintf(offset, 16, "%lld", *off);
  snprintf(size, 16, "%zu", len);

  int64_t ret = vtfs_http_call(
      "token", "read", response, sizeof(response), 3, "ino", ino, "offset", offset, "size", size
  );

  if (ret <= 0)
    return ret;

  if (copy_to_user(buffer, response, ret))
    return -EFAULT;

  *off += ret;
  return ret;
}

ssize_t vtfs_write(struct file* filp, const char* buffer, size_t len, loff_t* off) {
  char ino[16], offset[16], data[4096];
  snprintf(ino, 16, "%lu", filp->f_inode->i_ino);
  snprintf(offset, 16, "%d", *offset);

  if (copy_from_user(data, buffer, len))
    return -EFAULT;

  data[len] = 0;

  int64_t ret =
      vtfs_http_call("token", "write", NULL, 0, 3, "ino", ino, "offset", offset, "data", data);

  if (ret < 0)
    return ret;

  *offset += len;
  return len;
}

int vtfs_link(struct dentry* old_dentry, struct inode* parent_dir, struct dentry* new_dentry) {
  char old[16], parent[16];
  snprintf(old, 16, "%lu", old_dentry->d_inode->i_ino);
  snprintf(parent, 16, "%lu", parent_dir->i_ino);

  return vtfs_http_call(
             "token",
             "link",
             NULL,
             0,
             3,
             "ino",
             old,
             "parent_ino",
             parent,
             "name",
             new_dentry->d_name.name
         ) < 0
             ? -EIO
             : 0;
}

int vtfs_iterate(struct file* filp, struct dir_context* ctx) {
  char response[2048];
  char ino[16];
  snprintf(ino, 16, "%lu", filp->f_inode->i_ino);

  int64_t ret = vtfs_http_call("token", "list", response, sizeof(response), 1, "parent_ino", ino);

  if (ret < 0)
    return ret;

  char* p = response;
  while (*p) {
    ino_t child_ino;
    char name[64], type[8];
    sscanf(p, "%lu %63s %7s\n", &child_ino, name, type);

    dir_emit(ctx, name, strlen(name), child_ino, strcmp(type, "dir") == 0 ? DT_DIR : DT_REG);

    p = strchr(p, '\n') + 1;
  }

  return 0;
}

int vtfs_fill_super(struct super_block* sb, void* data, int silent) {
  struct inode* root = vtfs_get_inode(sb, NULL, S_IFDIR | 0777, 0);
  root->i_op = &vtfs_inode_ops;
  root->i_fop = &vtfs_dir_ops;

  sb->s_root = d_make_root(root);
  if (!sb->s_root)
    return -ENOMEM;

  LOG("Superblock filled\n");
  return 0;
}

struct file_system_type vtfs_fs_type = {
    .name = "vtfs",
    .mount = vtfs_mount,
    .kill_sb = vtfs_kill_sb,
};

static int __init vtfs_init(void) {
  int ret = register_filesystem(&vtfs_fs_type);
  if (ret) {
    pr_err("[vtfs] failed to register filesystem\n");
    return ret;
  }
  LOG("VTFS registered\n");
  return 0;
}

static void __exit vtfs_exit(void) {
  unregister_filesystem(&vtfs_fs_type);
  LOG("VTFS unregistered\n");
}

module_init(vtfs_init);
module_exit(vtfs_exit);