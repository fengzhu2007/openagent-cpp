#pragma once
#include "tool/tool.h"
#include "question/question.h"

// Question tool: asks the user a question and blocks until answered
class QuestionTool : public Tool {
public:
    QuestionTool(QuestionManager &questionMgr, const std::string &sessionId);

    std::string name() const override { return "question"; }
    std::string description() const override;
    json parameters() const override;
    ToolResult execute(const json &args) override;

private:
    QuestionManager &m_questionMgr;
    std::string m_sessionId;
};
