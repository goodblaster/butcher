/**
 * @file editor.cpp
 *
 * See editor.h.
 */
#include "editor.h"

std::string editor_sanitize_name(const std::string &in)
{
	std::string out;
	for (char c : in) {
		unsigned char u = (unsigned char)c;
		if (u < 0x20 || u == 0x7F)
			continue; /* control characters, including a stray newline */
		if (c == '/' || c == '\\' || c == ':')
			continue; /* path separators have no business in a name */
		if ((int)out.size() >= PLR_NAME_LEN - 1)
			break; /* the save holds 31 characters plus a terminator */
		out += c;
	}
	return out;
}
