// Descriptions and details from a doc comment (SPEC §4.2, §14.7).
#pragma once

#include <cstddef>
#include <string>

namespace docuconf {

/// The most characters (Unicode code points) an input's details may have.
inline constexpr std::size_t kMaxDetails = 4000;

/// A doc comment split into the contract's description and details.
struct Doc {
    /// The first paragraph, on one line, without a final period.
    std::string description;
    /// The rest of the comment, as CommonMark; empty when there is none.
    std::string details;
};

/// Splits a Doxygen doc comment, the way C++ programmers document a
/// setting, into a description and details:
///
/// - Comment markers (`///`, `//!`, `/**`, ` * `, `*/`, `///<`) are
///   stripped, so a comment can be pasted as is, or written without them.
/// - The first paragraph is the description, on one line, without a final
///   period. `@brief` is dropped.
/// - The rest is details, converted to CommonMark: `@c x`, `@p x`,
///   `@ref x`, `<tt>x</tt>` and `<code>x</code>` become code spans, `@a`
///   and `@e` emphasis, `@b` strong; `@code{.cpp}` ... `@endcode` and
///   `@verbatim` become fenced code blocks; `@li` a list item; `@note` and
///   `@warning` a bold label; `@see` "See". Tags that document functions
///   (`@param`, `@return`, `@throws`...) and grouping commands are dropped.
///
/// A comment that does not start with a paragraph (it starts with a list
/// or a code block) is all description.
Doc split_doc(const std::string& comment);

}  // namespace docuconf
