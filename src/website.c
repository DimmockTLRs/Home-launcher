#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <SDL.h>
#include "launcher.h"
#include "platform/platform.h"

static bool decimal(char c)
{
    return c >= '0' && c <= '9';
}

static bool alpha_numeric(char c)
{
    return decimal(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool hexadecimal(char c)
{
    return decimal(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool valid_hostname(const char *host, const char *end)
{
    if (host == end || end - host > 254)
        return false;

    while (host < end) {
        const char *dot = memchr(host, '.', (size_t)(end - host));
        const char *label_end = dot != NULL ? dot : end;
        if (label_end == host || label_end - host > 63 ||
            !alpha_numeric(*host) || !alpha_numeric(label_end[-1]))
            return false;
        for (const char *c = host; c < label_end; c++) {
            if (!alpha_numeric(*c) && *c != '-' && *c != '_')
                return false;
        }
        host = dot != NULL ? dot + 1 : end;
    }
    return true;
}

static bool valid_ipv4_tail(const char *start, const char *end)
{
    for (int i = 0; i < 4; i++) {
        const char *octet = start;
        unsigned int value = 0;
        while (start < end && decimal(*start)) {
            value = value * 10 + (unsigned int)(*start++ - '0');
            if (value > 255 || start - octet > 3)
                return false;
        }
        if (start == octet || (start - octet > 1 && *octet == '0'))
            return false;
        if (i == 3)
            return start == end;
        if (start == end || *start++ != '.')
            return false;
    }
    return false;
}

static bool valid_ipv6(const char *start, const char *end)
{
    int groups = 0;
    bool compressed = false;
    if (start < end && *start == ':') {
        if (end - start < 2 || start[1] != ':')
            return false;
        start += 2;
        compressed = true;
    }

    while (start < end) {
        const char *part = start;
        while (start < end && hexadecimal(*start))
            start++;
        if (start < end && *start == '.') {
            if (!valid_ipv4_tail(part, end))
                return false;
            groups += 2;
            break;
        }
        if (start == part || start - part > 4)
            return false;
        groups++;
        if (groups > 8)
            return false;
        if (start == end)
            break;
        if (*start++ != ':')
            return false;
        if (start < end && *start == ':') {
            if (compressed)
                return false;
            compressed = true;
            start++;
        }
        else if (start == end)
            return false;
    }
    return compressed ? groups < 8 : groups == 8;
}

static bool valid_port(const char *port, const char *end)
{
    if (port == end)
        return true;
    if (*port++ != ':' || port == end)
        return false;
    unsigned int number = 0;
    while (port < end) {
        if (!decimal(*port))
            return false;
        number = number * 10 + (unsigned int)(*port++ - '0');
        if (number > 65535)
            return false;
    }
    return number != 0;
}

bool valid_website_url(const char *url)
{
    if (url == NULL)
        return false;
    const char *host;
    if (SDL_strncasecmp(url, "https://", 8) == 0)
        host = url + 8;
    else if (SDL_strncasecmp(url, "http://", 7) == 0)
        host = url + 7;
    else
        return false;

    const char *authority_end = host + strcspn(host, "/?#");
    const char *port;
    if (*host == '[') {
        const char *close = memchr(host, ']', (size_t)(authority_end - host));
        if (close == NULL || !valid_ipv6(host + 1, close))
            return false;
        port = close + 1;
    }
    else {
        port = memchr(host, ':', (size_t)(authority_end - host));
        if (port == NULL)
            port = authority_end;
        if (!valid_hostname(host, port))
            return false;
    }
    if (!valid_port(port, authority_end))
        return false;

    // Treat the rest as URI data. Require percent escapes for whitespace,
    // double quotes, backslashes and characters outside the URI alphabet.
    for (const char *c = authority_end; *c != '\0'; c++) {
        if (*c == '%') {
            if (!hexadecimal(c[1]) || !hexadecimal(c[2]))
                return false;
            c += 2;
        }
        else if (!alpha_numeric(*c) && strchr("-._~!$&'()*+,;=:@/?#[]", *c) == NULL)
            return false;
    }
    return true;
}
