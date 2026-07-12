// libarchive-backed __archive_{list,extract,create} builtins.

#include "common.h"
#include "compiler_support.h"

#include <archive.h>
#include <archive_entry.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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

// Map libarchive filetype to a small string for scripts.
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

// Reject anything resembling an absolute path or a parent-traversal that
// would let an entry escape `dest_dir`. We refuse outright rather than try
// to "sanitize" - any such entry in a package is suspicious.
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

    // Walk components; reject any ".." that would escape the root.
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
        return std::equal(suf.rbegin(), suf.rend(), path.rbegin(),
                          [](char a, char b) { return std::tolower(a) == std::tolower(b); });
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

// File-local C++ exception used to unwind libarchive cleanup (handles, FILE*,
// archive_entry*) cleanly. Caught at the builtin entry points and converted
// into a script-catchable throw via runtime->flags.
struct ArchiveError {
    std::string msg;
};

[[noreturn]] void archive_throw(const std::string &msg) {
    throw ArchiveError{ msg };
}

[[noreturn]] void archive_throw_libarchive(struct archive *a, const std::string &prefix) {
    const char *msg = archive_error_string(a);
    throw ArchiveError{ prefix + ": " + (msg ? msg : "unknown libarchive error") };
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

} // namespace

// Convert an ArchiveError (internal C++ cleanup-unwind exception) into an
// Err(message) result value.
static Value raise_archive_err(ScriptRuntime *rt, const ArchiveError &e) {
    return rt->make_err(Value::make_string(e.msg));
}

Value ScriptRuntime::builtin_archive_list(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    // location embedded in throw value would be nice but the
    // runtime's throw handling carries only a Value, not a node;
    // we rely on the error message being descriptive instead.
    try {
        if (argc != 1 || !argvals[0].is_string()) {
            archive_throw("Archive.list expects a single path string argument");
        }
        const std::string &path = argvals[0].get_string();

        ArchiveReadGuard guard(archive_read_new());
        if (guard.a == nullptr) {
            // archive_read_new failure is OOM, not exactly something we can recover from
            runtime_fatal("Archive.list: archive_read_new failed", call);
        }
        archive_read_support_format_tar(guard.a);
        archive_read_support_format_zip(guard.a);
        archive_read_support_filter_gzip(guard.a);

        // Note: we deliberately do NOT call archive_read_support_format_all / archive_read_support_filter_all
        // this is so that unsupported formats fail explicitly.
        // please open an issue if we are missing any notable, easily addable formats.

        if (archive_read_open_filename(guard.a, path.c_str(), 64 * 1024) != ARCHIVE_OK) {
            archive_throw_libarchive(guard.a, "Archive.list: cannot open '" + path + "'");
        }

        std::vector<Value> entries;
        struct archive_entry *entry = nullptr;
        while (true) {
            int r = archive_read_next_header(guard.a, &entry);
            if (r == ARCHIVE_EOF) {
                break;
            }
            if (r < ARCHIVE_WARN) {
                archive_throw_libarchive(guard.a, "Archive.list");
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
    } catch (const ArchiveError &e) {
        return raise_archive_err(this, e);
    }
}

Value ScriptRuntime::builtin_archive_extract(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    (void)call;
    try {
        if (argc < 2 || argc > 3 || !argvals[0].is_string() || !argvals[1].is_string()) {
            archive_throw("Archive.extract(archive_path, dest_dir, opts?) expects (string, string, object?)");
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

        // Resolve dest_dir to an absolute, canonical path so we can do
        // containment checks robustly even if the script passed a relative path.
        std::error_code ec;
        stdfs::create_directories(dest_dir, ec);
        if (ec) {
            archive_throw("Archive.extract: cannot create dest_dir '" + dest_dir + "': " + ec.message());
        }
        stdfs::path dest_root = stdfs::canonical(dest_dir, ec);
        if (ec) {
            archive_throw("Archive.extract: cannot canonicalize dest_dir '" + dest_dir + "': " + ec.message());
        }

        ArchiveReadGuard guard(archive_read_new());
        if (guard.a == nullptr) {
            runtime_fatal("Archive.extract: archive_read_new failed", call);
        }
        archive_read_support_format_tar(guard.a);
        archive_read_support_format_zip(guard.a);
        archive_read_support_filter_gzip(guard.a);

        if (archive_read_open_filename(guard.a, archive_path.c_str(), 64 * 1024) != ARCHIVE_OK) {
            archive_throw_libarchive(guard.a, "Archive.extract: cannot open '" + archive_path + "'");
        }

        int64_t files = 0;
        int64_t bytes = 0;

        struct archive_entry *entry = nullptr;
        while (true) {
            int r = archive_read_next_header(guard.a, &entry);
            if (r == ARCHIVE_EOF) {
                break;
            }
            if (r < ARCHIVE_WARN) {
                archive_throw_libarchive(guard.a, "Archive.extract");
            }

            const char *raw_name = archive_entry_pathname(entry);
            std::string name = raw_name ? raw_name : "";
            if (!path_is_safe(name)) {
                archive_throw("Archive.extract: unsafe entry path '" + name + "' (absolute or escapes dest_dir)");
            }

            mode_t ft = archive_entry_filetype(entry);
            if ((ft == AE_IFLNK || archive_entry_hardlink(entry) != nullptr) && !allow_links) {
                archive_throw("Archive.extract: entry '" + name + "' is a link; refused (set opts.allow_links=true to permit)");
            }
            if (ft != AE_IFREG && ft != AE_IFDIR && ft != AE_IFLNK) {
                archive_throw("Archive.extract: entry '" + name + "' has unsupported file type");
            }

            ++files;
            if (files > max_files) {
                archive_throw("Archive.extract: entry count exceeds max_files=" + std::to_string(max_files));
            }

            // Resolve the on-disk target and verify it stays within dest_root
            // even after the OS resolves any symlinks in dest_root's parents.
            stdfs::path target = dest_root / name;
            stdfs::path target_parent = target.parent_path();
            stdfs::create_directories(target_parent, ec);
            if (ec) {
                archive_throw("Archive.extract: cannot create dir '" + target_parent.string() + "': " + ec.message());
            }
            // Check containment using weakly_canonical (target may not exist yet).
            stdfs::path canon = stdfs::weakly_canonical(target, ec);
            if (ec) {
                archive_throw("Archive.extract: cannot canonicalize target '" + target.string() + "': " + ec.message());
            }
            // Mismatch on the prefix means the entry escaped dest_root.
            // generic_string() normalizes separators on Windows so the rfind
            // works the same on both platforms.
            auto rel = stdfs::relative(canon, dest_root, ec);
            std::string rel_str = rel.generic_string();
            if (ec || rel_str.empty() || rel_str.rfind("..", 0) == 0) {
                archive_throw("Archive.extract: entry '" + name + "' escapes dest_dir");
            }

            if (ft == AE_IFDIR) {
                stdfs::create_directories(target, ec);
                if (ec) {
                    archive_throw("Archive.extract: cannot create dir '" + target.string() + "': " + ec.message());
                }
                continue;
            }

            if (ft == AE_IFLNK) {
                // allow_links == true was checked above. We don't follow the link;
                // we recreate it as-is. The link target itself is NOT validated
                // here against dest_root because POSIX symlinks resolve at access
                // time relative to the link's location. Callers who enable links
                // are accepting that risk.
                const char *linkname = archive_entry_symlink(entry);
                if (linkname == nullptr) {
                    archive_throw("Archive.extract: symlink entry '" + name + "' has no target");
                }
                std::error_code lec;
                stdfs::remove(target, lec); // overwrite if exists
                stdfs::create_symlink(linkname, target, lec);
                if (lec) {
                    archive_throw("Archive.extract: cannot create symlink '" + target.string() + "': " + lec.message());
                }
                continue;
            }

            // Regular file: stream entry data to disk in chunks; enforce
            // max_bytes. FileGuard closes the fp even if archive_throw
            // unwinds mid-loop.
            FileGuard fg(std::fopen(target.string().c_str(), "wb"));
            if (fg.fp == nullptr) {
                archive_throw("Archive.extract: cannot open '" + target.string() + "' for write: " + std::strerror(errno));
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
                    archive_throw_libarchive(guard.a, "Archive.extract");
                }
                bytes += static_cast<int64_t>(buf_size);
                if (bytes > max_bytes) {
                    archive_throw("Archive.extract: uncompressed size exceeds max_bytes=" + std::to_string(max_bytes));
                }
                // libarchive can emit sparse blocks at non-zero offsets. fseek
                // to honour them; on most archives offset just monotonically
                // tracks cumulative bytes written.
                if (std::fseek(fg.fp, static_cast<long>(offset), SEEK_SET) != 0) {
                    archive_throw(std::string("Archive.extract: fseek failed on '") + target.string() + "': " + std::strerror(errno));
                }
                if (std::fwrite(buf, 1, buf_size, fg.fp) != buf_size) {
                    archive_throw(std::string("Archive.extract: write failed on '") + target.string() + "': " + std::strerror(errno));
                }
            }
            // fg destructor closes fp here.

            // Apply the entry's permission bits, masked. We never honour setuid /
            // setgid / sticky from an archive - too easy to abuse.
            mode_t perm = archive_entry_perm(entry) & 0777;
            if (perm == 0) {
                perm = 0644;
            }
            std::error_code pec;
            stdfs::permissions(target, static_cast<stdfs::perms>(perm), stdfs::perm_options::replace, pec);
            // Permission setting is best-effort (e.g. on FAT); ignore failure.
        }

        Value result = Value::make_object();
        ObjectObj *r = result.get_obj_ptr();
        r->set_field("files", Value::make_int_checked(files));
        r->set_field("bytes", Value::make_int_checked(bytes));
        return make_ok(result);
    } catch (const ArchiveError &e) {
        return raise_archive_err(this, e);
    }
}

Value ScriptRuntime::builtin_archive_create(const Value *argvals, size_t argc, const nari::CallExpr *call) {
    (void)call;
    try {
        if (argc < 2 || argc > 3 || !argvals[0].is_string() || !argvals[1].is_array()) {
            archive_throw("Archive.create(archive_path, files, opts?) expects (string, array, object?)");
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
            archive_throw(
                "Archive.create: cannot infer format from '" + archive_path +
                "' (supported: .tar, .tar.gz, .tgz, .zip)");
        }

        ArchiveWriteGuard guard(archive_write_new());
        if (guard.a == nullptr) {
            runtime_fatal("Archive.create: archive_write_new failed", call);
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
            archive_throw_libarchive(guard.a, "Archive.create: cannot open '" + archive_path + "' for write");
        }

        int64_t file_count = 0;

        auto add_one = [&](const std::string &src_path, const std::string &entry_name) {
            std::error_code ec;
            stdfs::path src(src_path);
            auto status = stdfs::symlink_status(src, ec);
            if (ec) {
                archive_throw("Archive.create: cannot stat '" + src_path + "': " + ec.message());
            }

            ArchiveEntryGuard eg(archive_entry_new());
            archive_entry_set_pathname(eg.e, entry_name.c_str());

            if (stdfs::is_directory(status)) {
                archive_entry_set_filetype(eg.e, AE_IFDIR);
                archive_entry_set_perm(eg.e, 0755);
                archive_entry_set_size(eg.e, 0);
                int wr = archive_write_header(guard.a, eg.e);
                if (wr < ARCHIVE_OK) {
                    archive_throw(std::string("Archive.create: write_header failed for '") + entry_name + "'");
                }
                ++file_count;
                return;
            }

            if (stdfs::is_symlink(status)) {
                // Record the symlink as-is; we don't follow it.
                stdfs::path target = stdfs::read_symlink(src, ec);
                if (ec) {
                    archive_throw("Archive.create: cannot read symlink '" + src_path + "': " + ec.message());
                }
                archive_entry_set_filetype(eg.e, AE_IFLNK);
                archive_entry_set_symlink(eg.e, target.string().c_str());
                archive_entry_set_perm(eg.e, 0777);
                archive_entry_set_size(eg.e, 0);
                int wr = archive_write_header(guard.a, eg.e);
                if (wr < ARCHIVE_OK) {
                    archive_throw(std::string("Archive.create: write_header failed for symlink '") + entry_name + "'");
                }
                ++file_count;
                return;
            }

            if (!stdfs::is_regular_file(status)) {
                archive_throw("Archive.create: source '" + src_path + "' is not a regular file/dir/symlink");
            }

            auto sz = stdfs::file_size(src, ec);
            if (ec) {
                archive_throw("Archive.create: cannot stat '" + src_path + "': " + ec.message());
            }
            archive_entry_set_filetype(eg.e, AE_IFREG);
            archive_entry_set_perm(eg.e, 0644);
            archive_entry_set_size(eg.e, static_cast<la_int64_t>(sz));

            int wr = archive_write_header(guard.a, eg.e);
            if (wr < ARCHIVE_OK) {
                archive_throw_libarchive(guard.a, "Archive.create: write_header");
            }

            FileGuard fg(std::fopen(src_path.c_str(), "rb"));
            if (fg.fp == nullptr) {
                archive_throw(std::string("Archive.create: cannot read '") + src_path + "': " + std::strerror(errno));
            }
            char buf[64 * 1024];
            while (true) {
                size_t n = std::fread(buf, 1, sizeof(buf), fg.fp);
                if (n > 0) {
                    la_ssize_t w = archive_write_data(guard.a, buf, n);
                    if (w < 0 || static_cast<size_t>(w) != n) {
                        archive_throw_libarchive(guard.a, "Archive.create: write_data");
                    }
                }
                if (n < sizeof(buf)) {
                    if (std::ferror(fg.fp)) {
                        archive_throw(std::string("Archive.create: read error on '") + src_path + "': " + std::strerror(errno));
                    }
                    break;
                }
            }
            // eg + fg destructors clean up here.
            ++file_count;
        };

        for (const Value &item : files) {
            if (item.is_string()) {
                const std::string &src = item.get_string();
                std::string entry_name;
                if (!base_dir.empty()) {
                    std::error_code ec;
                    stdfs::path rel = stdfs::relative(stdfs::path(src), stdfs::path(base_dir), ec);
                    std::string rel_str = rel.generic_string();
                    if (ec || rel_str.empty() || rel_str.rfind("..", 0) == 0) {
                        archive_throw("Archive.create: source '" + src + "' is not inside base_dir '" + base_dir + "'");
                    }
                    entry_name = rel_str;
                } else {
                    entry_name = stdfs::path(src).filename().string();
                }
                add_one(src, entry_name);
            } else if (item.is_object()) {
                const ObjectObj *o = item.get_obj_ptr();
                const Value *src_v = o->get_field("src");
                const Value *dest_v = o->get_field("dest");
                if (src_v == nullptr || !src_v->is_string() ||
                    dest_v == nullptr || !dest_v->is_string()) {
                    archive_throw("Archive.create: file entries must be either a string or { src, dest } with both as strings");
                }
                std::string dest = dest_v->get_string();
                if (!path_is_safe(dest)) {
                    archive_throw("Archive.create: unsafe entry name '" + dest + "' (absolute or escapes archive root)");
                }
                add_one(src_v->get_string(), dest);
            } else {
                archive_throw("Archive.create: file entry has unexpected type " + value_type_name(item));
            }
        }

        if (archive_write_close(guard.a) != ARCHIVE_OK) {
            archive_throw_libarchive(guard.a, "Archive.create");
        }

        // Stat the output to report compressed size.
        std::error_code ec;
        auto sz = stdfs::file_size(archive_path, ec);
        int64_t out_bytes = ec ? 0 : static_cast<int64_t>(sz);

        Value result = Value::make_object();
        ObjectObj *r = result.get_obj_ptr();
        r->set_field("files", Value::make_int_checked(file_count));
        r->set_field("bytes", Value::make_int_checked(out_bytes));
        return make_ok(result);
    } catch (const ArchiveError &e) {
        return raise_archive_err(this, e);
    }
}
