#include "admin_unix_socket.h"
#include "metrics/metrics.h"
#include "table_registry.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

namespace yikv_server {

namespace {

static void TrimCrlf(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
}

static void AdminLoop(std::string path, TableRegistry* reg) {
    if (unlink(path.c_str()) < 0 && errno != ENOENT) {
        std::cerr << "admin socket: unlink " << path << ": " << std::strerror(errno) << "\n";
        return;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "admin socket: socket: " << std::strerror(errno) << "\n";
        return;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        std::cerr << "admin socket: path too long\n";
        close(fd);
        return;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "admin socket: bind: " << std::strerror(errno) << "\n";
        close(fd);
        return;
    }
    if (listen(fd, 8) < 0) {
        std::cerr << "admin socket: listen: " << std::strerror(errno) << "\n";
        close(fd);
        return;
    }
    std::cerr << "admin unix socket: " << path << " (reload <table>)\n";

    for (;;) {
        int c = accept(fd, nullptr, nullptr);
        if (c < 0) continue;
        std::string line;
        char        buf[1024];
        ssize_t     n = read(c, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            line   = buf;
        }
        TrimCrlf(line);

        std::string resp = "err: empty command\n";
        std::string cmd_label = "unknown";
        std::string result    = "error";
        if (line.rfind("reload ", 0) == 0) {
            cmd_label = "reload";
            std::string table = line.substr(7);
            while (!table.empty() && table[0] == ' ') table.erase(0, 1);
            TrimCrlf(table);
            if (table.empty()) {
                resp = "err: missing table name\n";
            } else {
                try {
                    reg->ReloadTable(table);
                    resp = "ok\n";
                    result = "ok";
                } catch (const std::exception& e) {
                    resp = std::string("err: ") + e.what() + "\n";
                }
            }
        } else if (line.empty()) {
            cmd_label = "empty";
        } else {
            cmd_label = "unknown";
            resp = "err: expected 'reload <table>'\n";
        }
        ::yikv_server::metrics::Metrics::instance()
            .admin.commands_total.IncFor(cmd_label, result);
        (void)write(c, resp.data(), resp.size());
        close(c);
    }
}

}  // namespace

void StartAdminUnixSocket(const std::string& path, TableRegistry* reg) {
    if (path.empty() || !reg) return;
    std::thread(AdminLoop, path, reg).detach();
}

}  // namespace yikv_server
