#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/wait.h>

#include "engine.h"

using namespace zw;

static std::string env(const char* k, const std::string& def = "") {
    const char* v = getenv(k);
    return v && *v ? v : def;
}

// ---- start at login ---------------------------------------------------------------
//
// Installed by hand (/usr/local/bin/zrn-whatsapp), the engine writes two systemd user units next to the
// user's own and enables them for default.target. Systemd then starts the engine and the
// notifier at every login and restarts them on failure. A copy the app started is kept
// out by the locks: a second engine exits 3, a second notifier exits 0, and neither restarts.
// When the program is removed, ConditionPathExists keeps both units from starting;
// `zrn-whatsapp --daemon --remove-units` deletes them.

namespace fs = std::filesystem;

static fs::path unitDir() {
    return fs::path(env("XDG_CONFIG_HOME", env("HOME", ".") + "/.config")) / "systemd/user";
}
static const char* kUnits[] = {"zrn-ztwhatsappd.service", "zrn-ztwhatsapp-notify.service"};

// The installed program the units run. Where it is missing (removed), ConditionPathExists
// keeps both units from starting.
static const char* kProgram = "/usr/local/bin/zrn-whatsapp";

static std::string unitText(bool engine) {
    std::string prog = kProgram;
    std::ostringstream u;
    if (engine) {
        u << "[Unit]\nDescription=ztOS WhatsApp engine\n"
          << "ConditionPathExists=" << prog << "\n\n[Service]\n"
          << "ExecStart=" << prog << " --daemon\n"
          << "# Always: it must run for messages to arrive, whatever ended it (but a stop).\n"
          << "Restart=always\nRestartSec=5\n"
          << "# Exit code 3 means another instance already holds the lock.\n"
          << "RestartPreventExitStatus=3\nUMask=0077\nNoNewPrivileges=yes\n";
    } else {
        u << "[Unit]\nDescription=ztWhatsApp notifications\n"
          << "ConditionPathExists=" << prog << "\nAfter=zrn-ztwhatsappd.service\n"
          << "# Not Requires: it reconnects whenever the engine comes back.\n\n[Service]\n"
          << "Environment=ZTWHATSAPP_UI=" << prog << "\n"
          << "ExecStart=" << prog << " --notifier\n"
          << "# Exit 3 is a second copy finding the lock taken: no restart for that.\n"
          << "Restart=always\nRestartSec=3\nRestartPreventExitStatus=3\nNice=5\nNoNewPrivileges=yes\nMemoryHigh=24M\n";
    }
    u << "\n[Install]\nWantedBy=default.target\n";
    return u.str();
}

static void systemctlUser(const char* verb) {
    pid_t p = fork();
    if (p == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0), dup2(null, 1), dup2(null, 2);
        execlp("systemctl", "systemctl", "--user", "--no-block", verb, (char*)nullptr);
        _exit(127);
    }
    if (p > 0) waitpid(p, nullptr, 0);
}

static void ensureUnits() {
    std::error_code ec;
    fs::path dir = unitDir(), wants = dir / "default.target.wants";
    fs::create_directories(wants, ec);
    bool changed = false;
    for (int i = 0; i < 2; i++) {
        fs::path f = dir / kUnits[i];
        std::string want = unitText(i == 0);
        std::ifstream in(f);
        std::stringstream have;
        have << in.rdbuf();
        if (have.str() != want) {
            fs::path tmp = f;
            tmp += ".tmp";
            std::ofstream(tmp) << want;
            fs::rename(tmp, f, ec);
            if (ec) {
                fprintf(stderr, "ztwhatsappd: cannot write %s: %s\n", f.c_str(), ec.message().c_str());
                return;
            }
            changed = true;
        }
        fs::path link = wants / kUnits[i];
        if (!fs::is_symlink(link, ec)) {
            fs::create_symlink(fs::path("..") / kUnits[i], link, ec);
            changed = true;
        }
    }
    // Once, so `systemctl --user status` knows them before the next login.
    if (changed) {
        fprintf(stderr, "ztwhatsappd: installed the login units\n");
        systemctlUser("daemon-reload");
    }
}

static int removeUnits() {
    std::error_code ec;
    fs::path dir = unitDir();
    for (auto* u : kUnits) {
        fs::remove(dir / "default.target.wants" / u, ec);
        fs::remove(dir / u, ec);
    }
    systemctlUser("daemon-reload");
    return 0;
}

int main(int argc, char** argv) {
    bool fake = false, probe = false;
    std::string sock, data;
    RealConfig cfg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s needs a value\n", a.c_str());
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--fake") fake = true;
        else if (a == "--remove-units") return removeUnits();
        else if (a == "--socket") sock = next();
        else if (a == "--data") data = next();
        else if (a == "--probe-handshake") probe = true;
        else if (a == "--test-server") {  // host:port, plain TCP. Local fake server only.
            std::string v = next();
            auto c = v.rfind(':');
            cfg.host = v.substr(0, c);
            cfg.port = std::stoi(v.substr(c + 1));
            cfg.tls = false;
        } else if (a == "--test-root") {
            cfg.rootKey = toKey(unhex(next()));
        } else if (a == "--test-media") {
            cfg.mediaBase = next();
        } else {
            fprintf(stderr,
                    "usage: ztwhatsappd [--fake] [--socket PATH] [--data DIR] [--probe-handshake]\n"
                    "See ztmetro/whatsapp/IPC.md.\n");
            return 2;
        }
    }
    cryptoInit();
    umask(0077);

    std::string runtime = env("XDG_RUNTIME_DIR", "/tmp");
    if (sock.empty()) sock = runtime + "/ztwhatsapp.sock";
    if (data.empty()) data = env("XDG_DATA_HOME", env("HOME", ".") + "/.local/share") + "/ztwhatsapp";
    cfg.dataDir = data;

    // The probe uses a throwaway identity and must never see the real store or socket.
    std::string probeDir;
    if (probe) {
        std::string t = env("TMPDIR", "/tmp") + "/ztwa-probe-XXXXXX";
        char* d = mkdtemp(t.data());
        if (!d) {
            perror("mkdtemp");
            return 1;
        }
        probeDir = d;
        cfg.dataDir = probeDir;
        cfg.probeOnly = true;
        sock = probeDir + "/s.sock";
    }

    std::string lockPath = sock + ".lock";
    int lock = open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "ztwhatsappd: another instance holds %s\n", lockPath.c_str());
        return 3;
    }

    // Only the real engine, installed by hand at kProgram, registers itself; units whose
    // text differs are rewritten. Run by zrn-exec (ZRN_APP_ID set) it never does: from 0.3.0
    // the package declares its services and the store client writes, restarts and removes
    // the units, under these same names.
    if (!fake && !probe && argc == 1 && !env("ZTWHATSAPP_NO_UNITS").size() && env("ZRN_APP_ID").empty()) {
        char self[1024];
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        bool installed = n > 0 && std::string(self, n) == kProgram;
        if (installed) ensureUnits();
    }

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigprocmask(SIG_BLOCK, &set, nullptr);
    signal(SIGPIPE, SIG_IGN);
    int sfd = signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);

    int rc = 0;
    try {
        // Deliberately never freed: a detached download thread may still post to it at exit.
        Loop& loop = *new Loop;
        loop.add(sfd, EPOLLIN, [&](uint32_t) { loop.stop(); });
        IpcServer ipc(loop, sock);
        std::unique_ptr<Engine> engine = fake ? makeFakeEngine(loop, ipc) : makeRealEngine(loop, ipc, cfg);
        ipc.setEngine(engine.get());
        if (probe) {
            engine->command("pair_qr", json::object(), Reply{[](const json&) {}, [](const std::string&, const std::string&) {}});
            loop.after(30000, [&] {
                printf("probe: no result within 30 s\n");
                rc = 1;
                loop.stop();
            });
        }
        loop.run();
    } catch (const std::exception& e) {
        fprintf(stderr, "ztwhatsappd: %s\n", e.what());
        rc = 1;
    }
    unlink(lockPath.c_str());
    if (!probeDir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(probeDir, ec);
    }
    return rc;
}
