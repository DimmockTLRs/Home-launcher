#ifndef FLEX_LAUNCHER_WEBSITE_H
#define FLEX_LAUNCHER_WEBSITE_H

#include <stdbool.h>

// Returns NULL for other commands and an empty string for an unconfigured card.
const char *home_assistant_address(const char *command);
bool valid_home_assistant_url(const char *url);

#endif
