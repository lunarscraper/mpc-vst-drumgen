#!/usr/bin/env python3
"""Writes params.json and layout.conf for DrumGen (run by hand after a change; both files are committed).
The parameter ORDER is the VST index: append new parameters at the end, never reorder (saved projects
and Q-Link maps store values by index)."""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
LANES = ["Kick", "Snare", "Hat", "Perc", "Clap", "Open Hat", "Tom", "Ride"]
NOTES = [36, 38, 42, 37, 39, 46, 45, 51]
SOURCES = ["NGEN", "HOUSE", "TECHNO", "GARAGE", "DNB", "HIPHOP", "TRAP", "FUNK", "SOUL", "JAZZ",
           "LATIN", "REGGAE", "ROCK", "AFRO", "WORLD", "BASIS"]
AUTO = ["OFF", "VAR 1", "VAR 2", "VAR 4", "VAR 8", "GEN 4", "GEN 8", "GEN 16"]

params = [
    {"key": "style", "name": "Style", "min": 0, "max": 15, "default": 0, "display": "string"},
    {"key": "generate", "name": "Generate", "momentary": True},
    {"key": "variate", "name": "Variate", "momentary": True},
    {"key": "random", "name": "Random", "min": 0, "max": 100, "default": 0, "unit": "%", "display": "int"},
    {"key": "swing", "name": "Swing", "min": 50, "max": 75, "default": 50, "unit": "%", "display": "int"},
    {"key": "gate", "name": "Gate", "min": 5, "max": 95, "default": 50, "unit": "%", "display": "int"},
    {"key": "auto", "name": "Auto", "options": AUTO, "default": "OFF"},
    {"key": "preset", "name": "Preset", "min": 1, "max": 32, "default": 1, "display": "int"},
    {"key": "preset_load", "name": "Load", "momentary": True},
    {"key": "preset_save", "name": "Save", "momentary": True},
    {"key": "cc_ch", "name": "Control Ch", "options": ["OFF"] + [str(c) for c in range(1, 17)], "default": "16"},
    {"key": "status", "name": "Status", "min": 0, "max": 1, "display": "string", "type": "readout"},
]
for l in range(8):
    n = l + 1
    params += [
        {"key": "l%d_on" % n, "name": "On", "options": ["OFF", "ON"], "default": "ON"},
        {"key": "l%d_src" % n, "name": "Source", "options": SOURCES, "default": "NGEN" if l < 4 else "BASIS"},
        {"key": "l%d_item" % n, "name": "Pattern", "min": 0, "max": 63, "default": l if l < 4 else 0, "display": "string"},
        {"key": "l%d_dens" % n, "name": "Density", "min": 0, "max": 200, "default": 100, "unit": "%", "display": "int"},
        {"key": "l%d_len" % n, "name": "Length", "min": 1, "max": 32, "default": 32 if l < 4 else 16, "display": "int"},
        {"key": "l%d_note" % n, "name": "Note", "min": 0, "max": 127, "default": NOTES[l], "display": "int"},
        {"key": "l%d_ch" % n, "name": "Channel", "min": 1, "max": 16, "default": 10, "display": "int"},
        {"key": "l%d_lock" % n, "name": "Lock", "options": ["OFF", "ON"], "default": "OFF"},
        {"key": "l%d_dice" % n, "name": "Dice", "momentary": True},
        {"key": "l%d_pat" % n, "name": "Steps", "min": 0, "max": 1, "display": "string", "type": "readout"},
    ]
# ---- MIDI FX (appended in 1.1.0; keep after the lanes: VST index order) ----
FX_TARGETS = ["ALL", "1 KICK", "2 SNARE", "3 HAT", "4 PERC", "5 CLAP", "6 OPEN HAT", "7 TOM", "8 RIDE",
              "1-4", "5-8", "ALL BUT 1", "2+5", "3+6"]
params += [
    {"key": "rmx_on", "name": "Remix", "options": ["OFF", "ON"], "default": "OFF"},
    {"key": "rmx_mode", "name": "Mode", "options": ["NORMAL", "BREAK", "ROLL", "FILL"], "default": "NORMAL"},
    {"key": "rmx_type", "name": "Type", "min": 1, "max": 16, "default": 1, "display": "int"},
    {"key": "rmx_every", "name": "Every", "options": ["1 BAR", "2 BARS", "4 BARS", "8 BARS"], "default": "4 BARS"},
    {"key": "rmx_target", "name": "Target", "options": FX_TARGETS, "default": "ALL"},
    {"key": "echo_on", "name": "Echo", "options": ["OFF", "ON"], "default": "OFF"},
    {"key": "echo_time", "name": "Time", "options": ["1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D", "1/4"],
     "default": "1/8D"},
    {"key": "echo_rep", "name": "Repeats", "min": 1, "max": 8, "default": 3, "display": "int"},
    {"key": "echo_prob", "name": "Probability", "min": 0, "max": 100, "default": 100, "unit": "%", "display": "int"},
    {"key": "echo_fall", "name": "Falloff", "min": 0, "max": 100, "default": 40, "unit": "%", "display": "int"},
    {"key": "echo_target", "name": "Target", "options": FX_TARGETS, "default": "2+5"},
    {"key": "gl_on", "name": "Glitch", "options": ["OFF", "ON"], "default": "OFF"},
    {"key": "gl_rep", "name": "Repeats", "min": 2, "max": 8, "default": 4, "display": "int"},
    {"key": "gl_gate", "name": "Gate", "min": 10, "max": 100, "default": 50, "unit": "%", "display": "int"},
    {"key": "gl_prob", "name": "Probability", "min": 0, "max": 100, "default": 25, "unit": "%", "display": "int"},
    {"key": "gl_rnd", "name": "Random", "min": 0, "max": 100, "default": 50, "unit": "%", "display": "int"},
    {"key": "gl_target", "name": "Target", "options": FX_TARGETS, "default": "3+6"},
]
json.dump({"name": "DrumGen", "params": params}, open(os.path.join(HERE, "params.json"), "w"), indent=1)

# ---- skin layout (shadow_page.conf syntax, see mpc-vst-plugins docs/SKIN_STUDIO.md) ----
# Palette: graphite chassis, amber OLED ink like the NGEN's display, one warm accent.
out = ["""# DrumGen MPC skin layout. Tabs: MAIN (style, generate/variate, feel, all 8 lanes' steps as text),
# LANES 1-4 / LANES 5-8 (source, pattern, density, length, note, channel, lock, dice per lane), PRESETS.
# Q-Links carry knobs only (no switches): MAIN = style, random, swing, gate + the 8 densities;
# lane tabs = pattern, density, length, note of four lanes. Generated by gen_layout.py.
style=td3
theme_bg=1b1e22
theme_panel=1b1e22
theme_line=07080a
theme_ink=e8e4da
theme_ink_dim=9a9890
theme_ink_faint=5c5a55
theme_accent=ffb547
theme_accent_hi=ffd08a
theme_knob_face=2b2f35
theme_knob_ring=050506
theme_bar=ffb547
theme_seg_active=ffb547
theme_seg_inactive=2b2f35
theme_seg_active_tx=1b1408
theme_btn_text=1b1408
theme_well=07080a
theme_knob_off=474b52
theme_tab_on=ffb547
theme_tab_on_tx=1b1408
theme_lcd=07080a
theme_box=24282d
theme_btn_bg=e8e4da
theme_chrome_ink=e8e4da
theme_go_on=7ad34d
theme_go_off=ff6b4a
theme_tabs=ffb547
"""]
out += ["[tab MAIN]",
        'frame x=36 y=88 w=1207 h=232 title="-" caption="DRUMGEN"',
        'knob cx=120 cy=200 r=44 label="STYLE" key=style',
        'button cx=300 cy=168 label="GENERATE" key=generate',
        'button cx=300 cy=236 label="VARIATE" key=variate',
        'knob cx=470 cy=200 r=40 label="RANDOM" key=random',
        'knob cx=600 cy=200 r=40 label="SWING" key=swing',
        'knob cx=730 cy=200 r=40 label="GATE" key=gate',
        'popup cx=930 cy=168 w=200 h=48 label="-" caption="AUTO" key=auto',
        'readout cx=1000 cy=240 w=440 h=40 label="-" caption="STATUS" key=status',
        'frame x=36 y=330 w=1207 h=382 title="-" caption="STEPS"']
for l in range(8):   # two columns of four: lanes 1-4 left, 5-8 right; the label sits above each line
    out.append('readout cx=%d cy=%d w=560 h=40 label="-" caption="%d %s" key=l%d_pat' % (340 if l < 4 else 940, 420 + (l % 4) * 80,
                                                                         l + 1, LANES[l].upper(), l + 1))
out.append('qlinks "MAIN" = style,random,swing,gate,' + ",".join("l%d_dens" % (l + 1) for l in range(8)))

for t, lanes in (("LANES 1-4", range(0, 4)), ("LANES 5-8", range(4, 8))):
    out += ["[tab %s]" % t, 'frame x=36 y=88 w=1207 h=624 title="-" caption="%s"' % t]
    for k, l in enumerate(lanes):
        n = l + 1
        cy = 172 + k * 145   # one row per lane; the lane's name is its ON switch's label
        out += ['toggle cx=110 cy=%d label="%d %s" key=l%d_on' % (cy, n, LANES[l].upper(), n),
                'popup cx=290 cy=%d w=190 h=44 label="-" caption="SOURCE" key=l%d_src' % (cy, n),
                'knob cx=480 cy=%d r=32 label="PATTERN" key=l%d_item' % (cy, n),
                'knob cx=610 cy=%d r=32 label="DENSITY" key=l%d_dens' % (cy, n),
                'knob cx=730 cy=%d r=32 label="LENGTH" key=l%d_len' % (cy, n),
                'knob cx=850 cy=%d r=32 label="NOTE" key=l%d_note' % (cy, n),
                'knob cx=970 cy=%d r=32 label="CHANNEL" key=l%d_ch' % (cy, n),
                'toggle cx=1080 cy=%d label="LOCK" key=l%d_lock' % (cy, n),
                'button cx=1175 cy=%d label="DICE" key=l%d_dice' % (cy, n)]
    out.append('qlinks "%s" = %s' % (t.replace("LANES", "LN"), ",".join(
        "l%d_item,l%d_dens,l%d_len,l%d_note" % ((l + 1,) * 4) for l in lanes)))

# FX tab: three columns REMIX / ECHO / GLITCH, each ON + TARGET on top, 2x2 knobs below
out.append("[tab FX]")
for k, (title, key_on, key_t, knobs) in enumerate((
        ("REMIX", "rmx_on", "rmx_target", [("MODE", "rmx_mode"), ("TYPE", "rmx_type"), ("EVERY", "rmx_every")]),
        ("ECHO", "echo_on", "echo_target", [("TIME", "echo_time"), ("REPEATS", "echo_rep"), ("PROBABILITY", "echo_prob"),
                                            ("FALLOFF", "echo_fall")]),
        ("GLITCH", "gl_on", "gl_target", [("REPEATS", "gl_rep"), ("GATE", "gl_gate"), ("PROBABILITY", "gl_prob"),
                                          ("RANDOM", "gl_rnd")]))):
    x = 36 + k * 406
    out += ['frame x=%d y=88 w=395 h=624 title="-" caption="%s"' % (x, title),
            'toggle cx=%d cy=190 label="ON" key=%s' % (x + 70, key_on),
            'popup cx=%d cy=190 w=220 h=48 label="-" caption="TARGET" key=%s' % (x + 255, key_t)]
    for j, (lab, key) in enumerate(knobs):
        out.append('knob cx=%d cy=%d r=44 label="%s" key=%s' % (x + 110 + (j % 2) * 175, 360 + (j // 2) * 190, lab, key))
out.append('qlinks "FX" = rmx_mode,rmx_type,rmx_every,echo_time,echo_rep,echo_prob,echo_fall,gl_rep,gl_gate,gl_prob,gl_rnd')
out += ["[tab PRESETS]",
        'frame x=36 y=88 w=1207 h=624 title="-" caption="PRESETS"',
        'knob cx=200 cy=260 r=54 label="PRESET" key=preset',
        'button cx=420 cy=260 label="LOAD" key=preset_load',
        'button cx=580 cy=260 label="SAVE" key=preset_save',
        'knob cx=1040 cy=260 r=54 label="CONTROL CH" key=cc_ch',
        'readout cx=640 cy=480 w=900 h=40 label="-" caption="STATUS" key=status',
        'qlinks "PRESETS" = preset,cc_ch']
open(os.path.join(HERE, "layout.conf"), "w").write("\n".join(out) + "\n")
print("params.json: %d params, layout.conf written" % len(params))
