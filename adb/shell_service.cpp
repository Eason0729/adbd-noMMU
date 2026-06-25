/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Functionality for launching and managing shell subprocesses.
//
// There are two types of subprocesses, PTY or raw. PTY is typically used for
// an interactive session, raw for non-interactive. There are also two methods
// of communication with the subprocess, passing raw data or using a simple
// protocol to wrap packets. The protocol allows separating stdout/stderr and
// passing the exit code back, but is not backwards compatible.
//   ----------------+--------------------------------------
//   Type  Protocol  |   Exit code?  Separate stdout/stderr?
//   ----------------+--------------------------------------
//   PTY   No        |   No          No
//   Raw   No        |   No          No
//   PTY   Yes       |   Yes         No
//   Raw   Yes       |   Yes         Yes
//   ----------------+--------------------------------------
//
// Non-protocol subprocesses work by passing subprocess stdin/out/err through
// a single pipe which is registered with a local socket in adbd. The local
// socket uses the fdevent loop to pass raw data between this pipe and the
// transport, which then passes data back to the adb client. Cleanup is done by
// waiting in a separate thread for the subprocesses to exit and then signaling
// a separate fdevent to close out the local socket from the main loop.
//
// ------------------+-------------------------+------------------------------
//   Subprocess      |  adbd subprocess thread |   adbd main fdevent loop
// ------------------+-------------------------+------------------------------
//                   |                         |
//   stdin/out/err <----------------------------->       LocalSocket
//      |            |                         |
//      |            |      Block on exit      |
//      |            |           *             |
//      v            |           *             |
//     Exit         --->      Unblock          |
//                   |           |             |
//                   |           v             |
//                   |   Notify shell exit FD --->    Close LocalSocket
// ------------------+-------------------------+------------------------------
//
// The protocol requires the thread to intercept stdin/out/err in order to
// wrap/unwrap data with shell protocol packets.
//
// ------------------+-------------------------+------------------------------
//   Subprocess      |  adbd subprocess thread |   adbd main fdevent loop
// ------------------+-------------------------+------------------------------
//                   |                         |
//     stdin/out   <--->      Protocol       <--->       LocalSocket
//     stderr       --->      Protocol        --->       LocalSocket
//       |           |                         |
//       v           |                         |
//      Exit        --->  Exit code protocol  --->       LocalSocket
//                   |           |             |
//                   |           v             |
//                   |   Notify shell exit FD --->    Close LocalSocket
// ------------------+-------------------------+------------------------------
//
// An alternate approach is to put the protocol wrapping/unwrapping in the main
// fdevent loop, which has the advantage of being able to re-use the existing
// select() code for handling data streams. However, implementation turned out
// to be more complex due to partial reads and non-blocking I/O so this model
// was chosen instead.

#define TRACE_TAG SHELL

#include "sysdeps.h"

#include "shell_service.h"

#include <errno.h>
#include <pty.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/select.h>
#include <termios.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <paths.h>
#include <log/log.h>

#include "adb.h"
#include "adb_io.h"
#include "adb_trace.h"
#include "adb_utils.h"
#include "security_log_tags.h"

namespace {

// On noMMU Linux we cannot use fork()/forkpty() because there is no MMU to
// copy-on-write the address space. Instead we use posix_spawn(), which on
// uClibc/musl is implemented over vfork() and handles the FD-table hand-off
// atomically. All child-side setup (dup2 of std fds, closing parent-side FDs)
// must be expressed via posix_spawn_file_actions_t instead of being done in
// the child process body, since under vfork the child shares the parent's
// memory and FD table until execve().
#if defined(ADB_NOMMU)
#define ADB_USE_POSIX_SPAWN 1
#endif

void init_subproc_child()
{
    setsid();

    // Set OOM score adjustment to prevent killing. Skipped on noMMU since
    // the child runs in the parent's address space until exec; the daemon
    // itself should not have its OOM score lowered here.
#if !defined(ADB_NOMMU)
    int fd = adb_open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        adb_write(fd, "0", 1);
        adb_close(fd);
    } else {
       D("adb: unable to update oom_score_adj");
    }
#endif
}

// Reads from |fd| until close or failure.
std::string ReadAll(int fd) {
    char buffer[512];
    std::string received;

    while (1) {
        int bytes = adb_read(fd, buffer, sizeof(buffer));
        if (bytes <= 0) {
            break;
        }
        received.append(buffer, bytes);
    }

    return received;
}

// Creates a bidirectional pipe channel and saves the endpoints.
bool CreateChannelPair(unique_fd* a_read, unique_fd* a_write,
                       unique_fd* b_read, unique_fd* b_write) {
    adb_channel a, b;
    if (adb_channel_pair(&a, &b) < 0) {
        PLOG(ERROR) << "cannot create channel pair";
        return false;
    }
    a_read->reset(a.read_fd);
    a_write->reset(a.write_fd);
    b_read->reset(b.read_fd);
    b_write->reset(b.write_fd);
    return true;
}

// Creates a uni-directional pipe. pipe_read gets the read end, pipe_write the write end.
bool CreatePipe(unique_fd* pipe_read, unique_fd* pipe_write) {
    int fds[2];
    if (adb_pipe(fds) < 0) {
        PLOG(ERROR) << "cannot create pipe";
        return false;
    }
    pipe_read->reset(fds[0]);
    pipe_write->reset(fds[1]);
    return true;
}

#ifdef __GNUC__
#define _Nonnull
#endif

class Subprocess {
  public:
    Subprocess(const std::string& command, const char* terminal_type,
               SubprocessType type, SubprocessProtocol protocol);
    ~Subprocess();

    const std::string& command() const { return command_; }

    int ReleaseLocalSocket() {
        int rfd = local_socket_read_sfd_.release();
        local_socket_write_sfd_.release();
        return rfd;
    }
    adb_channel ReleaseLocalSocketChannel() {
        local_socket_read_fd_for_notify_ = local_socket_read_sfd_.get();
        adb_channel ch = {local_socket_read_sfd_.release(),
                          local_socket_write_sfd_.release()};
        return ch;
    }

    pid_t pid() const { return pid_; }

    // Sets up FDs, forks a subprocess, starts the subprocess manager thread,
    // and exec's the child. Returns false and sets error on failure.
    bool ForkAndExec(std::string* _Nonnull error);

    // Start the subprocess manager thread. Consumes the subprocess, regardless of success.
    // Returns false and sets error on failure.
    static bool StartThread(std::unique_ptr<Subprocess> subprocess,
                            std::string* _Nonnull error);

  private:
    // Opens the file at |pts_name|.
    int OpenPtyChildFd(const char* pts_name, unique_fd* error_sfd);

    static void ThreadHandler(void* userdata);
    void PassDataStreams();
    void WaitForExit();

    unique_fd* SelectLoop(fd_set* master_read_set_ptr,
                          fd_set* master_write_set_ptr);

    // Input/output stream handlers. Success returns nullptr, failure returns
    // a pointer to the failed FD.
    unique_fd* PassInput();
    unique_fd* PassOutput(unique_fd* sfd, ShellProtocol::Id id);

    const std::string command_;
    const std::string terminal_type_;
    bool make_pty_raw_ = false;
    SubprocessType type_;
    SubprocessProtocol protocol_;
    pid_t pid_ = -1;
    // Local socket channel — the main-loop side of the protocol channel.
    // For kNone protocol, read_fd==write_fd (a single PTY fd); for kShell
    // protocol these are distinct pipe ends.
    unique_fd local_socket_read_sfd_, local_socket_write_sfd_;

    // Shell protocol variables.
    // stdinout: parent reads child stdout from read, writes child stdin to write.
    // For PTY mode, read==write (single PTY fd).
    // stderr: parent reads child stderr (uni-directional pipe, read only).
    // protocol: management thread reads from read, writes to write.
    unique_fd stdinout_read_sfd_, stdinout_write_sfd_, stderr_sfd_;
    unique_fd protocol_read_sfd_, protocol_write_sfd_;
    std::unique_ptr<ShellProtocol> input_, output_;
    size_t input_bytes_left_ = 0;
    int local_socket_read_fd_for_notify_ = -1;

    DISALLOW_COPY_AND_ASSIGN(Subprocess);
};

Subprocess::Subprocess(const std::string& command, const char* terminal_type,
                       SubprocessType type, SubprocessProtocol protocol)
    : command_(command),
      terminal_type_(terminal_type ? terminal_type : ""),
      type_(type),
      protocol_(protocol) {
    // If we aren't using the shell protocol we must allocate a PTY to properly close the
    // subprocess. PTYs automatically send SIGHUP to the slave-side process when the master side
    // of the PTY closes, which we rely on. If we use a raw pipe, processes that don't read/write,
    // e.g. screenrecord, will never notice the broken pipe and terminate.
    // The shell protocol doesn't require a PTY because it's always monitoring the local socket FD
    // with select() and will send SIGHUP manually to the child process.
    if (protocol_ == SubprocessProtocol::kNone && type_ == SubprocessType::kRaw) {
        // Disable PTY input/output processing since the client is expecting raw data.
        D("Can't create raw subprocess without shell protocol, using PTY in raw mode instead");
        type_ = SubprocessType::kPty;
        make_pty_raw_ = true;
    }
}

Subprocess::~Subprocess() {
    WaitForExit();
}

bool Subprocess::ForkAndExec(std::string* error) {
    unique_fd child_stdin_read, child_stdout_write, child_stderr_write;
    unique_fd parent_error_sfd, child_error_sfd;
    char pts_name[PATH_MAX];

    if (command_.empty()) {
        __android_log_security_bswrite(SEC_TAG_ADB_SHELL_INTERACTIVE, "");
    } else {
        __android_log_security_bswrite(SEC_TAG_ADB_SHELL_CMD, command_.c_str());
    }

    // Create a pipe for the fork() child to report any errors back to the parent. Since we
    // use threads, logging directly from the child might deadlock due to locks held in another
    // thread during the fork. On noMMU we use posix_spawn, which reports exec failures via its
    // return value, so this channel is not needed.
#if !defined(ADB_NOMMU)
    if (!CreatePipe(&parent_error_sfd, &child_error_sfd)) {
        *error = android::base::StringPrintf(
            "failed to create pipe for subprocess error reporting: %s", strerror(errno));
        return false;
    }
#endif

    // Construct the environment for the child before we fork.
    passwd* pw = getpwuid(getuid());
    std::unordered_map<std::string, std::string> env;
    if (environ) {
        char** current = environ;
        while (char* env_cstr = *current++) {
            std::string env_string = env_cstr;
            char* delimiter = strchr(&env_string[0], '=');

            // Drop any values that don't contain '='.
            if (delimiter) {
                *delimiter++ = '\0';
                env[env_string.c_str()] = delimiter;
            }
        }
    }

    if (pw != nullptr) {
        // TODO: $HOSTNAME? Normally bash automatically sets that, but mksh doesn't.
        env["HOME"] = pw->pw_dir;
        env["LOGNAME"] = pw->pw_name;
        env["USER"] = pw->pw_name;
        env["SHELL"] = pw->pw_shell;
    }

    if (!terminal_type_.empty()) {
        env["TERM"] = terminal_type_;
    }

    std::vector<std::string> joined_env;
    for (auto it : env) {
        const char* key = it.first.c_str();
        const char* value = it.second.c_str();
        joined_env.push_back(android::base::StringPrintf("%s=%s", key, value));
    }

    std::vector<const char*> cenv;
    for (const std::string& str : joined_env) {
        cenv.push_back(str.c_str());
    }
    cenv.push_back(nullptr);

    if (type_ == SubprocessType::kPty) {
#if defined(ADB_NOMMU)
        // openpty() creates a PTY pair without forking. The parent keeps the
        // master side and the child will open the slave side via the
        // posix_spawn file-actions.
        int master_fd = -1, slave_fd = -1;
        if (openpty(&master_fd, &slave_fd, pts_name, nullptr, nullptr) != 0) {
            *error = android::base::StringPrintf("openpty failed: %s", strerror(errno));
            return false;
        }
        // A PTY is bidirectional through a single fd.
        stdinout_read_sfd_.reset(master_fd);
        stdinout_write_sfd_.reset(master_fd);
        child_stdin_read.reset(slave_fd);
        child_stdout_write.reset(slave_fd);
        // Apply raw-mode to the slave side in the parent since we can't run
        // OpenPtyChildFd() inside a vforked child.
        if (make_pty_raw_) {
            termios tattr;
            if (tcgetattr(slave_fd, &tattr) == -1) {
                *error = android::base::StringPrintf("tcgetattr failed: %s", strerror(errno));
                return false;
            }
            cfmakeraw(&tattr);
            if (tcsetattr(slave_fd, TCSADRAIN, &tattr) == -1) {
                *error = android::base::StringPrintf("tcsetattr failed: %s", strerror(errno));
                return false;
            }
        }
#else
        int fd;
        pid_ = forkpty(&fd, pts_name, nullptr, nullptr);
        if (pid_ > 0) {
          stdinout_read_sfd_.reset(fd);
          stdinout_write_sfd_.reset(fd);
        }
#endif
    } else {
        // Raw subprocess: create a bidirectional channel (two pipes) for
        // stdin/stdout. Parent reads child stdout from stdinout_read, writes
        // child stdin to stdinout_write.
        if (!CreateChannelPair(&stdinout_read_sfd_, &stdinout_write_sfd_,
                               &child_stdin_read, &child_stdout_write)) {
            *error = android::base::StringPrintf("failed to create channel for stdin/out: %s",
                                                 strerror(errno));
            return false;
        }
        // Raw subprocess + shell protocol allows for splitting stderr.
        if (protocol_ == SubprocessProtocol::kShell &&
                !CreatePipe(&stderr_sfd_, &child_stderr_write)) {
            *error = android::base::StringPrintf("failed to create pipe for stderr: %s",
                                                 strerror(errno));
            return false;
        }
#if !defined(ADB_NOMMU)
        pid_ = fork();
#endif
    }

#if !defined(ADB_NOMMU)
    if (pid_ == -1) {
        *error = android::base::StringPrintf("fork failed: %s", strerror(errno));
        return false;
    }

    if (pid_ == 0) {
        // Subprocess child.
        init_subproc_child();

        if (type_ == SubprocessType::kPty) {
            int pty_child = OpenPtyChildFd(pts_name, &child_error_sfd);
            child_stdin_read.reset(pty_child);
            child_stdout_write.reset(pty_child);
        }

        dup2(child_stdin_read, STDIN_FILENO);
        dup2(child_stdout_write, STDOUT_FILENO);
        dup2(child_stderr_write != -1 ? child_stderr_write : child_stdout_write, STDERR_FILENO);

        // exec doesn't trigger destructors, close the FDs manually.
        stdinout_read_sfd_.reset(-1);
        stdinout_write_sfd_.reset(-1);
        stderr_sfd_.reset(-1);
        child_stdin_read.reset(-1);
        child_stdout_write.reset(-1);
        child_stderr_write.reset(-1);
        parent_error_sfd.reset(-1);
        close_on_exec(child_error_sfd);

        if (command_.empty()) {
            execle(_PATH_BSHELL, _PATH_BSHELL, "-", nullptr, cenv.data());
        } else {
            execle(_PATH_BSHELL, _PATH_BSHELL, "-c", command_.c_str(), nullptr, cenv.data());
        }
        WriteFdExactly(child_error_sfd, "exec '" _PATH_BSHELL "' failed: ");
        WriteFdExactly(child_error_sfd, strerror(errno));
        child_error_sfd.reset(-1);
        _Exit(1);
    }
#else
    // ---- noMMU path: use posix_spawn ----
    {
        // Build the argv for /bin/sh.
        std::vector<const char*> argv;
        argv.push_back(_PATH_BSHELL);
        if (command_.empty()) {
            argv.push_back("-");
        } else {
            argv.push_back("-c");
            argv.push_back(command_.c_str());
        }
        argv.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawnattr_t attrs;
        posix_spawn_file_actions_init(&actions);
        posix_spawnattr_init(&attrs);

        // dup2 the child's std fds into place, then close the originals in
        // the child.
        posix_spawn_file_actions_adddup2(&actions, child_stdin_read.get(), STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, child_stdout_write.get(), STDOUT_FILENO);
        if (child_stderr_write != -1) {
            posix_spawn_file_actions_adddup2(&actions, child_stderr_write.get(), STDERR_FILENO);
        } else {
            posix_spawn_file_actions_adddup2(&actions, child_stdout_write.get(), STDERR_FILENO);
        }
        posix_spawn_file_actions_addclose(&actions, child_stdin_read.get());
        posix_spawn_file_actions_addclose(&actions, child_stdout_write.get());
        if (child_stderr_write != -1) {
            posix_spawn_file_actions_addclose(&actions, child_stderr_write.get());
        }
        // Close parent-side FDs in the child so it doesn't hold them open.
        if (stdinout_read_sfd_.get() >= 0) {
            posix_spawn_file_actions_addclose(&actions, stdinout_read_sfd_.get());
        }
        if (stdinout_write_sfd_.get() >= 0 &&
            stdinout_write_sfd_.get() != stdinout_read_sfd_.get()) {
            posix_spawn_file_actions_addclose(&actions, stdinout_write_sfd_.get());
        }
        if (stderr_sfd_.get() >= 0) {
            posix_spawn_file_actions_addclose(&actions, stderr_sfd_.get());
        }

        // On noMMU we can't safely run init_subproc_child() (setsid + oom
        // write) in the vforked child, so skip it. The daemon is typically a
        // single-session process on uClinux and the PTY already provides its
        // own session semantics for interactive shells.
        (void)attrs;

        int spawn_err = posix_spawn(&pid_, _PATH_BSHELL, &actions, &attrs,
                                    const_cast<char* const*>(argv.data()),
                                    const_cast<char* const*>(cenv.data()));
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attrs);

        if (spawn_err != 0) {
            *error = android::base::StringPrintf(
                "posix_spawn '%s' failed: %s", _PATH_BSHELL, strerror(spawn_err));
            pid_ = -1;
            return false;
        }
    }
#endif

    // Subprocess parent.
    D("subprocess parent: stdin/stdout FD = %d/%d, stderr FD = %d",
      stdinout_read_sfd_.get(), stdinout_write_sfd_.get(), stderr_sfd_.get());

#if !defined(ADB_NOMMU)
    // Wait to make sure the subprocess exec'd without error. On noMMU the
    // posix_spawn return value already covers exec failure.
    child_error_sfd.reset(-1);
    std::string error_message = ReadAll(parent_error_sfd);
    if (!error_message.empty()) {
        *error = error_message;
        return false;
    }
#else
    // On noMMU the child's dup targets (the slave PTY / child pipe ends)
    // are owned by child_stdin_read / child_stdout_write / child_stderr_write
    // and must be closed in the parent now that the child has its own dup'd
    // copies.
    child_stdin_read.reset(-1);
    child_stdout_write.reset(-1);
    child_stderr_write.reset(-1);
#endif

    D("subprocess parent: exec completed");
    if (protocol_ == SubprocessProtocol::kNone) {
        // No protocol: all streams pass through the stdinout FD and hook
        // directly into the local socket for raw data transfer.
        local_socket_read_sfd_.reset(stdinout_read_sfd_.release());
        local_socket_write_sfd_.reset(stdinout_write_sfd_.release());
    } else {
        // Shell protocol: create a bidirectional channel to intercept data.
        if (!CreateChannelPair(&protocol_read_sfd_, &protocol_write_sfd_,
                               &local_socket_read_sfd_, &local_socket_write_sfd_)) {
            *error = android::base::StringPrintf(
                "failed to create channel to intercept data: %s", strerror(errno));
            kill(pid_, SIGKILL);
            return false;
        }
        D("protocol FD = %d/%d", protocol_read_sfd_.get(), protocol_write_sfd_.get());

        input_.reset(new ShellProtocol(protocol_read_sfd_.get(), protocol_write_sfd_.get()));
        output_.reset(new ShellProtocol(protocol_read_sfd_.get(), protocol_write_sfd_.get()));
        if (!input_ || !output_) {
            *error = "failed to allocate shell protocol objects";
            kill(pid_, SIGKILL);
            return false;
        }

        // Don't let reads/writes to the subprocess block our thread. This isn't
        // likely but could happen under unusual circumstances, such as if we
        // write a ton of data to stdin but the subprocess never reads it and
        // the pipe fills up.
        for (int fd : {stdinout_read_sfd_.get(), stdinout_write_sfd_.get(), stderr_sfd_.get()}) {
            if (fd >= 0) {
                if (!set_file_block_mode(fd, false)) {
                    *error = android::base::StringPrintf(
                        "failed to set non-blocking mode for fd %d", fd);
                    kill(pid_, SIGKILL);
                    return false;
                }
            }
        }
    }

    D("subprocess parent: completed");
    return true;
}

bool Subprocess::StartThread(std::unique_ptr<Subprocess> subprocess, std::string* error) {
    Subprocess* raw = subprocess.release();
    if (!adb_thread_create(ThreadHandler, raw)) {
        *error =
            android::base::StringPrintf("failed to create subprocess thread: %s", strerror(errno));
        kill(raw->pid_, SIGKILL);
        return false;
    }

    return true;
}

int Subprocess::OpenPtyChildFd(const char* pts_name, unique_fd* error_sfd) {
    int child_fd = adb_open(pts_name, O_RDWR | O_CLOEXEC);
    if (child_fd == -1) {
        // Don't use WriteFdFmt; since we're in the fork() child we don't want
        // to allocate any heap memory to avoid race conditions.
        const char* messages[] = {"child failed to open pseudo-term slave ",
                                  pts_name, ": ", strerror(errno)};
        for (const char* message : messages) {
            WriteFdExactly(*error_sfd, message);
        }
        abort();
    }

    if (make_pty_raw_) {
        termios tattr;
        if (tcgetattr(child_fd, &tattr) == -1) {
            int saved_errno = errno;
            WriteFdExactly(*error_sfd, "tcgetattr failed: ");
            WriteFdExactly(*error_sfd, strerror(saved_errno));
            abort();
        }

        cfmakeraw(&tattr);
        if (tcsetattr(child_fd, TCSADRAIN, &tattr) == -1) {
            int saved_errno = errno;
            WriteFdExactly(*error_sfd, "tcsetattr failed: ");
            WriteFdExactly(*error_sfd, strerror(saved_errno));
            abort();
        }
    }

    return child_fd;
}

void Subprocess::ThreadHandler(void* userdata) {
    Subprocess* subprocess = reinterpret_cast<Subprocess*>(userdata);

    adb_thread_setname(android::base::StringPrintf(
            "shell srvc %d", subprocess->pid()));

    D("passing data streams for PID %d", subprocess->pid());
    subprocess->PassDataStreams();

    D("deleting Subprocess for PID %d", subprocess->pid());
    delete subprocess;
}

void Subprocess::PassDataStreams() {
    if (protocol_read_sfd_ == -1) {
        return;
    }

    // Start by trying to read from the protocol FD, stdout, and stderr.
    fd_set master_read_set, master_write_set;
    FD_ZERO(&master_read_set);
    FD_ZERO(&master_write_set);
    for (unique_fd* sfd : {&protocol_read_sfd_, &stdinout_read_sfd_, &stderr_sfd_}) {
        if (*sfd != -1) {
            FD_SET(*sfd, &master_read_set);
        }
    }

    // Pass data until the protocol FD or both the subprocess pipes die, at
    // which point we can't pass any more data.
    while (protocol_read_sfd_ != -1 &&
           (stdinout_read_sfd_ != -1 || stderr_sfd_ != -1)) {
        unique_fd* dead_sfd = SelectLoop(&master_read_set, &master_write_set);
        if (dead_sfd) {
            D("closing FD %d", dead_sfd->get());
            FD_CLR(*dead_sfd, &master_read_set);
            FD_CLR(*dead_sfd, &master_write_set);
            if (dead_sfd == &protocol_read_sfd_) {
                // Using SIGHUP is a decent general way to indicate that the
                // controlling process is going away. If specific signals are
                // needed (e.g. SIGINT), pass those through the shell protocol
                // and only fall back on this for unexpected closures.
                D("protocol FD died, sending SIGHUP to pid %d", pid_);
                kill(pid_, SIGHUP);

                // We also need to close the pipes connected to the child process
                // so that if it ignores SIGHUP and continues to write data it
                // won't fill up the pipe and block.
                stdinout_read_sfd_.clear();
                stdinout_write_sfd_.clear();
                stderr_sfd_.clear();
            }
            dead_sfd->clear();
        }
    }
}

namespace {

inline bool ValidAndInSet(const unique_fd& sfd, fd_set* set) {
    return sfd != -1 && FD_ISSET(sfd, set);
}

}   // namespace

unique_fd* Subprocess::SelectLoop(fd_set* master_read_set_ptr,
                                  fd_set* master_write_set_ptr) {
    fd_set read_set, write_set;
    int select_n = std::max(std::max(protocol_read_sfd_, stdinout_read_sfd_), stderr_sfd_) + 1;
    // The stdin write fd may have a higher number than the read fds.
    if (stdinout_write_sfd_ >= select_n) select_n = stdinout_write_sfd_ + 1;
    unique_fd* dead_sfd = nullptr;

    // Keep calling select() and passing data until an FD closes/errors.
    while (!dead_sfd) {
        memcpy(&read_set, master_read_set_ptr, sizeof(read_set));
        memcpy(&write_set, master_write_set_ptr, sizeof(write_set));
        if (select(select_n, &read_set, &write_set, nullptr, nullptr) < 0) {
            if (errno == EINTR) {
                continue;
            } else {
                PLOG(ERROR) << "select failed, closing subprocess pipes";
                stdinout_read_sfd_.reset(-1);
                stdinout_write_sfd_.reset(-1);
                stderr_sfd_.reset(-1);
                return nullptr;
            }
        }

        // Read stdout, write to protocol FD.
        if (ValidAndInSet(stdinout_read_sfd_, &read_set)) {
            dead_sfd = PassOutput(&stdinout_read_sfd_, ShellProtocol::kIdStdout);
        }

        // Read stderr, write to protocol FD.
        if (!dead_sfd && ValidAndInSet(stderr_sfd_, &read_set)) {
            dead_sfd = PassOutput(&stderr_sfd_, ShellProtocol::kIdStderr);
        }

        // Read protocol FD, write to stdin.
        if (!dead_sfd && ValidAndInSet(protocol_read_sfd_, &read_set)) {
            dead_sfd = PassInput();
            // If we didn't finish writing, block on stdin write.
            if (input_bytes_left_) {
                FD_CLR(protocol_read_sfd_, master_read_set_ptr);
                FD_SET(stdinout_write_sfd_, master_write_set_ptr);
            }
        }

        // Continue writing to stdin; only happens if a previous write blocked.
        if (!dead_sfd && ValidAndInSet(stdinout_write_sfd_, &write_set)) {
            dead_sfd = PassInput();
            // If we finished writing, go back to blocking on protocol read.
            if (!input_bytes_left_) {
                FD_SET(protocol_read_sfd_, master_read_set_ptr);
                FD_CLR(stdinout_write_sfd_, master_write_set_ptr);
            }
        }
    }  // while (!dead_sfd)

    return dead_sfd;
}

unique_fd* Subprocess::PassInput() {
    // Only read a new packet if we've finished writing the last one.
    if (!input_bytes_left_) {
        if (!input_->Read()) {
            // Read() uses ReadFdExactly() which sets errno to 0 on EOF.
            if (errno != 0) {
                PLOG(ERROR) << "error reading protocol FD " << protocol_read_sfd_;
            }
            return &protocol_read_sfd_;
        }

        if (stdinout_write_sfd_ != -1) {
            switch (input_->id()) {
                case ShellProtocol::kIdWindowSizeChange:
                    int rows, cols, x_pixels, y_pixels;
                    if (sscanf(input_->data(), "%dx%d,%dx%d",
                               &rows, &cols, &x_pixels, &y_pixels) == 4) {
                        winsize ws;
                        ws.ws_row = rows;
                        ws.ws_col = cols;
                        ws.ws_xpixel = x_pixels;
                        ws.ws_ypixel = y_pixels;
                        ioctl(stdinout_read_sfd_, TIOCSWINSZ, &ws);
                    }
                    break;
                case ShellProtocol::kIdStdin:
                    input_bytes_left_ = input_->data_length();
                    break;
                case ShellProtocol::kIdCloseStdin:
                    if (type_ == SubprocessType::kRaw) {
                        // Close the write end of the stdin pipe to signal EOF
                        // to the child while keeping the read end (stdout)
                        // open. For PTY mode we can't half-close.
                        if (stdinout_write_sfd_.get() >= 0 &&
                            stdinout_write_sfd_.get() != stdinout_read_sfd_.get()) {
                            stdinout_write_sfd_.reset(-1);
                            return nullptr;
                        }
                        PLOG(ERROR) << "failed to close stdin write FD";
                        return &stdinout_write_sfd_;
                    } else {
                        // PTYs can't close just input, so rather than close the
                        // FD and risk losing subprocess output, leave it open.
                        // This only happens if the client starts a PTY shell
                        // non-interactively which is rare and unsupported.
                        // If necessary, the client can manually close the shell
                        // with `exit` or by killing the adb client process.
                        D("can't close input for PTY FD %d", stdinout_read_sfd_.get());
                    }
                    break;
            }
        }
    }

    if (input_bytes_left_ > 0) {
        int index = input_->data_length() - input_bytes_left_;
        int bytes = adb_write(stdinout_write_sfd_, input_->data() + index, input_bytes_left_);
        if (bytes == 0 || (bytes < 0 && errno != EAGAIN)) {
            if (bytes < 0) {
                PLOG(ERROR) << "error writing stdin FD " << stdinout_write_sfd_;
            }
            // stdin is done, mark this packet as finished and we'll just start
            // dumping any further data received from the protocol FD.
            input_bytes_left_ = 0;
            return &stdinout_write_sfd_;
        } else if (bytes > 0) {
            input_bytes_left_ -= bytes;
        }
    }

    return nullptr;
}

unique_fd* Subprocess::PassOutput(unique_fd* sfd, ShellProtocol::Id id) {
    int bytes = adb_read(*sfd, output_->data(), output_->data_capacity());
    if (bytes == 0 || (bytes < 0 && errno != EAGAIN)) {
        // read() returns EIO if a PTY closes; don't report this as an error,
        // it just means the subprocess completed.
        if (bytes < 0 && !(type_ == SubprocessType::kPty && errno == EIO)) {
            PLOG(ERROR) << "error reading output FD " << *sfd;
        }
        return sfd;
    }

    if (bytes > 0 && !output_->Write(id, bytes)) {
        if (errno != 0) {
            PLOG(ERROR) << "error writing protocol FD " << protocol_write_sfd_;
        }
        return &protocol_read_sfd_;
    }

    return nullptr;
}

void Subprocess::WaitForExit() {
    int exit_code = 1;

    D("waiting for pid %d", pid_);
    while (true) {
        int status;
        if (pid_ == waitpid(pid_, &status, 0)) {
            D("post waitpid (pid=%d) status=%04x", pid_, status);
            if (WIFSIGNALED(status)) {
                exit_code = 0x80 | WTERMSIG(status);
                D("subprocess killed by signal %d", WTERMSIG(status));
                break;
            } else if (!WIFEXITED(status)) {
                D("subprocess didn't exit");
                break;
            } else if (WEXITSTATUS(status) >= 0) {
                exit_code = WEXITSTATUS(status);
                D("subprocess exit code = %d", WEXITSTATUS(status));
                break;
            }
        }
    }

    // If we have an open protocol FD send an exit packet.
    if (protocol_write_sfd_ != -1) {
        output_->data()[0] = exit_code;
        if (output_->Write(ShellProtocol::kIdExit, 1)) {
            D("wrote the exit code packet: %d", exit_code);
        } else {
            PLOG(ERROR) << "failed to write the exit code packet";
        }
        protocol_write_sfd_.reset(-1);
    }
    protocol_read_sfd_.reset(-1);

    // Pass the local socket read FD to the shell cleanup fdevent.
    if (SHELL_EXIT_NOTIFY_FD >= 0) {
        int fd = local_socket_read_fd_for_notify_;
        if (WriteFdExactly(SHELL_EXIT_NOTIFY_FD, &fd, sizeof(fd))) {
            D("passed fd %d to SHELL_EXIT_NOTIFY_FD (%d) for pid %d",
              fd, SHELL_EXIT_NOTIFY_FD, pid_);
            // The shell exit fdevent now owns the read FD and will close it once
            // the last bit of data flushes through.
            static_cast<void>(local_socket_read_sfd_.release());
            // Also close the write end since the fdevent only tracks the read fd.
            local_socket_write_sfd_.reset(-1);
        } else {
            PLOG(ERROR) << "failed to write fd " << fd
                        << " to SHELL_EXIT_NOTIFY_FD (" << SHELL_EXIT_NOTIFY_FD
                        << ") for pid " << pid_;
        }
    }
}

}  // namespace

// Create a pipe containing the error.
static adb_channel ReportError(SubprocessProtocol protocol, const std::string& message) {
    int pipefd[2];
    if (adb_pipe(pipefd) != 0) {
        LOG(ERROR) << "failed to create pipe to report error";
        return {-1, -1};
    }

    std::string buf = android::base::StringPrintf("error: %s\n", message.c_str());
    if (protocol == SubprocessProtocol::kShell) {
        ShellProtocol::Id id = ShellProtocol::kIdStderr;
        uint32_t length = buf.length();
        WriteFdExactly(pipefd[1], &id, sizeof(id));
        WriteFdExactly(pipefd[1], &length, sizeof(length));
    }

    WriteFdExactly(pipefd[1], buf.data(), buf.length());

    if (protocol == SubprocessProtocol::kShell) {
        ShellProtocol::Id id = ShellProtocol::kIdExit;
        uint32_t length = 1;
        char exit_code = 126;
        WriteFdExactly(pipefd[1], &id, sizeof(id));
        WriteFdExactly(pipefd[1], &length, sizeof(length));
        WriteFdExactly(pipefd[1], &exit_code, sizeof(exit_code));
    }

    adb_close(pipefd[1]);
    // Return a read-only channel: the write end is closed, so the reader
    // will drain the error data then get EOF. write_fd == -1 causes writes
    // to fail (which is fine — the remote just reads the error and closes).
    return {pipefd[0], -1};
}

adb_channel StartSubprocess(const char* name, const char* terminal_type,
                            SubprocessType type, SubprocessProtocol protocol) {
    D("starting %s subprocess (protocol=%s, TERM=%s): '%s'",
      type == SubprocessType::kRaw ? "raw" : "PTY",
      protocol == SubprocessProtocol::kNone ? "none" : "shell",
      terminal_type, name);

    auto subprocess = std::make_unique<Subprocess>(name, terminal_type, type, protocol);
    if (!subprocess) {
        LOG(ERROR) << "failed to allocate new subprocess";
        return ReportError(protocol, "failed to allocate new subprocess");
    }

    std::string error;
    if (!subprocess->ForkAndExec(&error)) {
        LOG(ERROR) << "failed to start subprocess: " << error;
        return ReportError(protocol, error);
    }

    // ReleaseLocalSocket returns the read fd; we also need the write fd.
    adb_channel local_socket = subprocess->ReleaseLocalSocketChannel();
    D("subprocess creation successful: local_socket_fd=%d/%d, pid=%d",
      local_socket.read_fd, local_socket.write_fd, subprocess->pid());

    if (!Subprocess::StartThread(std::move(subprocess), &error)) {
        LOG(ERROR) << "failed to start subprocess management thread: " << error;
        adb_channel_close(&local_socket);
        return ReportError(protocol, error);
    }

    return local_socket;
}
