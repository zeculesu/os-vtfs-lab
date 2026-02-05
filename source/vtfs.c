#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/mount.h>
#include <linux/string.h>


#define MODULE_NAME "vtfs"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("A simple FS kernel module");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

struct dentry* vtfs_mount(
    struct file_system_type* fs_type,
    int flags,
    const char* token,
    void* data
);
void vtfs_kill_sb(struct super_block* sb);
int vtfs_fill_super(struct super_block *sb, void *data, int silent);
struct inode* vtfs_get_inode(
  struct super_block* sb, 
  const struct inode* dir, 
  umode_t mode, 
  int i_ino
);

struct dentry* vtfs_lookup(
  struct inode* parent_inode,
  struct dentry* child_dentry,
  unsigned int flag
);
int vtfs_iterate(struct file* filp, struct dir_context* ctx);
int vtfs_create(
  struct inode *parent_inode, 
  struct dentry *child_dentry, 
  umode_t mode, 
  bool b
);
int vtfs_unlink(struct inode *parent_inode, struct dentry *child_dentry);

struct inode_operations vtfs_inode_ops = {
  .lookup = vtfs_lookup,
  .create = vtfs_create,
  .unlink = vtfs_unlink
};
struct file_operations vtfs_dir_ops = {
  .iterate_shared = vtfs_iterate,
};

struct dentry* vtfs_mount(
  struct file_system_type* fs_type,
  int flags,
  const char* token,
  void* data
) {
  struct dentry* ret = mount_nodev(fs_type, flags, data, vtfs_fill_super);
  if (ret == NULL) {
    printk(KERN_ERR "Can't mount file system");
  } else {
    printk(KERN_INFO "Mounted successfuly");
  }
  return ret;
}

struct inode* vtfs_get_inode(
  struct super_block* sb, 
  const struct inode* dir, 
  umode_t mode, 
  int i_ino
) {
  struct inode *inode = new_inode(sb);
  if (!inode) return NULL;

  inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
  inode->i_ino = i_ino;
  
  return inode;
}

struct dentry* vtfs_lookup(
  struct inode* parent_inode, 
  struct dentry* child_dentry, 
  unsigned int flag
) {
  ino_t root = parent_inode->i_ino;
  const char *name = child_dentry->d_name.name;
  if (root == 100 && !strcmp(name, "test.txt")) {
    struct inode *inode = vtfs_get_inode(parent_inode->i_sb, NULL, S_IFREG, 101);
    d_add(child_dentry, inode);
  } else if (root == 100 && !strcmp(name, "dir")) {
    struct inode *inode = vtfs_get_inode(parent_inode->i_sb, NULL, S_IFDIR, 200);
    d_add(child_dentry, inode);
  }
  return NULL;
}

int vtfs_create(
  struct inode *parent_inode, 
  struct dentry *child_dentry, 
  umode_t mode, 
  bool b
) {
  ino_t root = parent_inode->i_ino;
  const char *name = child_dentry->d_name.name;
  if (root == 100 && !strcmp(name, "test.txt")) {
    struct inode *inode = vtfs_get_inode(
        parent_inode->i_sb, NULL, S_IFREG | S_IRWXUGO, 101);
    inode->i_op = &vtfs_inode_ops;
    inode->i_fop = NULL;

    d_add(child_dentry, inode);
    mask |= 1;
  } else if (root == 100 && !strcmp(name, "new_file.txt")) {
    struct inode *inode = vtfs_get_inode(
        parent_inode->i_sb, NULL, S_IFREG | S_IRWXUGO, 102);
    inode->i_op = &vtfs_inode_ops;
    inode->i_fop = NULL;

    d_add(child_dentry, inode);
    mask |= 2;
  }
  return 0;
}

int vtfs_unlink(struct inode *parent_inode, struct dentry *child_dentry) {
  const char *name = child_dentry->d_name.name;
  ino_t root = parent_inode->i_ino;
  if (root == 100 && !strcmp(name, "test.txt")) {
    mask &= ~1;
  } else if (root == 100 && !strcmp(name, "new_file.txt")) {
    mask &= ~2;
  }
  return 0;
}

int vtfs_iterate(struct file *filp, struct dir_context *ctx)
{
    struct dentry *dentry = filp->f_path.dentry;
    struct inode *inode = dentry->d_inode;

    if (inode->i_ino != 100)
        return 0;

    if (ctx->pos == 0) {
        if (!dir_emit(ctx, ".", 1, inode->i_ino, DT_DIR))
            return 0;
        ctx->pos++;
    }

    if (ctx->pos == 1) {
        ino_t parent_ino = inode->i_ino;
        if (dentry->d_parent && dentry->d_parent->d_inode)
            parent_ino = dentry->d_parent->d_inode->i_ino;

        if (!dir_emit(ctx, "..", 2, parent_ino, DT_DIR))
            return 0;
        ctx->pos++;
    }

    if (ctx->pos == 2) {
        if (!dir_emit(ctx, "test.txt", 8, 101, DT_REG))
            return 0;
        ctx->pos++;
    }

    return 0;
}

struct file_system_type vtfs_fs_type = {
  .name = "vtfs",
  .mount = vtfs_mount,
  .kill_sb = vtfs_kill_sb,
};

void vtfs_kill_sb(struct super_block* sb) {
  printk(KERN_INFO "vtfs super block is destroyed. Unmount successfully.\n");
}

int vtfs_fill_super(struct super_block *sb, void *data, int silent) {
  struct inode* inode = vtfs_get_inode(sb, NULL, S_IFDIR | 0777, 1000);

  inode->i_op = &vtfs_inode_ops;
  inode->i_fop = &vtfs_dir_ops;

  sb->s_root = d_make_root(inode);
  if (sb->s_root == NULL) {
    return -ENOMEM;
  }

  printk(KERN_INFO "return 0\n");
  return 0;
}

static int __init vtfs_init(void) {
  int ret = register_filesystem(&vtfs_fs_type);
  if (ret != 0) {
      pr_err("[vtfs] failed to register filesystem\n");
      return ret;
  }
  
  LOG("VTFS joined the kernel\n");
  return 0;
}

static void __exit vtfs_exit(void) {
  unregister_filesystem(&vtfs_fs_type);
  LOG("VTFS left the kernel\n");
}

module_init(vtfs_init);
module_exit(vtfs_exit);
