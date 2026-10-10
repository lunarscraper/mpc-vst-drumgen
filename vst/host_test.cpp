/* Offline x86 test of drumgen_vst.cpp (built -DNO_ALSA by test.sh): instances, audio path of both
 * variants, NGEN templates from DRUMGEN_DIR, style -> lane sources, density is monotonic (no reshuffle
 * while turning), lock survives Generate, played notes match the shown steps exactly, swing delays odd
 * steps, stop leaves no hanging notes, MIDI CC control, chunk and preset round trips.
 * Usage: host_test <plugin.so> [fixture|dir|none]
 *   fixture (default): writes 11 own test templates (factory file names, simple made-up patterns) to
 *                      /tmp/drumgen_fixture -- the NGEN factory templates are not in this repo
 *   dir:               uses DRUMGEN_DIR as set (e.g. the real NGEN factory templates)
 *   none:              no template folder at all */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/stat.h>

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
typedef struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; unsigned char midiData[4];
                 char detune, noteOffVelocity, reserved1, reserved2; } VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; void *events[2]; } VstEvents;
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;

static VstTimeInfo ti;
static intptr_t master(AEffect *, int32_t op, int32_t, intptr_t, void *, float) { return op == 7 ? (intptr_t)&ti : 0; }
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static int find(AEffect *e, const char *name, int nth = 0) {
    char buf[64];
    for (int i = 0; i < e->numParams; i++) { e->dispatcher(e, 8, i, 0, buf, 0); if (!std::strcmp(buf, name) && nth-- == 0) return i; }
    return -1;
}
static std::string disp(AEffect *e, int i) { char b[64] = {0}; e->dispatcher(e, 7, i, 0, b, 0); return b; }
static void set_int(AEffect *e, int i, int v, int lo, int hi) { e->setParameter(e, i, (float)(v - lo) / (hi - lo)); }
static int hits(const std::string &pat) { int n = 0; for (char c : pat) n += (c == 'X' || c == 'x' || c == '-'); return n; }
static std::string steps_only(const std::string &pat) { std::string s; for (char c : pat) if (c != ' ') s += c; return s; }

struct Ev { long block; int st, n, v; };
/* blocks of 128 frames at 120 BPM / 44.1 kHz from ppq 0; collects the MIDI trace with block numbers */
static std::vector<Ev> play(AEffect *e, int blocks, bool stop_at_end = true) {
    float l[128], r[128]; float *o[2] = {l, r};
    float il[128], ir[128]; float *iv[2] = {il, ir};
    for (int i = 0; i < 128; i++) { il[i] = 0.25f; ir[i] = -0.5f; }
    float **in = e->numInputs ? iv : nullptr;
    std::vector<Ev> out;
    fflush(stdout);
    char *buf = nullptr; size_t len = 0;
    FILE *mem = open_memstream(&buf, &len);
    FILE *old = stdout; stdout = mem;
    std::vector<size_t> marks;
    ti.flags = (1 << 1) | (1 << 9) | (1 << 10); ti.tempo = 120; ti.ppqPos = 0; ti.sampleRate = 44100;
    for (int b = 0; b < blocks; b++) {
        e->processReplacing(e, in, o, 128);
        ti.ppqPos += 128 * (120.0 / 60.0) / 44100.0;
        fflush(mem); marks.push_back(len);
    }
    if (stop_at_end) { ti.flags = (1 << 9) | (1 << 10); e->processReplacing(e, in, o, 128); fflush(mem); marks.push_back(len); }
    stdout = old; fclose(mem);
    size_t pos = 0;
    for (size_t b = 0; b < marks.size(); b++) {
        while (pos < marks[b]) {
            unsigned st, n, v;
            if (std::sscanf(buf + pos, "MIDI %x %x %x", &st, &n, &v) == 3) out.push_back({(long)b, (int)st, (int)n, (int)v});
            const char *nl = (const char *)std::memchr(buf + pos, '\n', len - pos);
            pos = nl ? (size_t)(nl - buf) + 1 : len;
        }
    }
    free(buf);
    return out;
}
static void cc(AEffect *a, int ch, int num, int val) {
    VstMidiEvent m; std::memset(&m, 0, sizeof m);
    m.type = 1; m.byteSize = sizeof m;
    m.midiData[0] = (unsigned char)(0xB0 | (ch - 1)); m.midiData[1] = (unsigned char)num; m.midiData[2] = (unsigned char)val;
    VstEvents ev; ev.numEvents = 1; ev.reserved = 0; ev.events[0] = &m; ev.events[1] = nullptr;
    a->dispatcher(a, 25, 0, 0, &ev, 0);
    float l[128], r[128], il[128] = {0}, ir[128] = {0}; float *o[2] = {l, r}, *iv[2] = {il, ir};
    ti.flags = (1 << 9) | (1 << 10);
    a->processReplacing(a, a->numInputs ? iv : nullptr, o, 128);
}

static void write_fixture(const char *dir) {
    static const char *const NAMES[] = {"BOOMBAP", "BOSSA", "BREAKS", "DNB", "ELECTRO", "FUNK_BR", "GARAGE",
                                        "HOUSE", "JUNGLE", "MEMPHIS", "TECHNO"};
    mkdir(dir, 0755);
    for (const char *nm : NAMES) {
        unsigned char b[192] = {0};
        auto put = [&](int part, int layer, int step, int w) {
            unsigned char &x = b[(part * 3 + layer) * 16 + step / 2];
            x = (step % 2 == 0) ? (unsigned char)((x & 0xF0) | w) : (unsigned char)((x & 0x0F) | (w << 4));
        };
        for (int s = 0; s < 32; s++) {
            if (s % 4 == 0) put(0, 0, s, 10);   /* kick: four on the floor + a few 80s */
            if (s % 4 == 2) put(0, 1, s, 3);
            if (s % 8 == 4) put(1, 0, s, 10);   /* snare: backbeat */
            put(2, 1, s, 5);                    /* hat: every step at 50 % */
            if (s % 3 == 0) put(3, 1, s, 4);    /* perc */
        }
        std::string path = std::string(dir) + "/" + nm + ".hex";
        FILE *f = std::fopen(path.c_str(), "wb");
        if (f) { std::fwrite(b, 1, sizeof b, f); std::fclose(f); }
    }
}

int main(int argc, char **argv) {
    setenv("DRUMGEN_TRACE", "1", 1);
    const char *mode = argc > 2 ? argv[2] : "fixture";
    bool with_tpl = std::strcmp(mode, "none") != 0;
    if (!std::strcmp(mode, "fixture")) { write_fixture("/tmp/drumgen_fixture"); setenv("DRUMGEN_DIR", "/tmp/drumgen_fixture", 1); }
    if (!std::strcmp(mode, "none")) setenv("DRUMGEN_DIR", "/nonexistent-drumgen", 1);
    if (!std::getenv("DRUMGEN_BANK")) { unlink("/tmp/drumgen_test_bank.txt"); setenv("DRUMGEN_BANK", "/tmp/drumgen_test_bank.txt", 1); }
    void *h = dlopen(argc > 1 ? argv[1] : "./build/x86/drumgen-x86.so", RTLD_NOW);
    if (!h) { std::printf("dlopen: %s\nFAILED\n", dlerror()); return 1; }
    auto entry = (AEffect *(*)(audioMasterCallback))dlsym(h, "VSTPluginMain");
    AEffect *a = entry(master), *b = entry(master);
    CHECK(a && b && a->magic == 0x56737450, "instances");
    std::printf("params: %d, inputs: %d, category: %d, templates: %s\n", a->numParams, a->numInputs,
                (int)a->dispatcher(a, 35, 0, 0, nullptr, 0), with_tpl ? "yes" : "none");
    {   /* audio path */
        float il[128], ir[128], ol[128], orr[128];
        for (int i = 0; i < 128; i++) { il[i] = i / 128.0f; ir[i] = -i / 256.0f; ol[i] = orr[i] = 9.0f; }
        float *iv[2] = {il, ir}, *ov[2] = {ol, orr};
        a->processReplacing(a, a->numInputs ? iv : nullptr, ov, 128);
        if (a->numInputs) {
            CHECK(a->numInputs == 2 && !(a->flags & (1 << 8)) && a->dispatcher(a, 35, 0, 0, nullptr, 0) == 1, "effect: category Effect");
            CHECK(!std::memcmp(il, ol, sizeof il) && !std::memcmp(ir, orr, sizeof ir), "effect passes audio through");
            float keep[128]; std::memcpy(keep, il, sizeof il);
            a->processReplacing(a, iv, iv, 128);
            CHECK(!std::memcmp(keep, il, sizeof il), "effect in-place pass-through");
        } else {
            CHECK((a->flags & (1 << 8)) && a->dispatcher(a, 35, 0, 0, nullptr, 0) == 2, "instrument: synth flag");
            CHECK(ol[5] == 0.0f && orr[5] == 0.0f, "instrument writes silence");
        }
    }
    int style = find(a, "Style"), gen = find(a, "Generate"), var = find(a, "Variate"), swing = find(a, "Swing"),
        status = find(a, "Status"), cch = find(a, "Control Ch"), preset = find(a, "Preset"),
        save = find(a, "Save"), load = find(a, "Load"), rnd = find(a, "Random");
    int pat[8], dens[8], len[8], item[8], src[8], lock[8], note[8], on[8];
    for (int l = 0; l < 8; l++) {
        pat[l] = find(a, "Steps", l); dens[l] = find(a, "Density", l); len[l] = find(a, "Length", l);
        item[l] = find(a, "Pattern", l); src[l] = find(a, "Source", l); lock[l] = find(a, "Lock", l);
        note[l] = find(a, "Note", l); on[l] = find(a, "On", l);
        CHECK(pat[l] >= 0 && dens[l] >= 0 && len[l] >= 0 && item[l] >= 0 && src[l] >= 0 && lock[l] >= 0, "lane %d params", l + 1);
    }
    CHECK(style >= 0 && gen >= 0 && var >= 0 && swing >= 0 && status >= 0 && cch >= 0 && preset >= 0 && rnd >= 0, "global params");

    if (!with_tpl) {   /* no template folder: no crash, readable hints, groove lanes still play */
        CHECK(disp(a, style) == "no templates", "style shows hint: %s", disp(a, style).c_str());
        CHECK(disp(a, status).find("No templates") == 0, "status hint: %s", disp(a, status).c_str());
        CHECK(hits(disp(a, pat[0])) == 0, "lane 1 empty without templates");
        CHECK(hits(disp(a, pat[4])) > 0, "lane 5 (basis) has hits: %s", disp(a, pat[4]).c_str());
        std::vector<Ev> ev = play(a, 200);
        int ons = 0; for (auto &x : ev) ons += ((x.st & 0xF0) == 0x90 && x.v);
        CHECK(ons > 0, "notes without templates %d", ons);
        a->dispatcher(a, 1, 0, 0, nullptr, 0); b->dispatcher(b, 1, 0, 0, nullptr, 0);
        std::printf(fails ? "FAILED\n" : "PASSED\n");
        return fails ? 1 : 0;
    }

    CHECK(disp(a, style) == "TECHNO", "default style TECHNO: %s", disp(a, style).c_str());
    CHECK(disp(a, item[0]) == "TECHNO Kick" && disp(a, item[2]) == "TECHNO Hat", "lanes 1-4 from the template: %s / %s",
          disp(a, item[0]).c_str(), disp(a, item[2]).c_str());
    CHECK(disp(a, src[5]) == "TECHNO" && disp(a, item[5]) == "Driving", "lane 6 groove: %s %s", disp(a, src[5]).c_str(), disp(a, item[5]).c_str());
    CHECK(disp(a, src[4]) == "BASIS" && disp(a, item[4]) == "Backbeat", "lane 5 basis: %s", disp(a, item[4]).c_str());
    CHECK(disp(a, len[0]) == "32" && disp(a, len[5]) == "16", "lengths %s %s", disp(a, len[0]).c_str(), disp(a, len[5]).c_str());
    std::printf("TECHNO kick: %s\n", disp(a, pat[0]).c_str());
    std::string k = steps_only(disp(a, pat[0]));
    CHECK(k.size() == 32 && k[0] == 'X' && k[4] == 'X' && k[8] == 'X' && k[12] == 'X', "techno kick has the four on the floor: %s", k.c_str());

    /* style change: BOSSA (index of BOSSA in the sorted factory list = 1) */
    set_int(a, style, 1, 0, 15);
    CHECK(disp(a, style) == "BOSSA" && disp(a, item[0]) == "BOSSA Kick", "style BOSSA: %s / %s", disp(a, style).c_str(), disp(a, item[0]).c_str());
    CHECK(disp(a, item[4]) == "Bossa Nova" && disp(a, src[4]) == "LATIN", "BOSSA lane 5: %s %s", disp(a, src[4]).c_str(), disp(a, item[4]).c_str());
    set_int(a, style, 10, 0, 15);   /* TECHNO again */

    /* density is monotonic: same seeds, more density -> superset of hits */
    {
        std::string lo, mid, hi;
        set_int(a, dens[2], 40, 0, 200); lo = steps_only(disp(a, pat[2]));
        set_int(a, dens[2], 100, 0, 200); mid = steps_only(disp(a, pat[2]));
        set_int(a, dens[2], 200, 0, 200); hi = steps_only(disp(a, pat[2]));
        bool sub = true;
        for (size_t i = 0; i < lo.size() && i < mid.size() && i < hi.size(); i++) {
            if (lo[i] != '.' && mid[i] == '.') sub = false;
            if (mid[i] != '.' && hi[i] == '.') sub = false;
        }
        std::printf("hat 40%%: %d hits, 100%%: %d, 200%%: %d\n", hits(lo), hits(mid), hits(hi));
        CHECK(sub && hits(lo) <= hits(mid) && hits(mid) <= hits(hi) && hits(hi) > hits(lo), "density monotonic");
        set_int(a, dens[2], 0, 0, 200);
        CHECK(hits(disp(a, pat[2])) == 0, "density 0 -> silent lane");
        set_int(a, dens[2], 100, 0, 200);
        CHECK(steps_only(disp(a, pat[2])) == mid, "density back to 100 restores the same pattern");
    }
    /* lock survives Generate; unlocked lanes change */
    {
        a->setParameter(a, lock[2], 1.0f);
        std::string keep = disp(a, pat[2]), before[8];
        for (int l = 0; l < 8; l++) before[l] = disp(a, pat[l]);
        int changed = 0;
        for (int t = 0; t < 5; t++) { a->setParameter(a, gen, 1.0f); for (int l = 0; l < 8; l++) if (l != 2 && disp(a, pat[l]) != before[l]) changed++; }
        CHECK(disp(a, pat[2]) == keep, "locked lane unchanged");
        CHECK(changed > 0, "generate changes unlocked lanes");
        set_int(a, style, 7, 0, 15);   /* HOUSE: the locked lane keeps its TECHNO source */
        CHECK(disp(a, item[2]) == "TECHNO Hat" && disp(a, item[0]) == "HOUSE Kick", "lock keeps source on style change: %s", disp(a, item[2]).c_str());
        a->setParameter(a, lock[2], 0.0f);
        a->setParameter(a, var, 1.0f);
    }
    /* played notes == shown steps: 2 bars at 120 BPM = 4 s = 1378 blocks of 128 */
    {
        std::string pats[8]; int lens[8];
        for (int l = 0; l < 8; l++) { pats[l] = steps_only(disp(a, pat[l])); lens[l] = std::atoi(disp(a, len[l]).c_str()); }
        std::vector<Ev> ev = play(a, 1378);   /* 176400 frames = ppq 7.9997: exactly steps 0..31 */
        std::map<int, int> count, held; int hanging = 0;
        for (auto &x : ev) {
            int key = (x.st & 15) * 128 + x.n;
            if ((x.st & 0xF0) == 0x90 && x.v) { count[x.n]++; held[key]++; }
            else held[key] = 0;
        }
        for (auto &kv : held) if (kv.second > 0) hanging++;
        int total = 0;
        for (int l = 0; l < 8; l++) {
            int want = 0;
            for (int s = 0; s < 32; s++) want += pats[l][s % lens[l]] != '.';
            int n = std::atoi(disp(a, note[l]).c_str());
            total += want;
            CHECK(count[n] == want, "lane %d (note %d): played %d, shown %d", l + 1, n, count[n], want);
        }
        std::printf("2 bars: %zu MIDI events, %d note-ons expected, hanging after stop %d\n", ev.size(), total, hanging);
        CHECK(hanging == 0, "hanging notes %d", hanging);
        CHECK(disp(a, status).find("stopped") != std::string::npos, "status after stop: %s", disp(a, status).c_str());
    }
    /* swing 75 %: an odd step comes half a step (= 1/32 note = 0.125 beat = 86 blocks/... ) later */
    {
        for (int l = 1; l < 8; l++) a->setParameter(a, on[l], 0.0f);
        a->setParameter(a, src[0], 15.0f / 15.0f);         /* BASIS */
        set_int(a, item[0], 3, 0, 63);                      /* 16th Shaker: every step hits */
        set_int(a, dens[0], 200, 0, 200);
        std::vector<Ev> straight = play(a, 200);
        set_int(a, swing, 75, 50, 75);
        std::vector<Ev> swung = play(a, 200);
        auto on_blocks = [](const std::vector<Ev> &v) { std::vector<long> r; for (auto &x : v) if ((x.st & 0xF0) == 0x90 && x.v) r.push_back(x.block); return r; };
        std::vector<long> s0 = on_blocks(straight), s1 = on_blocks(swung);
        /* a 16th at 120 BPM = 5512.5 frames = 43 blocks; 75 % swing delays odd steps by half of that */
        CHECK(s0.size() >= 4 && s1.size() >= 4, "shaker plays");
        if (s0.size() >= 4 && s1.size() >= 4) {
            std::printf("straight %ld %ld %ld | swung %ld %ld %ld\n", s0[0], s0[1], s0[2], s1[0], s1[1], s1[2]);
            CHECK(s1[0] == s0[0] && s1[2] == s0[2], "even steps stay on the grid");
            CHECK(s1[1] - s0[1] >= 20 && s1[1] - s0[1] <= 23, "odd step delayed ~21 blocks: %ld", s1[1] - s0[1]);
        }
        set_int(a, swing, 50, 50, 75);
        for (int l = 1; l < 8; l++) a->setParameter(a, on[l], 1.0f);
    }
    /* MIDI CC on Control Ch 16 (default): CC 13 = lane 1 density, CC 18 = dice, CC 100 = style */
    {
        CHECK(disp(a, cch) == "16", "control channel default 16: %s", disp(a, cch).c_str());
        cc(a, 16, 13, 0);
        CHECK(hits(disp(a, pat[0])) == 0, "CC 13 -> lane 1 density 0");
        cc(a, 1, 13, 127);
        CHECK(hits(disp(a, pat[0])) == 0, "CC on another channel ignored");
        cc(a, 16, 13, 64);
        CHECK(disp(a, dens[0]) == "101%", "CC 13 = 64 -> density %s", disp(a, dens[0]).c_str());
        a->setParameter(a, cch, 0.0f);
        cc(a, 16, 13, 0);
        CHECK(disp(a, dens[0]) == "101%", "Control Ch OFF ignores CCs");
        a->setParameter(a, cch, 1.0f);
        cc(a, 16, 100, 127);   /* style knob range is 16, the card has 11 templates: stops at the last */
        CHECK(disp(a, style) == "TECHNO", "CC 100 = 127 -> last style: %s", disp(a, style).c_str());
        cc(a, 16, 100, 0);
        CHECK(disp(a, style) == "BOOMBAP", "CC 100 = 0 -> first style: %s", disp(a, style).c_str());
        cc(a, 16, 100, 127);
    }
    /* chunk round trip: same parameters, same steps */
    {
        std::string pats[8];
        for (int l = 0; l < 8; l++) pats[l] = disp(a, pat[l]);
        void *ck = nullptr;
        intptr_t n = a->dispatcher(a, 23, 0, 0, &ck, 0);
        std::string chunk((char *)ck, n);
        CHECK(n > 500 && chunk.find("s8=") != std::string::npos, "chunk has seeds (%ld bytes)", (long)n);
        a->setParameter(a, gen, 1.0f);
        set_int(a, style, 2, 0, 15);
        a->dispatcher(a, 24, 0, n, (void *)chunk.data(), 0);
        bool same = true;
        for (int l = 0; l < 8; l++) same = same && disp(a, pat[l]) == pats[l];
        CHECK(same && disp(a, style) == "TECHNO", "chunk restores steps and style");
        /* a second instance takes the chunk too (project load) */
        b->dispatcher(b, 24, 0, n, (void *)chunk.data(), 0);
        CHECK(disp(b, pat[0]) == pats[0] && disp(b, pat[7]) == pats[7], "chunk into another instance");
    }
    /* presets: save slot 3, change, load slot 3 */
    {
        set_int(a, preset, 3, 1, 32);
        std::string p0 = disp(a, pat[0]), i5 = disp(a, item[5]);
        a->setParameter(a, save, 1.0f);
        a->setParameter(a, gen, 1.0f);
        set_int(a, style, 4, 0, 15);
        a->setParameter(a, load, 1.0f);
        CHECK(disp(a, pat[0]) == p0 && disp(a, item[5]) == i5, "preset 3 restores: %s / %s", disp(a, pat[0]).c_str(), disp(a, item[5]).c_str());
        CHECK(disp(a, preset) == "3", "preset number kept");
        usleep(300000);   /* the bank file is written off the audio thread */
        const char *bp = std::getenv("DRUMGEN_BANK");
        FILE *f = bp ? std::fopen(bp, "r") : nullptr;
        char line[64] = {0};
        CHECK(f && std::fgets(line, sizeof line, f) && !std::strncmp(line, "3:v=1;", 6), "bank file has slot 3: %s", line);
        if (f) std::fclose(f);
    }
    /* ---- MIDI FX ---- */
    if (!std::strcmp(mode, "fixture")) {   /* exact counts rely on the fixture's fixed weights */
        int rmx = find(a, "Remix"), rmode = find(a, "Mode"), rtype = find(a, "Type"), revery = find(a, "Every"),
            echo = find(a, "Echo"), etime = find(a, "Time"), erep = find(a, "Repeats", 0), eprob = find(a, "Probability", 0),
            efall = find(a, "Falloff"), glitch = find(a, "Glitch"), grep_ = find(a, "Repeats", 1), gprob = find(a, "Probability", 1),
            grnd = find(a, "Random", 1), rtgt = find(a, "Target", 0), etgt = find(a, "Target", 1), gtgt = find(a, "Target", 2);
        CHECK(rmx >= 0 && rmode >= 0 && rtype >= 0 && revery >= 0 && echo >= 0 && etime >= 0 && erep >= 0 && eprob >= 0 &&
              efall >= 0 && glitch >= 0 && grep_ >= 0 && gprob >= 0 && grnd >= 0 && rtgt >= 0 && etgt >= 0 && gtgt >= 0, "FX params");
        CHECK(disp(a, rmx) == "OFF" && disp(a, echo) == "OFF" && disp(a, glitch) == "OFF", "FX off by default");
        a->setParameter(a, cch, 0.0f);
        set_int(a, style, 10, 0, 15);                           /* TECHNO fixture, all lanes fresh */
        set_int(a, swing, 50, 50, 75);
        auto only = [&](int lane) { for (int l = 0; l < 8; l++) a->setParameter(a, on[l], l == lane ? 1.0f : 0.0f); };
        auto count = [&](const std::vector<Ev> &ev, int n, std::map<int, int> *vels = nullptr) {
            int c = 0; for (auto &x : ev) if ((x.st & 0xF0) == 0x90 && x.v && x.n == n) { c++; if (vels) (*vels)[x.v]++; } return c; };
        auto hanging = [&](const std::vector<Ev> &ev) {
            std::map<int, int> h; for (auto &x : ev) { int k = (x.st & 15) * 128 + x.n; if ((x.st & 0xF0) == 0x90 && x.v) h[k]++; else h[k] = 0; }
            int n = 0; for (auto &kv : h) n += kv.second > 0; return n; };
        auto opt = [&](int i, int o, int n) { a->setParameter(a, i, (float)o / (n - 1)); };
        /* ECHO: snare backbeat (steps 4, 12, 20, 28), 1/8 = 48 pulses, 3 repeats, falloff 40 % */
        only(1);
        opt(echo, 1, 2); opt(etgt, 2, 14); opt(etime, 5, 9); set_int(a, erep, 3, 1, 8); set_int(a, eprob, 100, 0, 100);
        set_int(a, efall, 40, 0, 100);
        std::map<int, int> vel;
        std::vector<Ev> ev = play(a, 1378);
        int n38 = count(ev, 38, &vel);
        std::printf("echo: %d snare note-ons in 2 bars (vel 127 x%d, 76 x%d, 46 x%d, 27 x%d)\n", n38, vel[127], vel[76], vel[46], vel[27]);
        CHECK(n38 == 14 && vel[127] == 4 && vel[76] == 4 && vel[46] == 3 && vel[27] == 3, "echo count/falloff: %d", n38);
        CHECK(hanging(ev) == 0, "echo leaves no hanging notes");
        opt(etgt, 1, 14);                                       /* target KICK: snare dry again */
        CHECK(count(play(a, 1378), 38) == 4, "echo target excludes snare");
        opt(echo, 0, 2);
        /* GLITCH: hat at density 200 hits every step; prob 100, 4 ratchets, random 0 -> 4 per step */
        only(2);
        set_int(a, dens[2], 200, 0, 200);
        opt(glitch, 1, 2); opt(gtgt, 3, 14); set_int(a, grep_, 4, 2, 8); set_int(a, gprob, 100, 0, 100); set_int(a, grnd, 0, 0, 100);
        ev = play(a, 1378);
        std::printf("glitch: %d hat note-ons in 2 bars\n", count(ev, 42));
        CHECK(count(ev, 42) == 128, "glitch 4 ratchets per step: %d", count(ev, 42));
        CHECK(hanging(ev) == 0, "glitch leaves no hanging notes");
        set_int(a, gprob, 0, 0, 100);
        CHECK(count(play(a, 1378), 42) == 32, "glitch prob 0 -> plain hats");
        opt(glitch, 0, 2);
        /* REMIX: hat on every step; every 2 bars -> bar 1 untouched, bar 2 remixed */
        opt(rmx, 1, 2); opt(rtgt, 0, 14); opt(revery, 1, 4);
        opt(rmode, 1, 4); set_int(a, rtype, 16, 1, 16);         /* BREAK 16: cuts */
        ev = play(a, 1378);
        int c1 = 0, c2 = 0;
        for (auto &x : ev) if ((x.st & 0xF0) == 0x90 && x.v && x.n == 42) (x.block < 689 ? c1 : c2)++;
        std::printf("remix break: bar 1 %d, bar 2 %d hats\n", c1, c2);
        CHECK(c1 == 16 && c2 > 0 && c2 < 16, "break cuts only the remix bar: %d / %d", c1, c2);
        CHECK(count(play(a, 1378), 42) == c1 + c2, "remix is repeatable (same settings, same result)");
        opt(rmode, 2, 4);                                       /* ROLL 16: rolls add hits */
        ev = play(a, 1378);
        std::printf("remix roll: %d hats\n", count(ev, 42));
        CHECK(count(ev, 42) > 32 && hanging(ev) == 0, "roll adds hits: %d", count(ev, 42));
        opt(rmode, 3, 4); set_int(a, rtype, 1, 1, 16);          /* FILL 1: last beat rolls */
        ev = play(a, 1378);
        CHECK(count(ev, 42) == 32 - 4 + 4 * 2, "fill type 1 = last beat as 2-step roll: %d", count(ev, 42));
        opt(rmode, 0, 4); set_int(a, rtype, 16, 1, 16);         /* NORMAL: hats everywhere -> count unchanged */
        CHECK(count(play(a, 1378), 42) == 32, "normal repositions without adding or losing hits on a full lane");
        opt(rmx, 0, 2);
        /* FX settings are part of the project */
        opt(echo, 1, 2);
        void *ck = nullptr;
        intptr_t n = a->dispatcher(a, 23, 0, 0, &ck, 0);
        CHECK(n > 0 && std::strstr((char *)ck, "echo_on=1;") && std::strstr((char *)ck, "rmx_type=16;"), "chunk has FX settings");
        /* CC 95 echo on/off */
        a->setParameter(a, cch, 1.0f);
        cc(a, 16, 95, 0);
        CHECK(disp(a, echo) == "OFF", "CC 95 switches echo off");
        for (int l = 0; l < 8; l++) a->setParameter(a, on[l], 1.0f);
    }
    a->dispatcher(a, 1, 0, 0, nullptr, 0);
    b->dispatcher(b, 1, 0, 0, nullptr, 0);
    std::printf(fails ? "FAILED\n" : "PASSED\n");
    return fails ? 1 : 0;
}
