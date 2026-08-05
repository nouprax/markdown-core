/** The canonical one-complete-comment rule shared by `HTMLBlock` and `HTML`:
 * after surrounding whitespace the literal opens with `<!--` and its first
 * `-->` is the terminal bytes. Comment-prefixed HTML with a same-line tail is
 * not a comment. Derived purely from the literal, matching the C facade's
 * `markdown_core_node_html_comment`. */
export function htmlLiteralIsComment(literal: string): boolean {
    let end = literal.length;
    while (end > 0 && (literal[end - 1] === "\n" || literal[end - 1] === " " || literal[end - 1] === "\t")) end--;
    let start = 0;
    while (start < end && (literal[start] === " " || literal[start] === "\t")) start++;
    if (end - start < 4 || !literal.startsWith("<!--", start)) return false;
    for (let index = start + 1; index + 3 <= end; index++) {
        if (literal[index] === "-" && literal[index + 1] === "-" && literal[index + 2] === ">") {
            return index + 3 === end;
        }
    }
    return false;
}
