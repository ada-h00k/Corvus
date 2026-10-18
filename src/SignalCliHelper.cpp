// Runs signal-cli outside the app's process tree (macOS bundle: Contents/Helpers/wren-signal-cli.app).
//
// The app sets LSFileQuarantineEnabled and its child processes inherit it, so signal-cli could not load
// the native libraries it unpacks to $TMPDIR. The app starts this program in place of signal-cli; it
// launches its own bundle through Launch Services (not quarantined), which runs signal-cli with the
// caller's stdin/stdout/stderr, arguments and environment and passes the exit status back. Files
// signal-cli reads must not be in protected folders (Desktop, Documents, …): the helper is another app.

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <pthread/spawn.h>
#include <signal.h>
#include <spawn.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {

constexpr int kStartTimeoutSeconds = 5; // Launch Services start of the helper (normally well below 1 s)
constexpr uint32_t kMaxPayload = 16u << 20;
constexpr int kStopSignals[] = {SIGTERM, SIGINT, SIGHUP};

[[noreturn]] void fail(const std::string &what, int code = 255)
{
    std::fprintf(stderr, "wren-signal-cli: %s\n", what.c_str());
    _exit(code);
}

// No helper: run signal-cli directly; quarantined, it may fail to load libsignal, but says so itself.
[[noreturn]] void runDirectly(char **argv, const std::string &why)
{
    std::fprintf(stderr, "wren-signal-cli: %s; running %s directly\n", why.c_str(), argv[1]);
    for (int s : kStopSignals) signal(s, SIG_DFL); // ignored signals would stay ignored after exec
    execvp(argv[1], argv + 1);
    fail(std::string("cannot start ") + argv[1] + ": " + std::strerror(errno), 127);
}

bool writeAll(int fd, const void *data, size_t n)
{
    const char *p = static_cast<const char *>(data);
    while (n > 0) {
        const ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= size_t(w);
    }
    return true;
}

bool readAll(int fd, void *data, size_t n)
{
    char *p = static_cast<char *>(data);
    while (n > 0) {
        const ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r;
        n -= size_t(r);
    }
    return true;
}

// Messages: helper -> relay 'E' errno (not started), 'X' wait status; relay -> helper 'T' (terminate).
bool sendMsg(int fd, char type, int32_t value)
{
    char b[5] = {type};
    std::memcpy(b + 1, &value, sizeof value);
    return writeAll(fd, b, sizeof b);
}

bool recvMsg(int fd, char &type, int32_t &value)
{
    char b[5];
    if (!readAll(fd, b, sizeof b)) return false;
    type = b[0];
    std::memcpy(&value, b + 1, sizeof value);
    return true;
}

bool socketAddress(const std::string &path, sockaddr_un &addr)
{
    addr = {};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

// ----------------------------------------------------------------- relay

int relay(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    // Termination requests (signals, or Wren quitting) arrive through kqueue from the start, so a stop
    // during startup still removes the socket directory.
    const int kq = kqueue();
    if (kq < 0) runDirectly(argv, "kqueue failed");
    for (int s : kStopSignals) {
        signal(s, SIG_IGN);
        struct kevent ev;
        EV_SET(&ev, s, EVFILT_SIGNAL, EV_ADD, 0, 0, nullptr);
        if (kevent(kq, &ev, 1, nullptr, 0, nullptr) < 0) runDirectly(argv, "kevent failed");
    }
    struct kevent parent;
    EV_SET(&parent, getppid(), EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, nullptr);
    if (getppid() == 1 || kevent(kq, &parent, 1, nullptr, 0, nullptr) < 0) _exit(128 + SIGTERM); // Wren gone

    // Private directory for the socket (mkdtemp creates it with mode 0700).
    const char *tmp = getenv("TMPDIR");
    std::string dir = tmp && *tmp ? tmp : "/tmp/";
    if (dir.back() != '/') dir += '/';
    dir += "wren-sc-XXXXXX";
    if (!mkdtemp(dir.data())) runDirectly(argv, "cannot create a temporary directory");
    const std::string path = dir + "/s";
    auto cleanup = [&] {
        unlink(path.c_str());
        rmdir(dir.c_str());
    };

    sockaddr_un addr;
    const int lsock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (!socketAddress(path, addr) || lsock < 0 || bind(lsock, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0 ||
        listen(lsock, 1) < 0) {
        cleanup();
        runDirectly(argv, "cannot create the helper socket");
    }

    // This bundle: <bundle>/Contents/MacOS/wren-signal-cli
    char exe[PATH_MAX], real[PATH_MAX];
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) != 0 || !realpath(exe, real)) {
        cleanup();
        runDirectly(argv, "cannot locate the helper app");
    }
    std::string bundle = real;
    for (int i = 0; i < 3; ++i) bundle.erase(bundle.rfind('/'));

    // Started by Launch Services, the helper does not inherit this process's quarantine.
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t stopSignals;
    sigemptyset(&stopSignals);
    for (int s : kStopSignals) sigaddset(&stopSignals, s);
    posix_spawnattr_setsigdefault(&attr, &stopSignals);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF);
    const char *openArgv[] = {"open", "-n", "-g", bundle.c_str(), "--args", "--serve", path.c_str(), nullptr};
    pid_t openPid;
    int status = 0;
    int rc = posix_spawn(&openPid, "/usr/bin/open", &fa, &attr, const_cast<char **>(openArgv), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc == 0)
        while (waitpid(openPid, &status, 0) < 0 && errno == EINTR) {}
    if (rc != 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        cleanup();
        runDirectly(argv, "cannot launch the helper app " + bundle);
    }

    struct kevent ev;
    EV_SET(&ev, lsock, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    kevent(kq, &ev, 1, nullptr, 0, nullptr);
    const timespec startTimeout = {kStartTimeoutSeconds, 0};
    bool terminate = false;
    int sock = -1;
    while (!terminate && sock < 0) {
        struct kevent e;
        const int k = kevent(kq, nullptr, 0, &e, 1, &startTimeout);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        if (e.filter == EVFILT_READ) sock = accept(lsock, nullptr, nullptr);
        else terminate = true;
    }
    close(lsock);
    cleanup();
    if (terminate) _exit(128 + SIGTERM);
    if (sock < 0) runDirectly(argv, "the helper app did not start");
    uid_t uid;
    gid_t gid;
    if (getpeereid(sock, &uid, &gid) != 0 || uid != getuid()) fail("unexpected connection to the helper socket");

    // Payload: counts, then NUL-terminated working directory, arguments and environment.
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) std::strcpy(cwd, "/");
    uint32_t counts[2] = {uint32_t(argc - 1), 0};
    for (char **e = environ; *e; ++e) ++counts[1];
    std::string payload(reinterpret_cast<const char *>(counts), sizeof counts);
    auto add = [&payload](const char *s) { payload.append(s).push_back('\0'); };
    add(cwd);
    for (int i = 1; i < argc; ++i) add(argv[i]);
    for (char **e = environ; *e; ++e) add(*e);

    // stdin/stdout/stderr go along with the first message: signal-cli uses Wren's pipes directly.
    uint32_t len = uint32_t(payload.size());
    iovec iov = {&len, sizeof len};
    const int fds[3] = {0, 1, 2};
    alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof fds)] = {};
    msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof ctrl;
    cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof fds);
    std::memcpy(CMSG_DATA(c), fds, sizeof fds);
    if (sendmsg(sock, &msg, 0) != ssize_t(sizeof len) || !writeAll(sock, payload.data(), payload.size()))
        fail("cannot talk to the helper app");

    // Forward termination requests until the exit status arrives.
    EV_SET(&ev, sock, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    if (kevent(kq, &ev, 1, nullptr, 0, nullptr) < 0) fail("kevent failed");
    bool terminateSent = false;

    for (;;) {
        if (terminate && !terminateSent) terminateSent = sendMsg(sock, 'T', 0);
        struct kevent e;
        const int k = kevent(kq, nullptr, 0, &e, 1, nullptr);
        if (k < 0 && errno == EINTR) continue;
        if (k < 0) fail("kevent failed");
        if (e.filter == EVFILT_SIGNAL || e.filter == EVFILT_PROC) {
            terminate = true;
            continue;
        }
        char type;
        int32_t value;
        if (!recvMsg(sock, type, value)) fail("the helper app quit unexpectedly");
        if (type == 'E') fail(std::string("cannot start ") + argv[1] + ": " + std::strerror(value), 127);
        if (type == 'X') // shell convention for a signal: 128 + number
            _exit(WIFEXITED(value) ? WEXITSTATUS(value) : 128 + WTERMSIG(value));
    }
}

// ---------------------------------------------------------------- helper

int serve(const char *path)
{
    signal(SIGPIPE, SIG_IGN);
    sockaddr_un addr;
    const int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (!socketAddress(path, addr) || sock < 0 || connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0)
        return 1;
    uid_t uid;
    gid_t gid;
    if (getpeereid(sock, &uid, &gid) != 0 || uid != getuid()) return 1;

    uint32_t len = 0;
    int fds[3];
    iovec iov = {&len, sizeof len};
    alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof fds)] = {};
    msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof ctrl;
    const ssize_t r = recvmsg(sock, &msg, MSG_WAITALL);
    const cmsghdr *c = CMSG_FIRSTHDR(&msg);
    if (r != ssize_t(sizeof len) || !c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
        c->cmsg_len != CMSG_LEN(sizeof fds) || len < 2 * sizeof(uint32_t) || len > kMaxPayload)
        return 1;
    std::memcpy(fds, CMSG_DATA(c), sizeof fds);

    std::string payload(len, '\0');
    if (!readAll(sock, payload.data(), len)) return 1;
    uint32_t counts[2];
    std::memcpy(counts, payload.data(), sizeof counts);
    std::vector<char *> strings;
    for (size_t pos = sizeof counts; pos < payload.size();) {
        const size_t end = payload.find('\0', pos);
        if (end == std::string::npos) return 1;
        strings.push_back(&payload[pos]);
        pos = end + 1;
    }
    if (counts[0] < 1 || strings.size() != 1 + size_t(counts[0]) + counts[1]) return 1;
    std::vector<char *> args(strings.begin() + 1, strings.begin() + 1 + counts[0]);
    std::vector<char *> env(strings.begin() + 1 + counts[0], strings.end());
    args.push_back(nullptr);
    env.push_back(nullptr);
    for (char *e : env) // posix_spawnp searches the caller's PATH: use Wren's
        if (e && std::strncmp(e, "PATH=", 5) == 0) setenv("PATH", e + 5, 1);
    if (chdir(strings[0]) != 0) (void)chdir("/");

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    for (int i = 0; i < 3; ++i) posix_spawn_file_actions_adddup2(&fa, fds[i], i);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_CLOEXEC_DEFAULT);
    // Not throttled like background work: signal-cli has to keep its connection alive.
    posix_spawnattr_set_qos_class_np(&attr, QOS_CLASS_USER_INITIATED);
    pid_t child;
    const int rc = posix_spawnp(&child, args[0], &fa, &attr, args.data(), env.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    for (int fd : fds) close(fd); // the relay must see EOF once signal-cli exits
    if (rc != 0) {
        sendMsg(sock, 'E', rc);
        return 0;
    }

    auto reportExit = [&](bool relayGone) {
        int st = 0;
        while (waitpid(child, &st, 0) < 0 && errno == EINTR) {}
        if (!relayGone) sendMsg(sock, 'X', st);
        return 0;
    };
    const int kq = kqueue();
    struct kevent ev;
    EV_SET(&ev, child, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, nullptr);
    if (kq < 0 || kevent(kq, &ev, 1, nullptr, 0, nullptr) < 0) return reportExit(false); // already exited
    EV_SET(&ev, sock, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    kevent(kq, &ev, 1, nullptr, 0, nullptr);

    for (;;) {
        struct kevent e;
        const int k = kevent(kq, nullptr, 0, &e, 1, nullptr);
        if (k < 0 && errno == EINTR) continue;
        if (k < 0) break;
        if (e.filter == EVFILT_PROC) return reportExit(false);
        char type;
        int32_t value;
        if (!recvMsg(sock, type, value)) break; // relay killed (QProcess::kill): end signal-cli the same way
        if (type == 'T') kill(child, SIGTERM);
    }
    kill(child, SIGKILL);
    return reportExit(true);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc >= 3 && std::strcmp(argv[1], "--serve") == 0) return serve(argv[2]);
    if (argc < 2) {
        std::fprintf(stderr, "usage: wren-signal-cli <signal-cli> [args...]\n");
        return 2;
    }
    return relay(argc, argv);
}
