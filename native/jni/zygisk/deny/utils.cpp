#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <set>

#include <magisk.hpp>
#include <utils.hpp>
#include <db.hpp>

#include "deny.hpp"

using namespace std;

static set<pair<string, string>> *deny_set;          /* set of <pkg, process> pair */
static map<int, vector<string_view>> *uid_proc_map;  /* uid -> list of process */

// Locks the variables above
static pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;

atomic<bool> denylist_enabled = false;

static void rebuild_uid_map() {
    uid_proc_map->clear();
    string data_path(APP_DATA_DIR);
    size_t len = data_path.length();
    auto dir = open_dir(APP_DATA_DIR);
    bool first_iter = true;
    for (dirent *entry; (entry = xreaddir(dir.get()));) {
        data_path.resize(len);
        data_path += '/';
        data_path += entry->d_name;  // multiuser user id
        data_path += '/';
        size_t user_len = data_path.length();
        struct stat st;
        for (const auto &hide : *deny_set) {
            if (hide.first == ISOLATED_MAGIC) {
                if (!first_iter) continue;
                // Setup isolated processes
                (*uid_proc_map)[-1].emplace_back(hide.second);
            }
            data_path.resize(user_len);
            data_path += hide.first;
            if (stat(data_path.data(), &st))
                continue;
            (*uid_proc_map)[st.st_uid].emplace_back(hide.second);
        }
        first_iter = false;
    }
}

// Leave /proc fd opened as we're going to read from it repeatedly
static DIR *procfp;

template<class F>
static void crawl_procfs(F &&fn) {
    rewinddir(procfp);
    dirent *dp;
    int pid;
    while ((dp = readdir(procfp))) {
        pid = parse_int(dp->d_name);
        if (pid > 0 && !fn(pid))
            break;
    }
}

template <bool str_op(string_view, string_view)>
static bool proc_name_match(int pid, const char *name) {
    char buf[4019];
    sprintf(buf, "/proc/%d/cmdline", pid);
    if (auto fp = open_file(buf, "re")) {
        fgets(buf, sizeof(buf), fp.get());
        if (str_op(buf, name)) {
            LOGD("denylist: kill PID=[%d] (%s)\n", pid, buf);
            return true;
        }
    }
    return false;
}

static inline bool str_eql(string_view a, string_view b) { return a == b; }

template<bool str_op(string_view, string_view) = &str_eql>
static void kill_process(const char *name, bool multi = false) {
    crawl_procfs([=](int pid) -> bool {
        if (proc_name_match<str_op>(pid, name)) {
            kill(pid, SIGKILL);
            return multi;
        }
        return true;
    });
}

static bool validate(const char *pkg, const char *proc) {
    bool pkg_valid = false;
    bool proc_valid = true;

    if (str_eql(pkg, ISOLATED_MAGIC)) {
        pkg_valid = true;
        for (char c; (c = *proc); ++proc) {
            if (isalnum(c) || c == '_' || c == '.')
                continue;
            if (c == ':')
                break;
            proc_valid = false;
            break;
        }
    } else {
        for (char c; (c = *pkg); ++pkg) {
            if (isalnum(c) || c == '_')
                continue;
            if (c == '.') {
                pkg_valid = true;
                continue;
            }
            pkg_valid = false;
            break;
        }

        for (char c; (c = *proc); ++proc) {
            if (isalnum(c) || c == '_' || c == ':' || c == '.')
                continue;
            proc_valid = false;
            break;
        }
    }
    return pkg_valid && proc_valid;
}

static void add_hide_set(const char *pkg, const char *proc) {
    LOGI("denylist add: [%s/%s]\n", pkg, proc);
    deny_set->emplace(pkg, proc);
    if (strcmp(pkg, ISOLATED_MAGIC) == 0) {
        // Kill all matching isolated processes
        kill_process<&str_starts>(proc, true);
    } else {
        kill_process(proc);
    }
}

static int add_list(const char *pkg, const char *proc) {
    if (proc[0] == '\0')
        proc = pkg;

    if (!validate(pkg, proc))
        return DENYLIST_INVALID_PKG;

    {
        mutex_guard lock(data_lock);
        for (const auto &hide : *deny_set)
            if (hide.first == pkg && hide.second == proc)
                return DENYLIST_ITEM_EXIST;
        add_hide_set(pkg, proc);
        rebuild_uid_map();
    }

    // Add to database
    char sql[4096];
    snprintf(sql, sizeof(sql),
            "INSERT INTO denylist (package_name, process) VALUES('%s', '%s')", pkg, proc);
    char *err = db_exec(sql);
    db_err_cmd(err, return DAEMON_ERROR);
    return DAEMON_SUCCESS;
}

int add_list(int client) {
    string pkg = read_string(client);
    string proc = read_string(client);
    return add_list(pkg.data(), proc.data());
}

static int rm_list(const char *pkg, const char *proc) {
    {
        bool remove = false;
        mutex_guard lock(data_lock);
        for (auto it = deny_set->begin(); it != deny_set->end();) {
            if (it->first == pkg && (proc[0] == '\0' || it->second == proc)) {
                remove = true;
                LOGI("denylist rm: [%s/%s]\n", it->first.data(), it->second.data());
                it = deny_set->erase(it);
            } else {
                ++it;
            }
        }
        if (!remove)
            return DENYLIST_ITEM_NOT_EXIST;
        rebuild_uid_map();
    }

    char sql[4096];
    if (proc[0] == '\0')
        snprintf(sql, sizeof(sql), "DELETE FROM denylist WHERE package_name='%s'", pkg);
    else
        snprintf(sql, sizeof(sql),
                "DELETE FROM denylist WHERE package_name='%s' AND process='%s'", pkg, proc);
    char *err = db_exec(sql);
    db_err_cmd(err, return DAEMON_ERROR);
    return DAEMON_SUCCESS;
}

int rm_list(int client) {
    string pkg = read_string(client);
    string proc = read_string(client);
    return rm_list(pkg.data(), proc.data());
}

static bool str_ends_safe(string_view s, string_view ss) {
    // Never kill webview zygote
    if (s == "webview_zygote")
        return false;
    return str_ends(s, ss);
}

static bool init_list() {
    LOGD("denylist: initialize\n");

    char *err = db_exec("SELECT * FROM denylist", [](db_row &row) -> bool {
        add_hide_set(row["package_name"].data(), row["process"].data());
        return true;
    });
    db_err_cmd(err, return false);

    // If Android Q+, also kill blastula pool and all app zygotes
    if (SDK_INT >= 29) {
        kill_process("usap32", true);
        kill_process("usap64", true);
        kill_process<&str_ends_safe>("_zygote", true);
    }

    return true;
}

void ls_list(int client) {
    write_int(client, DAEMON_SUCCESS);
    {
        mutex_guard lock(data_lock);
        for (const auto &hide : *deny_set) {
            write_int(client, hide.first.size() + hide.second.size() + 1);
            xwrite(client, hide.first.data(), hide.first.size());
            xwrite(client, "|", 1);
            xwrite(client, hide.second.data(), hide.second.size());
        }
    }
    write_int(client, 0);
    close(client);
}

static void update_deny_config() {
    char sql[64];
    sprintf(sql, "REPLACE INTO settings (key,value) VALUES('%s',%d)",
            DB_SETTING_KEYS[DENYLIST_CONFIG], denylist_enabled.load());
    char *err = db_exec(sql);
    db_err(err);
}

int enable_deny() {
    if (denylist_enabled) {
        return DAEMON_SUCCESS;
    } else {
        mutex_guard lock(data_lock);

        if (access("/proc/self/ns/mnt", F_OK) != 0) {
            LOGW("The kernel does not support mount namespace\n");
            return DENY_NO_NS;
        }

        if (procfp == nullptr && (procfp = opendir("/proc")) == nullptr)
            return DAEMON_ERROR;

        LOGI("* Enable DenyList\n");

        default_new(deny_set);
        if (!init_list()) {
            delete deny_set;
            deny_set = nullptr;
            return DAEMON_ERROR;
        }

        denylist_enabled = true;

        default_new(uid_proc_map);
        rebuild_uid_map();
    }

    update_deny_config();
    return DAEMON_SUCCESS;
}

int disable_deny() {
    if (denylist_enabled) {
        denylist_enabled = false;
        LOGI("* Disable DenyList\n");

        mutex_guard lock(data_lock);
        delete uid_proc_map;
        delete deny_set;
        uid_proc_map = nullptr;
        deny_set = nullptr;
    }
    update_deny_config();
    return DAEMON_SUCCESS;
}

void initialize_denylist() {
    if (!denylist_enabled) {
        db_settings dbs;
        get_db_settings(dbs, DENYLIST_CONFIG);
        if (dbs[DENYLIST_CONFIG])
            enable_deny();
    }
}

bool is_deny_target(int uid, string_view process) {
    mutex_guard lock(data_lock);

    if (uid % 100000 >= 90000) {
        // Isolated processes
        auto it = uid_proc_map->find(-1);
        if (it == uid_proc_map->end())
            return false;

        for (auto &s : it->second) {
            if (str_starts(process, s))
                return true;
        }
    } else {
        auto it = uid_proc_map->find(uid);
        if (it == uid_proc_map->end())
            return false;

        for (auto &s : it->second) {
            if (s == process)
                return true;
        }
    }
    return false;
}
