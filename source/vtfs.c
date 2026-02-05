#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mount.h>
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

struct vtfs_inode {
  ino_t ino;
  char data[MAX_FILE_SIZE];
  size_t size;
  umode_t mode;
  int nlink;
  int used;
  enum vtfs_type type;
};

struct vtfs_file {
  char name[MAX_FILENAME];
  ino_t ino;
  ino_t parent_ino;
  int used;
};

static struct vtfs_inode vtfs_inodes[MAX_FILES];
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

static struct vtfs_inode* vtfs_find_inode(ino_t ino) {
  for (int i = 0; i < MAX_FILES; i++) {
    if (vtfs_inodes[i].used && vtfs_inodes[i].ino == ino)
      return &vtfs_inodes[i];
  }
  return NULL;
}

static struct vtfs_file* vtfs_find_file(const char* name, ino_t parent_ino) {
  for (int i = 0; i < MAX_FILES; i++) {
    if (vtfs_files[i].used && vtfs_files[i].parent_ino == parent_ino &&
        !strcmp(vtfs_files[i].name, name))
      return &vtfs_files[i];
  }
  return NULL;
}

struct inode_operations vtfs_inode_ops;
struct file_operations vtfs_file_ops;
struct file_operations vtfs_dir_ops;

struct dentry* vtfs_mount(
    struct file_system_type* fs_type, int flags, const char* token, void* data
) {
  struct dentry* ret = mount_nodev(fs_type, flags, data, NULL);
  if (!ret)
    printk(KERN_ERR "[vtfs] Can't mount FS\n");
  else
    printk(KERN_INFO "[vtfs] Mounted\n");
  return ret;
}

void vtfs_kill_sb(struct super_block* sb) {
  printk(KERN_INFO "[vtfs] Superblock destroyed\n");
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
  struct vtfs_file* f = vtfs_find_file(child_dentry->d_name.name, parent_inode->i_ino);
  if (!f)
    return NULL;

  struct vtfs_inode* inode_file = vtfs_find_inode(f->ino);
  if (!inode_file)
    return NULL;

  umode_t mode = (inode_file->type == VTFS_DIR) ? (S_IFDIR | (inode_file->mode & ~S_IFMT))
                                                : (S_IFREG | (inode_file->mode & ~S_IFMT));

  struct inode* inode = vtfs_get_inode(parent_inode->i_sb, parent_inode, mode, inode_file->ino);
  if (!inode)
    return NULL;

  if (inode_file->type == VTFS_DIR) {
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
  int i;
  struct vtfs_inode* inode_entry = NULL;
  struct vtfs_file* dentry_entry = NULL;

  for (i = 0; i < MAX_FILES; i++) {
    if (!vtfs_inodes[i].used) {
      inode_entry = &vtfs_inodes[i];
      break;
    }
  }
  if (!inode_entry)
    return -ENOSPC;

  for (i = 0; i < MAX_FILES; i++) {
    if (!vtfs_files[i].used) {
      dentry_entry = &vtfs_files[i];
      break;
    }
  }
  if (!dentry_entry)
    return -ENOSPC;

  inode_entry->used = 1;
  inode_entry->ino = next_ino++;
  inode_entry->size = 0;
  inode_entry->mode = mode;
  inode_entry->type = VTFS_FILE;
  inode_entry->nlink = 1;

  dentry_entry->used = 1;
  strncpy(dentry_entry->name, child_dentry->d_name.name, MAX_FILENAME - 1);
  dentry_entry->name[MAX_FILENAME - 1] = 0;
  dentry_entry->ino = inode_entry->ino;
  dentry_entry->parent_ino = parent_inode->i_ino;

  struct inode* inode =
      vtfs_get_inode(parent_inode->i_sb, parent_inode, S_IFREG | mode, inode_entry->ino);
  inode->i_op = &vtfs_inode_ops;
  inode->i_fop = &vtfs_file_ops;
  set_nlink(inode, 1);

  d_add(child_dentry, inode);
  return 0;
}

int vtfs_unlink(struct inode* parent_inode, struct dentry* child_dentry) {
  struct vtfs_file* f = vtfs_find_file(child_dentry->d_name.name, parent_inode->i_ino);
  if (!f)
    return -ENOENT;

  struct vtfs_inode* inode_file = vtfs_find_inode(f->ino);
  if (!inode_file)
    return -EIO;

  f->used = 0;
  inode_file->nlink--;
  drop_nlink(child_dentry->d_inode);

  if (inode_file->nlink == 0)
    inode_file->used = 0;
  return 0;
}

int vtfs_link(struct dentry* old_dentry, struct inode* parent_dir, struct dentry* new_dentry) {
  struct vtfs_inode* inode_file = vtfs_find_inode(old_dentry->d_inode->i_ino);
  if (!inode_file)
    return -ENOENT;

  int i;
  struct vtfs_file* dentry_entry = NULL;
  for (i = 0; i < MAX_FILES; i++)
    if (!vtfs_files[i].used) {
      dentry_entry = &vtfs_files[i];
      break;
    }
  if (!dentry_entry)
    return -ENOSPC;

  dentry_entry->used = 1;
  strncpy(dentry_entry->name, new_dentry->d_name.name, MAX_FILENAME - 1);
  dentry_entry->name[MAX_FILENAME - 1] = 0;
  dentry_entry->ino = inode_file->ino;
  dentry_entry->parent_ino = parent_dir->i_ino;

  inode_file->nlink++;
  inc_nlink(old_dentry->d_inode);

  d_add(new_dentry, old_dentry->d_inode);
  return 0;
}

ssize_t vtfs_read(struct file* filp, char* buffer, size_t len, loff_t* offset) {
  struct vtfs_inode* inode_file = vtfs_find_inode(filp->f_inode->i_ino);
  if (!inode_file)
    return -ENOENT;
  if (inode_file->type != VTFS_FILE)
    return -EISDIR;

  if (*offset >= inode_file->size)
    return 0;

  size_t to_read = min(len, inode_file->size - *offset);
  if (copy_to_user(buffer, inode_file->data + *offset, to_read))
    return -EFAULT;
  *offset += to_read;
  return to_read;
}

ssize_t vtfs_write(struct file* filp, const char* buffer, size_t len, loff_t* offset) {
  struct vtfs_inode* inode_file = vtfs_find_inode(filp->f_inode->i_ino);
  if (!inode_file)
    return -ENOENT;
  if (inode_file->type != VTFS_FILE)
    return -EISDIR;

  if (*offset + len > MAX_FILE_SIZE)
    return -ENOSPC;
  if (copy_from_user(inode_file->data + *offset, buffer, len))
    return -EFAULT;

  size_t new_end = *offset + len;
  if (new_end > inode_file->size)
    inode_file->size = new_end;
  filp->f_inode->i_size = inode_file->size;

  *offset += len;
  return len;
}

int vtfs_iterate(struct file* filp, struct dir_context* ctx) {
  struct dentry* dentry = filp->f_path.dentry;
  ino_t current_dir_ino = dentry->d_inode->i_ino;
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

  for (int i = 0; i < MAX_FILES; i++) {
    if (vtfs_files[i].used && vtfs_files[i].parent_ino == current_dir_ino) {
      if (ctx->pos == pos) {
        struct vtfs_inode* inode_file = vtfs_find_inode(vtfs_files[i].ino);
        if (!inode_file)
          continue;

        unsigned char d_type = (inode_file->type == VTFS_DIR) ? DT_DIR : DT_REG;
        if (!dir_emit(
                ctx, vtfs_files[i].name, strlen(vtfs_files[i].name), inode_file->ino, d_type
            )) {
          return 0;
        }
        ctx->pos++;
      }
      pos++;
    }
  }

  return 0;
}

struct file_operations vtfs_file_ops = {.read = vtfs_read, .write = vtfs_write};
struct file_operations vtfs_dir_ops = {.iterate_shared = vtfs_iterate};
struct inode_operations vtfs_inode_ops = {
    .lookup = vtfs_lookup, .create = vtfs_create, .unlink = vtfs_unlink, .link = vtfs_link
};

struct file_system_type vtfs_fs_type = {
    .name = "vtfs", .mount = vtfs_mount, .kill_sb = vtfs_kill_sb
};

static int __init vtfs_init(void) {
  int ret = register_filesystem(&vtfs_fs_type);
  if (ret) {
    pr_err("[vtfs] failed to register FS\n");
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
