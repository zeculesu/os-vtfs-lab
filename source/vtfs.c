#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#define MODULE_NAME "vtfs"
MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("VTFS - simple RAM FS");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

#define MAX_FILES 16
#define MAX_FILENAME 32
#define MAX_FILE_SIZE 4096

enum vtfs_type { VTFS_FILE, VTFS_DIR };

struct vtfs_inode_data {
  char data[MAX_FILE_SIZE];
  size_t size;
  int link_count;
};

struct vtfs_file {
  char name[MAX_FILENAME];
  umode_t mode;
  ino_t ino;
  int used;
  enum vtfs_type type;
  ino_t parent_ino;
  struct vtfs_inode_data* idata;
};

static struct vtfs_file vtfs_files[MAX_FILES];
static ino_t next_ino = 101;

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

static struct vtfs_file* vtfs_find_primary_file(ino_t ino) {
  for (int i = 0; i < MAX_FILES; i++) {
    if (vtfs_files[i].used && vtfs_files[i].ino == ino) {
      return &vtfs_files[i];
    }
  }
  return NULL;
}

static struct vtfs_file* vtfs_find_file_by_ino(ino_t ino) {
  for (int i = 0; i < MAX_FILES; i++) {
    if (vtfs_files[i].used && vtfs_files[i].ino == ino)
      return &vtfs_files[i];
  }
  return NULL;
}
static struct vtfs_file* vtfs_find_file(const char* name, ino_t parent_ino) {
  int i;
  for (i = 0; i < MAX_FILES; i++) {
    if (vtfs_files[i].used && vtfs_files[i].parent_ino == parent_ino &&
        !strcmp(vtfs_files[i].name, name))
      return &vtfs_files[i];
  }
  return NULL;
}

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

struct dentry* vtfs_lookup(
    struct inode* parent_inode, struct dentry* child_dentry, unsigned int flag
) {
  const char* name = child_dentry->d_name.name;
  struct vtfs_file* file = vtfs_find_file(name, parent_inode->i_ino);
  if (!file || file->parent_ino != parent_inode->i_ino)
    return NULL;

  umode_t mode;
  if (file->type == VTFS_DIR) {
    mode = S_IFDIR | (file->mode & ~S_IFMT);
  } else {
    mode = S_IFREG | (file->mode & ~S_IFMT);
  }

  struct inode* inode = vtfs_get_inode(parent_inode->i_sb, parent_inode, mode, file->ino);

  if (!inode)
    return NULL;

  if (file->type == VTFS_DIR) {
    inode->i_op = &vtfs_inode_ops;
    inode->i_fop = &vtfs_dir_ops;
  } else {
    inode->i_op = &vtfs_inode_ops;
    inode->i_fop = &vtfs_file_ops;
  }

  d_add(child_dentry, inode);
  return NULL;
}

int vtfs_create(
    struct mnt_idmap* idmap,
    struct inode* parent_inode,
    struct dentry* child_dentry,
    umode_t mode,
    bool excl
) {
  const char* name = child_dentry->d_name.name;
  int i;
  for (i = 0; i < MAX_FILES; i++)
    if (!vtfs_files[i].used)
      break;
  if (i == MAX_FILES)
    return -ENOSPC;

  vtfs_files[i].used = 1;
  strncpy(vtfs_files[i].name, name, MAX_FILENAME - 1);
  vtfs_files[i].name[MAX_FILENAME - 1] = 0;
  vtfs_files[i].mode = mode;
  vtfs_files[i].ino = next_ino++;
  vtfs_files[i].parent_ino = parent_inode->i_ino;
  vtfs_files[i].type = VTFS_FILE;

  vtfs_files[i].idata = kzalloc(sizeof(struct vtfs_inode_data), GFP_KERNEL);
  if (!vtfs_files[i].idata) {
    vtfs_files[i].used = 0;
    return -ENOMEM;
  }
  vtfs_files[i].idata->size = 0;
  vtfs_files[i].idata->link_count = 1;

  struct inode* inode =
      vtfs_get_inode(parent_inode->i_sb, parent_inode, S_IFREG | mode, vtfs_files[i].ino);
  inode->i_op = &vtfs_inode_ops;
  inode->i_fop = &vtfs_file_ops;
  d_add(child_dentry, inode);
  return 0;
}

int vtfs_unlink(struct inode* parent_inode, struct dentry* child_dentry) {
  struct vtfs_file* f = vtfs_find_file(child_dentry->d_name.name, parent_inode->i_ino);
  if (!f)
    return -ENOENT;

  if (f->type == VTFS_FILE) {
    f->idata->link_count--;
    if (f->idata->link_count == 0) {
      kfree(f->idata);
    }
  }
  f->used = 0;
  return 0;
}

int vtfs_mkdir(
    struct mnt_idmap*, struct inode* parent_inode, struct dentry* child_dentry, umode_t mode
) {
  const char* name = child_dentry->d_name.name;
  if (vtfs_find_file(name, parent_inode->i_ino))
    return -EEXIST;

  int i;
  for (i = 0; i < MAX_FILES; i++) {
    if (!vtfs_files[i].used)
      break;
  }
  if (i == MAX_FILES)
    return -ENOSPC;

  vtfs_files[i].used = 1;
  strncpy(vtfs_files[i].name, name, MAX_FILENAME - 1);
  vtfs_files[i].name[MAX_FILENAME - 1] = 0;
  if (vtfs_files[i].idata)
    vtfs_files[i].idata->size = 0;
  vtfs_files[i].mode = mode | S_IFDIR;
  vtfs_files[i].ino = next_ino++;
  vtfs_files[i].type = VTFS_DIR;
  vtfs_files[i].parent_ino = parent_inode->i_ino;

  struct inode* inode =
      vtfs_get_inode(parent_inode->i_sb, parent_inode, S_IFDIR | mode, vtfs_files[i].ino);
  if (!inode)
    return -ENOMEM;

  inode->i_op = &vtfs_inode_ops;
  inode->i_fop = &vtfs_dir_ops;

  d_add(child_dentry, inode);
  return 0;
}

int vtfs_rmdir(struct inode* parent_inode, struct dentry* child_dentry) {
  struct vtfs_file* dir = vtfs_find_file(child_dentry->d_name.name, parent_inode->i_ino);
  if (!dir)
    return -ENOENT;
  if (dir->type != VTFS_DIR)
    return -ENOTDIR;
  int i;
  for (i = 0; i < MAX_FILES; i++) {
    if (vtfs_files[i].used && vtfs_files[i].parent_ino == dir->ino) {
      return -ENOTEMPTY;
    }
  }

  dir->used = 0;
  return 0;
}

ssize_t vtfs_read(struct file* filp, char* buffer, size_t len, loff_t* offset) {
  struct vtfs_file* f = vtfs_find_primary_file(filp->f_inode->i_ino);
  if (!f)
    return -ENOENT;
  if (f->type != VTFS_FILE)
    return -EISDIR;

  if (*offset >= f->idata->size)
    return 0;

  size_t to_read = min(len, f->idata->size - *offset);
  if (copy_to_user(buffer, f->idata->data + *offset, to_read))
    return -EFAULT;

  *offset += to_read;
  return to_read;
}

ssize_t vtfs_write(struct file* filp, const char* buffer, size_t len, loff_t* offset) {
  struct vtfs_file* f = vtfs_find_primary_file(filp->f_inode->i_ino);
  if (!f)
    return -ENOENT;
  if (f->type != VTFS_FILE)
    return -EISDIR;

  if (filp->f_flags & O_TRUNC) {
    f->idata->size = 0;
    *offset = 0;
  }

  if (*offset + len > MAX_FILE_SIZE)
    return -ENOSPC;
  if (copy_from_user(f->idata->data + *offset, buffer, len))
    return -EFAULT;

  size_t new_end = *offset + len;
  if (new_end > f->idata->size)
    f->idata->size = new_end;
  filp->f_inode->i_size = f->idata->size;

  *offset += len;
  return len;
}

int vtfs_link(struct dentry* old_dentry, struct inode* parent_dir, struct dentry* new_dentry) {
  struct inode* old_inode = old_dentry->d_inode;
  struct vtfs_file* old_file = vtfs_find_file_by_ino(old_inode->i_ino);
  if (!old_file)
    return -ENOENT;
  if (old_file->type == VTFS_DIR)
    return -EPERM;

  const char* new_name = new_dentry->d_name.name;
  if (vtfs_find_file(new_name, parent_dir->i_ino))
    return -EEXIST;

  int i;
  for (i = 0; i < MAX_FILES; i++)
    if (!vtfs_files[i].used)
      break;
  if (i == MAX_FILES)
    return -ENOSPC;

  vtfs_files[i].used = 1;
  strncpy(vtfs_files[i].name, new_name, MAX_FILENAME - 1);
  vtfs_files[i].name[MAX_FILENAME - 1] = 0;
  vtfs_files[i].ino = old_file->ino;
  vtfs_files[i].parent_ino = parent_dir->i_ino;
  vtfs_files[i].type = VTFS_FILE;
  vtfs_files[i].mode = old_file->mode;
  vtfs_files[i].idata = old_file->idata;

  vtfs_files[i].idata->link_count++;

  struct inode* new_inode =
      vtfs_get_inode(parent_dir->i_sb, parent_dir, old_file->mode, old_file->ino);
  if (!new_inode)
    return -ENOMEM;

  inc_nlink(new_inode);
  new_inode->i_op = &vtfs_inode_ops;
  new_inode->i_fop = &vtfs_file_ops;
  d_add(new_dentry, new_inode);

  return 0;
}

int vtfs_iterate(struct file* filp, struct dir_context* ctx) {
  struct dentry* dentry = filp->f_path.dentry;
  struct inode* inode = dentry->d_inode;
  ino_t current_dir_ino = inode->i_ino;
  int pos = 0;

  if (ctx->pos == pos) {
    if (!dir_emit(ctx, ".", 1, current_dir_ino, DT_DIR))
      return 0;
    ctx->pos++;
  }
  pos++;

  if (ctx->pos == pos) {
    ino_t parent_ino = current_dir_ino;
    if (dentry->d_parent && dentry->d_parent->d_inode)
      parent_ino = dentry->d_parent->d_inode->i_ino;
    if (!dir_emit(ctx, "..", 2, parent_ino, DT_DIR))
      return 0;
    ctx->pos++;
  }
  pos++;

  int i;
  for (i = 0; i < MAX_FILES; i++) {
    if (vtfs_files[i].used && vtfs_files[i].parent_ino == current_dir_ino) {
      if (ctx->pos == pos) {
        unsigned char d_type;
        if (vtfs_files[i].type == VTFS_DIR) {
          d_type = DT_DIR;
        } else {
          d_type = DT_REG;
        }
        if (!dir_emit(
                ctx, vtfs_files[i].name, strlen(vtfs_files[i].name), vtfs_files[i].ino, d_type
            ))
          return 0;
        ctx->pos++;
      }
      pos++;
    }
  }

  return 0;
}

int vtfs_fill_super(struct super_block* sb, void* data, int silent) {
  struct inode* root = vtfs_get_inode(sb, NULL, S_IFDIR | 0777, 100);
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