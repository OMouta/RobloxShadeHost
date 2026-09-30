#pragma once

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// A ReShade preset: keys before the first section, such as Techniques, then a section per effect file. Keeps
// the order of sections and keys, so saving a preset changes only what changed.
class PresetIni
{
public:
    struct Section
    {
        std::string name; // empty for the keys before the first section
        std::vector<std::pair<std::string, std::string>> values;
    };

    PresetIni() { sections.push_back({}); }

    explicit PresetIni(const std::string& text) : PresetIni()
    {
        std::istringstream stream(text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? text.substr(3) : text);
        Section* current = &sections.front();
        for (std::string line; std::getline(stream, line);)
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            const size_t first = line.find_first_not_of(" \t");
            if (first == std::string::npos || line[first] == ';' || line[first] == '#')
                continue;
            line.erase(0, first);
            if (line.front() == '[' && line.back() == ']')
            {
                const std::string name = line.substr(1, line.size() - 2);
                current = FindSection(name);
                if (!current)
                {
                    sections.push_back({ name, {} });
                    current = &sections.back();
                }
                continue;
            }
            const size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            std::string key = line.substr(0, equals);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
                key.pop_back();
            std::string value = line.substr(equals + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            SetIn(*current, key, value);
        }
    }

    bool Get(const std::string& section, const std::string& key, std::string& value) const
    {
        if (const Section* found = FindSection(section))
            for (const auto& [name, text] : found->values)
                if (name == key)
                {
                    value = text;
                    return true;
                }
        return false;
    }

    bool HasSection(const std::string& section) const { return FindSection(section) != nullptr; }

    std::vector<std::string> SectionNames() const
    {
        std::vector<std::string> names;
        for (const Section& section : sections)
            if (!section.name.empty())
                names.push_back(section.name);
        return names;
    }

    void Set(const std::string& section, const std::string& key, const std::string& value)
    {
        Section* found = FindSection(section);
        if (!found)
        {
            sections.push_back({ section, {} });
            found = &sections.back();
        }
        SetIn(*found, key, value);
    }

    void Remove(const std::string& section, const std::string& key)
    {
        if (Section* found = FindSection(section))
            std::erase_if(found->values, [&key](const auto& entry) { return entry.first == key; });
    }

    std::string Text() const
    {
        std::string text;
        for (const Section& section : sections)
        {
            if (!section.name.empty())
            {
                if (section.values.empty())
                    continue;
                text += (text.empty() ? "[" : "\n[") + section.name + "]\n";
            }
            for (const auto& [key, value] : section.values)
                text += key + "=" + value + "\n";
        }
        return text;
    }

    // "a,b,c" as ReShade writes lists.
    static std::vector<std::string> Split(const std::string& text)
    {
        std::vector<std::string> items;
        for (size_t start = 0; start < text.size();)
        {
            const size_t end = std::min(text.find(',', start), text.size());
            if (end > start)
                items.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        return items;
    }

    static std::string Join(const std::vector<std::string>& items)
    {
        std::string text;
        for (const std::string& item : items)
            text += (text.empty() ? "" : ",") + item;
        return text;
    }

private:
    Section* FindSection(const std::string& name)
    {
        for (Section& section : sections)
            if (section.name == name)
                return &section;
        return nullptr;
    }
    const Section* FindSection(const std::string& name) const { return const_cast<PresetIni*>(this)->FindSection(name); }

    static void SetIn(Section& section, const std::string& key, const std::string& value)
    {
        for (auto& entry : section.values)
            if (entry.first == key)
            {
                entry.second = value;
                return;
            }
        section.values.emplace_back(key, value);
    }

    std::vector<Section> sections;
};
