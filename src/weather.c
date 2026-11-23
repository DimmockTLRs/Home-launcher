#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#elif defined(HAVE_WEATHER_CURL)
#include <curl/curl.h>
#endif
#include <SDL.h>
#include <SDL_ttf.h>
#include "launcher.h"
#include "image.h"
#include "weather.h"
#include "debug.h"

#define WEATHER_HOST "api.open-meteo.com"
#define WEATHER_REFRESH_MS (20u * 60 * 1000)
#define WEATHER_RETRY_MS (5u * 60 * 1000)
#define WEATHER_TIMEOUT_MS 6500
#define WEATHER_BODY_LIMIT 8192

typedef struct {
    char text[WEATHER_BODY_LIMIT];
    size_t length;
} WeatherBody;

typedef struct {
    double temperature;
    int code;
} WeatherReading;

static struct {
    TTF_Font *font;
    SDL_Texture *heading;
    SDL_Texture *caption;
    SDL_Thread *worker;
    SDL_atomic_t ready;
    Uint32 next_request;
    WeatherReading result;
    WeatherReading last;
    bool success;
    bool have_reading;
    bool request_enabled;
#ifdef HAVE_WEATHER_CURL
    bool curl_initialized;
#endif
} weather;

extern Config config;

#ifdef _WIN32
static bool fetch_weather(const char *path, WeatherBody *body)
{
    wchar_t target[256];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, target, SDL_arraysize(target)) == 0)
        return false;

    HINTERNET session = WinHttpOpen(L"Flex Launcher weather", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == NULL)
        return false;

    if (!WinHttpSetTimeouts(session, 2000, 2500, 2500, 2500)) {
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET host = WinHttpConnect(session, L"api.open-meteo.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = host == NULL ? NULL : WinHttpOpenRequest(host, L"GET", target, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    DWORD status = 0;
    DWORD size = sizeof(status);
    DWORD no_redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    Uint32 start = SDL_GetTicks();
    bool ok = request != NULL &&
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &no_redirects, sizeof(no_redirects)) &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, NULL) &&
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) && status == 200;

    bool complete = false;
    while (ok && SDL_GetTicks() - start < WEATHER_TIMEOUT_MS && body->length < sizeof(body->text) - 1) {
        DWORD read = 0;
        if (!WinHttpReadData(request, body->text + body->length,
                (DWORD)(sizeof(body->text) - body->length - 1), &read))
            break;
        if (read == 0) {
            complete = true;
            break;
        }
        body->length += read;
    }
    if (request != NULL)
        WinHttpCloseHandle(request);
    if (host != NULL)
        WinHttpCloseHandle(host);
    WinHttpCloseHandle(session);
    return complete;
}
#elif defined(HAVE_WEATHER_CURL)
static size_t read_weather(char *data, size_t size, size_t count, void *context)
{
    WeatherBody *body = context;
    size_t length = size * count;
    if (length >= sizeof(body->text) - body->length)
        return 0;
    memcpy(body->text + body->length, data, length);
    body->length += length;
    return length;
}

static bool fetch_weather(const char *path, WeatherBody *body)
{
    char url[320];
    SDL_snprintf(url, sizeof(url), "https://" WEATHER_HOST "%s", path);
    CURL *request = curl_easy_init();
    if (request == NULL)
        return false;
    curl_easy_setopt(request, CURLOPT_URL, url);
    curl_easy_setopt(request, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(request, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(request, CURLOPT_TIMEOUT_MS, (long)WEATHER_TIMEOUT_MS);
    curl_easy_setopt(request, CURLOPT_USERAGENT, "Flex Launcher weather");
    curl_easy_setopt(request, CURLOPT_WRITEFUNCTION, read_weather);
    curl_easy_setopt(request, CURLOPT_WRITEDATA, body);
    CURLcode result = curl_easy_perform(request);
    long status = 0;
    curl_easy_getinfo(request, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(request);
    return result == CURLE_OK && status == 200;
}
#else
static bool fetch_weather(const char *path, WeatherBody *body)
{
    (void)path;
    (void)body;
    return false;
}
#endif

static const char *skip_space(const char *text)
{
    while (isspace((unsigned char)*text))
        text++;
    return text;
}

// Open-Meteo puts these plain numeric fields inside its current conditions object.
static bool read_number(const char *object, const char *limit, const char *key, double *result)
{
    const char *field = strstr(object, key);
    if (field == NULL || field >= limit)
        return false;
    field = skip_space(field + strlen(key));
    if (*field != ':')
        return false;
    field = skip_space(field + 1);
    if (*field != '-' && !isdigit((unsigned char)*field))
        return false;
    char *end;
    double number = strtod(field, &end);
    if (end == field || end > limit)
        return false;
    const char *next = skip_space(end);
    if (next > limit || (*next != ',' && *next != '}'))
        return false;
    *result = number;
    return true;
}

static bool parse_weather(const char *json, WeatherReading *result)
{
    const char *field = strstr(json, "\"current\"");
    if (field == NULL)
        return false;
    field = skip_space(field + sizeof("\"current\"") - 1);
    if (*field != ':')
        return false;
    field = skip_space(field + 1);
    if (*field != '{')
        return false;
    const char *end = strchr(field, '}');
    double temperature, code;
    if (end == NULL || !read_number(field, end, "\"temperature_2m\"", &temperature) ||
        !read_number(field, end, "\"weather_code\"", &code) ||
        !(temperature >= -200 && temperature <= 200 && code >= 0 && code <= 99) || code != (int)code)
        return false;
    *result = (WeatherReading){ temperature, (int)code };
    return true;
}

static int request_weather(void *unused)
{
    (void)unused;
    char path[256];
    SDL_snprintf(path, sizeof(path),
        "/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code&temperature_unit=%s",
        config.weather.latitude, config.weather.longitude,
        config.weather.fahrenheit ? "fahrenheit" : "celsius");
    WeatherBody body = {0};
    weather.success = fetch_weather(path, &body) && parse_weather(body.text, &weather.result);
    SDL_AtomicSet(&weather.ready, 1);
    return 0;
}

static const char *condition(int code)
{
    if (code == 0) return "Clear";
    if (code == 1 || code == 2) return "Partly cloudy";
    if (code == 3) return "Cloudy";
    if (code == 45 || code == 48) return "Fog";
    if (code >= 51 && code <= 57) return "Drizzle";
    if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return "Rain";
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "Snow";
    if (code >= 95) return "Thunderstorms";
    return "Current conditions";
}

static void set_weather_text(bool stale)
{
    char heading[96];
    if (weather.have_reading)
        SDL_snprintf(heading, sizeof(heading), "%.0f \xc2\xb0%s  %s",
            weather.last.temperature, config.weather.fahrenheit ? "F" : "C", condition(weather.last.code));
    else
        SDL_strlcpy(heading, stale ? "Weather unavailable" : "Loading weather", sizeof(heading));
    char caption[128];
    const char *location = config.weather.location;
    SDL_snprintf(caption, sizeof(caption), "%s%sOpen-Meteo%s", location,
        location[0] != '\0' ? "  /  " : "", stale && weather.have_reading ? "  /  Last known" : "");

    SDL_DestroyTexture(weather.heading);
    SDL_DestroyTexture(weather.caption);
    SDL_Color white = { 247, 250, 255, 255 };
    SDL_Color subdued = { 191, 208, 228, 255 };
    weather.heading = load_texture(TTF_RenderUTF8_Blended(weather.font, heading, white));
    weather.caption = load_texture(TTF_RenderUTF8_Blended(weather.font, caption, subdued));
}

void init_weather(TTF_Font *font)
{
    if (!config.weather.enabled)
        return;

    weather.font = font;
    weather.request_enabled = config.weather.latitude >= -90 && config.weather.latitude <= 90 &&
        config.weather.longitude >= -180 && config.weather.longitude <= 180;
#if defined(HAVE_WEATHER_CURL)
    if (weather.request_enabled) {
        weather.curl_initialized = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
        weather.request_enabled = weather.curl_initialized;
    }
#elif !defined(_WIN32)
    weather.request_enabled = false;
#endif
    if (!weather.request_enabled)
        log_error("Weather unavailable: a network backend and valid latitude/longitude are required");
    weather.next_request = SDL_GetTicks();
    set_weather_text(!weather.request_enabled);
}

bool update_weather(Uint32 now)
{
    if (!weather.request_enabled)
        return false;
    if (weather.worker != NULL) {
        if (!SDL_AtomicGet(&weather.ready))
            return false;
        SDL_WaitThread(weather.worker, NULL);
        weather.worker = NULL;
        if (weather.success) {
            weather.last = weather.result;
            weather.have_reading = true;
        }
        else
            log_debug("Weather request failed; keeping the last available reading");
        weather.next_request = now + (weather.success ? WEATHER_REFRESH_MS : WEATHER_RETRY_MS);
        set_weather_text(!weather.success);
        return true;
    }
    if (!SDL_TICKS_PASSED(now, weather.next_request))
        return false;
    SDL_AtomicSet(&weather.ready, 0);
    weather.worker = SDL_CreateThread(request_weather, "Weather request", NULL);
    if (weather.worker == NULL) {
        weather.next_request = now + WEATHER_RETRY_MS;
        set_weather_text(true);
        return true;
    }
    return false;
}

static SDL_Rect text_bounds(SDL_Texture *texture, int height, int max_width)
{
    SDL_Rect rect = {0};
    if (texture != NULL && SDL_QueryTexture(texture, NULL, NULL, &rect.w, &rect.h) == 0 && rect.h > 0) {
        rect.w = SDL_max(1, (int)((Sint64)height * rect.w / rect.h));
        rect.h = height;
        if (rect.w > max_width) {
            rect.h = SDL_max(1, (int)((Sint64)rect.h * max_width / rect.w));
            rect.w = max_width;
        }
    }
    return rect;
}

void draw_weather(SDL_Renderer *renderer, int width, int height, bool clock_on_left)
{
    if (weather.heading == NULL && weather.caption == NULL)
        return;
    int margin = height / 20;
    int pad = SDL_max(3, height / 90);
    SDL_Rect heading = text_bounds(weather.heading, SDL_max(1, height / 25), SDL_max(1, width * 2 / 5));
    SDL_Rect caption = text_bounds(weather.caption, SDL_max(1, height / 44), SDL_max(1, width * 2 / 5));
    SDL_Rect card = {margin, margin, SDL_max(heading.w, caption.w) + 2 * pad,
        heading.h + caption.h + 2 * pad + pad / 2};
    if (clock_on_left)
        card.x = width - margin - card.w;
    heading.x = caption.x = card.x + pad;
    heading.y = card.y + pad;
    caption.y = heading.y + heading.h + pad / 2;

    Uint8 red, green, blue, alpha;
    SDL_GetRenderDrawColor(renderer, &red, &green, &blue, &alpha);
    SDL_SetRenderDrawColor(renderer, 12, 19, 32, 216);
    SDL_RenderFillRect(renderer, &card);
    SDL_SetRenderDrawColor(renderer, red, green, blue, alpha);
    if (weather.heading != NULL)
        SDL_RenderCopy(renderer, weather.heading, NULL, &heading);
    if (weather.caption != NULL)
        SDL_RenderCopy(renderer, weather.caption, NULL, &caption);
}

void quit_weather(void)
{
    if (weather.worker != NULL)
        SDL_WaitThread(weather.worker, NULL);
    SDL_DestroyTexture(weather.heading);
    SDL_DestroyTexture(weather.caption);
#ifdef HAVE_WEATHER_CURL
    if (weather.curl_initialized)
        curl_global_cleanup();
#endif
}
