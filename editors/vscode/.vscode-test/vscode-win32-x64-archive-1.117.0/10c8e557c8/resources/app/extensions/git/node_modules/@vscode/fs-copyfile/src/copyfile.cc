#ifndef __APPLE__
#error "This module currently only supports macOS"
#endif

#include <napi.h>
#include <sys/clonefile.h>
#include <sys/attr.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <string>
#include <atomic>
#include <sys/stat.h>
#include <uv.h>

// CLONE_ACL (macOS 13+) copies ACLs from the source file.
// On older macOS, fclonefileat returns EINVAL — we detect and skip.
static std::atomic<bool> g_no_clone_acl{false};

class FdGuard {
public:
    explicit FdGuard(int fd) : fd_(fd) {}
    ~FdGuard() { if (fd_ >= 0) ::close(fd_); }
    int get() const { return fd_; }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
private:
    int fd_;
};

static const char* ErrnoToCode(int err) {
    switch (err) {
        case EEXIST:  return "EEXIST";
        case EXDEV:   return "EXDEV";
        case ENOTSUP: return "ENOTSUP";
        case EACCES:  return "EACCES";
        case ENOENT:  return "ENOENT";
        case EPERM:   return "EPERM";
        case EISDIR:  return "EISDIR";
        case ENOSPC:  return "ENOSPC";
        case EINVAL:  return "EINVAL";
        case EIO:     return "EIO";
        default:      return "UNKNOWN";
    }
}

// Try fclonefileat with CLONE_ACL, fall back without on EINVAL (macOS < 13).
static int DoFclonefileat(int srcfd, const char* dst) {
    int flags = g_no_clone_acl.load(std::memory_order_relaxed) ? 0 : CLONE_ACL;

    if (::fclonefileat(srcfd, AT_FDCWD, dst, flags) == 0) return 0;

    if (flags != 0 && errno == EINVAL) {
        g_no_clone_acl.store(true, std::memory_order_relaxed);
        if (::fclonefileat(srcfd, AT_FDCWD, dst, 0) == 0) return 0;
    }

    return errno;
}

static int DoClone(const char* src, const char* dst, int mode) {
    int srcfd = ::open(src, O_RDONLY | O_CLOEXEC);
    if (srcfd < 0) return errno;
    FdGuard guard(srcfd);

    bool excl = (mode & UV_FS_COPYFILE_EXCL) != 0;

    int err = DoFclonefileat(srcfd, dst);
    if (err == 0) return 0;

    if (err == EEXIST && !excl) {
        // Don't overwrite if destination permissions disallow it (nodejs/node#26936).
        // Note: this is a best effort check based on 
        // https://github.com/libuv/libuv/pull/3969#issuecomment-1546026985 ignoring the TOCTOU.
        if (::faccessat(AT_FDCWD, dst, R_OK | W_OK, AT_EACCESS) != 0) return errno;

        // If dst is a symlink, resolve it so we unlink and reclone the target
        // file rather than replacing the symlink itself.
        const char* real_dst = dst;
        std::string resolved;
        struct stat st;
        if (::lstat(dst, &st) == 0 && S_ISLNK(st.st_mode)) {
            uv_fs_t req;
            int r = uv_fs_realpath(nullptr, &req, dst, nullptr);
            if (r != 0) { uv_fs_req_cleanup(&req); return -r; }
            resolved.assign(static_cast<const char*>(req.ptr));
            uv_fs_req_cleanup(&req);
            real_dst = resolved.c_str();
        }

        if (::unlink(real_dst) != 0 && errno != ENOENT) return errno;
        err = DoFclonefileat(srcfd, real_dst);
        return err;
    }

    return err;
}

static int DoUvCopyFile(const char* src, const char* dst, int flags) {
    uv_fs_t req;
    uv_loop_t loop;
    int r = uv_loop_init(&loop);
    if (r != 0) return r;
    r = uv_fs_copyfile(&loop, &req, src, dst, flags, nullptr);
    uv_fs_req_cleanup(&req);
    uv_loop_close(&loop);
    return r;
}

static Napi::Error MakeSystemError(Napi::Env env, int err,
                                    const std::string& src,
                                    const std::string& dst) {
    std::string msg = std::string(ErrnoToCode(err)) + ": " + strerror(err) +
                      ", copyfile '" + src + "' -> '" + dst + "'";
    Napi::Error e = Napi::Error::New(env, msg);
    Napi::Object obj = e.Value();
    obj.Set("errno", Napi::Number::New(env, -err));
    obj.Set("code", Napi::String::New(env, ErrnoToCode(err)));
    obj.Set("syscall", Napi::String::New(env, "copyfile"));
    obj.Set("path", Napi::String::New(env, src));
    obj.Set("dest", Napi::String::New(env, dst));
    return e;
}

static Napi::Error MakeUvError(Napi::Env env, int uv_err,
                               const std::string& src,
                               const std::string& dst) {
    std::string msg = std::string(uv_err_name(uv_err)) + ": " + uv_strerror(uv_err) +
                      ", copyfile '" + src + "' -> '" + dst + "'";
    Napi::Error e = Napi::Error::New(env, msg);
    Napi::Object obj = e.Value();
    obj.Set("errno", Napi::Number::New(env, uv_err));
    obj.Set("code", Napi::String::New(env, uv_err_name(uv_err)));
    obj.Set("syscall", Napi::String::New(env, "copyfile"));
    obj.Set("path", Napi::String::New(env, src));
    obj.Set("dest", Napi::String::New(env, dst));
    return e;
}

class CopyFileWorker : public Napi::AsyncWorker {
public:
    CopyFileWorker(Napi::Env env, std::string src, std::string dst,
                   int mode, Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env),
          src_(std::move(src)),
          dst_(std::move(dst)),
          mode_(mode),
          deferred_(deferred),
          err_(0),
          uv_err_(0),
          use_native_((mode & UV_FS_COPYFILE_FICLONE_FORCE) != 0) {}

    void Execute() override {
        if (use_native_) {
            err_ = DoClone(src_.c_str(), dst_.c_str(), mode_);
            if (err_ != 0) SetError("clone failed");
        } else {
            uv_err_ = DoUvCopyFile(src_.c_str(), dst_.c_str(), mode_);
            if (uv_err_ != 0) SetError("copy failed");
        }
    }

    void OnOK() override {
        deferred_.Resolve(Env().Undefined());
    }

    void OnError(const Napi::Error& /*error*/) override {
        Napi::Error e = use_native_
            ? MakeSystemError(Env(), err_, src_, dst_)
            : MakeUvError(Env(), uv_err_, src_, dst_);
        deferred_.Reject(e.Value());
    }

private:
    std::string src_;
    std::string dst_;
    int mode_;
    Napi::Promise::Deferred deferred_;
    int err_;
    int uv_err_;
    bool use_native_;
};

static Napi::Value CopyFile(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
        Napi::TypeError::New(env, "src and dst must be strings")
            .ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string src = info[0].As<Napi::String>().Utf8Value();
    std::string dst = info[1].As<Napi::String>().Utf8Value();
    int mode = (info.Length() > 2 && info[2].IsNumber())
               ? info[2].As<Napi::Number>().Int32Value()
               : 0;

    Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
    auto* worker = new CopyFileWorker(env, std::move(src), std::move(dst),
                                       mode, deferred);
    worker->Queue();
    return deferred.Promise();
}

static Napi::Value CopyFileSync(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
        Napi::TypeError::New(env, "src and dst must be strings")
            .ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string src = info[0].As<Napi::String>().Utf8Value();
    std::string dst = info[1].As<Napi::String>().Utf8Value();
    int mode = (info.Length() > 2 && info[2].IsNumber())
               ? info[2].As<Napi::Number>().Int32Value()
               : 0;

    if (mode & UV_FS_COPYFILE_FICLONE_FORCE) {
        int err = DoClone(src.c_str(), dst.c_str(), mode);
        if (err != 0) {
            MakeSystemError(env, err, src, dst).ThrowAsJavaScriptException();
            return env.Undefined();
        }
    } else {
        int r = DoUvCopyFile(src.c_str(), dst.c_str(), mode);
        if (r != 0) {
            MakeUvError(env, r, src, dst).ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }

    return env.Undefined();
}

static Napi::Value IsCloneSupported(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "path must be a string")
            .ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string pathStr = info[0].As<Napi::String>().Utf8Value();

    struct attrlist al = {};
    al.bitmapcount = ATTR_BIT_MAP_COUNT;
    al.volattr = ATTR_VOL_CAPABILITIES;

    struct {
        uint32_t length;
        vol_capabilities_attr_t caps;
    } buf;

    if (::getattrlist(pathStr.c_str(), &al, &buf, sizeof(buf), 0) != 0) {
        return Napi::Boolean::New(env, false);
    }

    bool supported =
        (buf.caps.valid[VOL_CAPABILITIES_INTERFACES] & VOL_CAP_INT_CLONE) &&
        (buf.caps.capabilities[VOL_CAPABILITIES_INTERFACES] & VOL_CAP_INT_CLONE);

    return Napi::Boolean::New(env, supported);
}

static Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("copyFile", Napi::Function::New(env, CopyFile));
    exports.Set("copyFileSync", Napi::Function::New(env, CopyFileSync));
    exports.Set("isCloneSupported", Napi::Function::New(env, IsCloneSupported));

    Napi::Object constants = Napi::Object::New(env);
    constants.Set("COPYFILE_EXCL",
                  Napi::Number::New(env, UV_FS_COPYFILE_EXCL));
    constants.Set("COPYFILE_FICLONE",
                  Napi::Number::New(env, UV_FS_COPYFILE_FICLONE));
    constants.Set("COPYFILE_FICLONE_FORCE",
                  Napi::Number::New(env, UV_FS_COPYFILE_FICLONE_FORCE));
    exports.Set("constants", constants);

    return exports;
}

NODE_API_MODULE(vscode_fs, Init)
