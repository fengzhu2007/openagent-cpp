#include "tool/builtin/fetch_tool.h"
#include "util/httplib_client.h"
#include <curl/curl.h>
#include <cctype>
#include <string>

namespace {

// Web pages can be huge; stop reading past this and work with what we have
const size_t kMaxDownloadBytes = 2 * 1024 * 1024;
const size_t kDefaultMaxLength = 20000;

// ASCII-only lowering: UTF-8 multibyte sequences stay untouched so the result
// remains index-aligned with the input
std::string asciiLower(const std::string &s)
{
    std::string out(s);
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string trimStr(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool isValidUtf8(const std::string &s)
{
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int bytes = 1;
        if ((c & 0x80) == 0) bytes = 1;
        else if ((c & 0xE0) == 0xC0) bytes = 2;
        else if ((c & 0xF0) == 0xE0) bytes = 3;
        else if ((c & 0xF8) == 0xF0) bytes = 4;
        else return false;
        if (i + bytes > s.size()) return false;
        for (int k = 1; k < bytes; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += bytes;
    }
    return true;
}

// Convert non-UTF-8 page bytes to UTF-8 using the declared charset. Windows
// maps charsets to codepages; other platforms pass through unchanged.
std::string toUtf8(const std::string &input, const std::string &charset)
{
    if (input.empty()) return input;
    if (isValidUtf8(input)) return input;  // also covers misdeclared pages
    if (charset.empty() || charset == "utf-8" || charset == "utf8") return input;
#ifdef _WIN32
    UINT cp = CP_ACP;
    if (charset == "gbk" || charset == "gb2312" || charset == "gb18030") cp = 936;
    else if (charset == "big5") cp = 950;
    else if (charset == "shift-jis" || charset == "shift_jis" || charset == "sjis") cp = 932;
    else if (charset == "euc-kr" || charset == "euckr") cp = 949;
    else if (charset == "iso-8859-1" || charset == "latin1" || charset == "iso8859-1") cp = 28591;
    else if (charset == "windows-1252" || charset == "cp1252") cp = 1252;
    else if (charset == "ascii" || charset == "us-ascii") return input;

    int wlen = MultiByteToWideChar(cp, 0, input.data(), (int)input.size(), nullptr, 0);
    if (wlen <= 0) return input;
    std::wstring wide(wlen, L'\0');
    MultiByteToWideChar(cp, 0, input.data(), (int)input.size(), &wide[0], wlen);
    int ulen = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (ulen <= 0) return input;
    std::string out(ulen, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen, &out[0], ulen, nullptr, nullptr);
    return out;
#else
    return input;
#endif
}

// Read charset=... from a Content-Type header, falling back to a <meta> tag
std::string extractCharset(const std::string &contentType, const std::string &body)
{
    auto findIn = [](const std::string &hay) -> std::string {
        size_t p = hay.find("charset=");
        if (p == std::string::npos) return "";
        p += 8;
        if (p < hay.size() && (hay[p] == '"' || hay[p] == '\'')) ++p;
        size_t e = p;
        while (e < hay.size() &&
               (std::isalnum(static_cast<unsigned char>(hay[e])) || hay[e] == '-' || hay[e] == '_')) {
            ++e;
        }
        return hay.substr(p, e - p);
    };
    std::string cs = findIn(asciiLower(contentType));
    if (cs.empty()) cs = findIn(asciiLower(body.substr(0, 2048)));
    return cs;
}

void appendUtf8(std::string &out, unsigned int cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Decode the common named and numeric HTML entities (&#x..; / &#..;)
std::string decodeEntities(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i + 1);
            if (semi != std::string::npos && semi - i <= 10) {
                std::string ent = s.substr(i + 1, semi - i - 1);
                if (ent == "amp") { out += '&'; i = semi + 1; continue; }
                if (ent == "lt") { out += '<'; i = semi + 1; continue; }
                if (ent == "gt") { out += '>'; i = semi + 1; continue; }
                if (ent == "quot") { out += '"'; i = semi + 1; continue; }
                if (ent == "apos") { out += '\''; i = semi + 1; continue; }
                if (ent == "nbsp") { out += ' '; i = semi + 1; continue; }
                if (!ent.empty() && ent[0] == '#') {
                    unsigned int cp = 0;
                    bool ok = false;
                    if (ent.size() > 2 && (ent[1] == 'x' || ent[1] == 'X')) {
                        ok = true;
                        for (size_t k = 2; k < ent.size(); ++k) {
                            char c = ent[k];
                            int d = (c >= '0' && c <= '9') ? c - '0'
                                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                            if (d < 0) { ok = false; break; }
                            cp = cp * 16 + static_cast<unsigned int>(d);
                            if (cp > 0x10FFFF) { ok = false; break; }
                        }
                    } else {
                        ok = ent.size() > 1;
                        for (size_t k = 1; k < ent.size(); ++k) {
                            char c = ent[k];
                            if (c < '0' || c > '9') { ok = false; break; }
                            cp = cp * 10 + static_cast<unsigned int>(c - '0');
                            if (cp > 0x10FFFF) { ok = false; break; }
                        }
                    }
                    if (ok && cp != 0) { appendUtf8(out, cp); i = semi + 1; continue; }
                }
            }
        }
        out += s[i++];
    }
    return out;
}

// Collapse runs of blanks to one space and 3+ newlines to a blank line
std::string collapseWhitespace(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    bool pendingSpace = false;
    int newlines = 0;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            pendingSpace = true;
            continue;
        }
        if (c == '\n') { ++newlines; pendingSpace = false; continue; }
        if (newlines) { out.append(newlines > 2 ? 2 : newlines, '\n'); newlines = 0; }
        if (pendingSpace) { out += ' '; pendingSpace = false; }
        out += c;
    }
    if (newlines) out.append(newlines > 2 ? 2 : newlines, '\n');
    size_t b = out.find_first_not_of(" \t\n");
    if (b == std::string::npos) return "";
    size_t e = out.find_last_not_of(" \t\n");
    return out.substr(b, e - b + 1);
}

// Remove <...> markup (quote-aware) without adding line breaks
std::string stripTags(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    bool inTag = false;
    char quote = 0;
    for (char c : s) {
        if (inTag) {
            if (quote) { if (c == quote) quote = 0; }
            else if (c == '\'' || c == '"') quote = c;
            else if (c == '>') inTag = false;
        } else if (c == '<') {
            inTag = true;
        } else {
            out += c;
        }
    }
    return out;
}

// True when a lowercased "<...>" tag text is a <br> or closes a block element
bool isBlockTag(const std::string &tag)
{
    if (tag.rfind("<br", 0) == 0) return true;
    static const char *kClosers[] = {
        "</p>", "</div>", "</h1>", "</h2>", "</h3>", "</h4>", "</h5>", "</h6>",
        "</li>", "</tr>", "</table>", "</thead>", "</tbody>", "</ul>", "</ol>",
        "</dl>", "</dt>", "</dd>", "</blockquote>", "</pre>", "</section>",
        "</article>", "</header>", "</footer>", "</title>", "</figcaption>"
    };
    for (const char *c : kClosers) {
        if (tag == c) return true;
    }
    return false;
}

// HTML to readable text: drop script/style/comments, strip tags, keep block
// boundaries as line breaks, decode entities, collapse whitespace
std::string htmlToText(const std::string &html)
{
    std::string orig = html;
    std::string low = asciiLower(orig);

    // Drop <!-- comments --> (both strings stay index-aligned)
    size_t pos;
    while ((pos = low.find("<!--")) != std::string::npos) {
        size_t end = low.find("-->", pos + 4);
        size_t len = (end == std::string::npos) ? orig.size() - pos : end + 3 - pos;
        orig.erase(pos, len);
        low.erase(pos, len);
    }

    // Drop <script>/<style>/<noscript> blocks entirely
    const char *kDropBlocks[] = {"script", "style", "noscript"};
    for (const char *tag : kDropBlocks) {
        std::string open = std::string("<") + tag;
        while ((pos = low.find(open)) != std::string::npos) {
            size_t tagEnd = low.find('>', pos);
            size_t len;
            if (tagEnd == std::string::npos) {
                len = orig.size() - pos;  // unterminated tag: drop the rest
            } else {
                size_t close = low.find(std::string("</") + tag, tagEnd + 1);
                if (close == std::string::npos) {
                    len = tagEnd + 1 - pos;  // unterminated block: drop the open tag
                } else {
                    size_t closeEnd = low.find('>', close);
                    len = (closeEnd == std::string::npos) ? orig.size() - pos : closeEnd + 1 - pos;
                }
            }
            orig.erase(pos, len);
            low.erase(pos, len);
        }
    }

    // Single pass: strip tags (quote-aware), turn block boundaries into newlines
    std::string out;
    out.reserve(orig.size());
    size_t i = 0;
    while (i < orig.size()) {
        if (orig[i] == '<') {
            size_t j = i + 1;
            char quote = 0;
            for (; j < orig.size(); ++j) {
                char c = orig[j];
                if (quote) { if (c == quote) quote = 0; }
                else if (c == '\'' || c == '"') quote = c;
                else if (c == '>') break;
            }
            if (j >= orig.size()) break;  // unterminated tag: drop the rest
            if (isBlockTag(low.substr(i, j - i + 1))) out += '\n';
            i = j + 1;
        } else {
            out += orig[i++];
        }
    }

    return collapseWhitespace(decodeEntities(out));
}

// Text of <title>...</title>, entities decoded, whitespace collapsed
std::string extractTitle(const std::string &html)
{
    std::string low = asciiLower(html);
    size_t a = low.find("<title");
    if (a == std::string::npos) return "";
    size_t gt = low.find('>', a);
    if (gt == std::string::npos) return "";
    size_t b = low.find("</title", gt);
    if (b == std::string::npos) return "";
    return collapseWhitespace(decodeEntities(html.substr(gt + 1, b - gt - 1)));
}

// Multibyte-safe cut: never splits a UTF-8 sequence in half
std::string truncateUtf8(const std::string &s, size_t maxChars)
{
    if (s.size() <= maxChars) return s;
    size_t cut = maxChars;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "\n\n[Content truncated at " + std::to_string(cut) +
           " characters; re-run with a larger max_length if needed]";
}

// ---- libcurl fetch ----

struct WebFetchResult {
    std::string body;
    std::string contentType;
    std::string effectiveUrl;
    long httpCode = 0;
    CURLcode code = CURLE_OK;
};

struct DownloadCtx {
    std::string *body;
    bool capped = false;
};

size_t bodyCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    auto *ctx = static_cast<DownloadCtx *>(userp);
    size_t total = size * nmemb;
    if (ctx->body->size() + total > kMaxDownloadBytes) {
        ctx->capped = true;
        total = (ctx->body->size() < kMaxDownloadBytes)
                    ? kMaxDownloadBytes - ctx->body->size() : 0;
    }
    ctx->body->append(static_cast<char *>(contents), total);
    return total;
}

size_t headerCallback(char *buffer, size_t size, size_t nitems, void *userp)
{
    auto *r = static_cast<WebFetchResult *>(userp);
    std::string line(buffer, size * nitems);
    if (asciiLower(line).rfind("content-type:", 0) == 0) {
        r->contentType = trimStr(line.substr(13));  // last header wins (post-redirect)
    }
    return size * nitems;
}

WebFetchResult webFetch(const std::string &url)
{
    WebFetchResult r;
    CURL *curl = curl_easy_init();
    if (!curl) {
        r.code = CURLE_FAILED_INIT;
        return r;
    }

    DownloadCtx ctx;
    ctx.body = &r.body;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                     "(KHTML, like Gecko) Chrome/124.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // gzip/deflate when built with zlib
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, bodyCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &r);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#ifdef CURLSSLOPT_NATIVE_CA
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif
    // Same TLS posture as HttpClient: permissive for proxy/cert compatibility
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_ALPN, 0L);
    std::string proxyUrl = HttpClient::proxy();
    if (!proxyUrl.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, proxyUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
    }

    r.code = curl_easy_perform(curl);
    if (r.code == CURLE_WRITE_ERROR && ctx.capped) {
        r.code = CURLE_OK;  // size cap reached: keep the truncated body
    }
    if (r.code == CURLE_OK) {
        const char *eff = nullptr;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
        if (eff) r.effectiveUrl = eff;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.httpCode);
    }
    curl_easy_cleanup(curl);
    return r;
}

} // namespace

std::string FetchTool::description() const
{
    return "Fetch the content of a web page over HTTP/HTTPS using libcurl and "
           "return it as readable text (HTML converted to plain text). Use this "
           "tool when you need to read the latest content of a website — for "
           "example up-to-date information, documentation or news that you "
           "cannot access directly — so the page text is provided back to you "
           "for further processing.";
}

json FetchTool::parameters() const
{
    return {
        {"type", "object"},
        {"properties", {
            {"url", {
                {"type", "string"},
                {"description", "The http/https URL of the web page to fetch (scheme optional, defaults to https)"}
            }},
            {"max_length", {
                {"type", "integer"},
                {"description", "Maximum characters of text to return (default 20000)"}
            }}
        }}
    };
}

ToolResult FetchTool::execute(const json &args, const std::string &cwd)
{
    (void)cwd;  // web access does not depend on the working directory

    std::string url = args.value("url", "");
    int maxLength = args.value("max_length", static_cast<int>(kDefaultMaxLength));
    if (maxLength <= 0) maxLength = static_cast<int>(kDefaultMaxLength);

    if (!url.empty()) return fetchPage(url, maxLength);
    return {false, "", "Missing required parameter: 'url' (the web page to fetch)", "fetch"};
}

ToolResult FetchTool::fetchPage(const std::string &rawUrl, int maxLength)
{
    ToolResult result;
    result.title = "fetch: " + rawUrl;

    std::string url = rawUrl;
    auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        url = "https://" + url;
    } else {
        std::string scheme = asciiLower(url.substr(0, schemeEnd));
        if (scheme != "http" && scheme != "https") {
            result.success = false;
            result.error = "Unsupported URL scheme '" + scheme + "': only http/https";
            return result;
        }
    }

    WebFetchResult r = webFetch(url);
    if (r.code != CURLE_OK) {
        result.success = false;
        result.error = std::string("Request failed: ") + curl_easy_strerror(r.code) + " (" + url + ")";
        return result;
    }
    if (r.httpCode >= 400) {
        result.success = false;
        result.error = "HTTP " + std::to_string(r.httpCode) + " from " + url +
                       (r.body.empty() ? "" : (": " + trimStr(r.body.substr(0, 200))));
        return result;
    }
    if (r.body.empty()) {
        result.success = false;
        result.error = "Empty response from " + url;
        return result;
    }

    std::string lowType = asciiLower(r.contentType);
    if (lowType.rfind("image/", 0) == 0 || lowType.find("octet-stream") != std::string::npos ||
        lowType.find("application/pdf") != std::string::npos) {
        result.success = false;
        result.error = "Binary content (" + r.contentType + ") cannot be converted to text: " + url;
        return result;
    }

    bool isHtml = lowType.find("html") != std::string::npos;
    if (!isHtml && r.contentType.empty()) {
        std::string head = asciiLower(r.body.substr(0, 512));
        isHtml = head.find("<html") != std::string::npos || head.find("<!doctype html") != std::string::npos;
    }

    std::string charset = extractCharset(r.contentType, r.body);
    std::string title;
    std::string text;
    if (isHtml) {
        title = extractTitle(r.body);
        text = htmlToText(toUtf8(r.body, charset));
    } else {
        text = trimStr(toUtf8(r.body, charset));
    }

    std::string shownUrl = r.effectiveUrl.empty() ? url : r.effectiveUrl;
    std::string output = "URL: " + shownUrl + "\n";
    if (!title.empty()) output += "Title: " + title + "\n";
    output += "\n" + text;

    if (output.size() > static_cast<size_t>(maxLength)) {
        output = truncateUtf8(output, static_cast<size_t>(maxLength));
    }

    result.success = true;
    result.output = output;
    return result;
}
