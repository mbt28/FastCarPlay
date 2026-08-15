#include "settings.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/logger.h"

bool Settings::load(const std::string &filename)
{
    std::ifstream file(filename);
    if (!file.is_open())
    {
        log_e("Cannot open file > %s", filename.c_str());
        return false;
    }

    std::string line;
    while (std::getline(file, line))
    {
        // strip comments
        std::size_t p = line.find('#');
        if (p != std::string::npos)
            line.erase(p);

        // trim
        trim(line);
        if (line.empty())
            continue;

        // split on '='
        std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        trim(key);
        trim(value);

        // lookup in registry
        bool found = false;
        for (ISetting *setting : _settings())
        {
            if (setting->matches(key))
            {
                setting->parse(value);
                found = true;
                break;
            }
        }
        if (!found)
            log_w("Unknown key > %s", key.c_str());
    }
    return true;
}

std::string Settings::userPath()
{
    const char *home = getenv("HOME");
    if (home == nullptr || *home == '\0')
        home = "/root"; // the device runs as root with no environment
    return std::string(home) + "/.fastcarplay/usersettings.txt";
}

bool Settings::loadUser()
{
    const std::string path = userPath();
    std::ifstream file(path);
    // Remember what we did for print(): this runs before the log level is set
    // from the settings, so logging it here would go nowhere.
    _userPathTried = path;
    if (!file.is_open())
    {
        _userApplied = false;
        return false; // no overrides yet: not an error
    }

    file.close();
    _userApplied = true;
    return load(path);
}

bool Settings::setUser(const std::string &key, const std::string &value)
{
    // Apply to the live setting first: neither a bad key nor a bad value may
    // reach the file. Persisting something unparseable would be worse than
    // rejecting it -- it fails again at every subsequent boot, and the user
    // gets no hint why the setting they saved never took effect.
    ISetting *target = nullptr;
    for (ISetting *setting : _settings())
    {
        // matches(), not name ==, so a key that loads from a settings file can
        // also be saved from the UI. They used to disagree.
        if (setting->matches(key))
        {
            target = setting;
            break;
        }
    }
    if (target == nullptr)
    {
        log_e("Unknown setting > %s", key.c_str());
        return false;
    }

    std::string error;
    if (!target->tryParse(value, &error))
    {
        log_e("Rejected %s = %s > %s", key.c_str(), value.c_str(), error.c_str());
        return false;
    }
    // Store under the canonical name, so an alias write does not leave a stale
    // duplicate of the same setting under both spellings.
    const std::string canonical = target->name;

    // Merge into the existing overrides, preserving every other key.
    const std::string path = userPath();
    std::vector<std::pair<std::string, std::string>> entries;
    bool replaced = false;

    std::ifstream in(path);
    if (in.is_open())
    {
        std::string line;
        while (std::getline(in, line))
        {
            std::string stripped = line;
            std::size_t hash = stripped.find('#');
            if (hash != std::string::npos)
                stripped.erase(hash);

            std::size_t eq = stripped.find('=');
            if (eq == std::string::npos)
                continue;

            std::string k = stripped.substr(0, eq);
            std::string v = stripped.substr(eq + 1);
            trim(k);
            trim(v);
            if (k.empty())
                continue;

            // Fold every spelling of this setting (name or alias) into one
            // canonical entry, so saving under an alias cannot leave two lines
            // that disagree about the same setting.
            if (target->matches(k))
            {
                if (replaced)
                    continue;
                entries.emplace_back(canonical, value);
                replaced = true;
                continue;
            }
            entries.emplace_back(k, v);
        }
        in.close();
    }
    if (!replaced)
        entries.emplace_back(canonical, value);

    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos)
    {
        const std::string dir = path.substr(0, slash);
        // 0700: may hold a Wi-Fi passphrase.
        if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
        {
            log_e("Cannot create %s > %s", dir.c_str(), strerror(errno));
            return false;
        }
    }

    // Write via a temp file + fsync + rename so a power cut mid-write can never
    // leave a truncated settings file on the head unit. The head unit loses
    // power at ignition-off, so this is the normal case, not the rare one:
    // without the fsync the rename can land while the contents are still in
    // page cache, and the file comes back empty. 0600 -- it may hold a Wi-Fi
    // passphrase.
    std::string body =
        "# FastCarPlay user settings -- written by the on-device UI.\n"
        "# Applied after the shipped preset; safe across image updates.\n";
    for (const auto &entry : entries)
        body += entry.first + " = " + entry.second + "\n";

    const std::string tmp = path + ".tmp";
    {
        const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0)
        {
            log_e("Cannot write %s > %s", tmp.c_str(), strerror(errno));
            return false;
        }
        const char *p = body.data();
        std::size_t left = body.size();
        while (left > 0)
        {
            const ssize_t n = write(fd, p, left);
            if (n <= 0)
            {
                if (n < 0 && errno == EINTR)
                    continue;
                log_e("Write failed %s > %s", tmp.c_str(), strerror(errno));
                close(fd);
                unlink(tmp.c_str());
                return false;
            }
            p += n;
            left -= static_cast<std::size_t>(n);
        }
        if (fsync(fd) != 0)
        {
            log_e("Cannot flush %s > %s", tmp.c_str(), strerror(errno));
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        close(fd);
    }

    if (rename(tmp.c_str(), path.c_str()) != 0)
    {
        log_e("Cannot replace %s > %s", path.c_str(), strerror(errno));
        unlink(tmp.c_str());
        return false;
    }

    // Durability of the rename itself lives in the directory, not the file.
    if (slash != std::string::npos)
    {
        const int dirfd = open(path.substr(0, slash).c_str(), O_RDONLY | O_DIRECTORY);
        if (dirfd >= 0)
        {
            fsync(dirfd);
            close(dirfd);
        }
    }

    log_i("Saved %s = %s > %s", key.c_str(), value.c_str(), path.c_str());
    return true;
}

std::string Settings::_userPathTried;
bool Settings::_userApplied = false;

void Settings::print()
{
    // Say which override file won. These are applied after the preset given on
    // the command line, and the path follows $HOME -- so running under sudo
    // applies /root/... instead of the invoking user's, silently overriding the
    // preset that was passed. Info level: it changes what the run actually does.
    if (_userApplied)
        log_i("Applied user settings > %s", _userPathTried.c_str());
    else if (!_userPathTried.empty())
        log_i("No user settings at %s (using the preset as-is)", _userPathTried.c_str());

    for (ISetting *setting : _settings())
    {
        log_d("%s = %s", setting->name.c_str(), setting->asString().c_str());
    }
}

void Settings::trim(std::string &s)
{
    // Left trim
    s.erase(s.begin(),
            std::find_if(s.begin(), s.end(),
                         [](unsigned char c)
                         { return !std::isspace(c); }));

    // Right trim
    s.erase(std::find_if(s.rbegin(), s.rend(),
                         [](unsigned char c)
                         { return !std::isspace(c); })
                .base(),
            s.end());
}
