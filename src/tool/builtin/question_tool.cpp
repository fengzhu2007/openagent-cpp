#include "tool/builtin/question_tool.h"
#include "util/logger.h"

QuestionTool::QuestionTool(QuestionManager &questionMgr, const std::string &sessionId)
    : m_questionMgr(questionMgr), m_sessionId(sessionId)
{
}

std::string QuestionTool::description() const
{
    return "Ask the user a question and wait for their response. Use this when you need "
           "clarification, confirmation, or user input before proceeding. The question will "
           "be displayed to the user via the interface.";
}

json QuestionTool::parameters() const
{
    return {
        {"type", "object"},
        {"properties", {
            {"question", {
                {"type", "string"},
                {"description", "The question to ask the user"}
            }},
            {"options", {
                {"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "Optional list of choices for the user to select from"}
            }}
        }},
        {"required", {"question"}}
    };
}

ToolResult QuestionTool::execute(const json &args, const std::string &)
{
    std::string question = args.value("question", "");
    if (question.empty()) {
        return {false, "", "No question provided", "question"};
    }

    std::vector<std::string> options;
    if (args.contains("options") && args["options"].is_array()) {
        for (const auto &opt : args["options"]) {
            options.push_back(opt.get<std::string>());
        }
    }

    LOG_INFO("Question tool asking: " + question.substr(0, 100) + "...");

    // Block until user answers (via API)
    std::string answer = m_questionMgr.ask(m_sessionId, question, options);

    ToolResult result;
    result.success = true;
    result.output = "User answered: " + answer;
    result.title = "question";
    return result;
}
