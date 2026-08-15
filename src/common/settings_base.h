#ifndef SRC_HELPER_SETTINGS_BASE
#define SRC_HELPER_SETTINGS_BASE

#include <string>
#include <vector>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <type_traits>

// Base interface for any one setting
class ISetting
{
public:
    std::string name;
    // An older key that still parses into this same setting, so renaming a
    // setting does not silently reset it for anyone whose settings file (or
    // saved usersettings.txt) predates the rename.
    std::string alias;
    ISetting(std::string name_, std::string alias_ = "")
        : name(std::move(name_)), alias(std::move(alias_)) {}
    bool matches(const std::string &key) const { return key == name || (!alias.empty() && key == alias); }

    // Parse, and say why it failed. Anything that persists a value must use
    // this form: the old lenient parse() kept the previous value on a bad
    // input but could not report it, so a caller would happily write the bad
    // string to disk and then fail to parse it on every subsequent boot.
    virtual bool tryParse(std::string in, std::string *error) = 0;

    // Lenient form, for loading a settings file: one bad line must not stop
    // start-up, so it complains and moves on.
    void parse(const std::string &str)
    {
        std::string error;
        if (!tryParse(str, &error))
            std::cerr << "[Settings] " << error << std::endl;
    }

    virtual std::string asString() const = 0;
};

// Holds the global registry
inline std::vector<ISetting *> &_settings()
{
    static std::vector<ISetting *> settings;
    return settings;
}

// A “typed” setting that auto‑registers itself
template <typename T>
class Setting : public ISetting
{
public:
    T value;
    Setting(std::string name_, T default_, std::string alias_ = "")
        : ISetting(std::move(name_), std::move(alias_)), value(default_)
    {
        _settings().push_back(this);
    }

    // allow using this as if it were a T
    operator T() const { return value; }
    Setting &operator=(T newValue)
    {
        value = newValue;
        return *this;
    }

    // parse a string into T, leaving the current value untouched on failure
    bool tryParse(std::string str, std::string *error) override
    {
        try
        {
            if constexpr (std::is_same_v<T, bool>)
            {
                std::transform(str.begin(), str.end(), str.begin(), ::tolower);
                if (str == "1" || str == "true")
                    value = true;
                else if (str == "0" || str == "false")
                    value = false;
                else
                    throw std::runtime_error("expected true/false or 1/0");
            }
            else if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
            {
                // Accept hex for the settings that are naturally written that
                // way (i2c addresses, USB ids). Base 10 otherwise -- not base
                // 0, which would read a leading zero as octal.
                const bool hex = str.size() > 2 && str[0] == '0' &&
                                 (str[1] == 'x' || str[1] == 'X');
                size_t used = 0;
                value = static_cast<T>(std::stoll(str, &used, hex ? 16 : 10));
                if (used != str.size())
                    throw std::runtime_error("trailing junk after the number");
            }
            else if constexpr (std::is_floating_point_v<T>)
            {
                size_t used = 0;
                value = static_cast<T>(std::stold(str, &used));
                if (used != str.size())
                    throw std::runtime_error("trailing junk after the number");
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                value = str;
            }
            return true;
        }
        catch (const std::exception &e)
        {
            if (error)
                *error = "cannot parse \"" + str + "\" for " + name + ": " + e.what();
            return false;
        }
    }

    std::string asString() const override
    {
        if constexpr (std::is_same_v<T, bool>)
            return value ? "true" : "false";
        else if constexpr (std::is_same_v<T, std::string>)
            return value;
        else
            return std::to_string(value);
    }
};

template <typename T>
class KeySetting : public Setting<T>
{
public:
    int key;

    KeySetting(std::string name_, T default_, int key_)
        : Setting<T>(std::move(name_), default_), key(key_)
    {
    }
};

#endif /* SRC_HELPER_SETTINGS_BASE */
