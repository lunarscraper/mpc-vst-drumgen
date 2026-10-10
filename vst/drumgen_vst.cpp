/* =============================================================================
 * drumgen_vst.cpp - DrumGen: an 8-lane probability drum generator after the Spektro Audio
 * NGEN's DRUMGEN, as a VST2 MIDI generator for the MPC OS plugin host (MPC / Force),
 * hand-written VST2 shell after mpc-vst-euclidier (Euclidier FX).
 *
 * SOURCES (what a lane draws its hits from)
 *   NGEN    DRUMGEN templates (.hex, 192 bytes: 4 parts x 3 velocity layers 127/80/30 x 32 steps,
 *           one 4-bit weight 0..10 per step, format of Spektro Audio's NGEN_DrumGenTemplate.py).
 *           NOT shipped (no license on NGEN-Resources): read at load time from <plugin dir>/drumgen/
 *           (= /sdcard/vst/drumgen/), up to 16 files, sorted by name. Own templates work the same way.
 *   GROOVE  the 97 grooves of mission-minnow/groovebank (MIT, grooves.h), 14 genres. A groove is one
 *           rhythm with up to two authored variants; the main line becomes weight 8 (+1 per variant
 *           that agrees), a hit only in variants weight 2 per variant; A/x/g pick the layer 127/80/30.
 *   BASIS   six plain lanes (offbeat, backbeat, four on the floor, 16th shaker, 8th ride, clave).
 *
 * GENERATION: every step of every lane keeps a 16-bit seed. The hit is a pure function of
 * (source weights, Density, Random, seed): for each layer 127/80/30 a roll from the seed against
 * weight/10 * density. So turning Density only adds or removes hits (no reshuffle while turning),
 * Generate re-seeds every unlocked lane, Variate ~25 % of their steps, Dice one lane.
 * STYLE picks an NGEN template: lanes 1-4 get its parts, lanes 5-8 genre-matched grooves.
 *
 * OUTPUT like Euclidier FX: MPC OS ignores VST MIDI out, so notes go to the plugin's own ALSA seq
 * port "<PLUG_NAME>" / "MIDI Out". Clock: host ppqPos at 96 pulses per quarter (24 per 16th), by
 * absolute pulse index (PpqClock, from euclidier_vst.cpp), so lanes sit on the MPC grid; swing
 * delays odd 16ths by up to 12 pulses (75 %).
 *
 * MIDI FX (FX tab), applied on output only -- the lanes' steps and the pattern lines stay as they are:
 *   REMIX   after Yamaha's Real Time Loop Remix (RS7000, Motif/MOXF): in the last bar of every 1/2/4/8
 *           bars the bar is remixed; NORMAL repositions slices, BREAK cuts slices out (stop-action),
 *           ROLL turns beats into crescendo rolls, FILL rebuilds the end of the bar into a fill. TYPE 1-16
 *           = complexity; the result depends only on the settings (same settings, same remix).
 *   ECHO    after NGEN's ECHOES: tempo-synced MIDI delay, repeats 1-8, probability, velocity falloff.
 *   GLITCH  after NGEN's GLITCH: random ratchets (up to 8 per step), ratchet gate, probability, random.
 *   Each has its own ON switch and TARGET (all lanes, one lane or a group).
 *
 * CONTROL BY MIDI CC (Control In port, subscribed to every hardware input, + CCs sent to the track),
 * only on "Control Ch" (OFF, 1..16; default 16):
 *   CC 10*L + k, lane L = 1..8: k = 0 on, 1 source, 2 pattern, 3 density, 4 length, 5 note,
 *                                 6 channel, 7 lock, 8 dice (>= 64)
 *   CC 100 style, 101 generate, 102 variate, 103 random, 104 swing, 105 gate, 106 auto,
 *   CC 107 preset, 108 load, 109 save (buttons: value >= 64)
 *   FX: CC 90 remix on, 91 mode, 92 type, 93 every, 94 target, 95 echo on, 96 time, 97 repeats,
 *       98 probability, 99 falloff, 110 echo target, 111 glitch on, 112 repeats, 113 gate,
 *       114 probability, 115 random, 116 glitch target
 *
 * TWO VARIANTS from this file (params.h decides, ../vst-fx/vst.json "effect": true):
 *   DrumGen     instrument (0 in / 2 out, silence) - uses one of the 8 plugin instrument slots
 *   DrumGen FX  insert effect (2 in / 2 out, audio untouched) - uses no instrument slot
 * Each instance has its own engine and its own ALSA client, so several can run side by side.
 * Diagnostics: /tmp/drumgen_vst.log, one line every 5 s.
 * ========================================================================== */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <dirent.h>
#include <dlfcn.h>
#include <strings.h>
#include <sys/stat.h>

#ifndef NO_ALSA
#include <alsa/asoundlib.h>
#endif

#include "params.h"
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */
#include "grooves.h"

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
typedef struct { int32_t type, byteSize, deltaFrames, flags; char data[16]; } VstEvent;
typedef struct {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    unsigned char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstEvent *events[2]; } VstEvents;
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;

enum {
    effOpen = 0, effClose = 1, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0, audioMasterGetTime = 7, audioMasterUpdateDisplay = 42 };
enum { kVstTransportPlaying = 1 << 1, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10 };
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5, effFlagsIsSynth = 1 << 8 };

#define BUILD_ID "drumgen-1.1.0"
#ifdef PLUG_EFFECT
#define PLUG_MODE "effect"
#else
#define PLUG_MODE "instrument"
#endif

static FILE *g_log;
#define LOG(...) do { if (g_log) { std::fprintf(g_log, __VA_ARGS__); std::fflush(g_log); } } while (0)

static double now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ---------------------------------------------------------------------------
 * PpqClock (from euclidier_vst.cpp, generalised to RES pulses per quarter): pulse N IS ppq N/RES.
 * Feeds (sent, last-in-this-block]; an overlap feeds nothing twice, a small hole is caught up,
 * a real jump (loop, locate, restart) re-anchors. The step position is a pure function of the
 * pulse index, so nothing can drift against the MPC grid.
 * ------------------------------------------------------------------------- */
static const int RES = 96;                 /* pulses per quarter */
static const int PPS = RES / 4;            /* pulses per 16th step */
struct PpqClock {
    long long sent = 0;
    bool resync = true;
    long pulses = 0, gaps = 0, dups = 0, jumps = 0;
};
static bool ppq_clock_block(PpqClock *c, double ppq, double tempo, double sr, int frames,
                            long long *from, long long *to, bool *jumped) {
    const long long TOL = RES / 2;
    double end = ppq + frames * (tempo / 60.0) / sr;
    long long first = (long long)std::ceil(ppq * RES - 1e-6);
    long long last = (long long)std::ceil(end * RES - 1e-6) - 1;
    *jumped = false;
    if (c->resync) { c->sent = first - 1; c->resync = false; }
    else if (first > c->sent + 1 + TOL || last < c->sent - TOL) { *jumped = true; c->jumps++; c->sent = first - 1; }
    if (first > c->sent + 1) c->gaps += first - (c->sent + 1);
    else if (first <= c->sent) c->dups += (last < c->sent ? last : c->sent) - first + 1;
    if (last <= c->sent) return false;
    *from = c->sent + 1; *to = last;
    c->pulses += last - c->sent;
    c->sent = last;
    return true;
}

/* ===========================================================================
 * Sources (process-wide, read-only after load_sources())
 * ========================================================================= */
static const int NLANES = 8, NSTEPS = 32, MAXTPL = 16;
static const int VELS[3] = {127, 80, 30};
static const char *const PART_NAMES[4] = {"Kick", "Snare", "Hat", "Perc"};
enum { BANK_NGEN = 0, BANK_GB0 = 1, BANK_BASIS = 1 + GB_NGENRES };   /* = the "Source" options order */

struct Prob { int len; uint8_t w[3][NSTEPS]; };     /* weights 0..10 per layer per step */
struct Tpl { char name[16]; uint8_t w[4][3][NSTEPS]; };
static Tpl g_tpl[MAXTPL];
static int g_ntpl = 0;
static std::string g_tpl_dir;
static Prob g_gb[GB_N];
static int g_gb_of[GB_NGENRES][16], g_gb_cnt[GB_NGENRES];   /* genre -> groove indices */
struct Basis { const char *name; Prob p; };
static Basis g_basis[6];
static std::once_flag g_src_once;

static std::string plugin_dir() {
    Dl_info info;
    if (dladdr((void *)&plugin_dir, &info) && info.dli_fname) {
        std::string p = info.dli_fname;
        size_t s = p.rfind('/');
        if (s != std::string::npos) return p.substr(0, s);
    }
    return "/tmp";
}

/* NGEN_DrumGenTemplate.py createHexFile(): row = part*3 + layer, 16 bytes per row, low nibble = even step */
static void decode_hex(const uint8_t *b, Tpl *t) {
    for (int p = 0; p < 4; p++)
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < NSTEPS; c++) {
                uint8_t v = b[(p * 3 + r) * 16 + c / 2];
                int x = (c % 2 == 0) ? (v & 15) : (v >> 4);
                t->w[p][r][c] = (uint8_t)(x > 10 ? 10 : x);
            }
}
/* first start: create the template folder with a short note in it (never touches existing files) */
static const char LIESMICH[] =
    "DrumGen / DrumGen FX - Template-Ordner\n"
    "======================================\n"
    "\n"
    "In diesen Ordner (/sdcard/vst/drumgen/) gehoeren die DRUMGEN-Templates des NGEN:\n"
    "*.hex-Dateien mit genau 192 Bytes (4 Parts x 3 Velocity-Layer x 32 Steps).\n"
    "\n"
    "- Die 11 Factory-Templates liegen im Repo spektroaudio/NGEN-Resources unter\n"
    "  \"Factory Content/DRUMGEN\" (BOOMBAP, BOSSA, BREAKS, DNB, ELECTRO, FUNK_BR,\n"
    "  GARAGE, HOUSE, JUNGLE, MEMPHIS, TECHNO).\n"
    "- Eigene Templates aus dem NGEN DrumGen Template Editor funktionieren genauso.\n"
    "- Hoechstens 16 Dateien, alphabetisch sortiert; der Dateiname ist der Style-Name.\n"
    "- Neue Dateien werden nach einem Neustart von MPC (oder Projekt neu laden,\n"
    "  wenn DrumGen vorher ganz entfernt war) eingelesen.\n"
    "\n"
    "DrumGen legt diesen Ordner und diese Datei beim ersten Start selbst an.\n";
static void make_template_dir() {
    struct stat st;
    if (stat(g_tpl_dir.c_str(), &st) != 0 && mkdir(g_tpl_dir.c_str(), 0755) != 0) return;
    std::string note = g_tpl_dir + "/LIESMICH.txt";
    if (stat(note.c_str(), &st) == 0) return;
    if (FILE *f = std::fopen(note.c_str(), "w")) { std::fputs(LIESMICH, f); std::fclose(f); }
}
static void load_templates() {
    const char *env = std::getenv("DRUMGEN_DIR");
    g_tpl_dir = env ? env : plugin_dir() + "/drumgen";
    if (!env) make_template_dir();
    std::vector<std::string> files;
    if (DIR *d = opendir(g_tpl_dir.c_str())) {
        while (dirent *e = readdir(d)) {
            size_t n = std::strlen(e->d_name);
            if (n > 4 && !strcasecmp(e->d_name + n - 4, ".hex")) files.push_back(e->d_name);
        }
        closedir(d);
    }
    std::sort(files.begin(), files.end(), [](const std::string &a, const std::string &b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
    for (const std::string &f : files) {
        if (g_ntpl >= MAXTPL) break;
        std::string path = g_tpl_dir + "/" + f;
        FILE *fp = std::fopen(path.c_str(), "rb");
        if (!fp) continue;
        uint8_t buf[192];
        size_t got = std::fread(buf, 1, sizeof buf, fp);
        std::fclose(fp);
        if (got < 192) { LOG("[drumgen_vst] %s: %zu bytes, expected 192 - skipped\n", path.c_str(), got); continue; }
        Tpl *t = &g_tpl[g_ntpl];
        std::string nm = f.substr(0, f.size() - 4);
        size_t k = 0;
        for (; k < nm.size() && k < sizeof t->name - 1; k++) t->name[k] = (char)std::toupper((unsigned char)nm[k]);
        t->name[k] = 0;
        decode_hex(buf, t);
        g_ntpl++;
    }
    LOG("[drumgen_vst] %d NGEN templates from %s\n", g_ntpl, g_tpl_dir.c_str());
}
static int lane_rank(char c) { return c == 'A' ? 0 : c == 'x' ? 1 : c == 'g' ? 2 : -1; }
static void gb_layers(const gb_groove &g, Prob *p) {
    std::memset(p, 0, sizeof *p);
    p->len = g.steps > NSTEPS ? NSTEPS : g.steps;
    for (int s = 0; s < p->len; s++) {
        int main = -1, alts = 0, best = 3;
        for (int l = 0; l < 3; l++) {
            const char *ln = g.lanes[l];
            if (!ln || (int)std::strlen(ln) <= s) continue;
            int r = lane_rank(ln[s]);
            if (r < 0) continue;
            if (l == 0) main = r;
            else { alts++; if (r < best) best = r; }
        }
        if (main >= 0) p->w[main][s] = (uint8_t)std::min(10, 8 + alts);
        else if (alts) p->w[best][s] = (uint8_t)std::min(10, 2 * alts);
    }
}
static void basis_lane(Basis *b, const char *name, int (*f)(int, int *)) {
    b->name = name;
    std::memset(&b->p, 0, sizeof b->p);
    b->p.len = 16;
    for (int s = 0; s < 16; s++) { int r = 0, w = f(s, &r); if (w) b->p.w[r][s] = (uint8_t)w; }
}
static void load_sources() {
    load_templates();
    std::memset(g_gb_cnt, 0, sizeof g_gb_cnt);
    for (int i = 0; i < GB_N; i++) {
        gb_layers(GROOVES[i], &g_gb[i]);
        int g = GROOVES[i].genre;
        if (g_gb_cnt[g] < 16) g_gb_of[g][g_gb_cnt[g]++] = i;
    }
    basis_lane(&g_basis[0], "Offbeat 8th", [](int s, int *r) { *r = 1; return s % 4 == 2 ? 10 : 0; });
    basis_lane(&g_basis[1], "Backbeat", [](int s, int *r) { if (s % 8 == 4) { *r = 0; return 10; } *r = 2; return s == 15 ? 2 : 0; });
    basis_lane(&g_basis[2], "Four Floor", [](int s, int *r) { *r = 0; return s % 4 == 0 ? 10 : 0; });
    basis_lane(&g_basis[3], "16th Shaker", [](int s, int *r) { if (s % 2) { *r = 2; return 8; } *r = 1; return s % 4 ? 9 : 7; });
    basis_lane(&g_basis[4], "Ride 8th", [](int s, int *r) { if (s % 2) { *r = 2; return 2; } *r = 1; return s % 4 ? 8 : 10; });
    basis_lane(&g_basis[5], "Son Clave 3-2", [](int s, int *r) { *r = 0; return (s == 0 || s == 3 || s == 6 || s == 10 || s == 12) ? 10 : 0; });
}

/* (bank, item) -> weights; false if the source doesn't exist (e.g. no templates on the card) */
static bool get_prob(int bank, int item, Prob *out) {
    if (bank == BANK_NGEN) {
        if (!g_ntpl) return false;
        int t = std::min(item / 4, g_ntpl - 1), p = item % 4;
        out->len = NSTEPS;
        std::memcpy(out->w, g_tpl[t].w[p], sizeof out->w);
        return true;
    }
    if (bank >= BANK_GB0 && bank < BANK_BASIS) {
        int g = bank - BANK_GB0;
        if (!g_gb_cnt[g]) return false;
        *out = g_gb[g_gb_of[g][std::min(item, g_gb_cnt[g] - 1)]];
        return true;
    }
    if (bank == BANK_BASIS) { *out = g_basis[std::min(item, 5)].p; return true; }
    return false;
}
static std::string source_name(int bank, int item) {
    if (bank == BANK_NGEN) {
        if (!g_ntpl) return "no templates";
        int t = std::min(item / 4, g_ntpl - 1);
        return std::string(g_tpl[t].name) + " " + PART_NAMES[item % 4];
    }
    if (bank >= BANK_GB0 && bank < BANK_BASIS) {
        int g = bank - BANK_GB0;
        if (!g_gb_cnt[g]) return "-";
        return GROOVES[g_gb_of[g][std::min(item, g_gb_cnt[g] - 1)]].name;
    }
    if (bank == BANK_BASIS) return g_basis[std::min(item, 5)].name;
    return "-";
}
static int find_tpl(const char *name) {
    for (int i = 0; i < g_ntpl; i++) if (!strcasecmp(g_tpl[i].name, name)) return i;
    return -1;
}
static bool find_groove(const char *genre, const char *name, int *bank, int *item) {
    for (int g = 0; g < GB_NGENRES; g++) {
        if (std::strcmp(GB_GENRES[g], genre)) continue;
        for (int k = 0; k < g_gb_cnt[g]; k++)
            if (!std::strcmp(GROOVES[g_gb_of[g][k]].name, name)) { *bank = BANK_GB0 + g; *item = k; return true; }
    }
    return false;
}

/* Lanes 5-8 (Clap, Open Hat, Tom, Ride) per factory style: G = Groove Bank genre/name,
 * N = another NGEN template's part, B = basis lane. Unknown styles get BASIS_DEFAULT. */
struct XSrc { char kind; const char *a; const char *b; int n; };
struct StyleExtra { const char *style; XSrc x[4]; };
static const StyleExtra EXTRA[] = {
    {"HOUSE",   {{'B', 0, 0, 1}, {'G', "HOUSE", "Offbeat Stab", 0}, {'G', "HOUSE", "Jackin", 0}, {'G', "HOUSE", "Disco", 0}}},
    {"TECHNO",  {{'B', 0, 0, 1}, {'G', "TECHNO", "Driving", 0}, {'G', "TECHNO", "Hypnotic", 0}, {'G', "TECHNO", "Acid", 0}}},
    {"GARAGE",  {{'G', "GARAGE", "2-Step", 0}, {'G', "GARAGE", "Speed Garage", 0}, {'G', "GARAGE", "UK Garage", 0}, {'G', "GARAGE", "Future", 0}}},
    {"DNB",     {{'G', "DNB", "Two-Step", 0}, {'G', "DNB", "Rollers", 0}, {'G', "DNB", "Neurofunk", 0}, {'N', "JUNGLE", 0, 2}}},
    {"JUNGLE",  {{'G', "DNB", "Amen", 0}, {'G', "DNB", "Jungle", 0}, {'N', "BREAKS", 0, 3}, {'N', "DNB", 0, 2}}},
    {"BREAKS",  {{'G', "FUNK", "JB Push", 0}, {'G', "HIPHOP", "East Coast", 0}, {'N', "JUNGLE", 0, 3}, {'N', "ELECTRO", 0, 2}}},
    {"ELECTRO", {{'B', 0, 0, 1}, {'G', "FUNK", "16th Comp", 0}, {'N', "TECHNO", 0, 3}, {'G', "TECHNO", "Detroit", 0}}},
    {"BOOMBAP", {{'G', "HIPHOP", "Boom Bap", 0}, {'G', "HIPHOP", "Dilla", 0}, {'G', "HIPHOP", "Lofi", 0}, {'G', "SOUL", "Neo Soul", 0}}},
    {"MEMPHIS", {{'G', "TRAP", "Half-Time", 0}, {'G', "TRAP", "Trap Roll", 0}, {'G', "TRAP", "Drill", 0}, {'G', "TRAP", "Bounce", 0}}},
    {"FUNK_BR", {{'G', "REGGAE", "Dembow", 0}, {'G', "LATIN", "Tresillo", 0}, {'G', "FUNK", "Second Line", 0}, {'G', "LATIN", "Samba", 0}}},
    {"BOSSA",   {{'G', "LATIN", "Bossa Nova", 0}, {'G', "LATIN", "Son Clave", 0}, {'G', "LATIN", "Tumbao", 0}, {'G', "JAZZ", "Bossa Comp", 0}}},
};
static const int BASIS_DEFAULT[4] = {1, 0, 3, 4};   /* backbeat, offbeat, shaker, ride */
static void extra_source(const char *style, int k, int *bank, int *item) {
    for (const StyleExtra &e : EXTRA) {
        if (strcasecmp(e.style, style)) continue;
        const XSrc &x = e.x[k];
        if (x.kind == 'B') { *bank = BANK_BASIS; *item = x.n; return; }
        if (x.kind == 'G' && find_groove(x.a, x.b, bank, item)) return;
        if (x.kind == 'N') { int t = find_tpl(x.a); if (t >= 0) { *bank = BANK_NGEN; *item = t * 4 + x.n; return; } }
        break;
    }
    *bank = BANK_BASIS; *item = BASIS_DEFAULT[k];
}

/* ===========================================================================
 * Parameters: indices looked up by key once (the list order is params.json's)
 * ========================================================================= */
static int param_index(const char *key) {
    for (int i = 0; i < NPARAMS; i++) if (!std::strcmp(PARAMS[i].key, key)) return i;
    return -1;
}
static int P_STYLE, P_GEN, P_VAR, P_RND, P_SWING, P_GATE, P_AUTO, P_PRESET, P_LOAD, P_SAVE, P_CCCH, P_STATUS;
static int P_ON[NLANES], P_BANK[NLANES], P_ITEM[NLANES], P_DENS[NLANES], P_LEN[NLANES], P_NOTE[NLANES],
    P_CH[NLANES], P_LOCK[NLANES], P_DICE[NLANES], P_PAT[NLANES];
static int P_RMX_ON, P_RMX_MODE, P_RMX_TYPE, P_RMX_EVERY, P_RMX_TGT, P_ECHO_ON, P_ECHO_TIME, P_ECHO_REP,
    P_ECHO_PROB, P_ECHO_FALL, P_ECHO_TGT, P_GL_ON, P_GL_REP, P_GL_GATE, P_GL_PROB, P_GL_RND, P_GL_TGT;
static std::once_flag g_idx_once;
static void init_indices() {
    P_STYLE = param_index("style"); P_GEN = param_index("generate"); P_VAR = param_index("variate");
    P_RND = param_index("random"); P_SWING = param_index("swing"); P_GATE = param_index("gate");
    P_AUTO = param_index("auto"); P_PRESET = param_index("preset"); P_LOAD = param_index("preset_load");
    P_SAVE = param_index("preset_save"); P_CCCH = param_index("cc_ch"); P_STATUS = param_index("status");
    P_RMX_ON = param_index("rmx_on"); P_RMX_MODE = param_index("rmx_mode"); P_RMX_TYPE = param_index("rmx_type");
    P_RMX_EVERY = param_index("rmx_every"); P_RMX_TGT = param_index("rmx_target");
    P_ECHO_ON = param_index("echo_on"); P_ECHO_TIME = param_index("echo_time"); P_ECHO_REP = param_index("echo_rep");
    P_ECHO_PROB = param_index("echo_prob"); P_ECHO_FALL = param_index("echo_fall"); P_ECHO_TGT = param_index("echo_target");
    P_GL_ON = param_index("gl_on"); P_GL_REP = param_index("gl_rep"); P_GL_GATE = param_index("gl_gate");
    P_GL_PROB = param_index("gl_prob"); P_GL_RND = param_index("gl_rnd"); P_GL_TGT = param_index("gl_target");
    char k[24];
    for (int l = 0; l < NLANES; l++) {
#define IDX(arr, suffix) std::snprintf(k, sizeof k, "l%d_" suffix, l + 1); arr[l] = param_index(k);
        IDX(P_ON, "on") IDX(P_BANK, "src") IDX(P_ITEM, "item") IDX(P_DENS, "dens") IDX(P_LEN, "len")
        IDX(P_NOTE, "note") IDX(P_CH, "ch") IDX(P_LOCK, "lock") IDX(P_DICE, "dice") IDX(P_PAT, "pat")
#undef IDX
    }
}
static bool is_readout(int i) {
    if (i == P_STATUS) return true;
    for (int l = 0; l < NLANES; l++) if (i == P_PAT[l]) return true;
    return false;
}
static bool is_value_param(int i) { return !PARAMS[i].momentary && !popup_is(i) && !is_readout(i); }
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static int norm_to_ui(const param_t *p, float n) {
    if (p->nopts) return (int)std::lround(clamp01(n) * (p->nopts - 1));
    return (int)std::lround(p->min + (p->max - p->min) * clamp01(n));
}
static float ui_to_norm(const param_t *p, double v) {
    if (p->nopts) return p->nopts > 1 ? clamp01((float)(v / (p->nopts - 1))) : 0.0f;
    return p->max > p->min ? clamp01((float)((v - p->min) / (p->max - p->min))) : 0.0f;
}
static void copy_str(void *dst, const char *s, size_t max) {
    std::strncpy((char *)dst, s, max - 1);
    ((char *)dst)[max - 1] = 0;
}

/* ===========================================================================
 * Presets: 32 slots, one line each ("<slot>:<state>") in <plugin dir>/drumgen_BANK.txt
 * ========================================================================= */
static const int NPRESETS = 32;
static std::mutex g_bank_mx;
static std::string g_bank[NPRESETS];
static bool g_bank_loaded = false;
static std::string bank_path() { return plugin_dir() + "/drumgen_BANK.txt"; }
static void bank_load_locked() {
    if (g_bank_loaded) return;
    g_bank_loaded = true;
    const char *env = std::getenv("DRUMGEN_BANK");
    FILE *f = std::fopen(env ? env : bank_path().c_str(), "r");
    if (!f) return;
    static char line[16384];
    while (std::fgets(line, sizeof line, f)) {
        int slot = std::atoi(line);
        char *c = std::strchr(line, ':');
        if (!c || slot < 1 || slot > NPRESETS) continue;
        std::string s(c + 1);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        g_bank[slot - 1] = s;
    }
    std::fclose(f);
}
static void bank_write_async() {   /* file I/O off the audio thread */
    std::string out;
    {
        std::lock_guard<std::mutex> lk(g_bank_mx);
        for (int i = 0; i < NPRESETS; i++) if (!g_bank[i].empty()) out += std::to_string(i + 1) + ":" + g_bank[i] + "\n";
    }
    std::thread([out]() {
        const char *env = std::getenv("DRUMGEN_BANK");
        std::string p = env ? env : bank_path();
        std::lock_guard<std::mutex> lk(g_bank_mx);
        std::string tmp = p + ".tmp";
        if (FILE *f = std::fopen(tmp.c_str(), "w")) {
            std::fwrite(out.data(), 1, out.size(), f);
            std::fclose(f);
            std::rename(tmp.c_str(), p.c_str());
        }
    }).detach();
}

/* ===========================================================================
 * Per-instance state
 * ========================================================================= */
struct Plugin {
    AEffect fx;
    audioMasterCallback master = nullptr;
    std::mutex mx;                          /* guards everything below except cache[] reads */
    std::atomic<float> cache[NPARAMS];      /* what the host sees (normalised) */
    volatile int release[NPARAMS] = {0};    /* momentary triggers / popups to report back */
    float open[NPARAMS] = {0};              /* popup "open" flags (popup.h): wrapper-only */
    uint16_t seed[NLANES][NSTEPS];
    uint8_t steps[NLANES][NSTEPS];          /* derived: velocity per step (0 = rest) */
    int off_note[NLANES], off_ch[NLANES];
    long long off_at[NLANES];               /* pulse index of the pending note-off, -1 = none */
    uint32_t rng = 0x12345678u;
    float sr = 44100.0f;
    bool was_playing = false;
    PpqClock clk;
    long long last_step = -1, last_auto_bar = -1;
    bool refresh = false;
    uint8_t inq[64][3];
    int in_n = 0;
    long blocks = 0, no_ti = 0, notes = 0, cc_in = 0, cc_used = 0;
    double t_last_log = 0, last_ppq = 0, last_tempo = 0;
    char chunk[8192];
    char pat_txt[NLANES][48];
    struct QEv { long long at; uint8_t st, d1, d2; };
    QEv q[1024];                            /* FX notes between steps (ratchets, rolls, echoes) */
    int qn = 0;
    bool rmx_now = false;                   /* the current bar is being remixed (status line) */
#ifndef NO_ALSA
    snd_seq_t *seq = nullptr;
    int port = -1, ctl_port = -1;
#endif
};

static int ival(Plugin *w, int i) { return i < 0 ? 0 : norm_to_ui(&PARAMS[i], w->cache[i].load()); }
static void setv(Plugin *w, int i, int v) { if (i >= 0) w->cache[i].store(ui_to_norm(&PARAMS[i], v)); }

static uint32_t xr(Plugin *w) { uint32_t x = w->rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return w->rng = x; }
/* k-th uniform number in [0,1) of a step seed (lane and step mixed in, so equal seeds differ per place) */
static float roll(uint16_t seed, int lane, int step, int k) {
    uint32_t h = seed * 0x9E3779B1u ^ (uint32_t)(lane * 131 + step * 7 + k * 0x45D9F3B);
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return (h >> 8) * (1.0f / 16777216.0f);
}

/* steps[l] from (source, density, random, seeds): the DRUMGEN rule, see header */
static void eval_lane(Plugin *w, int l) {
    Prob p;
    bool ok = get_prob(ival(w, P_BANK[l]), ival(w, P_ITEM[l]), &p);
    float dens = ival(w, P_DENS[l]) / 100.0f, R = ival(w, P_RND) / 100.0f;
    int len = ival(w, P_LEN[l]);
    for (int s = 0; s < NSTEPS; s++) {
        w->steps[l][s] = 0;
        if (!ok || s >= len) continue;
        int ps = s % p.len;
        uint16_t sd = w->seed[l][s];
        for (int r = 0; r < 3; r++) {
            int wt = p.w[r][ps];
            if (R > 0) {
                if (roll(sd, l, s, 3 + r) < R * 0.6f)
                    wt = std::max(0, std::min(10, wt + (int)std::lround((roll(sd, l, s, 6 + r) * 2 - 1) * (1 + 4 * R))));
                if (wt == 0 && r == 2 && roll(sd, l, s, 9) < R * 0.07f) wt = 6;
            }
            float pr = std::min(1.0f, wt / 10.0f * dens);
            if (pr > 0 && roll(sd, l, s, r) < pr) { w->steps[l][s] = (uint8_t)VELS[r]; break; }
        }
    }
    /* "X..x X... ..X. ...." : X = 127, x = 80, - = 30, . = rest, only the lane's length */
    char *t = w->pat_txt[l];
    int n = 0;
    for (int s = 0; s < len && s < NSTEPS; s++) {
        if (s && s % 4 == 0) t[n++] = ' ';
        int v = w->steps[l][s];
        t[n++] = v == 127 ? 'X' : v == 80 ? 'x' : v == 30 ? '-' : '.';
    }
    t[n] = 0;
}
static void eval_all(Plugin *w) { for (int l = 0; l < NLANES; l++) eval_lane(w, l); w->refresh = true; }
static void reseed(Plugin *w, int l, float frac) {
    for (int s = 0; s < NSTEPS; s++)
        if (frac >= 1.0f || (xr(w) & 0xFFFF) < (uint32_t)(frac * 65536)) w->seed[l][s] = (uint16_t)xr(w);
}
static void lane_source(Plugin *w, int l, int bank, int item) {
    setv(w, P_BANK[l], bank);
    setv(w, P_ITEM[l], item);
    Prob p;
    setv(w, P_LEN[l], get_prob(bank, item, &p) ? p.len : 16);
}
static void apply_style(Plugin *w) {
    int st = ival(w, P_STYLE);
    const char *name = (g_ntpl && st < g_ntpl) ? g_tpl[st].name : "";
    for (int l = 0; l < NLANES; l++) {
        if (ival(w, P_LOCK[l])) continue;
        if (l < 4) {
            if (g_ntpl && st < g_ntpl) lane_source(w, l, BANK_NGEN, st * 4 + l);
        } else {
            int b, it;
            extra_source(name, l - 4, &b, &it);
            lane_source(w, l, b, it);
        }
        setv(w, P_DENS[l], 100);
        reseed(w, l, 1.0f);
    }
    eval_all(w);
}
static void generate(Plugin *w, float frac) {
    for (int l = 0; l < NLANES; l++) if (!ival(w, P_LOCK[l])) reseed(w, l, frac);
    eval_all(w);
}

/* ---- state string (project chunk and preset slots): "key=value;" + "sN=<seeds hex>;" ---- */
static std::string state_string(Plugin *w, bool with_globals) {
    std::string s = "v=1;";
    char buf[96];
    for (int i = 0; i < NPARAMS; i++) {
        if (!is_value_param(i)) continue;
        if (!with_globals && (i == P_CCCH || i == P_PRESET)) continue;
        std::snprintf(buf, sizeof buf, "%s=%d;", PARAMS[i].key, ival(w, i));
        s += buf;
    }
    for (int l = 0; l < NLANES; l++) {
        s += "s" + std::to_string(l + 1) + "=";
        for (int k = 0; k < NSTEPS; k++) { std::snprintf(buf, sizeof buf, "%04x", w->seed[l][k]); s += buf; }
        s += ";";
    }
    return s;
}
static void apply_state(Plugin *w, const char *str, bool with_globals) {
    std::string copy(str);
    char *save = nullptr;
    for (char *tok = strtok_r(&copy[0], ";", &save); tok; tok = strtok_r(nullptr, ";", &save)) {
        char *eq = std::strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 1;
        if (tok[0] == 's' && tok[1] >= '1' && tok[1] <= '8' && !tok[2]) {
            int l = tok[1] - '1';
            for (int k = 0; k < NSTEPS && std::strlen(v) >= (size_t)(k + 1) * 4; k++) {
                char h[5] = {v[k * 4], v[k * 4 + 1], v[k * 4 + 2], v[k * 4 + 3], 0};
                w->seed[l][k] = (uint16_t)std::strtoul(h, nullptr, 16);
            }
            continue;
        }
        int i = param_index(tok);
        if (i < 0 || !is_value_param(i)) continue;
        if (!with_globals && (i == P_CCCH || i == P_PRESET)) continue;
        setv(w, i, std::atoi(v));
    }
    eval_all(w);
}
static void preset_load(Plugin *w) {
    int slot = ival(w, P_PRESET) - 1;
    std::string s;
    {
        std::lock_guard<std::mutex> lk(g_bank_mx);
        bank_load_locked();
        s = g_bank[slot];
    }
    if (!s.empty()) apply_state(w, s.c_str(), false);
    LOG("[drumgen_vst] preset %d %s\n", slot + 1, s.empty() ? "empty" : "loaded");
}
static void preset_save(Plugin *w) {
    int slot = ival(w, P_PRESET) - 1;
    {
        std::lock_guard<std::mutex> lk(g_bank_mx);
        bank_load_locked();
        g_bank[slot] = state_string(w, false);
    }
    bank_write_async();
    LOG("[drumgen_vst] preset %d saved\n", slot + 1);
}

/* ---- MIDI out / Control In (ALSA) ------------------------------------------------ */
static void apply_cc(Plugin *w, int ch, int cc, int val);
#ifndef NO_ALSA
static void ctl_subscribe(Plugin *w, int client, int port) {
    if (client == snd_seq_client_id(w->seq) || client == SND_SEQ_CLIENT_SYSTEM) return;
    snd_seq_port_info_t *pi;
    snd_seq_port_info_alloca(&pi);
    if (snd_seq_get_any_port_info(w->seq, client, port, pi) < 0) return;
    unsigned cap = snd_seq_port_info_get_capability(pi), type = snd_seq_port_info_get_type(pi);
    if ((cap & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) != (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) return;
    if (cap & SND_SEQ_PORT_CAP_NO_EXPORT) return;
    if (!(type & SND_SEQ_PORT_TYPE_HARDWARE)) return;   /* not other plugins' virtual ports */
    int rc = snd_seq_connect_from(w->seq, w->ctl_port, client, port);
    LOG("[drumgen_vst] control in <- %d:%d '%s'%s\n", client, port, snd_seq_port_info_get_name(pi),
        rc < 0 ? " (already connected or refused)" : "");
}
static void ctl_subscribe_all(Plugin *w) {
    snd_seq_client_info_t *ci;
    snd_seq_port_info_t *pi;
    snd_seq_client_info_alloca(&ci);
    snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(w->seq, ci) >= 0) {
        int c = snd_seq_client_info_get_client(ci);
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(w->seq, pi) >= 0) ctl_subscribe(w, c, snd_seq_port_info_get_port(pi));
    }
}
static void alsa_open(Plugin *w) {
    if (snd_seq_open(&w->seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
        w->seq = nullptr; LOG("[drumgen_vst] snd_seq_open failed\n"); return;
    }
    snd_seq_set_client_pool_output(w->seq, 2048);
    snd_seq_set_client_name(w->seq, PLUG_NAME);
    w->port = snd_seq_create_simple_port(w->seq, "MIDI Out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                         SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    LOG("[drumgen_vst] ALSA client '" PLUG_NAME "' (%d) port %d\n", snd_seq_client_id(w->seq), w->port);
    w->ctl_port = snd_seq_create_simple_port(w->seq, "Control In", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                             SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (w->ctl_port < 0) { LOG("[drumgen_vst] no Control In port\n"); return; }
    snd_seq_connect_from(w->seq, w->ctl_port, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);
    ctl_subscribe_all(w);
}
static void midi_out(Plugin *w, int st, int d1, int d2) {
    if (!w->seq || w->port < 0) return;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, w->port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    int type = st & 0xF0, ch = st & 0x0F;
    if (type == 0x90 && d2) snd_seq_ev_set_noteon(&ev, ch, d1, d2);
    else if (type == 0x80 || type == 0x90) snd_seq_ev_set_noteoff(&ev, ch, d1, 0);
    else if (type == 0xB0) snd_seq_ev_set_controller(&ev, ch, d1, d2);
    else return;
    snd_seq_event_output_direct(w->seq, &ev);
}
static void alsa_poll(Plugin *w) {   /* audio thread, w->mx held */
    if (!w->seq || w->ctl_port < 0) return;
    snd_seq_event_t *ev = nullptr;
    for (int guard = 0; guard < 256 && snd_seq_event_input_pending(w->seq, 1) > 0; guard++) {
        if (snd_seq_event_input(w->seq, &ev) < 0 || !ev) break;
        if (ev->type == SND_SEQ_EVENT_CONTROLLER) { w->cc_in++; apply_cc(w, ev->data.control.channel, (int)ev->data.control.param, ev->data.control.value); }
        else if (ev->type == SND_SEQ_EVENT_PORT_START) ctl_subscribe(w, ev->data.addr.client, ev->data.addr.port);
    }
}
static void alsa_close(Plugin *w) { if (w->seq) snd_seq_close(w->seq); w->seq = nullptr; }
#else
/* Offline test build: DRUMGEN_TRACE=1 prints every outgoing message. */
static void alsa_open(Plugin *) {}
static void midi_out(Plugin *, int st, int d1, int d2) {
    static int trace = std::getenv("DRUMGEN_TRACE") ? 1 : 0;
    if (trace) std::printf("MIDI %02x %02x %02x\n", st, d1, d2);
}
static void alsa_poll(Plugin *) {}
static void alsa_close(Plugin *) {}
#endif

/* ---- parameter changes with side effects (w->mx held) ------------------------------ */
static void trigger(Plugin *w, int i) {
    if (i == P_GEN) generate(w, 1.0f);
    else if (i == P_VAR) generate(w, 0.25f);
    else if (i == P_LOAD) preset_load(w);
    else if (i == P_SAVE) preset_save(w);
    else for (int l = 0; l < NLANES; l++) if (i == P_DICE[l]) { reseed(w, l, 1.0f); eval_lane(w, l); w->refresh = true; }
}
/* value of i changed from old to the current cache value */
static void changed(Plugin *w, int i, int old) {
    int now = ival(w, i);
    if (now == old) return;
    if (i == P_STYLE) {
        if (g_ntpl && now >= g_ntpl) {      /* the knob range is 16; stop at the last template on the card */
            setv(w, i, g_ntpl - 1);
            if (old == g_ntpl - 1) return;
        }
        apply_style(w);
        return;
    }
    if (i == P_RND) { eval_all(w); return; }
    for (int l = 0; l < NLANES; l++) {
        if (i == P_BANK[l] || i == P_ITEM[l]) {
            if (i == P_BANK[l]) setv(w, P_ITEM[l], 0);
            Prob p;
            if (get_prob(ival(w, P_BANK[l]), ival(w, P_ITEM[l]), &p)) setv(w, P_LEN[l], p.len);
            eval_lane(w, l); w->refresh = true; return;
        }
        if (i == P_DENS[l] || i == P_LEN[l]) { eval_lane(w, l); return; }
    }
}

/* ---- control by MIDI CC (see header) ---------------------------------------------- */
static int cc_param(int cc) {
    static const int *const LANE[9] = {P_ON, P_BANK, P_ITEM, P_DENS, P_LEN, P_NOTE, P_CH, P_LOCK, P_DICE};
    if (cc >= 10 && cc <= 89 && cc % 10 <= 8) return LANE[cc % 10][cc / 10 - 1];
    const int GLOB[10] = {P_STYLE, P_GEN, P_VAR, P_RND, P_SWING, P_GATE, P_AUTO, P_PRESET, P_LOAD, P_SAVE};
    if (cc >= 100 && cc <= 109) return GLOB[cc - 100];
    const int FX1[10] = {P_RMX_ON, P_RMX_MODE, P_RMX_TYPE, P_RMX_EVERY, P_RMX_TGT, P_ECHO_ON, P_ECHO_TIME, P_ECHO_REP,
                         P_ECHO_PROB, P_ECHO_FALL};
    if (cc >= 90 && cc <= 99) return FX1[cc - 90];
    const int FX2[7] = {P_ECHO_TGT, P_GL_ON, P_GL_REP, P_GL_GATE, P_GL_PROB, P_GL_RND, P_GL_TGT};
    if (cc >= 110 && cc <= 116) return FX2[cc - 110];
    return -1;
}
static void apply_cc(Plugin *w, int ch, int cc, int val) {
    if (P_CCCH < 0) return;
    int sel = ival(w, P_CCCH);                 /* 0 = OFF, else channel 1..16 */
    if (sel == 0 || ch != sel - 1) return;
    int i = cc_param(cc);
    if (i < 0) return;
    const param_t *p = &PARAMS[i];
    val = std::max(0, std::min(127, val));
    w->cc_used++;
    if (p->momentary) { if (val >= 64) trigger(w, i); return; }
    int old = ival(w, i);
    w->cache[i].store(ui_to_norm(p, norm_to_ui(p, val / 127.0f)));
    changed(w, i, old);
    w->refresh = true;
}

/* ---- playback ---------------------------------------------------------------------- */
static void note_off_lane(Plugin *w, int l) {
    if (w->off_at[l] < 0) return;
    midi_out(w, 0x80 | w->off_ch[l], w->off_note[l], 0);
    w->off_at[l] = -1;
}
/* FX event queue: notes at a later pulse (ratchets, rolls, echoes) */
static void q_push(Plugin *w, long long at, int st, int d1, int d2) {
    if (w->qn >= (int)(sizeof w->q / sizeof w->q[0])) return;
    w->q[w->qn++] = {at, (uint8_t)st, (uint8_t)d1, (uint8_t)d2};
}
static void q_note(Plugin *w, long long at, int ch, int note, int vel, int gate) {
    if (vel < 1) return;
    q_push(w, at, 0x90 | ch, note, std::min(127, vel));
    q_push(w, at + std::max(1, gate), 0x80 | ch, note, 0);
}
static void q_run(Plugin *w, long long idx) {   /* due note-offs first, then note-ons */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < w->qn;) {
            bool off = (w->q[i].st & 0xF0) == 0x80;
            if (w->q[i].at <= idx && off == (pass == 0)) {
                midi_out(w, w->q[i].st, w->q[i].d1, w->q[i].d2);
                if (!off) w->notes++;
                w->q[i] = w->q[--w->qn];
            } else i++;
        }
}
static void all_off(Plugin *w) {
    for (int l = 0; l < NLANES; l++) note_off_lane(w, l);
    for (int i = 0; i < w->qn; i++) if ((w->q[i].st & 0xF0) == 0x80) midi_out(w, w->q[i].st, w->q[i].d1, 0);
    w->qn = 0;
}

/* FX targets (options order of rmx_target / echo_target / gl_target): lane bit masks */
static const uint8_t TARGET_MASK[14] = {0xFF, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x0F, 0xF0, 0xFE, 0x12, 0x24};
static bool targets(Plugin *w, int p_on, int p_tgt, int l) {
    return ival(w, p_on) && (TARGET_MASK[std::min(13, ival(w, p_tgt))] >> l & 1);
}
/* deterministic [0,1) for the remix: same settings -> same result */
static float rh(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x9E3779B1u ^ b * 0x85EBCA77u ^ c * 0xC2B2AE3Du;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return (h >> 8) * (1.0f / 16777216.0f);
}
struct Remix { bool active; int src; int roll; float velmul; bool beat; };
/* REMIX for one step of the bar (lane-independent: all target lanes are cut the same way, like slices
 * of one loop). src = position in the bar to play instead (-1 = silent), roll = ratchets per step,
 * beat = take the strongest hit of the step's beat (rolls run through the whole beat). */
static Remix remix_at(Plugin *w, long long step) {
    Remix r = {false, (int)(step % 16), 0, 1.0f, false};
    if (!ival(w, P_RMX_ON)) return r;
    static const int EVERY[4] = {1, 2, 4, 8};
    int every = EVERY[std::min(3, ival(w, P_RMX_EVERY))];
    long long bar = step / 16;
    if (bar % every != every - 1) return r;
    r.active = true;
    int mode = ival(w, P_RMX_MODE), type = std::max(1, ival(w, P_RMX_TYPE)), pos = (int)(step % 16);
    float cx = (type - 1) / 15.0f;                        /* complexity 0..1 */
    uint32_t base = (uint32_t)(mode * 100 + type);
    if (mode == 0) {                                      /* NORMAL: reposition slices */
        int slice = type <= 4 ? 4 : type <= 8 ? 2 : 1, nsl = 16 / slice, k = pos / slice, in = pos % slice;
        if (rh(base, k, 1) < 0.25f + 0.6f * cx) {
            int src = std::min(nsl - 1, (int)(rh(base, k, 2) * nsl));
            bool rev = type >= 12 && rh(base, k, 3) < 0.3f;
            r.src = src * slice + (rev ? slice - 1 - in : in);
        }
    } else if (mode == 1) {                               /* BREAK: stop-action cuts, never the downbeat */
        int slice = type <= 8 ? 2 : 1, k = pos / slice;
        if (k > 0 && rh(base, k, 4) < 0.2f + 0.5f * cx) r.src = -1;
    } else if (mode == 2) {                               /* ROLL: whole beats become crescendo rolls */
        int beat = pos / 4;
        if (rh(base, beat, 5) < 0.2f + 0.6f * cx) {
            r.roll = type <= 6 ? 2 : type <= 11 ? 3 : 4;
            r.velmul = 0.45f + 0.55f * ((pos % 4) + 1) / 4.0f;
            r.beat = true;
        }
    } else {                                              /* FILL: the bar's end, leading back to the top */
        int zone = 16 - 4 * ((type + 3) / 4);             /* type 1-4: last beat ... 13-16: whole bar */
        if (pos >= zone) {
            if (rh(base, pos, 6) < 0.6f) r.src = zone + std::min(15 - zone, (int)(rh(base, pos, 7) * (16 - zone)));
            if (pos >= 12) {
                r.roll = 2 + (type > 8) + (type > 12);
                r.velmul = 0.5f + 0.5f * ((pos % 4) + 1) / 4.0f;
                r.beat = true;
            }
        }
    }
    return r;
}

static void fire_step(Plugin *w, long long step, long long pulse) {
    /* auto variate / generate at a bar line */
    if (step % 16 == 0 && step > 0) {
        static const int BARS[8] = {0, 1, 2, 4, 8, 4, 8, 16};
        int a = ival(w, P_AUTO), bar = (int)(step / 16);
        if (a > 0 && bar % BARS[a] == 0 && bar != w->last_auto_bar) { w->last_auto_bar = bar; generate(w, a <= 4 ? 0.25f : 1.0f); }
    }
    int gate = std::max(1, ival(w, P_GATE) * PPS / 100);
    Remix rx = remix_at(w, step);
    w->rmx_now = rx.active;
    long long bar0 = step - step % 16;
    static const int ECHO_T[9] = {12, 16, 24, 32, 36, 48, 64, 72, 96};   /* pulses at 96 per quarter */
    for (int l = 0; l < NLANES; l++) {
        if (!ival(w, P_ON[l])) continue;
        int len = std::max(1, ival(w, P_LEN[l]));
        int v = w->steps[l][step % len];
        int ratchets = 1;
        if (rx.active && targets(w, P_RMX_ON, P_RMX_TGT, l)) {
            if (rx.src < 0) v = 0;
            else if (rx.beat) {                           /* a roll runs through the beat if it has a hit */
                long long b = bar0 + (rx.src / 4) * 4;
                v = 0;
                for (int k = 0; k < 4; k++) v = std::max(v, (int)w->steps[l][(b + k) % len]);
            } else v = w->steps[l][(bar0 + rx.src) % len];
            if (v) v = std::max(1, (int)std::lround(v * rx.velmul));
            if (rx.roll > 1) ratchets = rx.roll;
        }
        if (!v) continue;
        int ch = (ival(w, P_CH[l]) - 1) & 15, note = ival(w, P_NOTE[l]) & 127;
        float vj = 0;                                     /* glitch velocity jitter */
        if (ratchets == 1 && targets(w, P_GL_ON, P_GL_TGT, l) && (xr(w) % 100) < (uint32_t)ival(w, P_GL_PROB)) {
            int maxr = std::max(2, ival(w, P_GL_REP));
            float rnd = ival(w, P_GL_RND) / 100.0f;
            ratchets = ((xr(w) % 1000) < (uint32_t)(rnd * 1000)) ? 2 + (int)(xr(w) % (uint32_t)(maxr - 1)) : maxr;
            vj = rnd * 0.4f;
        }
        note_off_lane(w, l);
        if (ratchets > 1) {                               /* ratchets / roll: all hits through the queue */
            int sp = std::max(1, PPS / ratchets);
            int g = rx.roll > 1 ? std::max(1, sp / 2) : std::max(1, sp * ival(w, P_GL_GATE) / 100);
            for (int k = 0; k < ratchets; k++) {
                int vk = v;
                if (vj > 0) vk = (int)std::lround(v * (1.0f - vj * (xr(w) % 1000) / 1000.0f));
                q_note(w, pulse + (long long)k * sp, ch, note, std::max(1, vk), g);
            }
        } else {
            midi_out(w, 0x90 | ch, note, v);
            w->off_note[l] = note; w->off_ch[l] = ch; w->off_at[l] = pulse + gate;
            w->notes++;
        }
        if (targets(w, P_ECHO_ON, P_ECHO_TGT, l) && (xr(w) % 100) < (uint32_t)ival(w, P_ECHO_PROB)) {
            int t = ECHO_T[std::min(8, ival(w, P_ECHO_TIME))], reps = std::max(1, ival(w, P_ECHO_REP));
            float keep = 1.0f - ival(w, P_ECHO_FALL) / 100.0f, ev = (float)v;
            for (int k = 1; k <= reps; k++) {
                ev *= keep;
                if (ev < 1.0f) break;
                q_note(w, pulse + (long long)k * t, ch, note, (int)std::lround(ev), std::max(1, std::min(t / 2, gate)));
            }
        }
    }
    w->last_step = step;
}
static void run_pulse(Plugin *w, long long idx) {
    for (int l = 0; l < NLANES; l++) if (w->off_at[l] >= 0 && idx >= w->off_at[l]) note_off_lane(w, l);
    q_run(w, idx);
    if (idx < 0) return;                                   /* pre-roll before bar 1 */
    int sw = (int)std::lround((ival(w, P_SWING) - 50) * PPS / 50.0);   /* 0..12 pulses */
    if (idx % PPS == 0 && (idx / PPS) % 2 == 0) fire_step(w, idx / PPS, idx);
    long long o = idx - sw;
    if (o >= 0 && o % PPS == 0 && (o / PPS) % 2 == 1) fire_step(w, o / PPS, idx);
    q_run(w, idx);                                         /* this pulse's own ratchet/roll hits */
}

static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    Plugin *w = (Plugin *)e->object;
#ifdef PLUG_EFFECT
    for (int ch = 0; ch < 2; ch++) {     /* insert: the track's audio goes through untouched (in may == out) */
        if (in && in[ch]) { if (out[ch] != in[ch]) std::memmove(out[ch], in[ch], (size_t)n * sizeof(float)); }
        else std::memset(out[ch], 0, (size_t)n * sizeof(float));
    }
#else
    (void)in;
    for (int32_t i = 0; i < n; i++) out[0][i] = out[1][i] = 0.0f;
#endif
    VstTimeInfo *ti = (VstTimeInfo *)w->master(&w->fx, audioMasterGetTime, 0, kVstTempoValid | kVstPpqPosValid, 0, 0);
    if (!ti) w->no_ti++;
    bool playing = ti && (ti->flags & kVstTransportPlaying);
    bool update_display = false;
    {
        std::lock_guard<std::mutex> lk(w->mx);
        for (int i = 0; i < w->in_n; i++)              /* CCs sent to the track */
            if ((w->inq[i][0] & 0xF0) == 0xB0) apply_cc(w, w->inq[i][0] & 0x0F, w->inq[i][1], w->inq[i][2]);
        w->in_n = 0;
        alsa_poll(w);
        if (playing && !w->was_playing) { w->clk.resync = true; w->last_auto_bar = -1; }
        if (!playing && w->was_playing) { all_off(w); w->last_step = -1; w->refresh = true; }
        w->was_playing = playing;
        if (playing && (ti->flags & kVstPpqPosValid) && ti->tempo > 0) {
            double sr = ti->sampleRate > 0 ? ti->sampleRate : (double)w->sr;
            long long from = 0, to = -1;
            bool jumped = false;
            if (ppq_clock_block(&w->clk, ti->ppqPos, ti->tempo, sr, n, &from, &to, &jumped)) {
                if (jumped) { all_off(w); LOG("[drumgen_vst] ppq jump to %.4f\n", ti->ppqPos); }
                for (long long idx = from; idx <= to; idx++) run_pulse(w, idx);
            }
            w->last_ppq = ti->ppqPos; w->last_tempo = ti->tempo;
        }
        if (w->refresh) { w->refresh = false; update_display = true; }
    }
    w->blocks++;
    double t1 = now_ms();
    if (t1 - w->t_last_log > 5000.0) {
        if (w->t_last_log > 0)
            LOG("[drumgen_vst] %ld blocks, playing %d, ppq %.3f, bpm %.2f | clock: %ld pulses, %ld caught up, %ld overlapped,"
                " %ld jumps | notes %ld | no timeinfo %ld | control cc %ld seen, %ld used\n",
                w->blocks, (int)w->was_playing, w->last_ppq, w->last_tempo, w->clk.pulses, w->clk.gaps, w->clk.dups,
                w->clk.jumps, w->notes, w->no_ti, w->cc_in, w->cc_used);
        w->blocks = w->no_ti = w->notes = w->cc_in = w->cc_used = 0;
        w->clk.pulses = w->clk.gaps = w->clk.dups = w->clk.jumps = 0;
        w->t_last_log = t1;
    }
    for (int i = 0; i < NPARAMS; i++)
        if (w->release[i]) { w->release[i] = 0; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
    if (update_display) w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
}

/* ---- parameters ------------------------------------------------------------------ */
static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (popup_set(w->open, i, n)) return;
    if (is_readout(i)) return;
    std::lock_guard<std::mutex> lk(w->mx);
    if (p->momentary) {
        if (n > 0.5f) { trigger(w, i); w->release[i] = 1; }
        return;
    }
    bool nudge = false;
    if (p->nopts > 1) {   /* a Q-Link nudge lands between options -> step one option */
        float pos = clamp01(n) * (p->nopts - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            float cur = w->cache[i].load() * (p->nopts - 1);
            int idx = (int)std::lround(cur) + (pos > cur ? 1 : -1);
            idx = std::max(0, std::min(p->nopts - 1, idx));
            n = (float)idx / (p->nopts - 1);
            nudge = true;
        }
    }
    int old = ival(w, i);
    w->cache[i].store(clamp01(n));      /* unrounded: slow Q-Link turns accumulate */
    changed(w, i, old);
    w->refresh = true;
    if (!nudge) popup_picked(w->open, w->release, i);
}
static float getParameter(AEffect *e, int32_t i) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    if (popup_is(i)) return w->open[i];
    if (is_readout(i)) return 0.0f;
    return PARAMS[i].momentary ? 0.0f : w->cache[i].load();
}
static void param_display(Plugin *w, int i, char *dst) {
    const param_t *p = &PARAMS[i];
    char buf[64];
    if (p->momentary) { copy_str(dst, "", 24); return; }
    if (i == P_STATUS) {
        long long st = w->last_step;
        if (!g_ntpl) std::snprintf(buf, sizeof buf, "No templates in vst/drumgen");
        else if (st < 0) std::snprintf(buf, sizeof buf, "%d templates - stopped", g_ntpl);
        else std::snprintf(buf, sizeof buf, "Bar %lld . %lld  |  Step %lld%s", st / 16 + 1, (st % 16) / 4 + 1, st % 32 + 1,
                           w->rmx_now ? "  |  REMIX" : "");
        copy_str(dst, buf, 48); return;
    }
    for (int l = 0; l < NLANES; l++) if (i == P_PAT[l]) { copy_str(dst, w->pat_txt[l], 48); return; }
    if (i == P_STYLE) {
        int st = ival(w, i);
        copy_str(dst, g_ntpl ? (st < g_ntpl ? g_tpl[st].name : "-") : "no templates", 24); return;
    }
    for (int l = 0; l < NLANES; l++)
        if (i == P_ITEM[l]) { copy_str(dst, source_name(ival(w, P_BANK[l]), ival(w, i)).c_str(), 24); return; }
    float nv = popup_is(i) ? w->open[i] : w->cache[i].load();
    int ui = norm_to_ui(p, nv);
    if (p->nopts) { copy_str(dst, p->opts[ui], 24); return; }
    std::snprintf(buf, sizeof buf, "%d%s", ui, p->unit);
    copy_str(dst, buf, 24);
}

/* ---- dispatcher ---------------------------------------------------------------- */
static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose:
        {
            std::lock_guard<std::mutex> lk(w->mx);
            all_off(w);
            for (int ch = 0; ch < 16; ch++) midi_out(w, 0xB0 | ch, 123, 0);   /* All Notes Off */
        }
        alsa_close(w);
        LOG("[drumgen_vst] closed\n");
        delete w;
        return 1;
#ifdef PLUG_EFFECT
    case effGetPlugCategory: return 1;   /* kPlugCategEffect */
#else
    case effGetPlugCategory: return 2;   /* kPlugCategSynth */
#endif
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS && !is_readout(idx);
    case effGetParamName:
        if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].name, 32);
        return 1;
    case effGetParamLabel:
        if (idx >= 0 && idx < NPARAMS) copy_str(p, "", 8);
        return 1;
    case effGetParamDisplay: {
        if (idx < 0 || idx >= NPARAMS) return 0;
        std::lock_guard<std::mutex> lk(w->mx);
        param_display(w, idx, (char *)p);
        return 1;
    }
    case effSetSampleRate: if (o > 0) w->sr = o; return 1;
    case effSetBlockSize: case effMainsChanged: return 1;
    case effProcessEvents: {
        VstEvents *ev = (VstEvents *)p;
        std::lock_guard<std::mutex> lk(w->mx);
        for (int i = 0; ev && i < ev->numEvents && w->in_n < 64; i++) {
            if (ev->events[i]->type != 1) continue;
            std::memcpy(w->inq[w->in_n++], ((VstMidiEvent *)ev->events[i])->midiData, 3);
        }
        return 1;
    }
    case effCanDo: {
        const char *s = (const char *)p;
        return (!std::strcmp(s, "receiveVstTimeInfo") || !std::strcmp(s, "receiveVstEvents") ||
                !std::strcmp(s, "receiveVstMidiEvent")) ? 1 : -1;
    }
    case effGetChunk: {
        std::lock_guard<std::mutex> lk(w->mx);
        copy_str(w->chunk, state_string(w, true).c_str(), sizeof w->chunk);
        *(void **)p = w->chunk;
        return (intptr_t)std::strlen(w->chunk) + 1;
    }
    case effSetChunk: {
        if (v <= 0 || (size_t)v > sizeof w->chunk) return 0;
        std::lock_guard<std::mutex> lk(w->mx);
        std::string s((const char *)p, (size_t)v);
        apply_state(w, s.c_str(), true);
        return 1;
    }
    default: return 0;
    }
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    if (!g_log) g_log = std::fopen("/tmp/drumgen_vst.log", "a");
    std::call_once(g_idx_once, init_indices);
    std::call_once(g_src_once, load_sources);
    Plugin *w = new Plugin();
    w->master = master;
    w->rng ^= (uint32_t)time(nullptr) ^ (uint32_t)(uintptr_t)w;
    if (!w->rng) w->rng = 1;
    for (int i = 0; i < NPARAMS; i++) w->cache[i].store(PARAMS[i].def);
    for (int l = 0; l < NLANES; l++) { w->off_at[l] = -1; w->pat_txt[l][0] = 0; }
    {
        std::lock_guard<std::mutex> lk(w->mx);
        int t = find_tpl("TECHNO");
        setv(w, P_STYLE, t >= 0 ? t : 0);
        apply_style(w);
    }
    alsa_open(w);

    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450; /* 'VstP' */
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
#ifdef PLUG_EFFECT
    e->numInputs = 2;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsProgramChunks;
#else
    e->numInputs = 0;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsIsSynth | effFlagsProgramChunks;
#endif
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    LOG("[drumgen_vst] " PLUG_NAME " up (" BUILD_ID ", " PLUG_MODE "), %d params, %d templates\n", NPARAMS, g_ntpl);
    return e;
}
