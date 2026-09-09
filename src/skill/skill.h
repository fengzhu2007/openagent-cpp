#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "json.hpp"

using json = nlohmann::json;

// Skill definition
struct Skill {
    std::string name;
    std::string description;
    std::string content;        // Markdown body (the skill instructions)
    std::string source;         // "directory", "config", "builtin"
    std::vector<std::string> files;  // Associated files in the same directory

    json toJson() const;
};

// Skill manager: discovers and loads skills from directories and config
class SkillManager {
public:
    SkillManager();

    // Scan .agents/skills/ and ~/.agents/skills/ directories for SKILL.md files
    void loadFromDirectory(const std::string &projectDir);

    // Load skills from config.skills array
    void loadFromConfig(const json &config);

    // Get a skill by name (returns nullptr if not found)
    const Skill *getSkill(const std::string &name) const;

    // List all available skills
    std::vector<Skill> listSkills() const;

    // Check if a skill name exists
    bool hasSkill(const std::string &name) const;

private:
    // Parse a SKILL.md file: extract frontmatter (name, description) and body
    bool parseSkillMd(const std::string &filePath, Skill &skill);

    std::unordered_map<std::string, Skill> m_skills;
};
