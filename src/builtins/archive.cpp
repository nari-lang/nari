// libarchive-backed __archive_{list,extract,create} builtins.

#include "common.h"
#include "compiler_support.h"

#include <archive.h>
#include <archive_entry.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#if !defined(_WIN32) && !defined(NARI_MCU) && !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if MSVC_COMPAT
// TODO: for some reason I have to do this or else the build breaks, even though we are including archive.h?
typedef unsigned short mode_t;
#endif

namespace stdfs = std::filesystem;

namespace {

constexpr int64_t kDefaultMaxFiles = 10'000;
constexpr int64_t kDefaultMaxBytes = 1024LL * 1024LL * 1024LL; // 1 GiB

// Read a numeric option from an object field, falling back to `def` if the
// field is missing, none, or not numeric. Integer and float both accepted.
int64_t opt_int(const ObjectObj *opts, const char *name, int64_t def) {
    if (opts == nullptr) {
        return def;
    }
    const Value *v = opts->get_field(name);
    if (v == nullptr || v->is_none()) {
        return def;
    }
    if (v->is_int()) {
        return v->get_int();
    }
    if (v->is_float()) {
        return static_cast<int64_t>(v->get_float());
    }
    return def;
}

bool opt_bool(const ObjectObj *opts, const char *name, bool def) {
    if (opts == nullptr) {
        return def;
    }
    const Value *v = opts->get_field(name);
    if (v == nullptr || v->is_none()) {
        return def;
    }
    if (v->is_bool()) {
        return v->get_bool();
    }
    return def;
}

std::string opt_string(const ObjectObj *opts, const char *name) {
    if (opts == nullptr) {
        return {};
    }
    const Value *v = opts->get_field(name);
    if (v == nullptr || !v->is_string()) {
        return {};
    }
    return v->get_string();
}

// map libarchive filetype to a small string for scripts.
const char *filetype_name(mode_t ft) {
    switch (ft) {
        case AE_IFREG:
            return "file";
        case AE_IFDIR:
            return "dir";
        case AE_IFLNK:
            return "symlink";
        default:
            return "other";
    }
}

// reject anything resembling an absolute path or a parent-traversal that would let an entry escape `dest_dir`.
bool path_is_safe(const std::string &p) {
    if (p.empty()) {
        return false;
    }
    // Absolute on POSIX.
    if (p.front() == '/') {
        return false;
    }
    // Absolute on Windows (drive letter or UNC).
    if (p.size() >= 2 && p[1] == ':') {
        return false;
    }
    if (p.size() >= 2 && (p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/')) {
        return false;
    }

    // walk components; reject any ".." that would escape the root.
    int depth = 0;
    size_t i = 0;
    while (i < p.size()) {
        size_t j = i;
        while (j < p.size() && p[j] != '/' && p[j] != '\\') {
            ++j;
        }
        std::string_view comp(p.data() + i, j - i);
        if (comp == "..") {
            if (depth == 0) {
                return false;
            }
            --depth;
        } else if (!comp.empty() && comp != ".") {
            ++depth;
        }
        i = (j < p.size()) ? j + 1 : j;
    }
    return true;
}

// Pick the libarchive write format + filter from the destination filename.
// Returns false if the extension is not a format we support.
bool pick_write_format(const std::string &path, std::string &format, std::string &filter) {
    auto ends_with = [&](std::string_view suf) {
        if (path.size() < suf.size()) {
            return false;
        }
        return std::equal(suf.rbegin(), suf.rend(), path.rbegin(), [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    };

    if (ends_with(".tar.gz") || ends_with(".tgz")) {
        format = "ustar";
        filter = "gzip";
        return true;
    }
    if (ends_with(".tar")) {
        format = "ustar";
        filter = "";
        return true;
    }
    if (ends_with(".zip")) {
        format = "zip";
        filter = "";
        return true;
    }
    return false;
}

// file-local error value. Cleanup is handled by the RAII guards above,
// so early `return` already frees handles/FILEs/entries; no unwind exception needed.
struct ArchiveError {
    std::string msg;
};

inline ArchiveError make_archive_lib_error(struct archive *a, const std::string &prefix) {
    const char *msg = archive_error_string(a);
    if (!msg) {
        return ArchiveError{ "unknown libarchive error" };
    }
    return ArchiveError{ prefix + ": " + msg };
}

// RAII handle for a libarchive read context
struct ArchiveReadGuard {
    struct archive *a = nullptr;

    explicit ArchiveReadGuard(struct archive *p) : a(p) {
    }

    ~ArchiveReadGuard() {
        if (a) {
            archive_read_free(a);
        }
    }
    ArchiveReadGuard(const ArchiveReadGuard &) = delete;
    ArchiveReadGuard &operator=(const ArchiveReadGuard &) = delete;
};

struct ArchiveWriteGuard {
    struct archive *a = nullptr;
    explicit ArchiveWriteGuard(struct archive *p) : a(p) {
    }
    ~ArchiveWriteGuard() {
        if (a) {
            archive_write_free(a);
        }
    }
    ArchiveWriteGuard(const ArchiveWriteGuard &) = delete;
    ArchiveWriteGuard &operator=(const ArchiveWriteGuard &) = delete;
};

struct ArchiveEntryGuard {
    struct archive_entry *e = nullptr;
    explicit ArchiveEntryGuard(struct archive_entry *p) : e(p) {
    }
    ~ArchiveEntryGuard() {
        if (e) {
            archive_entry_free(e);
        }
    }
    ArchiveEntryGuard(const ArchiveEntryGuard &) = delete;
    ArchiveEntryGuard &operator=(const ArchiveEntryGuard &) = delete;
};

struct FileGuard {
    FILE *fp = nullptr;
    explicit FileGuard(FILE *p) : fp(p) {
    }
    ~FileGuard() {
        if (fp) {
            std::fclose(fp);
        }
    }
    FileGuard(const FileGuard &) = delete;
    FileGuard &operator=(const FileGuard &) = delete;
};

#if !defined(_WIN32) && !defined(NARI_MCU) && !defined(__EMSCRIPTEN__)
struct FdGuard {
    int fd = -1;
    explicit FdGuard(int value = -1) : fd(value) {}
    ~FdGuard() { if (fd >= 0) ::close(fd); }
    FdGuard(const FdGuard &) = delete;
    FdGuard &operator=(const FdGuard &) = delete;
};

// Walk from an already opened destination root. Never follow an archive-created
// or pre-existing symlink while creating parent directories.
int open_archive_dir(int root_fd, const stdfs::path &relative, bool create) {
    int current = ::dup(root_fd);
    if (current < 0) return -1;
    for (const auto &component : relative) {
        std::string name = component.string();
        if (name.empty() || name == ".") continue;
        if (name == "..") { ::close(current); errno = EINVAL; return -1; }
        if (create && ::mkdirat(current, name.c_str(), 0755) != 0 && errno != EEXIST) {
            int saved = errno;
            ::close(current);
            errno = saved;
            return -1;
        }
        int next = ::openat(current, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) {
            int saved = errno;
            ::close(current);
            errno = saved;
            return -1;
        }
        ::close(current);
        current = next;
    }
    return current;
}
#endif

} // namespace

// Convert an ArchiveError value into an Err(message) result value.
static Value raise_archive_err(ScriptRuntime *rt, const ArchiveError &e) {
    return rt->make_err(Value::make_string(e.msg));
}

// Early-return helpers: RAII guards clean up on return, so no exception unwind is needed.
#define ARCHIVE_FAIL(msg) return raise_archive_err(this, ArchiveError{ msg })
#define ARCHIVE_FAIL_LIB(a, prefix) return raise_archive_err(this, make_archive_lib_error(a, prefix))

Value ScriptRuntime::builtin_archive_list(Value *argvals, size_t argc, const nari::CallExpr *call) {
    // location embedded in throw value would be nice but the
    // runtime's throw handling carries only a Value, not a node.
    if (argc != 1 || !argvals[0].is_string()) {
        ARCHIVE_FAIL("Archive.list expects a single path string argument");
    }
    const std::string &path = argvals[0].get_string();

    ArchiveReadGuard guard(archive_read_new());
    if (guard.a == nullptr) {
        // archive_read_new failure is OOM, not exactly something we can recover from
        return runtime_fatal("Archive.list: archive_read_new failed", call);
    }
    archive_read_support_format_tar(guard.a);
    archive_read_support_format_zip(guard.a);
    archive_read_support_filter_gzip(guard.a);

    // Note: we deliberately do NOT call archive_read_support_format_all / archive_read_support_filter_all
    // this is so that unsupported formats fail explicitly.
    // please open an issue if we are missing any notable, easily addable formats.

    if (archive_read_open_filename(guard.a, path.c_str(), 64 * 1024) != ARCHIVE_OK) {
        ARCHIVE_FAIL_LIB(guard.a, "Archive.list: cannot open '" + path + "'");
    }

    std::vector<Value> entries;
    struct archive_entry *entry = nullptr;
    while (true) {
        int r = archive_read_next_header(guard.a, &entry);
        if (r == ARCHIVE_EOF) {
            break;
        }
        if (r < ARCHIVE_WARN) {
            ARCHIVE_FAIL_LIB(guard.a, "Archive.list");
        }

        Value e = Value::make_object();
        ObjectObj *eo = e.get_obj_ptr();
        const char *name = archive_entry_pathname(entry);
        eo->set_field("name", Value::make_string(name ? name : ""));
        eo->set_field("size", Value::make_int_checked(static_cast<int64_t>(archive_entry_size(entry))));
        eo->set_field("type", Value::make_string(filetype_name(archive_entry_filetype(entry))));
        eo->set_field("mtime", Value::make_int_checked(static_cast<int64_t>(archive_entry_mtime(entry))));
        entries.push_back(e);
    }

    return make_ok(Value::make_array(std::move(entries)));
}

Value ScriptRuntime::builtin_archive_extract(Value *argvals, size_t argc, const nari::CallExpr *call) {
    (void)call;
    if (argc < 2 || argc > 3 || !argvals[0].is_string() || !argvals[1].is_string()) {
        ARCHIVE_FAIL("Archive.extract(archive_path, dest_dir, opts?) expects (string, string, object?)");
    }
    const std::string &archive_path = argvals[0].get_string();
    const std::string &dest_dir = argvals[1].get_string();

    const ObjectObj *opts = nullptr;
    if (argc == 3 && argvals[2].is_object()) {
        opts = argvals[2].get_obj_ptr();
    }
    int64_t max_files = opt_int(opts, "max_files", kDefaultMaxFiles);
    int64_t max_bytes = opt_int(opts, "max_bytes", kDefaultMaxBytes);
    bool allow_links = opt_bool(opts, "allow_links", false);

    // resolve dest_dir to an absolute, canonical path so we can do
    // containment checks robustly even if the script passed a relative path.
    std::error_code err;
    stdfs::create_directories(dest_dir, err);
    if (err) {
        ARCHIVE_FAIL("Archive.extract: cannot create dest_dir '" + dest_dir + "': " + err.message());
    }
    stdfs::path dest_root = stdfs::canonical(dest_dir, err);
    if (err) {
        ARCHIVE_FAIL("Archive.extract: cannot canonicalize dest_dir '" + dest_dir + "': " + err.message());
    }
#if !defined(_WIN32) && !defined(NARI_MCU) && !defined(__EMSCRIPTEN__)
    FdGuard root_fd(::open(dest_root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (root_fd.fd < 0) {
        ARCHIVE_FAIL("Archive.extract: cannot open dest_dir '" + dest_dir + "': " + std::strerror(errno));
    }
#endif

    ArchiveReadGuard guard(archive_read_new());
    if (guard.a == nullptr) {
        return runtime_fatal("Archive.extract: archive_read_new failed", call);
    }
    archive_read_support_format_tar(guard.a);
    archive_read_support_format_zip(guard.a);
    archive_read_support_filter_gzip(guard.a);

    if (archive_read_open_filename(guard.a, archive_path.c_str(), 64 * 1024) != ARCHIVE_OK) {
        ARCHIVE_FAIL_LIB(guard.a, "Archive.extract: cannot open '" + archive_path + "'");
    }

    int64_t files = 0;
    int64_t bytes = 0;
    std::unordered_set<std::string> extracted_files;

    struct archive_entry *entry = nullptr;
    while (true) {
        int r = archive_read_next_header(guard.a, &entry);
        if (r == ARCHIVE_EOF) {
            break;
        }
        if (r < ARCHIVE_WARN) {
            ARCHIVE_FAIL_LIB(guard.a, "Archive.extract");
        }

        const char *raw_name = archive_entry_pathname(entry);
        std::string name = raw_name ? raw_name : "";
        if (!path_is_safe(name)) {
            ARCHIVE_FAIL("Archive.extract: unsafe entry path '" + name + "' (absolute or escapes dest_dir)");
        }

        mode_t ft = archive_entry_filetype(entry);
        const char *hardlink_name = archive_entry_hardlink(entry);
        if ((ft == AE_IFLNK || hardlink_name != nullptr) && !allow_links) {
            ARCHIVE_FAIL("Archive.extract: entry '" + name + "' is a link; refused (set opts.allow_links=true to permit)");
        }
        if (hardlink_name == nullptr && ft != AE_IFREG && ft != AE_IFDIR && ft != AE_IFLNK) {
            ARCHIVE_FAIL("Archive.extract: entry '" + name + "' has unsupported file type");
        }

        ++files;
        if (files > max_files) {
            ARCHIVE_FAIL("Archive.extract: entry count exceeds max_files=" + std::to_string(max_files));
        }

        stdfs::path relative = stdfs::path(name).lexically_normal();
        while (relative.filename().empty() && relative.has_parent_path()) relative = relative.parent_path();
        const std::string relative_name = relative.generic_string();
        stdfs::path target = dest_root / relative;
        stdfs::path target_parent = target.parent_path();
#if !defined(_WIN32) && !defined(NARI_MCU) && !defined(__EMSCRIPTEN__)
        FdGuard parent_fd(open_archive_dir(root_fd.fd, relative.parent_path(), true));
        if (parent_fd.fd < 0) {
            ARCHIVE_FAIL("Archive.extract: unsafe or inaccessible parent of '" + name + "': " + std::strerror(errno));
        }
        std::string leaf = relative.filename().string();
#else
        // Check before creating anything: a pre-existing symlink may resolve
        // the parent outside the destination.
        stdfs::path checked_parent = stdfs::weakly_canonical(target_parent, err);
        if (err) {
            ARCHIVE_FAIL("Archive.extract: cannot canonicalize parent of '" + name + "': " + err.message());
        }
        auto parent_rel = stdfs::relative(checked_parent, dest_root, err).generic_string();
        if (err || parent_rel.rfind("..", 0) == 0) {
            ARCHIVE_FAIL("Archive.extract: entry '" + name + "' escapes dest_dir");
        }
        stdfs::create_directories(target_parent, err);
        if (err) {
            ARCHIVE_FAIL("Archive.extract: cannot create dir '" + target_parent.string() + "': " + err.message());
        }
        // Check containment using weakly_canonical (target may not exist yet).
        stdfs::path canon = stdfs::weakly_canonical(target, err);
        if (err) {
            ARCHIVE_FAIL("Archive.extract: cannot canonicalize target '" + target.string() + "': " + err.message());
        }
        // Mismatch on the prefix means the entry escaped dest_root.
        auto rel = stdfs::relative(canon, dest_root, err);
        std::string rel_str = rel.generic_string();
        if (err || rel_str.empty() || rel_str.rfind("..", 0) == 0) {
            ARCHIVE_FAIL("Archive.extract: entry '" + name + "' escapes dest_dir");
        }
#endif

        if (hardlink_name != nullptr) {
            std::string source_name(hardlink_name);
            if (!path_is_safe(source_name)) {
                ARCHIVE_FAIL("Archive.extract: unsafe hardlink target '" + source_name + "'");
            }
            stdfs::path source_rel = stdfs::path(source_name).lexically_normal();
            while (source_rel.filename().empty() && source_rel.has_parent_path()) source_rel = source_rel.parent_path();
            if (source_rel == relative) {
                ARCHIVE_FAIL("Archive.extract: hardlink entry '" + name + "' targets itself");
            }
            if (!extracted_files.count(source_rel.generic_string())) {
                ARCHIVE_FAIL("Archive.extract: hardlink target '" + source_name + "' was not extracted from this archive");
            }
#if !defined(_WIN32) && !defined(NARI_MCU) && !defined(__EMSCRIPTEN__)
            FdGuard source_parent(open_archive_dir(root_fd.fd, source_rel.parent_path(), false));
            struct stat source_stat{};
            std::string source_leaf = source_rel.filename().string();
            if (source_parent.fd < 0 || ::fstatat(source_parent.fd, source_leaf.c_str(), &source_stat, AT_SYMLINK_NOFOLLOW) != 0 ||
                !S_ISREG(source_stat.st_mode)) {
                ARCHIVE_FAIL("Archive.extract: hardlink target '" + source_name + "' is not an extracted regular file");
            }
            if (::unlinkat(parent_fd.fd, leaf.c_str(), 0) != 0 && errno != ENOENT) {
                ARCHIVE_FAIL("Archive.extract: cannot replace hardlink entry '" + name + "': " + std::strerror(errno));
            }
            if (::linkat(source_parent.fd, source_leaf.c_str(), parent_fd.fd, leaf.c_str(), 0) != 0) {
                ARCHIVE_FAIL("Archive.extract: cannot create hardlink entry '" + name + "': " + std::strerror(errno));
            }
#else
            stdfs::path source = stdfs::weakly_canonical(dest_root / source_rel, err);
            if (err || !stdfs::is_regular_file(source, err)) {
                ARCHIVE_FAIL("Archive.extract: hardlink target '" + source_name + "' is not an extracted regular file");
            }
            auto source_inside = stdfs::relative(source, dest_root, err).generic_string();
            if (err || source_inside.empty() || source_inside.rfind("..", 0) == 0) {
                ARCHIVE_FAIL("Archive.extract: hardlink target '" + source_name + "' escapes dest_dir");
            }
            stdfs::remove(target, err);
            if (err) ARCHIVE_FAIL("Archive.extract: cannot replace hardlink entry '" + name + "': " + err.message());
            stdfs::create_hard_link(source, target, err);
            if (err) ARCHIVE_FAIL("Archive.extract: cannot create hardlink entry '" + name + "': " + err.message());
#endif
            extracted_files.insert(relative_name);
            continue;
        }

        if (ft == AE_IFDIR) {
#if !defined(_WIN32) && !defined(NARI_MCU) && !defined(__EMSCRIPTEN__)
            FdGuard dir_fd(open_archive_dir(root_fd.fd, relative, true));
            if (dir_fd.fd < 0) {
                ARCHIVE_FAIL("Archive.extract: cannot create dir '" + name + "': " + std::strerror(errno));
            }
#else
            stdfs::create_directories(target, err);
            if (err) {
                ARCHIVE_FAIL("Archive.extract: cannot create dir '" + target.string() + "': " + err.message());
            }
#endif
            extracted_files.erase(relative_name);
            continue;
        }

        if (ft == AE_IFLNK) {
            // allow_links == true was checked above.
            // we don't follow the link, instead we recreate it as-is
            const char *linkname = archive_entry_symlink(entry);
            if (linkname == nullptr) {
                ARCHIVE_FAIL("Archive.extract: symlink entry '" + name + "' has no target");
            }
#if !defined(_WIN32) && !defined(NARI_MCU) && !defined(__EMSCRIPTEN__)
            if (::unlinkat(parent_fd.fd, leaf.c_str(), 0) != 0 && errno != ENOENT) {
                ARCHIVE_FAIL("Archive.extract: cannot replace symlink '" + name + "': " + std::strerror(errno));
            }
            if (::symlinkat(linkname, parent_fd.fd, leaf.c_str()) != 0) {
                ARCHIVE_FAIL("Archive.extract: cannot create symlink '" + name + "': " + std::strerror(errno));
            }
#else
            std::error_code lec;
            stdfs::remove(target, lec);
            if (lec) ARCHIVE_FAIL("Archive.extract: cannot replace symlink '" + name + "': " + lec.message());
            stdfs::create_symlink(linkname, target, lec);
            if (lec) ARCHIVE_FAIL("Archive.extract: cannot create symlink '" + name + "': " + lec.message());
#endif
            extracted_files.erase(relative_name);
            continue;
        }

        // stream entry data to disk in chunks
        FileGuard fg(nullptr);
#if !defined(_WIN32) && !defined(NARI_MCU) && !defined(__EMSCRIPTEN__)
        // Replace an existing name before opening the new file. Truncating an
        // existing hardlink could change a file outside the destination.
        if (::unlinkat(parent_fd.fd, leaf.c_str(), 0) != 0 && errno != ENOENT) {
            ARCHIVE_FAIL("Archive.extract: cannot replace file '" + name + "': " + std::strerror(errno));
        }
        FdGuard output_fd(::openat(parent_fd.fd, leaf.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0644));
        struct stat output_stat{};
        if (output_fd.fd < 0 || ::fstat(output_fd.fd, &output_stat) != 0) {
            ARCHIVE_FAIL("Archive.extract: cannot open file '" + name + "' for write: " + std::strerror(errno));
        }
        if (!S_ISREG(output_stat.st_mode)) {
            ARCHIVE_FAIL("Archive.extract: entry '" + name + "' is not a regular file on disk");
        }
        fg.fp = ::fdopen(output_fd.fd, "wb");
        if (fg.fp != nullptr) output_fd.fd = -1;
#else
        stdfs::remove(target, err);
        if (err) {
            ARCHIVE_FAIL("Archive.extract: cannot replace file '" + name + "': " + err.message());
        }
        fg.fp = std::fopen(target.string().c_str(), "wb");
#endif
        if (fg.fp == nullptr) {
            ARCHIVE_FAIL("Archive.extract: cannot open '" + target.string() + "' for write: " + std::strerror(errno));
        }

        const void *buf = nullptr;
        size_t buf_size = 0;
        la_int64_t offset = 0;
        while (true) {
            int rr = archive_read_data_block(guard.a, &buf, &buf_size, &offset);
            if (rr == ARCHIVE_EOF) {
                break;
            }
            if (rr < ARCHIVE_WARN) {
                ARCHIVE_FAIL_LIB(guard.a, "Archive.extract");
            }
            bytes += static_cast<int64_t>(buf_size);
            if (bytes > max_bytes) {
                ARCHIVE_FAIL("Archive.extract: uncompressed size exceeds max_bytes=" + std::to_string(max_bytes));
            }
            // libarchive can emit sparse blocks at non-zero offsets
            if (std::fseek(fg.fp, static_cast<long>(offset), SEEK_SET) != 0) {
                ARCHIVE_FAIL(std::string("Archive.extract: fseek failed on '") + target.string() + "': " + std::strerror(errno));
            }
            if (std::fwrite(buf, 1, buf_size, fg.fp) != buf_size) {
                ARCHIVE_FAIL(std::string("Archive.extract: write failed on '") + target.string() + "': " + std::strerror(errno));
            }
        }
        // fg destructor closes fp here

        // apply the entry's permission bits, masked. we don't honour setuid / setgid / sticky from an archive
        mode_t perm = archive_entry_perm(entry) & 0777;
        if (perm == 0) {
            perm = 0644;
        }
#if !defined(_WIN32) && !defined(NARI_MCU) && !defined(__EMSCRIPTEN__)
        ::fchmod(::fileno(fg.fp), perm);
#else
        std::error_code pec;
        stdfs::permissions(target, (stdfs::perms)perm, stdfs::perm_options::replace, pec);
#endif
        // permission setting is best-effort, failure can be ignored
        extracted_files.insert(relative_name);
    }

    Value result = Value::make_object();
    ObjectObj *r = result.get_obj_ptr();
    r->set_field("files", Value::make_int_checked(files));
    r->set_field("bytes", Value::make_int_checked(bytes));
    return make_ok(result);
}

Value ScriptRuntime::builtin_archive_create(Value *argvals, size_t argc, const nari::CallExpr *call) {
    (void)call;
    if (argc < 2 || argc > 3 || !argvals[0].is_string() || !argvals[1].is_array()) {
        ARCHIVE_FAIL("Archive.create(archive_path, files, opts?) expects (string, array, object?)");
    }
    const std::string &archive_path = argvals[0].get_string();
    const Array &files = argvals[1].get_array();

    const ObjectObj *opts = nullptr;
    if (argc == 3 && argvals[2].is_object()) {
        opts = argvals[2].get_obj_ptr();
    }
    std::string base_dir = opt_string(opts, "base_dir");

    std::string format;
    std::string filter;
    if (!pick_write_format(archive_path, format, filter)) {
        ARCHIVE_FAIL("Archive.create: cannot infer format from '" + archive_path + "' (supported: .tar, .tar.gz, .tgz, .zip)");
    }

    ArchiveWriteGuard guard(archive_write_new());
    if (guard.a == nullptr) {
        return runtime_fatal("Archive.create: archive_write_new failed", call);
    }

    if (format == "ustar") {
        archive_write_set_format_ustar(guard.a);
    } else if (format == "zip") {
        archive_write_set_format_zip(guard.a);
    }
    if (filter == "gzip") {
        archive_write_add_filter_gzip(guard.a);
    }

    if (archive_write_open_filename(guard.a, archive_path.c_str()) != ARCHIVE_OK) {
        ARCHIVE_FAIL_LIB(guard.a, "Archive.create: cannot open '" + archive_path + "' for write");
    }

    int64_t file_count = 0;

    // Returns nullopt on success, or the error to raise. No exceptions:
    // RAII guards clean up on every return path.
    auto add_one = [&](const std::string &src_path, const std::string &entry_name) -> std::optional<ArchiveError> {
        std::error_code err;
        stdfs::path src(src_path);
        auto status = stdfs::symlink_status(src, err);
        if (err) {
            return ArchiveError{ "Archive.create: cannot stat '" + src_path + "': " + err.message() };
        }

        ArchiveEntryGuard eg(archive_entry_new());
        archive_entry_set_pathname(eg.e, entry_name.c_str());

        if (stdfs::is_directory(status)) {
            archive_entry_set_filetype(eg.e, AE_IFDIR);
            archive_entry_set_perm(eg.e, 0755);
            archive_entry_set_size(eg.e, 0);
            int wr = archive_write_header(guard.a, eg.e);
            if (wr < ARCHIVE_OK) {
                return ArchiveError{ std::string("Archive.create: write_header failed for '") + entry_name + "'" };
            }
            ++file_count;
            return std::nullopt;
        }

        if (stdfs::is_symlink(status)) {
            // Record the symlink as-is; we don't follow it.
            stdfs::path target = stdfs::read_symlink(src, err);
            if (err) {
                return ArchiveError{ "Archive.create: cannot read symlink '" + src_path + "': " + err.message() };
            }
            archive_entry_set_filetype(eg.e, AE_IFLNK);
            archive_entry_set_symlink(eg.e, target.string().c_str());
            archive_entry_set_perm(eg.e, 0777);
            archive_entry_set_size(eg.e, 0);
            int wr = archive_write_header(guard.a, eg.e);
            if (wr < ARCHIVE_OK) {
                return ArchiveError{ std::string("Archive.create: write_header failed for symlink '") + entry_name + "'" };
            }
            ++file_count;
            return std::nullopt;
        }

        if (!stdfs::is_regular_file(status)) {
            return ArchiveError{ "Archive.create: source '" + src_path + "' is not a regular file/dir/symlink" };
        }

        auto sz = stdfs::file_size(src, err);
        if (err) {
            return ArchiveError{ "Archive.create: cannot stat '" + src_path + "': " + err.message() };
        }
        archive_entry_set_filetype(eg.e, AE_IFREG);
        archive_entry_set_perm(eg.e, 0644);
        archive_entry_set_size(eg.e, static_cast<la_int64_t>(sz));

        int wr = archive_write_header(guard.a, eg.e);
        if (wr < ARCHIVE_OK) {
            return make_archive_lib_error(guard.a, "Archive.create: write_header");
        }

        FileGuard fg(std::fopen(src_path.c_str(), "rb"));
        if (fg.fp == nullptr) {
            return ArchiveError{ std::string("Archive.create: cannot read '") + src_path + "': " + std::strerror(errno) };
        }
        char buf[64 * 1024];
        while (true) {
            size_t n = std::fread(buf, 1, sizeof(buf), fg.fp);
            if (n > 0) {
                la_ssize_t w = archive_write_data(guard.a, buf, n);
                if (w < 0 || static_cast<size_t>(w) != n) {
                    return make_archive_lib_error(guard.a, "Archive.create: write_data");
                }
            }
            if (n < sizeof(buf)) {
                if (std::ferror(fg.fp)) {
                    return ArchiveError{ std::string("Archive.create: read error on '") + src_path + "': " + std::strerror(errno) };
                }
                break;
            }
        }
        // eg + fg destructors clean up here.
        ++file_count;
        return std::nullopt;
    };

    for (const Value &item : files) {
        if (item.is_string()) {
            const std::string &src = item.get_string();
            std::string entry_name;
            if (!base_dir.empty()) {
                std::error_code err;
                stdfs::path rel = stdfs::relative(stdfs::path(src), stdfs::path(base_dir), err);
                std::string rel_str = rel.generic_string();
                if (err || rel_str.empty() || rel_str.rfind("..", 0) == 0) {
                    ARCHIVE_FAIL("Archive.create: source '" + src + "' is not inside base_dir '" + base_dir + "'");
                }
                entry_name = rel_str;
            } else {
                entry_name = stdfs::path(src).filename().string();
            }
            if (auto err = add_one(src, entry_name)) {
                return raise_archive_err(this, *err);
            }
        } else if (item.is_object()) {
            const ObjectObj *o = item.get_obj_ptr();
            const Value *src_v = o->get_field("src");
            const Value *dest_v = o->get_field("dest");
            if (src_v == nullptr || !src_v->is_string() || dest_v == nullptr || !dest_v->is_string()) {
                ARCHIVE_FAIL("Archive.create: file entries must be either a string or { src, dest } with both as strings");
            }
            std::string dest = dest_v->get_string();
            if (!path_is_safe(dest)) {
                ARCHIVE_FAIL("Archive.create: unsafe entry name '" + dest + "' (absolute or escapes archive root)");
            }
            if (auto err = add_one(src_v->get_string(), dest)) {
                return raise_archive_err(this, *err);
            }
        } else {
            ARCHIVE_FAIL("Archive.create: file entry has unexpected type " + value_type_name(item));
        }
    }

    if (archive_write_close(guard.a) != ARCHIVE_OK) {
        ARCHIVE_FAIL_LIB(guard.a, "Archive.create");
    }

    // stat the output to report compressed size.
    std::error_code err;
    auto sz = stdfs::file_size(archive_path, err);
    int64_t out_bytes = err ? 0 : static_cast<int64_t>(sz);

    Value result = Value::make_object();
    ObjectObj *res = result.get_obj_ptr();
    res->set_field("files", Value::make_int_checked(file_count));
    res->set_field("bytes", Value::make_int_checked(out_bytes));
    return make_ok(result);
}
