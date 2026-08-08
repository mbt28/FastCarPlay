#include "settings.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

#include <sys/stat.h>

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
    // Apply to the live setting first: a bad key must not reach the file.
    ISetting *target = nullptr;
    for (ISetting *setting : _settings())
    {
        if (setting->name == key)
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

    std::string parsed = value;
    target->parse(parsed);

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

            if (k == key)
            {
                v = value;
                replaced = true;
            }
            entries.emplace_back(k, v);
        }
        in.close();
    }
    if (!replaced)
        entries.emplace_back(key, value);

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

    // Write via a temp file + rename so a power cut mid-write can never leave
    // a truncated settings file on the head unit.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.is_open())
        {
            log_e("Cannot write > %s", tmp.c_str());
            return false;
        }
        out << "# FastCarPlay user settings -- written by the on-device UI.\n"
            << "# Applied after the shipped preset; safe across image updates.\n";
        for (const auto &entry : entries)
            out << entry.first << " = " << entry.second << "\n";
        out.flush();
        if (!out.good())
        {
            log_e("Write failed > %s", tmp.c_str());
            return false;
        }
    }

    if (rename(tmp.c_str(), path.c_str()) != 0)
    {
        log_e("Cannot replace %s > %s", path.c_str(), strerror(errno));
        return false;
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
