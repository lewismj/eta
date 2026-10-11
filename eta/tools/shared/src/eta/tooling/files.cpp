#include "files.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/mount.h>
#include <sys/acl.h>
#else
#include <sys/vfs.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#endif
#endif

namespace eta::tooling {
    namespace fs = std::filesystem;
    namespace {
        std::atomic_uint64_t sequence{0};
        std::string error_text() {
#ifdef _WIN32
            return std::system_category().message(GetLastError());
#else
            return std::generic_category().message(errno);
#endif
        }

#ifdef _WIN32
        struct Handle {
            HANDLE value{INVALID_HANDLE_VALUE};
            Handle() = default;
            explicit Handle(HANDLE handle) : value(handle) {}
            ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
            Handle(const Handle &) = delete;
            Handle &operator=(const Handle &) = delete;
        };
        std::string identity(HANDLE handle) {
            FILE_ID_INFO info{};
            if (!GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info))) throw std::runtime_error(error_text());
            return std::string(reinterpret_cast<const char *>(&info), sizeof(info));
        }
        std::string security(HANDLE handle) {
            DWORD length = 0;
            const auto flags = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
            GetKernelObjectSecurity(handle, flags, nullptr, 0, &length);
            if (!length) throw std::runtime_error(error_text());
            std::string result(length, '\0');
            if (!GetKernelObjectSecurity(handle, flags, reinterpret_cast<PSECURITY_DESCRIPTOR>(result.data()), length, &length))
                throw std::runtime_error(error_text());
            result.resize(length);
            return result;
        }
        fs::path handle_path(HANDLE handle) {
            const auto size = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (!size) throw std::runtime_error(error_text());
            std::wstring result(size, L'\0');
            const auto written = GetFinalPathNameByHandleW(handle, result.data(), size, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (!written || written >= size) throw std::runtime_error(error_text());
            result.resize(written);
            return fs::path(result);
        }
        bool reparse(const fs::path &path) {
            const auto flags = GetFileAttributesW(path.c_str());
            return flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_REPARSE_POINT);
        }
        std::string read_bytes(HANDLE handle, std::size_t limit) {
            LARGE_INTEGER size{}, zero{};
            if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > limit)
                throw std::runtime_error("cannot read input within the byte limit");
            if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN)) throw std::runtime_error(error_text());
            std::string result(static_cast<std::size_t>(size.QuadPart), '\0');
            std::size_t offset = 0;
            while (offset < result.size()) {
                DWORD count = 0;
                if (!ReadFile(handle, result.data() + offset, static_cast<DWORD>(std::min<std::size_t>(65536, result.size() - offset)), &count, nullptr) || !count)
                    throw std::runtime_error("input changed or could not be read: " + error_text());
                offset += count;
            }
            char extra{};
            DWORD count = 0;
            if (!ReadFile(handle, &extra, 1, &count, nullptr) || count) throw std::runtime_error("input changed during read");
            return result;
        }
#else
        struct Handle {
            int value{-1};
            Handle() = default;
            explicit Handle(int descriptor) : value(descriptor) {}
            ~Handle() { if (value >= 0) close(value); }
            Handle(const Handle &) = delete;
            Handle &operator=(const Handle &) = delete;
        };
        std::string identity(int handle) {
            struct stat info{};
            if (fstat(handle, &info)) throw std::runtime_error(error_text());
            return std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
        }
        bool reparse(const fs::path &path) { return fs::is_symlink(fs::symlink_status(path)); }
        std::string read_bytes(int handle, std::size_t limit) {
            struct stat info{};
            if (fstat(handle, &info) || info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > limit)
                throw std::runtime_error("cannot read input within the byte limit");
            if (lseek(handle, 0, SEEK_SET) < 0) throw std::runtime_error(error_text());
            std::string result;
            char buffer[65536];
            for (;;) {
                const auto count = read(handle, buffer, sizeof(buffer));
                if (count < 0) { if (errno == EINTR) continue; throw std::runtime_error(error_text()); }
                if (!count) break;
                if (static_cast<std::size_t>(count) > limit - result.size()) throw std::runtime_error("input byte limit exceeded");
                result.append(buffer, count);
            }
            return result;
        }
        using Attributes = std::map<std::string, std::string>;
#ifdef __APPLE__
        struct Acl {
            acl_t value{nullptr};
            ~Acl() { if (value) acl_free(value); }
        };
        std::string acl_text(acl_t acl) {
            ssize_t size = 0;
            char *text = acl_to_text(acl, &size);
            if (!text) throw std::runtime_error(error_text());
            const auto owned = std::unique_ptr<char, decltype(&acl_free)>(text, acl_free);
            return std::string(text, size);
        }
#else
        long file_flags(int descriptor) {
            long flags = 0;
            if (ioctl(descriptor, FS_IOC_GETFLAGS, &flags)) throw std::runtime_error("cannot capture ext4 flags: " + error_text());
            return flags;
        }
#endif
        Attributes attributes(int handle) {
#ifdef __APPLE__
            const auto length = flistxattr(handle, nullptr, 0, 0);
#else
            const auto length = flistxattr(handle, nullptr, 0);
#endif
            if (length < 0) throw std::runtime_error("cannot capture extended attributes: " + error_text());
            std::string names(length, '\0');
#ifdef __APPLE__
            if (flistxattr(handle, names.data(), names.size(), 0) != length)
#else
            if (flistxattr(handle, names.data(), names.size()) != length)
#endif
                throw std::runtime_error("extended attributes changed during capture");
            Attributes result;
            for (std::size_t offset = 0; offset < names.size();) {
                const auto name = names.c_str() + offset;
#ifdef __APPLE__
                const auto size = fgetxattr(handle, name, nullptr, 0, 0, 0);
#else
                const auto size = fgetxattr(handle, name, nullptr, 0);
#endif
                if (size < 0) throw std::runtime_error(error_text());
                std::string value(size, '\0');
#ifdef __APPLE__
                if (fgetxattr(handle, name, value.data(), value.size(), 0, 0) != size)
#else
                if (fgetxattr(handle, name, value.data(), value.size()) != size)
#endif
                    throw std::runtime_error("extended attribute changed during capture");
                result.emplace(name, std::move(value));
                offset += std::strlen(name) + 1;
            }
            return result;
        }
#endif
    }

    struct NativeSnapshot {
        Handle original;
        Handle parent;
        std::string parent_id;
#ifdef _WIN32
        FILE_BASIC_INFO basic{};
        FILE_STANDARD_INFO standard{};
        std::string descriptor;
#else
        struct stat info{};
        Attributes xattrs;
#ifdef __APPLE__
        Acl acl;
        std::string acl_description;
#else
        std::optional<long> flags;
#endif
#endif
    };

    std::expected<Snapshot, std::string> read_snapshot(const fs::path &input, bool mutating, std::size_t limit) {
        try {
            if (mutating && reparse(input)) return std::unexpected("mutating symbolic links/reparse points is unsupported");
            const auto path = fs::canonical(input);
            auto native = std::make_shared<NativeSnapshot>();
#ifdef _WIN32
            native->original.value = CreateFileW(path.c_str(), GENERIC_READ | (mutating ? READ_CONTROL : 0),
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            native->parent.value = CreateFileW(path.parent_path().c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (native->original.value == INVALID_HANDLE_VALUE || native->parent.value == INVALID_HANDLE_VALUE) throw std::runtime_error(error_text());
            if (!GetFileInformationByHandleEx(native->original.value, FileBasicInfo, &native->basic, sizeof(native->basic))
                || !GetFileInformationByHandleEx(native->original.value, FileStandardInfo, &native->standard, sizeof(native->standard))) throw std::runtime_error(error_text());
            if (native->standard.Directory) return std::unexpected("input is not a regular file");
            if (mutating && native->standard.NumberOfLinks != 1) return std::unexpected("mutating multiply hard-linked files is unsupported");
            if (mutating) native->descriptor = security(native->original.value);
#else
            native->parent.value = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            native->original.value = openat(native->parent.value, path.filename().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (native->parent.value < 0 || native->original.value < 0 || fstat(native->original.value, &native->info)) throw std::runtime_error(error_text());
            if (!S_ISREG(native->info.st_mode)) return std::unexpected("input is not a regular file");
            if (mutating && native->info.st_nlink != 1) return std::unexpected("mutating multiply hard-linked files is unsupported");
            if (mutating) native->xattrs = attributes(native->original.value);
#ifdef __APPLE__
            if (mutating) {
                native->acl.value = acl_get_fd(native->original.value);
                if (!native->acl.value) throw std::runtime_error("cannot capture APFS ACL: " + error_text());
                native->acl_description = acl_text(native->acl.value);
            }
#else
            struct statfs filesystem{};
            if (mutating && !fstatfs(native->parent.value, &filesystem) && filesystem.f_type == 0xEF53)
                native->flags = file_flags(native->original.value);
#endif
#endif
            native->parent_id = identity(native->parent.value);
            Snapshot result{path, read_bytes(native->original.value, limit), identity(native->original.value), std::move(native)};
            return result;
        } catch (const std::exception &error) { return std::unexpected(std::string(error.what())); }
    }

    bool glob_matches(std::string_view pattern, std::string_view path) {
        std::string expression = "^";
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            const char c = pattern[i];
            if (c == '*') {
                if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                    ++i;
                    if (i + 1 < pattern.size() && pattern[i + 1] == '/') { ++i; expression += "(?:.*/)?"; }
                    else expression += ".*";
                } else expression += "[^/]*";
            } else if (c == '?') expression += "[^/]";
            else { if (std::string_view(".\\+(){}[]^$|").find(c) != std::string_view::npos) expression += '\\'; expression += c; }
        }
        expression += '$';
        return std::regex_match(path.begin(), path.end(), std::regex(expression));
    }

    Discovery discover(const std::vector<fs::path> &inputs, const std::vector<std::string> &excludes, const fs::path &root, bool mutating) {
        Discovery result;
        std::vector<Selection> candidates;
        auto display = [&](const fs::path &path) {
            const auto text = fs::absolute(path).lexically_normal().lexically_relative(fs::absolute(root).lexically_normal()).generic_u8string();
            return std::string(text.begin(), text.end());
        };
        auto excluded = [&](const fs::path &path) {
            for (const auto &component : path) {
                const auto raw = component.generic_u8string();
                const std::string text(raw.begin(), raw.end());
                if (text == ".git" || text == "out" || text == "build" || text == ".eta") return true;
            }
            const auto text = display(path);
            return std::any_of(excludes.begin(), excludes.end(), [&](const auto &pattern) { return glob_matches(pattern, text); });
        };
        const auto paths = inputs.empty() ? std::vector<fs::path>{root} : inputs;
        for (const auto &path : paths) {
            try {
                if (excluded(path)) continue;
                if (fs::is_directory(path)) {
                    if (reparse(path)) continue;
                    for (fs::recursive_directory_iterator iterator(path), end; iterator != end; ++iterator) {
                        const auto &entry = *iterator;
                        if (excluded(entry.path())) { if (entry.is_directory()) iterator.disable_recursion_pending(); continue; }
                        if (entry.is_directory() && reparse(entry.path())) { iterator.disable_recursion_pending(); continue; }
                        if (entry.is_regular_file() && entry.path().extension() == ".eta") candidates.push_back({entry.path(), display(entry.path())});
                    }
                } else if (fs::is_regular_file(path)) candidates.push_back({path, display(path)});
                else result.issues.push_back({display(path), "input does not name a regular file or directory"});
            } catch (const std::exception &error) { result.issues.push_back({display(path), error.what()}); }
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) { return a.display < b.display; });
        std::map<std::string, bool> seen;
        for (auto &candidate : candidates) {
            auto snapshot = read_snapshot(candidate.path, mutating, 16 * 1024 * 1024);
            if (!snapshot) { result.issues.push_back({candidate.display, snapshot.error()}); continue; }
            if (seen.emplace(snapshot->identity, true).second) result.files.push_back(std::move(candidate));
        }
        std::sort(result.issues.begin(), result.issues.end(), [](const auto &a, const auto &b) { return a.display < b.display; });
        return result;
    }

    CommitResult replace_atomically(const Snapshot &snapshot, std::string_view output, const CommitHooks &hooks) {
        if (output == snapshot.bytes) return {true, CommitOutcome::NotCommitted, {}};
        fs::path temporary;
        std::string temporary_id;
        bool replaced = false;
        auto checkpoint = [&] {
            if (hooks.cancelled && hooks.cancelled->load(std::memory_order_relaxed)) throw std::runtime_error("file operation cancelled");
        };
        auto stage = [&](CommitStage value) {
            checkpoint();
            if (hooks.observe) hooks.observe(value);
            if (hooks.fail && hooks.fail(value)) throw std::runtime_error("injected file-operation failure");
        };
        auto cleanup = [&] {
            if (temporary.empty()) return;
#ifdef _WIN32
            auto existing = read_snapshot(temporary, false, std::max<std::size_t>(output.size(), 16 * 1024 * 1024));
            if (existing && existing->identity == temporary_id) { std::error_code ignored; fs::remove(temporary, ignored); }
            if (snapshot.native) {
                try {
                    const auto actual = handle_path(snapshot.native->parent.value) / temporary.filename();
                    auto retained = read_snapshot(actual, false, std::max<std::size_t>(output.size(), 16 * 1024 * 1024));
                    if (retained && retained->identity == temporary_id) { std::error_code ignored; fs::remove(actual, ignored); }
                } catch (const std::exception &) { /* Unresolvable identity never authorizes deletion. */ }
            }
#else
            if (snapshot.native) {
                try {
                    Handle retained(openat(snapshot.native->parent.value, temporary.filename().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
                    if (retained.value >= 0 && identity(retained.value) == temporary_id)
                        unlinkat(snapshot.native->parent.value, temporary.filename().c_str(), 0);
                } catch (const std::exception &) { /* Unresolvable identity never authorizes deletion. */ }
            }
#endif
        };
        try {
            if (!snapshot.native) throw std::runtime_error("missing immutable file snapshot");
            const auto &native = *snapshot.native;
            stage(CommitStage::BeforeRecheck);
#ifdef _WIN32
            Handle current(CreateFileW(snapshot.path.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (current.value == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot exclude concurrent writers: " + error_text());
            wchar_t volume[MAX_PATH]{};
            if (!GetVolumePathNameW(snapshot.path.c_str(), volume, MAX_PATH) || GetDriveTypeW(volume) != DRIVE_FIXED)
                throw std::runtime_error("mutation requires a tested local NTFS volume");
            wchar_t filesystem[64]{};
            if (!GetVolumeInformationByHandleW(current.value, nullptr, 0, nullptr, nullptr, nullptr, filesystem, 64)
                || std::wstring_view(filesystem) != L"NTFS") throw std::runtime_error("mutation requires a tested local NTFS volume");
            auto verify = [&] {
                Handle parent(CreateFileW(snapshot.path.parent_path().c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                Handle path(CreateFileW(snapshot.path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                FILE_BASIC_INFO basic{};
                FILE_STANDARD_INFO standard{};
                if (parent.value == INVALID_HANDLE_VALUE || path.value == INVALID_HANDLE_VALUE || identity(parent.value) != native.parent_id
                    || identity(path.value) != snapshot.identity || identity(current.value) != snapshot.identity
                    || !GetFileInformationByHandleEx(current.value, FileBasicInfo, &basic, sizeof(basic))
                    || !GetFileInformationByHandleEx(current.value, FileStandardInfo, &standard, sizeof(standard))
                    || basic.FileAttributes != native.basic.FileAttributes || basic.CreationTime.QuadPart != native.basic.CreationTime.QuadPart
                    || standard.NumberOfLinks != 1 || security(current.value) != native.descriptor
                    || read_bytes(current.value, 16 * 1024 * 1024) != snapshot.bytes)
                    throw std::runtime_error("file identity, contents or permissions changed since reading");
            };
            verify();
            const auto unsupported = native.basic.FileAttributes & ~(FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_ARCHIVE);
            if (unsupported) throw std::runtime_error("read-only or special file attributes prevent safe replacement");
            stage(CommitStage::BeforeTemporary);
            SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), const_cast<char *>(native.descriptor.data()), FALSE};
            Handle sibling;
            for (int attempt = 0; attempt < 64; ++attempt) {
                temporary = snapshot.path.parent_path() / (L".eta-fmt-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(sequence.fetch_add(1)) + L".tmp");
                sibling.value = CreateFileW(temporary.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (sibling.value != INVALID_HANDLE_VALUE) break;
                if (GetLastError() != ERROR_FILE_EXISTS) throw std::runtime_error(error_text());
            }
            if (sibling.value == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create unique temporary sibling");
            temporary_id = identity(sibling.value);
            for (std::size_t offset = 0; offset < output.size();) {
                checkpoint();
                DWORD count = 0;
                if (!WriteFile(sibling.value, output.data() + offset, static_cast<DWORD>(std::min<std::size_t>(65536, output.size() - offset)), &count, nullptr) || !count)
                    throw std::runtime_error(error_text());
                offset += count;
            }
            stage(CommitStage::BeforeFlush);
            if (!FlushFileBuffers(sibling.value)) throw std::runtime_error(error_text());
            if (!CloseHandle(sibling.value)) { sibling.value = INVALID_HANDLE_VALUE; throw std::runtime_error(error_text()); }
            sibling.value = INVALID_HANDLE_VALUE;
            stage(CommitStage::BeforeReplace);
            verify();
            if (!ReplaceFileW(snapshot.path.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr)) {
                const auto message = error_text();
                auto actual = read_snapshot(snapshot.path, false);
                cleanup();
                if (actual && actual->bytes == output) return {false, CommitOutcome::DurabilityUnconfirmed, "committed but durability unconfirmed: " + message};
                if (actual && actual->identity == snapshot.identity && actual->bytes == snapshot.bytes)
                    return {false, CommitOutcome::NotCommitted, "not committed: " + message};
                return {false, CommitOutcome::Unknown, "replacement outcome unknown: " + message};
            }
            replaced = true;
#else
            struct statfs filesystem{};
            if (fstatfs(native.parent.value, &filesystem)) throw std::runtime_error(error_text());
#ifdef __APPLE__
            if (std::string_view(filesystem.f_fstypename) != "apfs") throw std::runtime_error("mutation requires local APFS");
            if (native.info.st_flags) throw std::runtime_error("special APFS file flags prevent safe replacement");
#else
            if (filesystem.f_type != 0xEF53) throw std::runtime_error("mutation requires local ext4");
            if (!native.flags || (*native.flags & ~static_cast<long>(FS_EXTENT_FL))) throw std::runtime_error("special ext4 flags prevent safe replacement");
#endif
            if (flock(native.original.value, LOCK_EX | LOCK_NB)) throw std::runtime_error("cannot acquire required advisory lock");
            struct Unlock { int fd; ~Unlock() { flock(fd, LOCK_UN); } } unlock{native.original.value};
            auto verify = [&] {
                Handle parent(open(snapshot.path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                struct stat path{}, opened{};
                if (parent.value < 0 || identity(parent.value) != native.parent_id
                    || fstatat(native.parent.value, snapshot.path.filename().c_str(), &path, AT_SYMLINK_NOFOLLOW)
                    || fstat(native.original.value, &opened) || path.st_dev != native.info.st_dev || path.st_ino != native.info.st_ino
                    || opened.st_dev != native.info.st_dev || opened.st_ino != native.info.st_ino || path.st_nlink != 1
                    || path.st_mode != native.info.st_mode || path.st_uid != native.info.st_uid || path.st_gid != native.info.st_gid
                    || attributes(native.original.value) != native.xattrs || read_bytes(native.original.value, 16 * 1024 * 1024) != snapshot.bytes)
                    throw std::runtime_error("file identity, contents or permissions changed since reading");
#ifdef __APPLE__
                Acl current_acl{acl_get_fd(native.original.value)};
                if (!current_acl.value) throw std::runtime_error("cannot recheck APFS ACL");
                const auto description = acl_text(current_acl.value);
                if (description != native.acl_description || opened.st_flags != native.info.st_flags)
                    throw std::runtime_error("APFS permissions/flags changed since reading");
#else
                if (file_flags(native.original.value) != *native.flags) throw std::runtime_error("ext4 flags changed since reading");
#endif
            };
            verify();
            if (!(native.info.st_mode & 0222)) throw std::runtime_error("read-only file requires changes");
            stage(CommitStage::BeforeTemporary);
            Handle sibling;
            for (int attempt = 0; attempt < 64; ++attempt) {
                temporary = snapshot.path.parent_path() / (".eta-fmt-" + std::to_string(getpid()) + "-" + std::to_string(sequence.fetch_add(1)) + ".tmp");
                sibling.value = openat(native.parent.value, temporary.filename().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
                if (sibling.value >= 0) break;
                if (errno != EEXIST) throw std::runtime_error(error_text());
            }
            if (sibling.value < 0) throw std::runtime_error("cannot create unique temporary sibling");
            temporary_id = identity(sibling.value);
            if (fchown(sibling.value, native.info.st_uid, native.info.st_gid)) throw std::runtime_error("cannot retain ownership: " + error_text());
            for (std::size_t offset = 0; offset < output.size();) {
                checkpoint();
                const auto count = write(sibling.value, output.data() + offset, output.size() - offset);
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) throw std::runtime_error(error_text());
                offset += count;
            }
            if (fchmod(sibling.value, native.info.st_mode & 07777)) throw std::runtime_error("cannot retain permissions: " + error_text());
            for (const auto &[name, value] : native.xattrs) {
#ifdef __APPLE__
                if (fsetxattr(sibling.value, name.c_str(), value.data(), value.size(), 0, 0))
#else
                if (fsetxattr(sibling.value, name.c_str(), value.data(), value.size(), 0))
#endif
                    throw std::runtime_error("cannot retain extended attributes: " + error_text());
            }
#ifdef __APPLE__
            if (acl_set_fd(sibling.value, native.acl.value)) throw std::runtime_error("cannot retain APFS ACL: " + error_text());
#endif
            stage(CommitStage::BeforeFlush);
            if (fsync(sibling.value)) throw std::runtime_error(error_text());
            if (close(sibling.value)) { sibling.value = -1; throw std::runtime_error(error_text()); }
            sibling.value = -1;
            stage(CommitStage::BeforeReplace);
            verify();
            if (renameat(native.parent.value, temporary.filename().c_str(), native.parent.value, snapshot.path.filename().c_str())) throw std::runtime_error(error_text());
            replaced = true;
#endif
            stage(CommitStage::AfterReplace);
#ifndef _WIN32
            if (fsync(native.parent.value)) return {false, CommitOutcome::DurabilityUnconfirmed, "committed but directory sync failed: " + error_text()};
#endif
            return {true, CommitOutcome::Committed, {}};
        } catch (const std::exception &error) {
            cleanup();
            return {false, replaced ? CommitOutcome::DurabilityUnconfirmed : CommitOutcome::NotCommitted,
                std::string(replaced ? "committed but durability unconfirmed: " : "not committed: ") + error.what()};
        }
    }

    std::string unified_diff(std::string_view original, std::string_view formatted, std::string_view display) {
        if (original == formatted) return {};
        auto lines = [](std::string_view source) {
            std::vector<std::string_view> result;
            while (!source.empty()) {
                const auto end = source.find('\n');
                const auto size = end == std::string_view::npos ? source.size() : end + 1;
                result.push_back(source.substr(0, size));
                source.remove_prefix(size);
            }
            return result;
        };
        const auto before = lines(original), after = lines(formatted);
        std::ostringstream stream;
        stream << "--- a/" << display << "\n+++ b/" << display << "\n@@ -" << (before.empty() ? 0 : 1) << ',' << before.size()
               << " +" << (after.empty() ? 0 : 1) << ',' << after.size() << " @@\n";
        auto emit = [&](char prefix, const auto &list) {
            for (auto line : list) {
                stream << prefix << line;
                if (!line.ends_with('\n')) stream << "\n\\ No newline at end of file\n";
            }
        };
        emit('-', before);
        emit('+', after);
        return stream.str();
    }
}
