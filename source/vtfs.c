#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/mount.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/slab.h>

#define MODULE_NAME "vtfs"
MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("VTFS - simple RAM FS");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

#define MAX_FILES 16
#define MAX_FILENAME 32
#define MAX_FILE_SIZE 4096

struct vtfs_file {
    char name[MAX_FILENAME];
    char data[MAX_FILE_SIZE];
    size_t size;
    umode_t mode;
    ino_t ino;
    int used;
};

static struct vtfs_file vtfs_files[MAX_FILES];
static ino_t next_ino = 101;

// Forward declarations
struct dentry* vtfs_mount(struct file_system_type* fs_type, int flags, const char* token, void* data);
void vtfs_kill_sb(struct super_block* sb);
int vtfs_fill_super(struct super_block *sb, void *data, int silent);
struct inode* vtfs_get_inode(struct super_block* sb, struct inode* dir, umode_t mode, int i_ino);
struct dentry* vtfs_lookup(struct inode* parent_inode, struct dentry* child_dentry, unsigned int flag);
int vtfs_iterate(struct file* filp, struct dir_context* ctx);
int vtfs_create(struct mnt_idmap *idmap, struct inode *parent_inode, struct dentry *child_dentry, umode_t mode, bool excl);
int vtfs_unlink(struct inode *parent_inode, struct dentry *child_dentry);

static struct vtfs_file* vtfs_find_file(const char *name) {
    int i;
    for (i = 0; i < MAX_FILES; i++) {
        if (vtfs_files[i].used && !strcmp(vtfs_files[i].name, name))
            return &vtfs_files[i];
    }
    return NULL;
}

// -------------------- INODE / DENTRY --------------------

struct inode_operations vtfs_inode_ops = {
    .lookup = vtfs_lookup,
    .create = vtfs_create,
    .unlink = vtfs_unlink
};

struct file_operations vtfs_dir_ops = {
    .iterate_shared = vtfs_iterate,
};

// -------------------- FILESYSTEM --------------------

struct dentry* vtfs_mount(struct file_system_type* fs_type, int flags, const char* token, void* data) {
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
    struct inode *inode = new_inode(sb);
    if (!inode) return NULL;

    inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
    inode->i_ino = i_ino;

    return inode;
}

// -------------------- LOOKUP --------------------

struct dentry* vtfs_lookup(struct inode* parent_inode, struct dentry* child_dentry, unsigned int flag) {
    if (parent_inode->i_ino != 100) // только корень
        return NULL;

    const char *name = child_dentry->d_name.name;
    struct vtfs_file *file = vtfs_find_file(name);
    if (!file)
        return NULL;

    struct inode *inode = vtfs_get_inode(parent_inode->i_sb, parent_inode, S_IFREG | S_IRWXUGO, file->ino);
    inode->i_op = &vtfs_inode_ops;
    inode->i_fop = NULL;

    d_add(child_dentry, inode);
    return NULL;
}

// -------------------- CREATE --------------------

int vtfs_create(struct mnt_idmap *idmap, struct inode *parent_inode, struct dentry *child_dentry, umode_t mode, bool excl) {
    if (parent_inode->i_ino != 100)
        return -EPERM;

    const char *name = child_dentry->d_name.name;
    int i;
    for (i = 0; i < MAX_FILES; i++) {
        if (!vtfs_files[i].used) break;
    }
    if (i == MAX_FILES)
        return -ENOSPC;

    vtfs_files[i].used = 1;
    strncpy(vtfs_files[i].name, name, MAX_FILENAME-1);
    vtfs_files[i].name[MAX_FILENAME-1] = 0;
    vtfs_files[i].size = 0;
    vtfs_files[i].mode = mode;
    vtfs_files[i].ino = next_ino++;

    struct inode *inode = vtfs_get_inode(parent_inode->i_sb, parent_inode, S_IFREG | mode, vtfs_files[i].ino);
    inode->i_op = &vtfs_inode_ops;
    inode->i_fop = NULL;

    d_add(child_dentry, inode);
    return 0;
}

// -------------------- UNLINK --------------------

int vtfs_unlink(struct inode *parent_inode, struct dentry *child_dentry) {
    if (parent_inode->i_ino != 100)
        return -EPERM;

    struct vtfs_file *file = vtfs_find_file(child_dentry->d_name.name);
    if (!file)
        return -ENOENT;

    file->used = 0;
    return 0;
}

// -------------------- ITERATE --------------------

int vtfs_iterate(struct file *filp, struct dir_context *ctx) {
    struct dentry *dentry = filp->f_path.dentry;
    struct inode *inode = dentry->d_inode;
    int pos = 0;

    if (inode->i_ino != 100)
        return 0;

    // "." 
    if (ctx->pos == pos) {
        if (!dir_emit(ctx, ".", 1, inode->i_ino, DT_DIR)) return 0;
        ctx->pos++;
    }
    pos++;

    // ".."
    if (ctx->pos == pos) {
        ino_t parent_ino = inode->i_ino;
        if (dentry->d_parent && dentry->d_parent->d_inode)
            parent_ino = dentry->d_parent->d_inode->i_ino;
        if (!dir_emit(ctx, "..", 2, parent_ino, DT_DIR)) return 0;
        ctx->pos++;
    }
    pos++;

    // файлы
    int i;
    for (i = 0; i < MAX_FILES; i++) {
        if (vtfs_files[i].used) {
            if (ctx->pos == pos) {
                if (!dir_emit(ctx, vtfs_files[i].name,
                              strlen(vtfs_files[i].name),
                              vtfs_files[i].ino,
                              DT_REG)) return 0;
                ctx->pos++;
            }
            pos++;
        }
    }

    return 0;
}

// -------------------- FILL SUPER --------------------

int vtfs_fill_super(struct super_block *sb, void *data, int silent) {
    struct inode* root = vtfs_get_inode(sb, NULL, S_IFDIR | 0777, 100);
    root->i_op = &vtfs_inode_ops;
    root->i_fop = &vtfs_dir_ops;

    sb->s_root = d_make_root(root);
    if (!sb->s_root)
        return -ENOMEM;

    LOG("Superblock filled\n");
    return 0;
}

// -------------------- FILESYSTEM TYPE --------------------

struct file_system_type vtfs_fs_type = {
    .name = "vtfs",
    .mount = vtfs_mount,
    .kill_sb = vtfs_kill_sb,
};

// -------------------- MODULE INIT/EXIT --------------------

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
