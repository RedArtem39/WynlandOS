/*
 * WynlandOS - Package Manager (wynpkg) Implementation
 *
 * Phase 6: Package manager — format parsing, local DB, dependency resolution,
 *          install/remove/list/update operations.
 */

#include <wynland/wynpkg.h>
#include <wynland/vfs.h>
#include <wynland/net.h>
#include <wynland/http.h>
#include <wynland/types.h>
#include <wynland/boot_info.h>

/* ============================================================
 * Heap memory allocator externs (needed for headers)
 * ============================================================ */
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);
extern void  console_print_string(BootInfo *info, const char *str, uint32_t fg, uint32_t bg);

/* ============================================================
 * Static structures representing the custom .wpkg archive format
 * ============================================================ */

#define WPKG_MAGIC "WPKG"

typedef struct {
    char magic[4];          /* "WPKG" */
    char name[64];          /* Package name */
    char version[16];       /* Package version */
    char description[128];  /* Package description */
    char depends[128];      /* Dependencies, comma-separated */
    uint32_t num_files;     /* Number of files in the package */
} __attribute__((packed)) WpkgHeader;

typedef struct {
    char path[128];         /* Target path on disk, e.g. "/bin/hello" */
    uint32_t size;          /* File size */
    uint32_t offset;        /* Byte offset of file data in archive */
} __attribute__((packed)) WpkgFileHeader;

/* ============================================================
 * Local helper string functions (avoiding libc dependencies)
 * ============================================================ */

static uint32_t str_len_local(const char *s)
{
    uint32_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

static int str_compare_local(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static bool str_starts_with_local(const char *str, const char *prefix)
{
    while (*prefix) {
        if (*str != *prefix) {
            return false;
        }
        str++;
        prefix++;
    }
    return true;
}

static char *str_copy_local(char *dst, const char *src)
{
    char *orig = dst;
    while ((*dst++ = *src++))
        ;
    return orig;
}

/* ============================================================
 * Helper: ensure all parent directories exist for a file path
 * e.g., if path is "/bin/subdir/hello", creates "/bin" and "/bin/subdir"
 * ============================================================ */
static void ensure_parent_dirs(const char *path)
{
    char tmp[256];
    uint32_t len = str_len_local(path);
    if (len >= sizeof(tmp)) {
        return;
    }
    
    str_copy_local(tmp, path);
    
    for (uint32_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            vfs_mkdir(tmp);
            tmp[i] = '/';
        }
    }
}

/* ============================================================
 * Helper: Download a file over HTTP to local FAT32 disk
 * Saves to local_dest. Returns true on success.
 * ============================================================ */
extern void serial_write_string(const char *s);

static bool http_download(const char *path, const char *local_dest)
{
    /* Use REPO_HOST = 10.0.2.2 and REPO_PORT = 8000 by default */
    uint32_t ip = IP4(10, 0, 2, 2);
    static char http_resp_buf[32768];
    HttpResponse resp;
    
    int body_len = http_get_ip(ip, 8000, "10.0.2.2", path, http_resp_buf, sizeof(http_resp_buf), &resp);
    if (body_len < 0 || resp.status_code != 200) {
        serial_write_string("[wynpkg] http_download failed: http_get_ip failed or status != 200\r\n");
        return false;
    }
    
    VfsFile *f = vfs_open_flags(local_dest, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (!f) {
        serial_write_string("[wynpkg] http_download failed: failed to open destination file\r\n");
        return false;
    }
    
    int written = vfs_write(f, http_resp_buf, (uint32_t)body_len);
    vfs_close(f);
    
    if (written != body_len) {
        serial_write_string("[wynpkg] http_download failed: written != body_len\r\n");
    }
    
    return (written == body_len);
}

/* ============================================================
 * Helper: Look up package details in /sys/pkg/index.txt
 * Returns true if package is found.
 * ============================================================ */
static bool index_lookup(const char *pkg_name, char *out_version, char *out_desc, char *out_depends)
{
    static char index_buf[32768];
    VfsFile *f = vfs_open("/sys/pkg/index.txt");
    if (!f) {
        return false;
    }
    
    int len = vfs_read(f, index_buf, sizeof(index_buf) - 1);
    vfs_close(f);
    if (len <= 0) {
        return false;
    }
    
    index_buf[len] = '\0';
    
    char *line = index_buf;
    while (line && *line) {
        char *next_line = NULL;
        char *p = line;
        while (*p && *p != '\n' && *p != '\r') {
            p++;
        }
        if (*p) {
            if (*p == '\r' && *(p+1) == '\n') {
                *p = '\0';
                next_line = p + 2;
            } else {
                *p = '\0';
                next_line = p + 1;
            }
        }
        
        /* Expected format: name;version;description;depends */
        char *name = line;
        char *sem1 = NULL, *sem2 = NULL, *sem3 = NULL;
        
        p = line;
        while (*p) {
            if (*p == ';') {
                if (!sem1) sem1 = p;
                else if (!sem2) sem2 = p;
                else if (!sem3) sem3 = p;
            }
            p++;
        }
        
        if (sem1 && sem2) {
            *sem1 = '\0';
            *sem2 = '\0';
            if (sem3) {
                *sem3 = '\0';
            }
            
            if (str_compare_local(name, pkg_name) == 0) {
                if (out_version) str_copy_local(out_version, sem1 + 1);
                if (out_desc) str_copy_local(out_desc, sem2 + 1);
                if (out_depends) {
                    if (sem3) {
                        str_copy_local(out_depends, sem3 + 1);
                    } else {
                        out_depends[0] = '\0';
                    }
                }
                return true;
            }
        }
        
        line = next_line;
    }
    
    return false;
}

/* ============================================================
 * Helper: Local package installation (with recursive dependencies)
 * ============================================================ */
static bool install_package_internal(BootInfo *info, const char *name)
{
    /* 1. Check if already installed */
    char metadata_path[256];
    str_copy_local(metadata_path, "/sys/pkg/");
    str_copy_local(metadata_path + 9, name);
    str_copy_local(metadata_path + 9 + str_len_local(name), "/version");
    
    VfsStat st;
    if (vfs_stat(metadata_path, &st)) {
        console_print_string(info, "Package ", 0x00FFFF00, 0x000F0F1A);
        console_print_string(info, name, 0x00FFFFFF, 0x000F0F1A);
        console_print_string(info, " is already installed.\n", 0x00FFFF00, 0x000F0F1A);
        return true;
    }
    
    /* 2. Retrieve metadata and dependencies from local index */
    char version[32], desc[128], depends[128];
    if (!index_lookup(name, version, desc, depends)) {
        console_print_string(info, "Error: Package ", 0x00FF0000, 0x000F0F1A);
        console_print_string(info, name, 0x00FFFFFF, 0x000F0F1A);
        console_print_string(info, " not found in index. Run 'wynpkg update'.\n", 0x00FF0000, 0x000F0F1A);
        return false;
    }
    
    /* 3. Recursively resolve and install dependencies */
    if (str_len_local(depends) > 0) {
        console_print_string(info, "Resolving dependencies for ", 0x00CCCCCC, 0x000F0F1A);
        console_print_string(info, name, 0x00FFFFFF, 0x000F0F1A);
        console_print_string(info, ": ", 0x00CCCCCC, 0x000F0F1A);
        console_print_string(info, depends, 0x00FFFFFF, 0x000F0F1A);
        console_print_string(info, "\n", 0, 0x000F0F1A);
        
        char dep_list[128];
        str_copy_local(dep_list, depends);
        char *dep = dep_list;
        while (dep && *dep) {
            char *comma = dep;
            while (*comma && *comma != ',') {
                comma++;
            }
            if (*comma == ',') {
                *comma = '\0';
                comma++;
            } else {
                comma = NULL;
            }
            
            while (*dep == ' ') {
                dep++;
            }
            
            if (str_len_local(dep) > 0) {
                console_print_string(info, "Installing dependency: ", 0x00CCCCCC, 0x000F0F1A);
                console_print_string(info, dep, 0x00FFFFFF, 0x000F0F1A);
                console_print_string(info, "\n", 0, 0x000F0F1A);
                if (!install_package_internal(info, dep)) {
                    console_print_string(info, "Error: Failed to install dependency: ", 0x00FF0000, 0x000F0F1A);
                    console_print_string(info, dep, 0x00FFFFFF, 0x000F0F1A);
                    console_print_string(info, "\n", 0, 0x000F0F1A);
                    return false;
                }
            }
            
            dep = comma;
        }
    }
    
    /* 4. Download .wpkg archive */
    console_print_string(info, "Downloading package ", 0x00CCCCCC, 0x000F0F1A);
    console_print_string(info, name, 0x00FFFFFF, 0x000F0F1A);
    console_print_string(info, "...\n", 0x00CCCCCC, 0x000F0F1A);
    
    char url_path[256];
    str_copy_local(url_path, "/packages/");
    str_copy_local(url_path + 10, name);
    str_copy_local(url_path + 10 + str_len_local(name), ".wpk");
    
    vfs_mkdir("/tmp");
    char temp_wpkg[64];
    str_copy_local(temp_wpkg, "/tmp/install.wpk");
    
    if (!http_download(url_path, temp_wpkg)) {
        console_print_string(info, "Error: Failed to download package ", 0x00FF0000, 0x000F0F1A);
        console_print_string(info, name, 0x00FFFFFF, 0x000F0F1A);
        console_print_string(info, "\n", 0, 0x000F0F1A);
        return false;
    }
    
    /* 5. Open and extract package */
    VfsFile *pkg_f = vfs_open(temp_wpkg);
    if (!pkg_f) {
        console_print_string(info, "Error: Failed to open downloaded archive.\n", 0x00FF0000, 0x000F0F1A);
        return false;
    }
    
    WpkgHeader header;
    if (vfs_read(pkg_f, &header, sizeof(WpkgHeader)) != sizeof(WpkgHeader)) {
        console_print_string(info, "Error: Failed to read package header.\n", 0x00FF0000, 0x000F0F1A);
        vfs_close(pkg_f);
        return false;
    }
    
    if (header.magic[0] != 'W' || header.magic[1] != 'P' || header.magic[2] != 'K' || header.magic[3] != 'G') {
        console_print_string(info, "Error: Invalid archive magic header.\n", 0x00FF0000, 0x000F0F1A);
        vfs_close(pkg_f);
        return false;
    }
    
    /* Setup installed metadata directory */
    char sys_pkg_dir[256];
    str_copy_local(sys_pkg_dir, "/sys/pkg/");
    str_copy_local(sys_pkg_dir + 9, name);
    vfs_mkdir(sys_pkg_dir);
    
    char files_path[256];
    str_copy_local(files_path, sys_pkg_dir);
    str_copy_local(files_path + str_len_local(sys_pkg_dir), "/files");
    
    VfsFile *files_list_f = vfs_open_flags(files_path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    
    uint32_t num_files = header.num_files;
    
    WpkgFileHeader *file_headers = (WpkgFileHeader *)kmalloc(num_files * sizeof(WpkgFileHeader));
    if (!file_headers) {
        console_print_string(info, "Error: Out of memory during package unpacking.\n", 0x00FF0000, 0x000F0F1A);
        vfs_close(pkg_f);
        if (files_list_f) vfs_close(files_list_f);
        return false;
    }
    
    if (vfs_read(pkg_f, file_headers, num_files * sizeof(WpkgFileHeader)) != (int)(num_files * sizeof(WpkgFileHeader))) {
        console_print_string(info, "Error: Failed to read package file headers.\n", 0x00FF0000, 0x000F0F1A);
        kfree(file_headers);
        vfs_close(pkg_f);
        if (files_list_f) vfs_close(files_list_f);
        return false;
    }
    
    static char file_buf[4096];
    
    for (uint32_t i = 0; i < num_files; i++) {
        WpkgFileHeader *fh = &file_headers[i];
        
        console_print_string(info, "  Extracting: ", 0x00CCCCCC, 0x000F0F1A);
        console_print_string(info, fh->path, 0x00FFFFFF, 0x000F0F1A);
        console_print_string(info, "\n", 0, 0x000F0F1A);
        
        if (files_list_f) {
            vfs_write(files_list_f, fh->path, str_len_local(fh->path));
            vfs_write(files_list_f, "\n", 1);
        }
        
        ensure_parent_dirs(fh->path);
        
        VfsFile *dest_f = vfs_open_flags(fh->path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
        if (!dest_f) {
            console_print_string(info, "Error: Failed to open path for extraction: ", 0x00FF0000, 0x000F0F1A);
            console_print_string(info, fh->path, 0x00FFFFFF, 0x000F0F1A);
            console_print_string(info, "\n", 0, 0x000F0F1A);
            kfree(file_headers);
            vfs_close(pkg_f);
            if (files_list_f) vfs_close(files_list_f);
            return false;
        }
        
        vfs_seek(pkg_f, (int32_t)fh->offset, VFS_SEEK_SET);
        
        uint32_t remaining = fh->size;
        while (remaining > 0) {
            uint32_t to_read = remaining > sizeof(file_buf) ? sizeof(file_buf) : remaining;
            int read_bytes = vfs_read(pkg_f, file_buf, to_read);
            if (read_bytes <= 0) {
                console_print_string(info, "Error: Corrupted package during extraction.\n", 0x00FF0000, 0x000F0F1A);
                vfs_close(dest_f);
                kfree(file_headers);
                vfs_close(pkg_f);
                if (files_list_f) vfs_close(files_list_f);
                return false;
            }
            vfs_write(dest_f, file_buf, (uint32_t)read_bytes);
            remaining -= (uint32_t)read_bytes;
        }
        vfs_close(dest_f);
    }
    
    kfree(file_headers);
    vfs_close(pkg_f);
    if (files_list_f) vfs_close(files_list_f);
    
    /* Write local registry logs */
    char path_buf[256];
    VfsFile *meta_f;
    
    /* version */
    str_copy_local(path_buf, sys_pkg_dir);
    str_copy_local(path_buf + str_len_local(sys_pkg_dir), "/version");
    meta_f = vfs_open_flags(path_buf, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (meta_f) {
        vfs_write(meta_f, header.version, str_len_local(header.version));
        vfs_close(meta_f);
    }
    
    /* description */
    str_copy_local(path_buf, sys_pkg_dir);
    str_copy_local(path_buf + str_len_local(sys_pkg_dir), "/desc");
    meta_f = vfs_open_flags(path_buf, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (meta_f) {
        vfs_write(meta_f, header.description, str_len_local(header.description));
        vfs_close(meta_f);
    }
    
    /* depends */
    str_copy_local(path_buf, sys_pkg_dir);
    str_copy_local(path_buf + str_len_local(sys_pkg_dir), "/depends");
    meta_f = vfs_open_flags(path_buf, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (meta_f) {
        vfs_write(meta_f, header.depends, str_len_local(header.depends));
        vfs_close(meta_f);
    }
    
    vfs_delete(temp_wpkg);
    
    console_print_string(info, "Successfully installed ", 0x0000FF00, 0x000F0F1A);
    console_print_string(info, name, 0x00FFFFFF, 0x000F0F1A);
    console_print_string(info, " (version ", 0x0000FF00, 0x000F0F1A);
    console_print_string(info, header.version, 0x00FFFFFF, 0x000F0F1A);
    console_print_string(info, ").\n", 0x0000FF00, 0x000F0F1A);
    
    return true;
}

/* ============================================================
 * Helper: Package removal
 * ============================================================ */
static void remove_package(BootInfo *info, const char *name)
{
    char sys_pkg_dir[256];
    str_copy_local(sys_pkg_dir, "/sys/pkg/");
    str_copy_local(sys_pkg_dir + 9, name);
    
    char version_path[256];
    str_copy_local(version_path, sys_pkg_dir);
    str_copy_local(version_path + str_len_local(sys_pkg_dir), "/version");
    
    VfsStat st;
    if (!vfs_stat(version_path, &st)) {
        console_print_string(info, "Error: Package ", 0x00FF0000, 0x000F0F1A);
        console_print_string(info, name, 0x00FFFFFF, 0x000F0F1A);
        console_print_string(info, " is not installed.\n", 0x00FF0000, 0x000F0F1A);
        return;
    }
    
    console_print_string(info, "Removing package ", 0x00CCCCCC, 0x000F0F1A);
    console_print_string(info, name, 0x00FFFFFF, 0x000F0F1A);
    console_print_string(info, "...\n", 0x00CCCCCC, 0x000F0F1A);
    
    char files_path[256];
    str_copy_local(files_path, sys_pkg_dir);
    str_copy_local(files_path + str_len_local(sys_pkg_dir), "/files");
    
    VfsFile *f = vfs_open(files_path);
    if (f) {
        static char path_buf[256];
        uint32_t offset = 0;
        int bytes_read;
        char ch;
        
        while ((bytes_read = vfs_read(f, &ch, 1)) > 0) {
            if (ch == '\n' || ch == '\r') {
                if (offset > 0) {
                    path_buf[offset] = '\0';
                    console_print_string(info, "  Deleting: ", 0x00CCCCCC, 0x000F0F1A);
                    console_print_string(info, path_buf, 0x00FFFFFF, 0x000F0F1A);
                    console_print_string(info, "\n", 0, 0x000F0F1A);
                    vfs_delete(path_buf);
                    offset = 0;
                }
            } else {
                if (offset < sizeof(path_buf) - 1) {
                    path_buf[offset++] = ch;
                }
            }
        }
        if (offset > 0) {
            path_buf[offset] = '\0';
            vfs_delete(path_buf);
        }
        vfs_close(f);
    }
    
    /* Clean up local registry files */
    str_copy_local(files_path, sys_pkg_dir);
    str_copy_local(files_path + str_len_local(sys_pkg_dir), "/version");
    vfs_delete(files_path);
    
    str_copy_local(files_path, sys_pkg_dir);
    str_copy_local(files_path + str_len_local(sys_pkg_dir), "/desc");
    vfs_delete(files_path);
    
    str_copy_local(files_path, sys_pkg_dir);
    str_copy_local(files_path + str_len_local(sys_pkg_dir), "/depends");
    vfs_delete(files_path);
    
    str_copy_local(files_path, sys_pkg_dir);
    str_copy_local(files_path + str_len_local(sys_pkg_dir), "/files");
    vfs_delete(files_path);
    
    vfs_delete(sys_pkg_dir);
    
    console_print_string(info, "Successfully removed package ", 0x0000FF00, 0x000F0F1A);
    console_print_string(info, name, 0x00FFFFFF, 0x000F0F1A);
    console_print_string(info, ".\n", 0x0000FF00, 0x000F0F1A);
}

/* ============================================================
 * Helper: Package listing readdir callback
 * ============================================================ */
static BootInfo *g_list_info;

static void list_callback(VfsNode *node)
{
    if (str_compare_local(node->name, ".") == 0 || str_compare_local(node->name, "..") == 0) {
        return;
    }
    
    if (str_compare_local(node->name, "index.txt") == 0) {
        return;
    }
    
    if (node->is_dir) {
        char version_path[256];
        str_copy_local(version_path, "/sys/pkg/");
        str_copy_local(version_path + 9, node->name);
        str_copy_local(version_path + 9 + str_len_local(node->name), "/version");
        
        char version[32] = "unknown";
        VfsFile *f = vfs_open(version_path);
        if (f) {
            int len = vfs_read(f, version, sizeof(version) - 1);
            vfs_close(f);
            if (len > 0) {
                version[len] = '\0';
            }
        }
        
        char desc_path[256];
        str_copy_local(desc_path, "/sys/pkg/");
        str_copy_local(desc_path + 9, node->name);
        str_copy_local(desc_path + 9 + str_len_local(node->name), "/desc");
        
        char desc[128] = "";
        f = vfs_open(desc_path);
        if (f) {
            int len = vfs_read(f, desc, sizeof(desc) - 1);
            vfs_close(f);
            if (len > 0) {
                desc[len] = '\0';
            }
        }
        
        console_print_string(g_list_info, "  ", 0, 0x000F0F1A);
        console_print_string(g_list_info, node->name, 0x0000FF00, 0x000F0F1A);
        console_print_string(g_list_info, " (", 0x00CCCCCC, 0x000F0F1A);
        console_print_string(g_list_info, version, 0x00FFFFFF, 0x000F0F1A);
        console_print_string(g_list_info, ")", 0x00CCCCCC, 0x000F0F1A);
        if (str_len_local(desc) > 0) {
            console_print_string(g_list_info, " - ", 0x00CCCCCC, 0x000F0F1A);
            console_print_string(g_list_info, desc, 0x00E0E0E0, 0x000F0F1A);
        }
        console_print_string(g_list_info, "\n", 0, 0x000F0F1A);
    }
}

static void print_wynpkg_help(BootInfo *info)
{
    console_print_string(info, "WynlandOS Package Manager (wynpkg)\n\n", 0x0000FFFF, 0x000F0F1A);
    console_print_string(info, "Usage:\n", 0x00FFFF00, 0x000F0F1A);
    console_print_string(info, "  wynpkg update          - Update package index from repository\n", 0x00FFFFFF, 0x000F0F1A);
    console_print_string(info, "  wynpkg install <name>  - Install a package and its dependencies\n", 0x00FFFFFF, 0x000F0F1A);
    console_print_string(info, "  wynpkg remove <name>   - Remove an installed package\n", 0x00FFFFFF, 0x000F0F1A);
    console_print_string(info, "  wynpkg list            - List all installed packages\n", 0x00FFFFFF, 0x000F0F1A);
    console_print_string(info, "  wynpkg help            - Show this help message\n", 0x00FFFFFF, 0x000F0F1A);
}

/* ============================================================
 * Public command router
 * ============================================================ */
void cmd_wynpkg(BootInfo *info, const char *cmd)
{
    const char *args = cmd + 6;
    while (*args == ' ') {
        args++;
    }
    
    serial_write_string("[wynpkg] cmd_wynpkg received: '");
    serial_write_string(cmd);
    serial_write_string("'\r\n");
    serial_write_string("[wynpkg] args: '");
    serial_write_string(args);
    serial_write_string("'\r\n");
    
    if (str_compare_local(args, "") == 0 || str_starts_with_local(args, "help")) {
        print_wynpkg_help(info);
        return;
    }
    
    if (str_starts_with_local(args, "update")) {
        console_print_string(info, "Updating package database index...\n", 0x00FFFF00, 0x000F0F1A);
        vfs_mkdir("/sys");
        vfs_mkdir("/sys/pkg");
        if (http_download("/packages/index.txt", "/sys/pkg/index.txt")) {
            console_print_string(info, "Index updated successfully.\n", 0x0000FF00, 0x000F0F1A);
        } else {
            console_print_string(info, "Error: Failed to download index file. Verify Python HTTP server is running on 10.0.2.2:8000\n", 0x00FF0000, 0x000F0F1A);
        }
        return;
    }
    
    if (str_starts_with_local(args, "list")) {
        g_list_info = info;
        console_print_string(info, "Installed packages:\n", 0x00FFFF00, 0x000F0F1A);
        if (!vfs_readdir("/sys/pkg", list_callback)) {
            console_print_string(info, "Base package directory '/sys/pkg' not found. Run 'wynpkg update' first.\n", 0x00FF0000, 0x000F0F1A);
        }
        return;
    }
    
    if (str_starts_with_local(args, "install ")) {
        const char *name = args + 8;
        while (*name == ' ') {
            name++;
        }
        if (str_len_local(name) == 0) {
            console_print_string(info, "Usage: wynpkg install <name>\n", 0x00FF0000, 0x000F0F1A);
            return;
        }
        
        vfs_mkdir("/sys");
        vfs_mkdir("/sys/pkg");
        install_package_internal(info, name);
        return;
    }
    
    if (str_starts_with_local(args, "remove ")) {
        const char *name = args + 7;
        while (*name == ' ') {
            name++;
        }
        if (str_len_local(name) == 0) {
            console_print_string(info, "Usage: wynpkg remove <name>\n", 0x00FF0000, 0x000F0F1A);
            return;
        }
        
        remove_package(info, name);
        return;
    }
    
    console_print_string(info, "Unknown command. Type 'wynpkg help' for help.\n", 0x00FF0000, 0x000F0F1A);
}
