/*
 * ui.cpp - the menus: the start screen, the settings window and the pause menu.
 *
 * They are RmlUi documents (res/ui: .rml files are HTML-like markup, .rcss files CSS-like style sheets) in our
 * own "Koholint at night" look. RmlUi lays them out, animates them and tells ui_render.cpp what to draw;
 * this file loads them, feeds them input and reacts to clicks. The controller moves the focus
 * between buttons (RmlUi's spatial navigation: the arrow keys) and A "clicks" the focused one
 * (RmlUi clicks the focused element on Enter); touch works as a mouse.
 *
 * Settings are a table (kSettings): each one is an environment variable the runtime already reads
 * (LAA_DOCKED, LAA_SHOW_FPS, ...) and its choices. They are saved in the config file as
 * {"NAME": "value", ...} and set as environment variables when the game starts; a variable that was
 * already set (env.txt, the shell) stays as it is, and the settings window shows it as fixed.
 */
#include "ui.h"
#include "ui_internal.h"
extern "C" {
#include "game_lib.h"
}
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <map>
#include <string>
#include <vector>

#ifndef LAA_VERSION
#define LAA_VERSION "dev"
#endif
#if __has_include("laa_build_info.h")
#include "laa_build_info.h"                              /* (LAA_COMMIT, made at every build) */
#endif
#ifndef LAA_COMMIT
#define LAA_COMMIT "unknown"
#endif

#define LAA_PROJECT_URL "https://github.com/timbo722/ZLAA"
#define LAA_PROJECT_URL_SHORT "github.com/timbo722/ZLAA"

namespace {

/* ---- settings ---- */

struct Choice { const char *label, *value; };          /* value NULL: the variable is left unset */
struct Setting {
    const char *tab, *name, *var;
    std::vector<Choice> choices;
    int fallback;                                       /* the choice when the config doesn't say */
    const char *help;
};

const char *const kTabs[] = { "Video", "Input", "Audio", "Interface", "About" };

#define NEXT_START " Applies the next time the game starts."

/* (named and worded as Dusklight's settings: a title, one short sentence) */
const std::vector<Setting> kSettings = {
    { "Video", "Console Mode", "LAA_DOCKED", { { "Handheld (720p)", NULL }, { "Docked (1080p)", "1" } }, 0,
      "Docked renders at up to 1080p: sharper, but uses more power." NEXT_START },
    { "Video", "Show FPS Counter", "LAA_SHOW_FPS", { { "Off", NULL }, { "On", "1" } }, 0,
      "Show the frame rate in a corner of the screen." },
    { "Video", "FPS Counter Position", "LAA_FPS_CORNER",
      { { "Bottom Left", NULL }, { "Bottom Right", "1" }, { "Top Left", "2" }, { "Top Right", "3" } }, 0,
      "The corner the FPS counter sits in." },
    { "Input", "Button Layout", "LAA_SWAP_FACE", { { "Nintendo Style", NULL }, { "Xbox Style", "1" } }, 0,
      "Xbox Style swaps A with B and X with Y, for controllers labelled that way." },
    { "Input", "Touch Controls", "LAA_TOUCH", { { "Auto", NULL }, { "Always", "1" }, { "Off", "0" } }, 0,
      "Auto shows the buttons when you touch the screen and hides them when a controller is used." },
    { "Input", "Touch Movement", "LAA_TOUCH_STICK", { { "D-Pad", NULL }, { "Joystick", "1" } }, 0,
      "Shows either a d-pad or a joystick on the touchscreen." },
    { "Input", "Touch Button Layout", "LAA_TOUCH_SWAP", { { "Nintendo Style", NULL }, { "Xbox Style", "1" } }, 0,
      "Changes the touchscreen layout of ABXY. Does not change anything if using a controller." },
    { "Input", "Touch Stick Position", "LAA_TOUCH_FIXED", { { "Follows Your Thumb", NULL }, { "Stays in Place", "1" } },
      0, "Choose if the joystick follows where your thumb is placed." },
    { "Input", "Touch Layout", "LAA_TOUCH_LAYOUT", { { "Default", NULL }, { "Custom", NULL } }, 0,
      "Move and resize the touch controls from the pause menu." },
    { "Input", "Stick Dead Zone", "LAA_DEADZONE",
      { { "Small", "2000" }, { "Normal", NULL }, { "Large", "8000" } }, 1,
      "How far a stick must move before it counts. Larger stops drift." },
    { "Audio", "Master Volume", "LAA_VOLUME",
      { { "Off", "0" }, { "25%", "0.75" }, { "50%", "1.5" }, { "75%", "2.25" }, { "100%", "3" }, { "125%", "3.75" },
        { "150%", "4.5" }, { "200%", "6" } }, 4,
      "The volume of the whole game." },
    { "Interface", "Game Language", "LAA_LANGUAGE",
      { { "English (US)", NULL }, { "English (UK)", "en-GB" }, { "French", "fr" }, { "French (Canada)", "fr-CA" },
        { "German", "de" }, { "Italian", "it" }, { "Spanish (Spain)", "es" }, { "Spanish (Latin America)", "es-419" },
        { "Dutch", "nl" }, { "Russian", "ru" }, { "Japanese", "ja" }, { "Korean", "ko" },
        { "Chinese (Simplified)", "zh-Hans" }, { "Chinese (Traditional)", "zh-Hant" } }, 0,
      "The language of the game's text." NEXT_START },
    { "Interface", "Check for Updates", "LAA_UPDATE_CHECK", { { "On", NULL }, { "Off", "0" } }, 0,
      "Check for updates on GitHub once the app starts." },
    { "Interface", "Skip Main Menu", "LAA_SKIP_MENU", { { "Off", NULL }, { "On", "1" } }, 0,
      "Start the game right away when opening ZLAA. Settings stay in the pause menu." },
    { "Interface", "Show Shader Compilation", "LAA_SHADER_OSD", { { "Off", NULL }, { "On", "1" } }, 0,
      "Show an overlay while shaders are being compiled for your device." },
};

std::map<std::string, std::string> config;              /* NAME -> value, as saved */
std::map<std::string, bool> fixed;                      /* set outside the menu (env.txt): not ours to change */
UiPaths paths;                                          /* (pointing into path_copies: the caller's may go away) */
std::string path_copies[6];

/* (Touch Layout: the controls' places as text, made by the editor in the game: Custom when there is any) */
bool is_layout(const Setting &s) { return strcmp(s.var, "LAA_TOUCH_LAYOUT") == 0; }

int choice_of(const Setting &s) {
    if (is_layout(s) && !fixed[s.var]) return config.count(s.var) && !config[s.var].empty() ? 1 : 0;
    if (fixed[s.var]) {
        const char *v = getenv(s.var);
        for (size_t i = 0; i < s.choices.size(); i++)
            if (s.choices[i].value && v && strcmp(s.choices[i].value, v) == 0) return (int)i;
        return v ? -1 : 0;
    }
    auto it = config.find(s.var);
    if (it == config.end()) return s.fallback;
    for (size_t i = 0; i < s.choices.size(); i++)
        if ((s.choices[i].value ? s.choices[i].value : "") == it->second) return (int)i;
    return s.fallback;
}

/* The config file: one flat JSON object of strings */
void load_config() {
    FILE *f = fopen(paths.config, "rb");
    if (!f) return;
    std::string text;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    std::vector<std::string> strings;                   /* every "..." in order: name, value, name, value */
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] != '"') continue;
        std::string s;
        for (i++; i < text.size() && text[i] != '"'; i++) {
            if (text[i] == '\\' && i + 1 < text.size()) i++;
            s += text[i];
        }
        strings.push_back(s);
    }
    for (size_t i = 0; i + 1 < strings.size(); i += 2) config[strings[i]] = strings[i + 1];
}

void save_config() {
    std::string tmp = std::string(paths.config) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) { perror(tmp.c_str()); return; }
    fprintf(f, "{\n");
    size_t k = 0;
    for (auto &kv : config) {
        std::string v;
        for (char c : kv.second) { if (c == '"' || c == '\\') v += '\\'; v += c; }
        fprintf(f, "  \"%s\": \"%s\"%s\n", kv.first.c_str(), v.c_str(), ++k < config.size() ? "," : "");
    }
    fprintf(f, "}\n");
    if (fclose(f) == 0) rename(tmp.c_str(), paths.config);
}

/* ---- RmlUi's view of the system: time, logging, files ---- */

class System : public Rml::SystemInterface {
public:
    double GetElapsedTime() override { return SDL_GetTicksNS() / 1e9; }
    bool LogMessage(Rml::Log::Type type, const Rml::String &message) override {
        if (type <= Rml::Log::LT_WARNING) fprintf(stderr, "ui: %s\n", message.c_str());
        return true;
    }
};

/* Files through SDL: on Android a relative path is read from the APK's assets */
class Files : public Rml::FileInterface {
public:
    Rml::FileHandle Open(const Rml::String &path) override {
        std::string full = path[0] == '/' ? path : std::string(paths.res) + "/" + path;
        SDL_IOStream *io = SDL_IOFromFile(full.c_str(), "rb");
        if (!io) fprintf(stderr, "ui: can't open %s: %s\n", full.c_str(), SDL_GetError());
        return (Rml::FileHandle)io;
    }
    void Close(Rml::FileHandle file) override { SDL_CloseIO((SDL_IOStream *)file); }
    size_t Read(void *buffer, size_t size, Rml::FileHandle file) override { return SDL_ReadIO((SDL_IOStream *)file, buffer, size); }
    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        SDL_IOWhence w = origin == SEEK_SET ? SDL_IO_SEEK_SET : origin == SEEK_CUR ? SDL_IO_SEEK_CUR : SDL_IO_SEEK_END;
        return SDL_SeekIO((SDL_IOStream *)file, offset, w) >= 0;
    }
    size_t Tell(Rml::FileHandle file) override { return (size_t)SDL_TellIO((SDL_IOStream *)file); }
    size_t Length(Rml::FileHandle file) override { return (size_t)SDL_GetIOSize((SDL_IOStream *)file); }
};

System sys;
Files files;
SDL_Window *window;
Rml::Context *context;
Rml::ElementDocument *start_doc, *settings_doc, *pause_doc, *build_doc, *update_doc, *news_doc;
int showing;                                             /* 0: no menu, 1: start screen, 2: pause menu */
int update_asked;                                        /* the update check: 1 asked, 2 answered */
std::string update_tag;                                  /* the newer release's tag ("v0.1.2") */
int result;                                              /* what ui_frame returns next */
int game_ready;                                          /* the start screen's Play works */
int needs_build;                                         /* the game files are there, their code isn't: Build Game */
int needs_files;                                         /* no game files found: Play is Choose Game Files */
int settings_changed;                                    /* (ui_settings_changed) */
std::string tab = "Video";
uint64_t held_since[UI_ACTIONS], repeat_at[UI_ACTIONS];  /* (ns) direction presses repeat while held */

/* LAA_UI_SCRIPT="frame:WHAT,...": presses at menu frames (UP DOWN LEFT RIGHT A B L R MENU), or SHOT (the frame
 * as a PNG in LAA_UI_SHOTS, default "."): menu tests without hands. Frames count from the first menu frame. */
struct ScriptStep { unsigned frame; std::string what; };
std::vector<ScriptStep> script;
unsigned menu_frames, shots;

void load_script() {
    const char *s = getenv("LAA_UI_SCRIPT");
    for (const char *p = s; p && *p;) {
        char *end;
        unsigned f = (unsigned)strtoul(p, &end, 10);
        if (*end != ':') break;
        const char *w = end + 1, *comma = strchr(w, ',');
        script.push_back({ f, std::string(w, comma ? (size_t)(comma - w) : strlen(w)) });
        p = comma ? comma + 1 : NULL;
    }
}

std::string esc(const std::string &s) {                  /* text -> RML */
    std::string o;
    for (char c : s) o += c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '&' ? "&amp;" : std::string(1, c);
    return o;
}

/* ---- the start screen ---- */

/* "v0.1.2" newer than "0.1.1"? (major.minor.patch, a leading v ignored) */
bool newer_version(const char *tag, const char *mine) {
    int a[3] = { 0, 0, 0 }, b[3] = { 0, 0, 0 };
    if (sscanf(tag + (*tag == 'v'), "%d.%d.%d", &a[0], &a[1], &a[2]) < 2) return false;
    sscanf(mine, "%d.%d.%d", &b[0], &b[1], &b[2]);
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

/* The update check (Settings > Interface > Check for Updates): asked once, when the start screen first opens; a
 * newer release shows the badge beside the version. LAA_UPDATE_TEST=v9.9.9 pretends GitHub answered that. */
void start_update_check() {
    if (update_asked) return;
    update_asked = 1;
    for (const Setting &s : kSettings)
        if (strcmp(s.var, "LAA_UPDATE_CHECK") == 0 && choice_of(s) != 0) { update_asked = 2; return; }
    host_update_check();                                 /* (also when faked: the APK's size comes with it) */
}

void poll_update_check() {
    char tag[64];
    if (update_asked != 1) return;
    if (!host_update_result(tag, sizeof tag)) return;
    if (getenv("LAA_UPDATE_TEST")) snprintf(tag, sizeof tag, "%s", getenv("LAA_UPDATE_TEST"));
    update_asked = 2;
    bool newer = newer_version(tag, LAA_VERSION);
    printf("update check: latest release %s, this is %s%s\n", tag[0] ? tag : "unknown", LAA_VERSION,
           newer ? ": update available" : "");
    if (!newer) return;
    update_tag = tag;
    start_doc->GetElementById("version-area")->SetClass("update", true);
}

void focus_first(Rml::ElementDocument *doc, const char *selector);

/* ---- What's New (res/ui/whatsnew.rml): once, the first time the start screen opens after an update ----
 * config "seen_version" = the last version the player saw it in. A new install (no config yet) only records the
 * version. LAA_WHATS_NEW=1 shows it anyway (tests; with LAA_CONFIG=a test file). */
void open_whats_new() {
    bool force = getenv("LAA_WHATS_NEW") && atoi(getenv("LAA_WHATS_NEW"));
    auto it = config.find("seen_version");
    bool updated = it != config.end() ? it->second != LAA_VERSION : !config.empty();
    if (!force && (it == config.end() || it->second != LAA_VERSION)) {
        config["seen_version"] = LAA_VERSION;
        save_config();
    }
    if (!force && !updated) return;
    news_doc->GetElementById("news-version")->SetInnerRML(esc(std::string("ZLAA ") + LAA_VERSION));
    news_doc->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::None);
    news_doc->GetElementById("window")->SetAttribute("open", "");
    focus_first(news_doc, "#news-ok");
}

void close_whats_new() {
    news_doc->GetElementById("window")->RemoveAttribute("open");
    news_doc->Hide();
    focus_first(start_doc, "#play");
}

/* ---- the update window (the badge, on Android): download, check, and Android's installer ---- */


int update_shown_state = -1;

void refresh_update() {
    double done, total;
    char message[512];
    int state = host_update_status(&done, &total, message, sizeof message);
    bool busy = state == UI_UPDATE_DOWNLOADING || state == UI_UPDATE_VERIFYING || state == UI_UPDATE_INSTALLING;
    char b[160];
    const char *v = update_tag.c_str() + (update_tag[0] == 'v');
    if (total > 0) snprintf(b, sizeof b, "ZLAA %s is out. Download it (%.0f MB) and update now?", v, total);
    else snprintf(b, sizeof b, "ZLAA %s is out. Download it and update now?", v);
    update_doc->GetElementById("update-intro")->SetInnerRML(esc(b));
    bool bar = state == UI_UPDATE_DOWNLOADING || state == UI_UPDATE_VERIFYING;
    update_doc->GetElementById("update-track")->SetProperty("display", bar ? "block" : "none");
    update_doc->GetElementById("update-line")->SetProperty("display", bar ? "flex" : "none");
    double f = total > 0 ? done / total : 0;
    update_doc->GetElementById("update-fill")->SetProperty("width", std::to_string((f > 1 ? 1 : f) * 100) + "%");
    snprintf(b, sizeof b, "%d%%", (int)(f * 100));
    update_doc->GetElementById("update-percent")->SetInnerRML(bar ? b : "");
    snprintf(b, sizeof b, total > 0 ? "%.0f of %.0f MB" : "%.0f MB", done, total);
    update_doc->GetElementById("update-size")->SetInnerRML(bar ? b : "");
    const char *detail = state == UI_UPDATE_PERMISSION
                             ? "Allow ZLAA to install apps on the page that opened, then come back and choose Update."
                       : state == UI_UPDATE_VERIFYING ? "Checking the download\xE2\x80\xA6"
                       : state == UI_UPDATE_FAILED ? message : "";
    update_doc->GetElementById("update-detail")->SetInnerRML(esc(detail));
    update_doc->GetElementById("update-detail")->SetProperty("display", *detail ? "block" : "none");
    if (state != update_shown_state) {                   /* the buttons for this state */
        update_shown_state = state;
        update_doc->GetElementById("update-go")->SetInnerRML(busy ? "Cancel" : state == UI_UPDATE_FAILED ? "Try Again" : "Update");
        update_doc->GetElementById("update-go")->SetProperty("display", state == UI_UPDATE_INSTALLING ? "none" : "block");
        update_doc->GetElementById("update-back")->SetInnerRML(busy ? "Hide" : "Not Now");
        if (state == UI_UPDATE_INSTALLING) update_doc->GetElementById("update-back")->Focus(true);
    }
}

void open_update() {
    if (!host_update_possible()) { SDL_OpenURL(LAA_PROJECT_URL "/releases/latest"); return; }
    update_shown_state = -1;
    update_doc->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::None);
    update_doc->GetElementById("window")->SetAttribute("open", "");
    refresh_update();
    focus_first(update_doc, "#update-go");
}

void close_update() {
    update_doc->GetElementById("window")->RemoveAttribute("open");
    update_doc->Hide();
    start_doc->Focus();
    focus_first(start_doc, "#update");
}

void update_go() {
    double d, t;
    char m[8];
    int state = host_update_status(&d, &t, m, sizeof m);
    if (state == UI_UPDATE_DOWNLOADING || state == UI_UPDATE_VERIFYING) host_update_cancel();
    else if (state != UI_UPDATE_INSTALLING) host_update_start();
    refresh_update();
}

void focus_first(Rml::ElementDocument *doc, const char *selector) {
    if (Rml::Element *e = doc->QuerySelector(selector)) e->Focus(true);
}

void show_files_help(const std::string &extra, bool bad);
std::string files_label();
std::string space_label();
void show_space_help();

/* The line above the status chip ("" hides it); with the settings window open, its help pane says it */
void note(const std::string &text, bool bad) {
    if (settings_doc && settings_doc->IsVisible()) { show_files_help(text, bad); return; }
    Rml::Element *e = start_doc->GetElementById("start-note");
    e->SetInnerRML(esc(text));
    e->SetClass("shown", !text.empty());
    e->SetClass("bad", bad);
}

void update_start_status() {
    char id[17] = "";
    std::string status, label, version;
    struct stat st;
    int lib = game_lib_find(paths.exefs, paths.game_dir, paths.game_import, id);
    game_ready = needs_build = needs_files = 0;
    if (lib < 0) {
        status = "bad", label = "No game files", needs_files = 1;
        version = "Choose the folder with your dump's exefs and romfs";
    } else if (stat(paths.romfs, &st) != 0) {
        status = "bad", label = "The game's romfs folder is missing", needs_files = 1;
        version = std::string("Expected at ") + paths.romfs;
    } else {
        const char *v = strncmp(id, "AE16F71E002AF8CB", 16) == 0 ? "1.0.0"
                      : strncmp(id, "909E904AF78AC1B8", 16) == 0 ? "1.0.1 / 1.1.0" : "unknown version";
        version = std::string("Link's Awakening ") + v;
        if (lib == 0) status = "good", label = "Ready", game_ready = 1;
        else status = "pending", label = lib == 2 ? "Game code is from an older version" : "Game code not built yet";
        needs_build = lib != 0;
    }
    Rml::Element *e = start_doc->GetElementById("game-status");
    e->SetAttribute("status", status);
    start_doc->GetElementById("game-status-label")->SetInnerRML(esc(label));
    start_doc->GetElementById("game-version")->SetInnerRML(esc(version));
    if (const char *crash = getenv("LAA_LAST_CRASH")) {   /* (main.c: the last session crashed) */
        const char *name = strrchr(crash, '/');
        note(std::string("The game closed unexpectedly last time. Its report is in crashes/") + (name ? name + 1 : crash) +
             ".", true);
        unsetenv("LAA_LAST_CRASH");                      /* (said once) */
    }
    std::vector<uint8_t> px;                         /* the game's logo as the title: with game files that have it */
    int lw, lh;
    bool logo = !needs_files && game_logo_rgba(paths.romfs, px, lw, lh);
    start_doc->QuerySelector("hero")->SetClass("logo", logo);
    Rml::Element *play = start_doc->GetElementById("play");
    play->SetInnerRML(needs_files ? "<icon class=\"folder\" />Choose Game Files"
                      : needs_build ? "<icon class=\"build\" />Build Game" : "<icon class=\"play\" />Play");
}

void build_settings();

/* ---- Game Files: the player picks the folder holding their dump (exefs/ and romfs/, or the exefs folder itself
 * with romfs/ beside it). Android asks for "All files access" first, so the files are read where they are ---- */
int picking;                                             /* 1: waiting for the permission, 2: for the picker */

bool is_dir(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
bool is_file(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }

void use_game_folder(const std::string &dir) {
    std::string exefs, romfs, parent = dir.substr(0, dir.find_last_of('/'));
    if (is_file(dir + "/exefs/main")) exefs = dir + "/exefs", romfs = dir + "/romfs";
    else if (is_file(dir + "/main")) exefs = dir, romfs = parent + "/romfs";
    if (exefs.empty()) {
        note("No game files there: the folder needs your dump's exefs folder (with a file named main) and its romfs "
             "folder.", true);
        return;
    }
    if (!is_dir(romfs)) { note("Found exefs, but no romfs folder beside it (" + romfs + ").", true); return; }
    config["LAA_EXEFS"] = exefs;
    config["LAA_ROMFS"] = romfs;
    save_config();
    path_copies[0] = exefs;
    path_copies[1] = romfs;
    paths.exefs = path_copies[0].c_str();
    paths.romfs = path_copies[1].c_str();
    Rml::ReleaseTextures();                          /* (images made from the game files: from the new ones) */
    note("", false);
    update_start_status();
    if (settings_doc->IsVisible()) {                     /* (chosen from Settings: back on its row) */
        build_settings();
        focus_first(settings_doc, "#files-row");
        show_files_help(game_ready ? "" : needs_build ? "Choose Build Game on the start screen next." : "", false);
    } else {
        focus_first(start_doc, "#play");
    }
}

void choose_game_files() {
    if (!host_files_access()) {
        picking = 1;
        note("Allow \u201cAll files access\u201d for ZLAA in the page that opens, then come back: it reads your game "
             "files where they are.", false);
        host_request_files_access();
        return;
    }
    picking = 2;
    note("Choose the folder with your dump's exefs and romfs folders.", false);
    host_pick_folder();
}

void poll_picking() {
    if (picking == 1 && host_files_access()) { picking = 2; host_pick_folder(); }
    char dir[4096];
    if (picking == 2 && host_picked_folder(dir, sizeof dir)) {
        picking = 0;
        if (dir[0]) use_game_folder(dir);
        else note("", false);
    }
}

/* ---- Build Game: the game's code built on the device (the host's builder), with its progress ---- */
int build_shown_step = -1;

/* How warm the device may get while it builds (config LAA_BUILD_SPEED: 0 cool, 1 balanced, 2 fast) */
const char *const kSpeedNames[] = { "Cool", "Balanced", "Fast" };
const char *const kSpeedHelp[] = { "Stays barely warm; the slowest.", "Gets warm; about twice as fast as Cool.",
                                   "Gets warm to the touch; the fastest." };

int build_speed() {
    auto it = config.find("LAA_BUILD_SPEED");
    int v = it == config.end() || it->second.empty() ? 1 : atoi(it->second.c_str());
    return v < 0 || v > 2 ? 1 : v;
}

void show_build_speed(bool running) {
    int v = build_speed();
    Rml::Element *b = build_doc->GetElementById("build-speed");
    b->SetInnerRML(std::string("Speed: ") + kSpeedNames[v]);
    if (running) b->SetAttribute("disabled", ""); else b->RemoveAttribute("disabled");
    build_doc->GetElementById("build-speed-help")->SetInnerRML(running ? "" : kSpeedHelp[v]);
}

std::string minutes(double s) {
    char b[32];
    snprintf(b, sizeof b, "%d:%02d", (int)s / 60, (int)s % 60);
    return b;
}

void update_build() {
    double fraction, elapsed;
    int cooling;
    char detail[256];
    int step = host_build_progress(&fraction, detail, sizeof detail, &elapsed, &cooling);
    bool running = step > UI_BUILD_IDLE && step < UI_BUILD_DONE;
    if (build_doc->IsVisible()) {
        char pct[16];
        snprintf(pct, sizeof pct, "%d%%", (int)(fraction * 100));
        build_doc->GetElementById("build-fill")->SetProperty("width", std::to_string(fraction * 100) + "%");
        build_doc->GetElementById("build-percent")->SetInnerRML(running || step == UI_BUILD_DONE ? pct : "");
        std::string t;
        if (running && elapsed > 0) {                    /* (the builder's fraction is of the time: done / total) */
            t = minutes(elapsed) + " \xC2\xB7 ";
            double left = fraction > 0 ? elapsed * (1 - fraction) / fraction : -1;
            int m = (int)(left / 60 + 0.5);
            char b[48];
            if (elapsed < 60 || left < 0) snprintf(b, sizeof b, "estimating\xE2\x80\xA6");
            else if (m < 2) snprintf(b, sizeof b, "about a minute left");
            else if (m < 60) snprintf(b, sizeof b, "about %d min left", m < 15 ? m : (m + 2) / 5 * 5);
            else snprintf(b, sizeof b, "about %d h %02d min left", (m + 2) / 5 * 5 / 60, (m + 2) / 5 * 5 % 60);
            t += b;
        }
        build_doc->GetElementById("build-time")->SetInnerRML(esc(t));
        build_doc->GetElementById("build-detail")->SetInnerRML(esc(detail));
        if (step != build_shown_step) {                  /* the buttons for this step */
            build_shown_step = step;
            Rml::Element *go = build_doc->GetElementById("build-go"), *back = build_doc->GetElementById("build-back");
            go->SetInnerRML(running ? "Cancel" : step == UI_BUILD_DONE ? "Play" : step == UI_BUILD_FAILED ? "Try Again" : "Start");
            back->SetInnerRML(running ? "Hide" : "Back");
            show_build_speed(running || step == UI_BUILD_DONE);
            if (step == UI_BUILD_DONE) update_start_status();
        }
    }
    if (running && !build_doc->IsVisible()) {             /* (hidden: the status chip shows how far it is) */
        char b[64];
        snprintf(b, sizeof b, "Building the game: %d%%", (int)(fraction * 100));
        start_doc->GetElementById("game-status-label")->SetInnerRML(b);
    }
    static int last_step = -1;                           /* (finished while hidden: the start screen once) */
    if (step != last_step && step == UI_BUILD_DONE) update_start_status();
    last_step = step;
}

void open_build() {
    build_shown_step = -1;
    build_doc->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::None);
    build_doc->GetElementById("window")->SetAttribute("open", "");
    update_build();
    focus_first(build_doc, "#build-go");
}

void close_build() {
    build_doc->GetElementById("window")->RemoveAttribute("open");
    build_doc->Hide();
    start_doc->Focus();
    update_start_status();
    focus_first(start_doc, "#play");
}

void build_go() {
    double f, e;
    int c;
    char d[256];
    int step = host_build_progress(&f, d, sizeof d, &e, &c);
    if (step > UI_BUILD_IDLE && step < UI_BUILD_DONE) host_build_cancel();
    else if (step == UI_BUILD_DONE && game_ready) { close_build(); result = UI_PLAY; }
    else if (host_build_start(paths.exefs, build_speed()) != 0) build_doc->GetElementById("build-detail")->SetInnerRML("Can't build here.");
    update_build();
}

/* ---- the settings window ---- */

/* Button Remapping (a page of the Input tab): which controller button presses each of the game's buttons. The list
 * (one source per target, host_pad_*) is kept as LAA_REMAP, "" = the defaults. A row waits for the next button
 * pressed (main.c hands it over: ui_button_pressed); a button another target had moves there (they swap). */
bool remap_page;
int remap_waiting = -1;                                  /* the target waiting for a button, or -1 */
Uint64 remap_since;
#define REMAP_WAIT_MS 5000

std::vector<int> remap_list() {
    std::vector<int> l;
    int n = host_pad_targets();
    const char *r = config.count("LAA_REMAP") ? config["LAA_REMAP"].c_str() : "";
    if (fixed["LAA_REMAP"]) r = getenv("LAA_REMAP") ? getenv("LAA_REMAP") : "";
    for (int t = 0; t < n; t++) {
        int src = host_pad_default(t);
        if (*r) {
            src = atoi(r);
            r = strchr(r, ',');
            r = r ? r + 1 : "";
        }
        l.push_back(src);
    }
    return l;
}

bool remap_custom() {
    std::vector<int> l = remap_list();
    for (int t = 0; t < (int)l.size(); t++) if (l[(size_t)t] != host_pad_default(t)) return true;
    return false;
}

void remap_save(const std::vector<int> &l) {
    std::string text;
    bool custom = false;
    for (int t = 0; t < (int)l.size(); t++) {
        text += (t ? "," : "") + std::to_string(l[(size_t)t]);
        custom |= l[(size_t)t] != host_pad_default(t);
    }
    if (!custom) text.clear();
    config["LAA_REMAP"] = text;
    save_config();
    if (custom) setenv("LAA_REMAP", text.c_str(), 1);
    else unsetenv("LAA_REMAP");
    settings_changed = 1;
    printf("menu: LAA_REMAP=%s\n", custom ? text.c_str() : "(default)");
}

std::string source_name(int src) {
    char b[64];
    host_pad_source_name(src, b, sizeof b);
    return b;
}

void show_remap_help() {
    settings_doc->GetElementById("help")->SetInnerRML(
        "<heading>Button Remapping</heading><p>Choose a button of the game, then press the controller button you want "
        "for it. Waiting " + std::to_string(REMAP_WAIT_MS / 1000) + " seconds cancels. The menus keep the usual "
        "buttons.</p>");
}


void build_settings() {
    std::string tabs, rows;
    for (const char *t : kTabs)
        tabs += std::string("<tab id=\"tab-") + t + "\"" + (tab == t ? " selected" : "") + ">" + t + "</tab>";
    if (remap_page) {                                    /* (the Button Remapping page, in place of the Input rows) */
        std::vector<int> l = remap_list();
        for (int t = 0; t < (int)l.size(); t++)
            rows += "<row id=\"remap-" + std::to_string(t) + "\"" + (fixed["LAA_REMAP"] ? " class=\"fixed\"" : "") + ">"
                    "<key>" + esc(host_pad_target_name(t)) + "</key><value><span>" +
                    esc(t == remap_waiting ? "Press a button..." : source_name(l[(size_t)t])) + "</span></value></row>";
        rows += "<row id=\"remap-reset\"><key>Reset to Default</key><value></value></row>";
        settings_doc->GetElementById("tab-bar")->SetInnerRML(tabs);
        settings_doc->GetElementById("rows")->SetInnerRML(rows);
        show_remap_help();
        settings_doc->GetElementById("window")->SetClass("about", false);
        settings_doc->GetElementById("hint-a")->SetInnerRML("Change");
        return;
    }
    for (size_t i = 0; i < kSettings.size(); i++) {
        const Setting &s = kSettings[i];
        if (tab != s.tab) continue;
        int c = choice_of(s);
        std::string value = c >= 0 ? s.choices[(size_t)c].label : getenv(s.var) ? getenv(s.var) : "";
        rows += "<row id=\"row-" + std::to_string(i) + "\"" + (fixed[s.var] ? " class=\"fixed\"" : "") + ">"
                "<key>" + esc(s.name) + "</key><value>" +
                (fixed[s.var] ? "<icon class=\"lock\"/><span>" + esc(value) + "</span>"
                               : "<icon class=\"left\"/><span>" + esc(value) + "</span><icon class=\"right\"/>") +
                "</value></row>";
        if (strcmp(s.var, "LAA_SWAP_FACE") == 0)         /* (Button Remapping: after Button Layout) */
            rows += std::string("<row id=\"remap-row\"><key>Button Remapping</key><value><span>") +
                    (remap_custom() ? "Custom" : "Default") + "</span><icon class=\"right\"/></value></row>";
    }
    if (tab == "Interface" && showing == 1)              /* (not over the running game) */
        rows = "<row id=\"files-row\"><key>Game Files</key><value><span>" + esc(files_label()) +
               "</span><icon class=\"folder\"/></value></row>" + rows;
#ifdef __ANDROID__
    if (tab == "Interface" && showing == 1)
        rows += "<row id=\"space-row\"><key>Free Up Space</key><value><span>" + esc(space_label()) + "</span></value></row>";
#endif
    if (tab == "About")
        rows = "<about>"
               "<about-head><about-title>ZLAA</about-title><about-version>Version " LAA_VERSION
               " \xC2\xB7 build " LAA_COMMIT "</about-version></about-head>"
               "<p>A static recompilation of The Legend of Zelda: Link's Awakening (Nintendo Switch, 2019) for "
               "Android. The game's code is translated into native code for this device, and the Switch's system and "
               "graphics are rebuilt around it. It is not an emulator. Nothing of the game comes with this app: it "
               "runs only from the files of your own copy.</p>"
               "<row id=\"link-row\"><key>Updates and Bug Reports</key><value><span>" LAA_PROJECT_URL_SHORT
               "</span><icon class=\"right\"/></value></row>"
               "<p class=\"hint legal\">Open source under the MIT License. Made almost entirely with AI (Claude), "
               "directed and tested by a human. A fan project, not affiliated with or endorsed by Nintendo.</p>"
               "</about>";
    settings_doc->GetElementById("tab-bar")->SetInnerRML(tabs);
    settings_doc->GetElementById("rows")->SetInnerRML(rows);
    settings_doc->GetElementById("help")->SetInnerRML(tab == "About" ? "" : "<p class=\"hint\">Choose a setting to see what it does.</p>");
    settings_doc->GetElementById("window")->SetClass("about", tab == "About");   /* (the About page has no help pane) */
    settings_doc->GetElementById("hint-a")->SetInnerRML(tab == "About" ? "Open" : "Change");
}

void open_settings() {
    build_settings();
    settings_doc->SetClass("over-game", showing == 2);   /* (the paused game is dimmed already) */
    settings_doc->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::None);
    settings_doc->GetElementById("window")->SetAttribute("open", "");
    if (!settings_doc->QuerySelector("row")) settings_doc->Focus();
    else focus_first(settings_doc, "row");
}

void close_settings() {
    remap_page = false;
    remap_waiting = -1;
    settings_doc->GetElementById("window")->RemoveAttribute("open");
    settings_doc->Hide();
    if (showing == 1) { start_doc->Focus(); focus_first(start_doc, "#settings"); }
    else { pause_doc->Focus(); focus_first(pause_doc, "#settings"); }
}

void remap_leave() {                                     /* back to the Input rows, on Button Remapping */
    remap_page = false;
    remap_waiting = -1;
    build_settings();
    focus_first(settings_doc, "#remap-row");
}

void remap_focus(const std::string &id) {             /* (after the rows were made again: laid out first, so the */
    settings_doc->UpdateDocument();                      /*  row can be scrolled into sight) */
    if (Rml::Element *e = settings_doc->GetElementById(id)) e->Focus(true);
}

void switch_tab(int step) {
    remap_page = false;
    remap_waiting = -1;
    size_t n = sizeof kTabs / sizeof *kTabs, k = 0;
    while (k < n && tab != kTabs[k]) k++;
    tab = kTabs[(k + n + (size_t)step) % n];
    build_settings();
    if (Rml::Element *e = settings_doc->QuerySelector("row")) e->Focus(true);
}

int row_of(Rml::Element *e) {
    for (; e; e = e->GetParentNode())
        if (e->GetId().rfind("row-", 0) == 0) return atoi(e->GetId().c_str() + 4);
    return -1;
}

int focused_row() { return row_of(context->GetFocusElement()); }

void change_setting(int row, int step) {
    const Setting &s = kSettings[(size_t)row];
    if (fixed[s.var]) return;
    int n = (int)s.choices.size(), c = (choice_of(s) + step + n) % n;
    const char *v = s.choices[(size_t)c].value;
    if (is_layout(s) && c == 1) return;              /* (Custom comes from the editor; from here only Default) */
    config[s.var] = v ? v : "";
    save_config();
    settings_changed = 1;
    build_settings();
    if (Rml::Element *e = settings_doc->GetElementById("row-" + std::to_string(row))) e->Focus(true);
}

/* ---- Free Up Space (Android): what the app keeps in its private storage and no longer needs - a build's work
 * folder, the unpacked build tools (unpacked again when needed), game code built against an older app's headers,
 * and game code folders not in use (files/game-*: test builds). Saves and working game code stay. ---- */
long long tree_bytes(const std::string &p) {
    struct stat st;
    if (lstat(p.c_str(), &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return st.st_size;
    long long n = 0;
    if (DIR *d = opendir(p.c_str())) {
        while (struct dirent *e = readdir(d))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n += tree_bytes(p + "/" + e->d_name);
        closedir(d);
    }
    return n;
}

void remove_tree(const std::string &p) {
    struct stat st;
    if (lstat(p.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        if (DIR *d = opendir(p.c_str())) {
            while (struct dirent *e = readdir(d))
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) remove_tree(p + "/" + e->d_name);
            closedir(d);
        }
        rmdir(p.c_str());
    } else {
        unlink(p.c_str());
    }
}

std::vector<std::string> unneeded_files() {
    std::vector<std::string> out;
#ifdef __ANDROID__
    const char *internal = SDL_GetAndroidInternalStoragePath();
    if (!internal) return out;
    std::string files = internal, game = paths.game_dir ? paths.game_dir : "";
    double f, e;
    int c;
    char d[8];
    int step = host_build_progress(&f, d, sizeof d, &e, &c);
    if (!(step > UI_BUILD_IDLE && step < UI_BUILD_DONE)) {   /* (never under a running build) */
        out.push_back(files + "/build");
        out.push_back(files + "/toolchain");
    }
    if (DIR *dir = opendir(game.c_str())) {
        while (struct dirent *ent = readdir(dir)) {
            std::string p = game + "/" + ent->d_name;
            if (strncmp(ent->d_name, "libgame-", 8) == 0 && game_lib_outdated(p.c_str())) out.push_back(p);
        }
        closedir(dir);
    }
    if (DIR *dir = opendir(files.c_str())) {
        while (struct dirent *ent = readdir(dir)) {
            std::string p = files + "/" + ent->d_name;
            if (strncmp(ent->d_name, "game-", 5) == 0 && p != game) out.push_back(p);
        }
        closedir(dir);
    }
#endif
    return out;
}

[[maybe_unused]] std::string space_label() {        /* (the row is Android's only) */
    long long n = 0;
    for (const std::string &p : unneeded_files()) n += tree_bytes(p);
    char b[32];
    if (n < 1000000) return "Nothing to clear";
    snprintf(b, sizeof b, "%.0f MB", n / 1e6);
    return b;
}

void free_up_space() {
    long long n = 0;
    for (const std::string &p : unneeded_files()) { n += tree_bytes(p); remove_tree(p); }
    printf("menu: freed %.0f MB\n", n / 1e6);
    build_settings();
    focus_first(settings_doc, "#space-row");
}

void show_space_help() {
    settings_doc->GetElementById("help")->SetInnerRML(
        "<heading>Free Up Space</heading><p>Removes what ZLAA no longer needs: what is left of a finished or "
        "stopped build, the unpacked build tools (unpacked again when needed), and game code from older versions of "
        "the app. Your saves and the game code in use are kept.</p>");
}

/* The game files' folder, short: "LADump/1.0.1" in the device's storage, "App folder" for the app's own */
std::string files_label() {
    if (needs_files) return "Not chosen";
    std::string d = paths.exefs ? paths.exefs : "";
    if (d.size() > 6 && d.compare(d.size() - 6, 6, "/exefs") == 0) d.erase(d.size() - 6);
    if (d.find("/Android/data/") != std::string::npos) return "App folder";
    const std::string storage = "/storage/emulated/0/";
    if (d.rfind(storage, 0) == 0) d = d.substr(storage.size());
    size_t a = d.find_last_of('/'), b = a == std::string::npos || a == 0 ? std::string::npos : d.find_last_of('/', a - 1);
    return d.size() > 28 && b != std::string::npos ? "\xE2\x80\xA6" + d.substr(b) : d;   /* (long: its last two folders) */
}

void show_files_help(const std::string &extra, bool bad) {
    std::string h = "<heading>Game Files</heading><p>The folder with your dump's exefs and romfs folders. Choose another "
                    "to play another version; its code may need preparing first.</p>";
    if (paths.exefs && !needs_files) h += "<p class=\"hint path\">" + esc(paths.exefs) + "</p>";
    if (!extra.empty()) h += std::string("<p") + (bad ? " class=\"bad\"" : "") + ">" + esc(extra) + "</p>";
    settings_doc->GetElementById("help")->SetInnerRML(h);
}

void show_help(int row) {
    if (row < 0) return;
    const Setting &s = kSettings[(size_t)row];
    std::string h = "<heading>" + esc(s.name) + "</heading><p>" + esc(s.help) + "</p>";
    if (fixed[s.var]) h += "<p class=\"hint\">Set in env.txt (" + std::string(s.var) + "): remove the line there to choose it here.</p>";
    settings_doc->GetElementById("help")->SetInnerRML(h);
}

/* ---- clicks ---- */

/* The last control that had focus (by id: the settings rows are made again on every change). A touch on the
 * background focuses whatever is there (any element can take focus in RmlUi), where the d-pad and A do nothing:
 * the next button press puts the focus back here (restore_focus), so touch and the controller can take turns. */
Rml::ElementDocument *last_focus_doc;
std::string last_focus_id;

bool is_control(Rml::Element *e) {                      /* buttons and rows (theme.rcss: tab-index: auto) */
    return e && e != e->GetOwnerDocument() && e->GetComputedValues().tab_index() == Rml::Style::TabIndex::Auto;
}

Rml::ElementDocument *top_doc() {                        /* the document the controller works in now */
    if (build_doc && build_doc->IsVisible()) return build_doc;
    if (update_doc && update_doc->IsVisible()) return update_doc;
    if (news_doc && news_doc->IsVisible()) return news_doc;
    if (settings_doc && settings_doc->IsVisible()) return settings_doc;
    return showing == 2 ? pause_doc : start_doc;
}

/* After a touch left no control focused: focus the last one (or the screen's first) and say so: that press only
 * shows where the focus is, as on a console */
bool restore_focus() {
    Rml::Element *f = context->GetFocusElement();
    Rml::ElementDocument *doc = top_doc();
    if (is_control(f) && f->GetOwnerDocument() == doc) return false;
    Rml::Element *to = last_focus_doc == doc && !last_focus_id.empty() ? doc->GetElementById(last_focus_id) : NULL;
    if (!to || !to->IsVisible(true))
        to = doc->QuerySelector(doc == build_doc ? "#build-go" : doc == update_doc ? "#update-go" : doc == news_doc ? "#news-ok" : doc == settings_doc ? "row" : doc == pause_doc ? "#resume" : "#play");
    return to && to->Focus(true);
}

class Listener : public Rml::EventListener {
public:
    void ProcessEvent(Rml::Event &event) override {
        Rml::Element *e = event.GetTargetElement();
        if (event.GetId() == Rml::EventId::Focus) {
            if (is_control(e) && !e->GetId().empty()) {   /* (where the controller carries on) */
                last_focus_doc = e->GetOwnerDocument();
                last_focus_id = e->GetId();
            }
            if (e && e->GetTagName() == "row")              /* (a long page: the focused row scrolled into sight) */
                e->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
            if (e && e->GetId().rfind("remap-", 0) == 0) { show_remap_help(); return; }
            if (e && e->GetId() == "files-row") show_files_help("", false);
            else if (e && e->GetId() == "space-row") show_space_help();
            else show_help(row_of(e));
            return;
        }
        for (; e; e = e->GetParentNode()) {
            const std::string &id = e->GetId();
            if (id.empty()) continue;
            if (id == "play" && game_ready) { result = UI_PLAY; return; }
            if (id == "play" && needs_build) { open_build(); return; }
            if (id == "play" && needs_files) { choose_game_files(); return; }
            if (id == "remap-row") { remap_page = true; build_settings(); remap_focus("remap-0"); return; }
            if (id == "remap-reset") {
                if (fixed["LAA_REMAP"]) return;
                remap_waiting = -1;
                std::vector<int> l;
                for (int t = 0; t < host_pad_targets(); t++) l.push_back(host_pad_default(t));
                remap_save(l);
                build_settings();
                remap_focus("remap-reset");
                return;
            }
            if (id.rfind("remap-", 0) == 0 && !fixed["LAA_REMAP"]) {   /* (a target: wait for its button) */
                remap_waiting = atoi(id.c_str() + 6);
                remap_since = SDL_GetTicks();
                build_settings();
                remap_focus(id);
                return;
            }
            if (id == "files-row") { choose_game_files(); return; }
            if (id == "space-row") { free_up_space(); return; }
            if (id == "link-row") { SDL_OpenURL(LAA_PROJECT_URL); return; }
            if (id == "update") { open_update(); return; }
            if (id == "update-go") { update_go(); return; }
            if (id == "update-notes") { SDL_OpenURL(LAA_PROJECT_URL "/releases/latest"); return; }
            if (id == "update-back") { close_update(); return; }
            if (id == "news-ok") { close_whats_new(); return; }
            if (id == "news-notes") { SDL_OpenURL(LAA_PROJECT_URL "/releases/tag/v" LAA_VERSION); return; }
            if (id == "build-go") { build_go(); return; }
            if (id == "build-speed") {                    /* Cool -> Balanced -> Fast -> Cool */
                config["LAA_BUILD_SPEED"] = std::to_string((build_speed() + 1) % 3);
                save_config();
                show_build_speed(false);
                return;
            }
            if (id == "build-back") { close_build(); return; }
            if (id == "resume") { result = UI_RESUME; return; }
            if (id == "touch-edit") { result = UI_EDIT_TOUCH; return; }
            if (id == "settings") { open_settings(); return; }
            if (id == "quit") { result = showing == 2 ? UI_RESTART : UI_QUIT; return; }
            if (id == "close") { close_settings(); return; }
            if (id.rfind("tab-", 0) == 0 && id != "tab-bar") { tab = id.substr(4); build_settings(); return; }
            if (id.rfind("row-", 0) == 0) { change_setting(atoi(id.c_str() + 4), 1); return; }
        }
    }
} listener;

Rml::ElementDocument *load(const char *name) {
    Rml::ElementDocument *d = context->LoadDocument(std::string("ui/") + name);
    if (!d) { fprintf(stderr, "ui: can't load %s\n", name); return NULL; }
    d->AddEventListener(Rml::EventId::Click, &listener);
    d->AddEventListener(Rml::EventId::Focus, &listener, true);
    return d;
}

void key(Rml::Input::KeyIdentifier k) {
    context->ProcessKeyDown(k, 0);
    context->ProcessKeyUp(k, 0);
}

/* One press of an action (or a repeat of a held direction) */
void act(int action) {
    bool in_settings = settings_doc && settings_doc->IsVisible();
    context->ProcessMouseLeave();                        /* (buttons: only the focus is highlighted, not where a touch was) */
    if (action <= UI_ACCEPT && restore_focus()) return;  /* (after a touch: this press finds the focus again) */
    switch (action) {
    case UI_UP: key(Rml::Input::KI_UP); break;
    case UI_DOWN: key(Rml::Input::KI_DOWN); break;
    case UI_LEFT: case UI_RIGHT:
        if (in_settings && focused_row() >= 0) change_setting(focused_row(), action == UI_LEFT ? -1 : 1);
        else key(action == UI_LEFT ? Rml::Input::KI_LEFT : Rml::Input::KI_RIGHT);
        break;
    case UI_ACCEPT: key(Rml::Input::KI_RETURN); break;
    case UI_BACK:
        if (build_doc && build_doc->IsVisible()) close_build();
        else if (update_doc && update_doc->IsVisible()) close_update();
        else if (news_doc && news_doc->IsVisible()) close_whats_new();
        else if (in_settings && remap_page) remap_leave();
        else if (in_settings) close_settings();
        else if (showing == 2) result = UI_RESUME;
        break;
    case UI_TAB_PREV: case UI_TAB_NEXT: if (in_settings) switch_tab(action == UI_TAB_PREV ? -1 : 1); break;
    case UI_MENU: if (showing == 2 && !in_settings) result = UI_RESUME; break;
    }
}

/* ---- the C side ---- */

extern "C" int ui_init(void *sdl_window, const UiPaths *p) {
    window = (SDL_Window *)sdl_window;
    const char *const in[6] = { p->exefs, p->romfs, p->game_dir, p->game_import, p->config, p->res };
    const char **out[6] = { &paths.exefs, &paths.romfs, &paths.game_dir, &paths.game_import, &paths.config, &paths.res };
    for (int k = 0; k < 6; k++) {
        path_copies[k] = in[k] ? in[k] : "";
        *out[k] = in[k] ? path_copies[k].c_str() : NULL;
    }
    for (const Setting &s : kSettings) fixed[s.var] = getenv(s.var) != NULL;
    fixed["LAA_REMAP"] = getenv("LAA_REMAP") != NULL;
    load_config();
    for (int k = 0; k < 2; k++) {                        /* the game files the player chose (env.txt lines win) */
        const char *var = k ? "LAA_ROMFS" : "LAA_EXEFS";
        auto it = config.find(var);
        if (!getenv(var) && it != config.end() && !it->second.empty()) {
            path_copies[k] = it->second;
            *out[k] = path_copies[k].c_str();
        }
    }
    load_script();
    if (ui_render_init() != 0) return -1;
    Rml::SetRenderInterface(ui_render_interface());
    Rml::SetSystemInterface(&sys);
    Rml::SetFileInterface(&files);
    if (!Rml::Initialise()) return -1;
    static const char *const fonts[] = { "Nunito-Regular.ttf", "Nunito-Bold.ttf", "Nunito-ExtraBold.ttf",
                                         "MaterialSymbolsRounded.ttf" };
    for (const char *f : fonts)
        if (!Rml::LoadFontFace(std::string("ui/fonts/") + f)) fprintf(stderr, "ui: can't load the font %s\n", f);
    int w = 1280, h = 720;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    context = Rml::CreateContext("main", Rml::Vector2i(w, h));
    if (!context) return -1;
    context->SetDensityIndependentPixelRatio(h / 720.0f);   /* (the layout is 720 dp tall on any screen) */
    start_doc = load("start.rml");
    settings_doc = load("settings.rml");
    pause_doc = load("pause.rml");
    build_doc = load("build.rml");
    update_doc = load("update.rml");
    news_doc = load("whatsnew.rml");
    if (!start_doc || !settings_doc || !pause_doc || !build_doc || !update_doc || !news_doc) return -1;
    start_doc->GetElementById("version")->SetInnerRML(LAA_VERSION);
    return 0;
}

extern "C" void ui_start_menu(void) {
    showing = 1;
    result = UI_NONE;
    update_start_status();
    start_update_check();
    start_doc->Show();
    start_doc->SetClass("animate-in", true);
    focus_first(start_doc, "#play");
    static bool news_checked;
    if (!news_checked) { news_checked = true; open_whats_new(); }
}

extern "C" int ui_skip_start(void) {
    ui_apply_settings();
    const char *v = getenv("LAA_SKIP_MENU");
    if (!v || !atoi(v)) return 0;
    update_start_status();
    return game_ready;
}

extern "C" void ui_pause(void) {
    showing = 2;
    result = UI_NONE;
    pause_doc->Show();
    pause_doc->GetElementById("window")->SetAttribute("open", "");
    focus_first(pause_doc, "#resume");
}

extern "C" int ui_open(void) { return showing != 0; }
extern "C" const char *ui_game_exefs(void) { return paths.exefs; }
extern "C" const char *ui_game_romfs(void) { return paths.romfs; }

extern "C" void ui_input(int action, int down) {
    if (!showing || action < 0 || action >= UI_ACTIONS) return;
    uint64_t now = SDL_GetTicksNS();
    if (!down) { held_since[action] = 0; return; }
    if (action <= UI_RIGHT) { held_since[action] = now; repeat_at[action] = now + 400000000ull; }
    act(action);
}

extern "C" void ui_pointer(const void *event) {
    const SDL_Event *e = (const SDL_Event *)event;
    if (!showing) return;
    float density = SDL_GetWindowPixelDensity(window);
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        context->ProcessMouseMove((int)(e->motion.x * density), (int)(e->motion.y * density), 0);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        context->ProcessMouseMove((int)(e->button.x * density), (int)(e->button.y * density), 0);
        if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) context->ProcessMouseButtonDown(e->button.button - 1, 0);
        else context->ProcessMouseButtonUp(e->button.button - 1, 0);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        context->ProcessMouseWheel(Rml::Vector2f(-e->wheel.x, -e->wheel.y), 0);
        break;
    }
}

const char *run_script() {                               /* this frame's steps; the PNG path if one is a shot */
    static char shot[4096];
    const char *path = NULL;
    for (const ScriptStep &st : script) {
        if (st.frame != menu_frames) continue;
        static const char *const names[UI_ACTIONS] = { "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "L", "R", "MENU" };
        for (int a = 0; a < UI_ACTIONS; a++)
            if (st.what == names[a]) { act(a); printf("ui: script: %s at %.2f s\n", names[a], SDL_GetTicksNS() / 1e9); }
        if (st.what.rfind("PAD", 0) == 0) {              /* PADn: controller button n (SDL's numbers) pressed */
            printf("ui: script: %s\n", st.what.c_str());
            ui_button_pressed(atoi(st.what.c_str() + 3));
        }
        if (st.what == "SHOT") {
            snprintf(shot, sizeof shot, "%s/ui_%03u.png", getenv("LAA_UI_SHOTS") ? getenv("LAA_UI_SHOTS") : ".", shots++);
            path = shot;
        }
    }
    menu_frames++;
    return path;
}

}

extern "C" int ui_frame(void) {
    if (!showing) return UI_NONE;
    static uint64_t last_frame;                          /* at most 60 frames a second (a menu needs no more; */
    uint64_t now = SDL_GetTicksNS();                     /*  some displays don't make us wait); 30 while the game */
    double bf, be;                                       /*  code builds (less heat beside it) */
    int bc;
    char bd[8];
    int bstep = host_build_progress(&bf, bd, sizeof bd, &be, &bc);
    uint64_t frame_ns = bstep > UI_BUILD_IDLE && bstep < UI_BUILD_DONE ? 33300000ull : 16600000ull;
    if (now - last_frame < frame_ns) SDL_DelayNS(frame_ns - (now - last_frame));
    last_frame = now = SDL_GetTicksNS();
    if (remap_waiting >= 0 && SDL_GetTicks() - remap_since > REMAP_WAIT_MS) {   /* (no button: the wait ends) */
        int t = remap_waiting;
        remap_waiting = -1;
        build_settings();
        remap_focus("remap-" + std::to_string(t));
    }
    const char *shot = run_script();
    if (showing == 1) { poll_picking(); update_build(); poll_update_check(); }
    if (update_doc->IsVisible()) refresh_update();
    for (int a = UI_UP; a <= UI_RIGHT; a++)              /* held directions repeat */
        if (held_since[a] && now >= repeat_at[a]) { repeat_at[a] = now + 90000000ull; act(a); }
    RenderUiTarget t;
    if (render_ui_begin(&t) != 0) return UI_NONE;
    if (context->GetDimensions() != Rml::Vector2i((int)t.width, (int)t.height)) {
        context->SetDimensions(Rml::Vector2i((int)t.width, (int)t.height));
        context->SetDensityIndependentPixelRatio(t.height / 720.0f);
    }
    context->Update();
    ui_render_begin(&t, showing != 2 || !t.last_view);
    if (showing == 2 && t.last_view) ui_render_image(t.last_view, 0.55f);   /* the paused game, darkened */
    context->Render();
    ui_render_end(shot);
    render_ui_end(&t);
    int r = result;
    if (r != UI_NONE) {                                  /* the menu closes */
        result = UI_NONE;
        if (settings_doc->IsVisible()) close_settings();
        build_doc->Hide();
        update_doc->Hide();
        start_doc->Hide();
        pause_doc->Hide();
        if (r == UI_PLAY) ui_apply_settings();
        showing = 0;
    }
    return r;
}

extern "C" int ui_settings_changed(void) {
    int c = settings_changed;
    settings_changed = 0;
    return c;
}

/* A value the game side made (the touch layout editor's), kept with the settings */
extern "C" void ui_set_value(const char *name, const char *value) {
    if (fixed[name]) return;
    config[name] = value;
    save_config();
    if (*value) setenv(name, value, 1);
    else unsetenv(name);
}

extern "C" int ui_waiting_for_button(void) {
    return remap_waiting >= 0;
}

extern "C" void ui_button_pressed(int source) {
    if (remap_waiting < 0) return;
    std::vector<int> l = remap_list();
    int t = remap_waiting;
    for (size_t u = 0; u < l.size(); u++)            /* (the button's old target gets this one's: they swap) */
        if (l[u] == source && (int)u != t) l[u] = l[(size_t)t];
    l[(size_t)t] = source;
    remap_waiting = -1;
    remap_save(l);
    build_settings();
    remap_focus("remap-" + std::to_string(t));
}

extern "C" void ui_apply_settings(void) {
    if (!fixed["LAA_REMAP"]) {                        /* (Button Remapping: kept in the config like the settings) */
        std::string r = config.count("LAA_REMAP") ? config["LAA_REMAP"] : "";
        if (!r.empty()) setenv("LAA_REMAP", r.c_str(), 1);
        else unsetenv("LAA_REMAP");
    }
    for (const Setting &s : kSettings) {
        if (fixed[s.var]) continue;
        int c = choice_of(s);
        const char *v = c >= 0 ? s.choices[(size_t)c].value : NULL;
        if (is_layout(s)) v = c == 1 ? config[s.var].c_str() : NULL;
        if (v) setenv(s.var, v, 1);
        else unsetenv(s.var);
        printf("menu: %s=%s\n", s.var, v ? v : "(unset)");
    }
}
