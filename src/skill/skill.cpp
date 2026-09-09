#include "skill/skill.h"
#include "util/logger.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

// ---- Skill ----

json Skill::toJson() const
{
    json j;
    j["name"] = name;
    j["description"] = description;
    j["source"] = source;
    if (!files.empty()) j["files"] = files;
    return j;
}

// ---- SkillManager ----

SkillManager::SkillManager() {}

bool SkillManager::parseSkillMd(const std::string &filePath, Skill &skill)
{
    std::ifstream file(filePath);
    if (!file.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    // Parse frontmatter: ---\nname: xxx\ndescription: xxx\n---\n
    if (content.substr(0, 3) == "---") {
        size_t endFm = content.find("---", 3);
        if (endFm != std::string::npos) {
            std::string frontmatter = content.substr(3, endFm - 3);

            // Parse key: value pairs
            std::istringstream fmStream(frontmatter);
            std::string line;
            while (std::getline(fmStream, line)) {
                // Trim whitespace
                auto start = line.find_first_not_of(" \t\r\n");
                if (start == std::string::npos) continue;
                line = line.substr(start);

                size_t colonPos = line.find(':');
                if (colonPos == std::string::npos) continue;

                std::string key = line.substr(0, colonPos);
                std::string value = line.substr(colonPos + 1);

                // Trim value
                start = value.find_first_not_of(" \t");
                if (start != std::string::npos) value = value.substr(start);
                auto end = value.find_last_not_of(" \t\r\n");
                if (end != std::string::npos) value = value.substr(0, end + 1);

                if (key == "name") skill.name = value;
                else if (key == "description") skill.description = value;
            }

            // Body is everything after the second ---
            size_t bodyStart = endFm + 3;
            if (bodyStart < content.size() && content[bodyStart] == '\n') ++bodyStart;
            if (bodyStart < content.size() && content[bodyStart] == '\r') ++bodyStart;
            skill.content = content.substr(bodyStart);
        } else {
            skill.content = content;
        }
    } else {
        skill.content = content;
    }

    // If name is still empty, derive from filename
    if (skill.name.empty()) {
        fs::path p(filePath);
        skill.name = p.parent_path().filename().string();
        if (skill.name.empty()) skill.name = p.stem().string();
    }

    return true;
}

void SkillManager::loadFromDirectory(const std::string &projectDir)
{
    // Search in project and home directories
    std::vector<std::string> searchDirs = {
        projectDir + "/.agents/skills",
        projectDir + "/.opencode/skills",
    };

    // Add home directory
    std::string homeDir;
#ifdef _WIN32
    const char *userProfile = std::getenv("USERPROFILE");
    if (userProfile) homeDir = std::string(userProfile);
#else
    const char *home = std::getenv("HOME");
    if (home) homeDir = std::string(home);
#endif

    if (!homeDir.empty()) {
        searchDirs.push_back(homeDir + "/.agents/skills");
        searchDirs.push_back(homeDir + "/.opencode/skills");
    }

    for (const auto &dir : searchDirs) {
        if (!fs::exists(dir) || !fs::is_directory(dir)) continue;

        try {
            for (const auto &entry : fs::directory_iterator(dir)) {
                if (!entry.is_directory()) continue;

                // Look for SKILL.md in each subdirectory
                std::string skillMdPath = entry.path().string() + "/SKILL.md";
                if (!fs::exists(skillMdPath)) {
                    // Also try lowercase
                    skillMdPath = entry.path().string() + "/skill.md";
                    if (!fs::exists(skillMdPath)) continue;
                }

                Skill skill;
                if (!parseSkillMd(skillMdPath, skill)) continue;

                // Collect associated files (other files in the same directory)
                for (const auto &fileEntry : fs::directory_iterator(entry.path())) {
                    if (fileEntry.is_regular_file()) {
                        std::string fname = fileEntry.path().filename().string();
                        if (fname != "SKILL.md" && fname != "skill.md") {
                            skill.files.push_back(fileEntry.path().string());
                        }
                    }
                }

                skill.source = "directory";
                m_skills[skill.name] = skill;
                LOG_DEBUG("Skill loaded from directory: " + skill.name);
            }
        } catch (const std::exception &e) {
            LOG_DEBUG("Error scanning skill directory " + dir + ": " + std::string(e.what()));
        }
    }
}

void SkillManager::loadFromConfig(const json &config)
{
    if (!config.contains("skills") || !config["skills"].is_array()) return;

    for (const auto &skillJson : config["skills"]) {
        Skill skill;
        skill.name = skillJson.value("name", "");
        skill.description = skillJson.value("description", "");
        skill.content = skillJson.value("content", "");
        skill.source = "config";

        if (skillJson.contains("files") && skillJson["files"].is_array()) {
            for (const auto &f : skillJson["files"]) {
                skill.files.push_back(f.get<std::string>());
            }
        }

        if (!skill.name.empty() && !skill.content.empty()) {
            m_skills[skill.name] = skill;
            LOG_DEBUG("Skill loaded from config: " + skill.name);
        }
    }
}

const Skill *SkillManager::getSkill(const std::string &name) const
{
    auto it = m_skills.find(name);
    return (it != m_skills.end()) ? &it->second : nullptr;
}

std::vector<Skill> SkillManager::listSkills() const
{
    std::vector<Skill> result;
    for (const auto &[name, skill] : m_skills) {
        result.push_back(skill);
    }
    return result;
}

bool SkillManager::hasSkill(const std::string &name) const
{
    return m_skills.find(name) != m_skills.end();
}
