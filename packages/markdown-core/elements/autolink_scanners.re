#include "autolink_scanners.h"
#include <string.h>

/*!include:re2c "scanner_common.re" */

/* The grammar's bounded repeats -- a scheme of 2..32 bytes, domain labels
 * of 1..63 -- are length checks on the recognized span, not DFA states:
 * scheme bytes exclude `:`, so the scheme ends at the first colon, and a
 * domain label ends at a dot or at the closing `>`. */
static bufsize_t autolink_scheme_length_ok(const unsigned char *input, size_t end)
{
  const unsigned char *colon = (const unsigned char *)memchr(input, ':', end);
  size_t length = (size_t)(colon - input);
  return length >= 2 && length <= 32;
}

static bufsize_t autolink_labels_ok(const unsigned char *input, size_t end)
{
  const unsigned char *at = (const unsigned char *)memchr(input, '@', end) + 1;
  const unsigned char *limit = input + end - 1;
  while (at < limit) {
    const unsigned char *dot = (const unsigned char *)memchr(at, '.', (size_t)(limit - at));
    const unsigned char *label_end = dot ? dot : limit;
    if (label_end - at > 63) {
      return 0;
    }
    at = label_end + 1;
  }
  return 1;
}

/*!re2c
  scheme = [A-Za-z][A-Za-z0-9.+-]*;
  label = [a-zA-Z0-9]([a-zA-Z0-9-]*[a-zA-Z0-9])?;
*/

bufsize_t scan_autolink_uri(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  scheme [:][^\x00-\x20<>]*[>]  { return autolink_scheme_length_ok(input, p) ? (bufsize_t)(p - start) : 0; }
  * { return 0; }
*/
}

bufsize_t scan_autolink_email(const unsigned char *data, bufsize_t length, bufsize_t offset)
{
  if (!data || offset < 0 || offset >= length) {
    return 0;
  }
  const unsigned char *input = data + offset;
  size_t p = 0, remaining = (size_t)(length - offset);
  size_t marker = 0;
  size_t start = p;
/*!re2c
  [a-zA-Z0-9.!#$%&'*+/=?^_`{|}~-]+
    [@]
    label
    ([.] label)*
    [>] { return autolink_labels_ok(input, p) ? (bufsize_t)(p - start) : 0; }
  * { return 0; }
*/
}
