#pragma once
#include "tool/tool.h"
#include "skill/skill.h"

// Skill tool: loads a skill's content and returns it as context for the LLM
class SkillTool : public Tool {
public:
    SkillTool(const SkillManager &skills);

    std::string name() const override { return "skill"; }
    std::string description() const override;
    json parameters() const override;
    ToolResult execute(const json &args, const std::string &cwd) override;

private:
    const SkillManager &m_skills;
};
