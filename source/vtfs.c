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

#define VTFS_TOKEN "vtfs"

#define MODULE_NAME "vtfs"
MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("VTFS - simple RAM FS");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char b64_pad = '=';

static void itostr(char *buf, size_t size, long val) {
    if (!buf || size == 0) return;

    char tmp[32];
    int i = 0;
    int is_negative = 0;

    if (val < 0) {
        is_negative = 1;
        val = -val;
    }

    // преобразуем число в обратном порядке
    do {
        tmp[i++] = '0' + (val % 10);
        val /= 10;
    } while (val > 0 && i < (int)sizeof(tmp));

    if (is_negative)
        tmp[i++] = '-';

    // копируем в buf в правильном порядке
    int j = 0;
    while (i > 0 && j < (int)(size - 1)) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
}

static int b64encode(const char *in, size_t inlen, char *out, size_t outlen) {
    size_t i = 0, j = 0;
    unsigned char a3[3];
    unsigned char a4[4];

    while (i < inlen) {
        size_t k;
        for (k = 0; k < 3; k++) {
            if (i < inlen)
                a3[k] = in[i++];
            else
                a3[k] = 0;
        }

        a4[0] = (a3[0] & 0xfc) >> 2;
        a4[1] = ((a3[0] & 0x03) << 4) | ((a3[1] & 0xf0) >> 4);
        a4[2] = ((a3[1] & 0x0f) << 2) | ((a3[2] & 0xc0) >> 6);
        a4[3] = a3[2] & 0x3f;

        int pad = (i > inlen) ? (3 - (inlen % 3)) : 0;

        int l;
        for (l = 0; l < 4 - pad; l++) {
            if (j >= outlen) return -ENOSPC;
            out[j++] = b64_table[a4[l]];
        }
        for (; l < 4; l++) {
            if (j >= outlen) return -ENOSPC;
            out[j++] = b64_pad;
        }
    }

    if (j >= outlen) return -ENOSPC;
    out[j] = '\0';
    return j;
}

static int b64decode(const char *in, char *out, size_t outlen) {
    int decoding_table[256];
    int i, j;
    for (i = 0; i < 256; i++) decoding_table[i] = -1;
    for (i = 0; i < 64; i++) decoding_table[(unsigned char)b64_table[i]] = i;

    size_t inlen = strlen(in);
    size_t pos = 0;
    unsigned char a3[3], a4[4];

    for (i = 0; i < inlen; i += 4) {
        int l;
        for (l = 0; l < 4; l++) {
            if (i + l < inlen)
                a4[l] = decoding_table[(unsigned char)in[i + l]];
            else
                a4[l] = 0;
        }

        a3[0] = (a4[0] << 2) | ((a4[1] & 0x30) >> 4);
        a3[1] = ((a4[1] & 0x0f) << 4) | ((a4[2] & 0x3c) >> 2);
        a3[2] = ((a4[2] & 0x03) << 6) | a4[3];

        int out_bytes = 3;
        if (in[i + 2] == b64_pad) out_bytes = 1;
        else if (in[i + 3] == b64_pad) out_bytes = 2;

        for (j = 0; j < out_bytes; j++) {
            if (pos >= outlen) return -ENOSPC;
            out[pos++] = a3[j];
        }
    }

    return pos;
}

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

struct dentry* vtfs_lookup(
    struct inode* parent_inode, struct dentry* child_dentry, unsigned int flag
) {
  char ino_str[32], resp[256], name[512];
  itostr(ino_str, sizeof(ino_str), parent_inode->i_ino);
  // encode(child_dentry->d_name.name, name);

  int64_t ret = vtfs_http_call(
      VTFS_TOKEN,
      "lookup",
      resp,
      sizeof(resp),
      2,
      "parent_ino",
      ino_str,
      "name",
      child_dentry->d_name.name
  );
  if (ret < 0)
    return NULL;

  if (ret != 0)
    return NULL;

  unsigned long ino;
  char type[16];
  umode_t mode;
  sscanf(resp, "%lu %15s %o", &ino, type, &mode);

  struct inode* inode = new_inode(parent_inode->i_sb);
  if (!inode)
    return NULL;

  inode->i_ino = ino;
  if (strcmp(type, "dir") == 0) {
    inode->i_op = &vtfs_inode_ops;
    inode->i_fop = &vtfs_dir_ops;
  } else {
    inode->i_op = &vtfs_inode_ops;
    inode->i_fop = &vtfs_file_ops;
  }

  d_add(child_dentry, inode);
  return child_dentry;
}

int vtfs_create(
    struct mnt_idmap* idmap,
    struct inode* parent_inode,
    struct dentry* child_dentry,
    umode_t mode,
    bool excl
) {
  char ino_str[32], mode_str[32], resp[64];
  itostr(ino_str, sizeof(ino_str), parent_inode->i_ino);
  itostr(mode_str, sizeof(mode_str), mode);

  int64_t ret = vtfs_http_call(
      VTFS_TOKEN,
      "create",
      resp,
      sizeof(resp),
      3,
      "parent_ino",
      ino_str,
      "name",
      child_dentry->d_name.name,
      "mode",
      mode_str
  );
  if (ret < 0)
    return ret;

  unsigned long ino = simple_strtoul(resp, NULL, 10);
  struct inode* inode = new_inode(parent_inode->i_sb);
  if (!inode)
    return -ENOMEM;

  inode->i_ino = ino;
  inode->i_op = &vtfs_inode_ops;
  inode->i_fop = &vtfs_file_ops;

  d_add(child_dentry, inode);
  return 0;
}

int vtfs_unlink(struct inode* parent_inode, struct dentry* child_dentry) {
  char ino_str[32], resp[32];
  if (!child_dentry->d_inode)
    return -ENOENT;

  itostr(ino_str, sizeof(ino_str), child_dentry->d_inode->i_ino);
  int64_t ret = vtfs_http_call(VTFS_TOKEN, "unlink", resp, sizeof(resp), 1, "ino", ino_str);
  if (ret < 0)
    return ret;
  return 0;
}

int vtfs_mkdir(
    struct mnt_idmap* idmap, struct inode* parent_inode, struct dentry* child_dentry, umode_t mode
) {
  char ino_str[32], mode_str[32], resp[64];
  itostr(ino_str, sizeof(ino_str), parent_inode->i_ino);
  itostr(mode_str, sizeof(mode_str), mode);

  int64_t ret = vtfs_http_call(
      VTFS_TOKEN,
      "mkdir",
      resp,
      sizeof(resp),
      3,
      "parent_ino",
      ino_str,
      "name",
      child_dentry->d_name.name,
      "mode",
      mode_str
  );
  if (ret < 0)
    return ret;

  unsigned long ino = simple_strtoul(resp, NULL, 10);
  struct inode* inode = new_inode(parent_inode->i_sb);
  if (!inode)
    return -ENOMEM;

  inode->i_ino = ino;
  inode->i_op = &vtfs_inode_ops;
  inode->i_fop = &vtfs_dir_ops;

  d_add(child_dentry, inode);
  return 0;
}

int vtfs_rmdir(struct inode* parent_inode, struct dentry* child_dentry) {
  char ino_str[32], resp[32];
  if (!child_dentry->d_inode)
    return -ENOENT;

  itostr(ino_str, sizeof(ino_str), child_dentry->d_inode->i_ino);
  int64_t ret = vtfs_http_call(VTFS_TOKEN, "rmdir", resp, sizeof(resp), 1, "ino", ino_str);
  if (ret < 0)
    return ret;
  if (strcmp(resp, "ENOTEMPTY") == 0)
    return -ENOTEMPTY;
  return 0;
}

ssize_t vtfs_read(struct file* filp, char* buffer, size_t len, loff_t* offset) {
    char ino_str[32], off_str[32], len_str[32], resp[8192];
    itostr(ino_str, sizeof(ino_str), filp->f_inode->i_ino);
    itostr(off_str, sizeof(off_str), *offset);
    itostr(len_str, sizeof(len_str), len);

    int64_t ret = vtfs_http_call(VTFS_TOKEN, "read", resp, sizeof(resp),
                                 3, "ino", ino_str, "offset", off_str, "length", len_str);
    if (ret < 0) return ret;

    int decoded = b64decode(resp, buffer, len);
    *offset += decoded;
    return decoded;
}

ssize_t vtfs_write(struct file* filp, const char* buffer, size_t len, loff_t* offset) {
    char ino_str[32], off_str[32], data_b64[8192], resp[32];
    itostr(ino_str, sizeof(ino_str), filp->f_inode->i_ino);
    itostr(off_str, sizeof(off_str), *offset);

    int b64len = b64encode(buffer, len, data_b64, sizeof(data_b64));
    if (b64len < 0) return -ENOSPC;

    int64_t ret = vtfs_http_call(VTFS_TOKEN, "write", resp, sizeof(resp),
                                 3, "ino", ino_str, "offset", off_str, "data", data_b64);
    if (ret < 0) return ret;

    *offset += len;
    filp->f_inode->i_size += len;
    return len;
}

int vtfs_link(struct dentry* old_dentry, struct inode* parent_dir, struct dentry* new_dentry) {
  char ino_str[32], parent_str[32], resp[32];
  itostr(ino_str, sizeof(ino_str), old_dentry->d_inode->i_ino);
  itostr(parent_str, sizeof(parent_str), parent_dir->i_ino);

  int64_t ret = vtfs_http_call(
      VTFS_TOKEN,
      "link",
      resp,
      sizeof(resp),
      3,
      "ino",
      ino_str,
      "parent_ino",
      parent_str,
      "name",
      new_dentry->d_name.name
  );
  if (ret < 0)
    return ret;

  struct inode* inode = new_inode(parent_dir->i_sb);
  if (!inode)
    return -ENOMEM;
  inode->i_ino = simple_strtoul(resp, NULL, 10);
  inode->i_op = &vtfs_inode_ops;
  inode->i_fop = &vtfs_file_ops;

  d_add(new_dentry, inode);
  return 0;
}

int vtfs_iterate(struct file* filp, struct dir_context* ctx) {
    char ino_str[32], resp[8192];
    itostr(ino_str, sizeof(ino_str), filp->f_inode->i_ino);

    int64_t ret = vtfs_http_call(VTFS_TOKEN, "list", resp, sizeof(resp),
                                 1, "parent_ino", ino_str);
    if (ret < 0)
        return ret;

    char* line = resp;
    char* next;
    int idx = 0;

    if (ctx->pos == 0) {
        if (!dir_emit(ctx, ".", 1, filp->f_inode->i_ino, DT_DIR))
            return 0;
        ctx->pos++;
    }
    if (ctx->pos == 1) {
        ino_t parent_ino = filp->f_inode->i_ino;
        if (filp->f_path.dentry->d_parent && filp->f_path.dentry->d_parent->d_inode)
            parent_ino = filp->f_path.dentry->d_parent->d_inode->i_ino;
        if (!dir_emit(ctx, "..", 2, parent_ino, DT_DIR))
            return 0;
        ctx->pos++;
    }

    idx = 2;

    while (line && *line) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;

        if (ctx->pos == idx) {
            unsigned long ino;
            char type[16];
            umode_t mode;
            char name[256];

            if (sscanf(line, "%lu %15s %o %255[^\n]", &ino, type, &mode, name) != 4)
                return -EIO;

            unsigned char d_type = (strcmp(type, "dir") == 0) ? DT_DIR : DT_REG;

            if (!dir_emit(ctx, name, strlen(name), ino, d_type))
                return 0;

            ctx->pos++; 
        }

        idx++;
        line = next;
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