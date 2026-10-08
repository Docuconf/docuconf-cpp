// Descriptions and details (SPEC §4.2, §14.7): a Doxygen doc comment split
// into the two, the details() option, their checks, and contract-first
// loading of a contract with details.
#include <gtest/gtest.h>

#include <string>

#include "docuconf/docuconf.hpp"

namespace {

void expect_doc(const std::string& comment, const std::string& desc, const std::string& details) {
    docuconf::Doc d = docuconf::split_doc(comment);
    EXPECT_EQ(d.description, desc) << comment;
    EXPECT_EQ(d.details, details) << comment;
}

TEST(SplitDoc, OneParagraphIsTheDescription) {
    expect_doc("", "", "");
    expect_doc("HTTP listen port.", "HTTP listen port", "");
    expect_doc("Certificate to serve HTTPS with.\nWithout it, the service serves HTTP.",
               "Certificate to serve HTTPS with. Without it, the service serves HTTP", "");
    expect_doc("/// @brief HTTP listen port.", "HTTP listen port", "");
}

TEST(SplitDoc, LaterParagraphsAreDetails) {
    expect_doc("Number of workers.\n\nEach holds a database connection,\nso keep it below the pool size.\n\n"
               "Raise it when the queue grows.\n",
               "Number of workers",
               "Each holds a database connection,\nso keep it below the pool size.\n\nRaise it when the queue grows.");
}

TEST(SplitDoc, CommentMarkersAreStripped) {
    expect_doc("    /// Queue depth.\n    ///\n    /// Alert above 10.\n", "Queue depth", "Alert above 10.");
    expect_doc("//! Queue depth.\n//!\n//! Alert above 10.", "Queue depth", "Alert above 10.");
    expect_doc("/**\n * Queue depth.\n *\n * Alert above 10.\n */", "Queue depth", "Alert above 10.");
    expect_doc("/** Queue depth.\n *\n * Alert above 10. */", "Queue depth", "Alert above 10.");
    expect_doc("///< Queue depth.", "Queue depth", "");
}

TEST(SplitDoc, ListsHeadingsAndCodeBlocks) {
    expect_doc(R"(/// Request timeout.
///
/// # Choosing a value
///
/// Measure first:
/// - p99 latency
/// - retries
///
/// ```sh
/// curl -w '%{time_total}' $URL
/// ```
///
///     indented code
///     @c stays as written)",
               "Request timeout",
               "# Choosing a value\n\nMeasure first:\n- p99 latency\n- retries\n\n```sh\ncurl -w '%{time_total}' $URL\n```\n\n"
               "    indented code\n    @c stays as written");
}

TEST(SplitDoc, DoxygenCommandsBecomeCommonMark) {
    expect_doc(R"(/**
 * @brief Upstream timeout, in @c Duration units.
 *
 * @details See @ref Gateway::timeout and \p limit, <tt>x < y</tt>, <code>z</code>,
 * @a soft, @b hard and `@c in a code span`.
 * @li first
 * @li second
 *
 * @note Applies to retries.
 * @warning Not below 1s.
 * @see LoadBalancer
 * @param limit Dropped: it documents a function.
 * @return Dropped too.
 *
 * @code
 * auto t = std::chrono::seconds(30); // @c not converted
 * @endcode
 *
 * \code{.py}
 * t = 30
 * \endcode
 *
 * @verbatim
 * raw <tt>text</tt>
 * @endverbatim
 */)",
               "Upstream timeout, in `Duration` units",
               "See `Gateway::timeout` and `limit`, `x < y`, `z`,\n*soft*, **hard** and `@c in a code span`.\n"
               "- first\n- second\n\n**Note:** Applies to retries.\n**Warning:** Not below 1s.\nSee LoadBalancer\n\n"
               "```cpp\nauto t = std::chrono::seconds(30); // @c not converted\n```\n\n```py\nt = 30\n```\n\n"
               "```\nraw <tt>text</tt>\n```");
}

TEST(SplitDoc, NonAscii) {
    expect_doc("Grußtext für die Startseite.\n\nZeigt «ça va» und 東京.", "Grußtext für die Startseite",
               "Zeigt «ça va» und 東京.");
}

TEST(SplitDoc, StartingWithAListIsAllDescription) {
    expect_doc("- first\n- second", "- first - second", "");
}

std::string japanese(std::size_t pairs) {
    std::string s;
    for (std::size_t i = 0; i < pairs; ++i) s += "日本";
    return s;
}

TEST(Details, AreExportedRightAfterTheDescription) {
    CLI::App app;
    docuconf::Declaration d{app, "svc"};
    int workers = 0;
    std::optional<int> queue;
    d.add_var("WORKERS", workers)
        .doc("/// Worker count.\n///\n/// Keep it below the pool size.\n")
        .default_val(4);
    d.add_var("QUEUE", queue, "Queue length per worker").details("Per worker, *not* in total.");
    std::string cue = d.export_cue();
    EXPECT_NE(cue.find("description: \"Worker count\"\n\t\t\tdetails: \"Keep it below the pool size.\"\n"),
              std::string::npos)
        << cue;
    EXPECT_NE(cue.find("details: \"Per worker, *not* in total.\""), std::string::npos) << cue;
    auto j = d.export_json();
    EXPECT_EQ(j["vars"]["WORKERS"].begin().key(), "type");
    EXPECT_EQ(std::next(j["vars"]["WORKERS"].begin(), 2).key(), "details");
}

TEST(Details, DeclarationMistakes) {
    CLI::App app;
    docuconf::Declaration d{app, "svc"};
    int a = 0, b = 0, c = 0, e = 0;
    d.add_var("UNDESCRIBED", a);
    d.add_var("BLANK", b, "Blank details").details(" \n\t");
    d.add_var("TOO_LONG", c).doc("Too much to say.\n\n" + japanese(2000) + "日");
    d.add_var("AT_LIMIT", e).doc("Just enough to say.\n\n" + japanese(2000));
    try {
        d.check();
        FAIL() << "expected a DeclarationError";
    } catch (const docuconf::DeclarationError& err) {
        std::string what = err.what();
        EXPECT_NE(what.find("UNDESCRIBED: needs a description"), std::string::npos) << what;
        EXPECT_NE(what.find("BLANK: details must not be blank"), std::string::npos) << what;
        EXPECT_NE(what.find("TOO_LONG: details are 4001 characters; details may have at most 4000"),
                  std::string::npos)
            << what;
        EXPECT_EQ(what.find("AT_LIMIT"), std::string::npos) << what;
    }
}

TEST(Details, ContractFirstLoadsAndIgnoresThem) {
    auto contract = docuconf::Contract::from_json(nlohmann::json::parse(R"({
        "apiVersion": "docuconf.dev/v1alpha1", "kind": "ConfigContract", "metadata": {"name": "svc"},
        "vars": {"PORT": {"type": "int", "description": "HTTP listen port",
                          "details": "Behind the mesh, keep the **default**.\n\n- one\n- two", "default": 8080}}})"));
    EXPECT_EQ(contract.load({}).get("PORT")->as_int(), 8080);

    for (auto [details, want] : std::vector<std::pair<nlohmann::json, std::string>>{
             {" \n", "PORT: details must not be blank"},
             {japanese(2000) + "日", "PORT: details are 4001 characters"},
             {42, "PORT: details must be a string"}}) {
        nlohmann::json c = {{"apiVersion", "docuconf.dev/v1alpha1"},
                            {"kind", "ConfigContract"},
                            {"metadata", {{"name", "svc"}}},
                            {"vars", {{"PORT", {{"type", "int"}, {"description", "HTTP listen port"}, {"details", details}}}}}};
        try {
            docuconf::Contract::from_json(c);
            FAIL() << "expected a DeclarationError for " << details.dump();
        } catch (const docuconf::DeclarationError& err) {
            EXPECT_NE(std::string(err.what()).find(want), std::string::npos) << err.what();
        }
    }
}

}  // namespace
