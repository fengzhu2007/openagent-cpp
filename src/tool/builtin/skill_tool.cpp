#include "tool/builtin/skill_tool.h"
#include "util/logger.h"
#include <sstream>

SkillTool::SkillTool(const SkillManager &skills)
    : m_skills(skills)
{
}

std::string SkillTool::description() const
{
    return "Load a skill's instructions and context. Skills provide specialized capabilities "
           "and domain knowledge. The skill content will be added as context for the current conversation.";
}

json SkillTool::parameters() const
{
    // Build list of available skill names for the LLM
    json skillNames = json::array();
    auto skills = m_skills.listSkills();
    for (const auto &s : skills) {
        skillNames.push_back(s.name);
    }

    return {
        {"type", "object"},
        {"properties", {
            {"skill", {
                {"type", "string"},
                {"description", "Name of the skill to load"},
                {"enum", skillNames}
            }},
            {"arguments", {
                {"type", "string"},
                {"description", "Optional arguments to pass to the skill"}
            }}
        }},
        {"required", {"skill"}}
    };
}

ToolResult SkillTool::execute(const json &args, const std::string &)
{
    std::string skillName = args.value("skill", "");
    std::string arguments = args.value("arguments", "");

    if (skillName.empty()) {
        return {false, "", "No skill name specified", "skill"};
    }

    const Skill *skill = m_skills.getSkill(skillName);
    if (!skill) {
        // List available skills as help
        auto skills = m_skills.listSkills();
        std::ostringstream oss;
        oss << "Skill not found: " << skillName << "\n\nAvailable skills:\n";
        for (const auto &s : skills) {
            oss << "  - " << s.name << ": " << s.description << "\n";
        }
        return {false, oss.str(), "Skill not found: " + skillName, "skill: " + skillName};
    }

    LOG_INFO("Loading skill: " + skillName);

    // Build output: skill content + associated files list
    std::ostringstream oss;
    oss << "[Skill: " << skill->name << "]\n";
    if (!skill->description.empty()) {
        oss << "Description: " << skill->description << "\n";
    }
    oss << "\n" << skill->content;

    if (!skill->files.empty()) {
        oss << "\n\n[Associated Files]\n";
        for (const auto &f : skill->files) {
            oss << "  - " << f << "\n";
        }
    }

    if (!arguments.empty()) {
        oss << "\n\n[User Arguments]\n" << arguments << "\n";
    }

    ToolResult result;
    result.success = true;
    result.output = oss.str();
    result.title = "skill: " + skillName;
    return result;
}
