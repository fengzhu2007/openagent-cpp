#pragma once
#include "tool/tool.h"

// Fetch a web page over HTTP/HTTPS via libcurl and return readable text,
// feeding the page content back to the LLM when it cannot access the web.
class FetchTool : public Tool {
public:
    std::string name() const override { return "fetch"; }
    std::string description() const override;
    json parameters() const override;
    ToolResult execute(const json &args, const std::string &cwd) override;

private:
    ToolResult fetchPage(const std::string &url, int maxLength);
};
