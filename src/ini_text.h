#pragma once

#include <sstream>
#include <string>
#include <vector>

// Edits an INI file as text, keeping its byte order mark, line endings and everything it does not change.
// ReShade.ini is UTF-8, which GetPrivateProfileString would read in the ANSI code page.
class IniText
{
public:
    explicit IniText(std::string text)
    {
        if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
        {
            bom = text.substr(0, 3);
            text.erase(0, 3);
        }
        newline = text.find("\r\n") != std::string::npos || text.empty() ? "\r\n" : "\n";
        std::istringstream stream(text);
        for (std::string line; std::getline(stream, line);)
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(line);
        }
    }

    // Returns false when the key is missing.
    bool Get(const std::string& section, const std::string& key, std::string& value) const
    {
        const size_t line = Find(section, key);
        if (line == std::string::npos)
            return false;
        value = lines[line].substr(lines[line].find('=') + 1);
        return true;
    }

    void Set(const std::string& section, const std::string& key, const std::string& value)
    {
        changed = true;
        if (const size_t line = Find(section, key); line != std::string::npos)
        {
            lines[line] = key + "=" + value;
            return;
        }
        const std::string header = "[" + section + "]";
        size_t line = 0;
        while (line < lines.size() && lines[line] != header)
            ++line;
        if (line == lines.size())
        {
            if (!lines.empty() && !lines.back().empty())
                lines.push_back("");
            line = lines.size();
            lines.push_back(header);
        }
        // After the section's last entry, before the blank line that ends it.
        size_t end = line + 1;
        while (end < lines.size() && !lines[end].empty() && lines[end][0] != '[')
            ++end;
        lines.insert(lines.begin() + end, key + "=" + value);
    }

    bool Changed() const { return changed; }

    std::string Text() const
    {
        std::string text = bom;
        for (const auto& line : lines)
            text += line + newline;
        return text;
    }

private:
    size_t Find(const std::string& section, const std::string& key) const
    {
        const std::string header = "[" + section + "]";
        bool inside = false;
        for (size_t line = 0; line < lines.size(); ++line)
        {
            if (!lines[line].empty() && lines[line][0] == '[')
                inside = lines[line] == header;
            else if (inside && lines[line].compare(0, key.size() + 1, key + "=") == 0)
                return line;
        }
        return std::string::npos;
    }

    std::vector<std::string> lines;
    std::string bom;
    std::string newline;
    bool changed = false;
};
