# mpc-vst-drumgen

**DrumGen**: ein 8-Spur-Drumgenerator nach dem Vorbild des DRUMGEN im Spektro Audio NGEN, als
VST2-Plugin für den Plugin-Host von MPC OS (Force, MPC Live/One/X/Key). Er erzeugt auf Knopfdruck
Drum-Sequenzen aus Stil-Templates und schickt sie über einen eigenen MIDI-Port an Drum-Tracks.

Aufbau, Build und Installation wie bei [mpc-vst-euclidier](https://github.com/lunarscraper/mpc-vst-euclidier)
(handgeschriebene VST2-Hülle, eigener ALSA-MIDI-Port, Steuerung per CC, Insert-Effekt-Variante).

## Zwei Varianten

| | Ordner | Datei | Typ | Workflow | ALSA-MIDI-Port |
|---|---|---|---|---|---|
| DrumGen | `vst/` | `drumgen.so` | Instrument (belegt einen der 8 Plugin-Slots) | VST release (draft) | `DrumGen` |
| DrumGen FX | `vst-fx/` | `drumgen_fx.so` | Insert-Effekt (belegt keinen Instrumenten-Slot) | VST release FX (draft) | `DrumGen FX` |

Beide entstehen aus derselben `vst/drumgen_vst.cpp`; `vst-fx/vst.json` setzt nur `"effect": true`.
Beide lassen sich nebeneinander installieren und teilen sich die Preset-Datei `drumgen_BANK.txt`.
Jede Instanz hat ihre eigene Engine; mehrere DrumGens im Projekt sind möglich.

## Wie der Generator arbeitet

Wie am NGEN sind die Templates **Wahrscheinlichkeiten**, keine festen Rhythmen: pro Step und
Velocity-Stufe (127 / 80 / 30) ein Gewicht 0–10. Jede Spur würfelt daraus ihr Pattern.

- **Style** wählt ein NGEN-Template: Spur 1–4 (Kick, Snare, Hat, Perc) bekommen dessen vier Parts,
  Spur 5–8 (Clap, Open Hat, Tom, Ride) passende Grooves aus der Groove Bank oder Parts anderer
  Templates (z. B. DNB → Ride aus JUNGLE). Gesperrte Spuren bleiben, wie sie sind.
- **Generate** würfelt alle nicht gesperrten Spuren neu, **Variate** nur etwa ein Viertel ihrer
  Steps, **Dice** eine einzelne Spur.
- **Density** (0–200 %) skaliert die Wahrscheinlichkeit wie am NGEN. Weil jeder Step seinen Würfelwurf
  behält, fügt Aufdrehen nur Schläge hinzu und Zudrehen nimmt nur welche weg: das Pattern springt beim
  Drehen nicht.
- **Random** verschiebt die Gewichte und streut Ghost-Notes ein, ebenfalls ohne neu zu würfeln.
- **Length** (1–32) pro Spur erlaubt Polymetrik, z. B. die 12- und 18-Step-Grooves der Groove Bank.
- **Swing** 50–75 % verzögert jeden zweiten 16tel, **Gate** ist die Notenlänge in % eines Steps.
- **Auto** würfelt live alle 1/2/4/8 Takte neu: `VAR n` = Variate, `GEN n` = Generate.
- Takt und Step kommen aus der MPC-Transportposition (96 Pulse pro Viertel), die Spuren sitzen also
  immer auf dem Raster, auch nach Loop oder Sprung.

### Quellen pro Spur (SOURCE + PATTERN)

| SOURCE | PATTERN-Regler wählt | Herkunft |
|---|---|---|
| `NGEN` | Template × Part (`TECHNO Kick`, `TECHNO Snare` …) | `.hex`-Dateien auf der Karte, siehe unten |
| `HOUSE` … `WORLD` (14 Genres) | einen der 97 Grooves des Genres | [mission-minnow/groovebank](https://github.com/mission-minnow/groovebank), MIT, im Plugin enthalten |
| `BASIS` | Offbeat 8th, Backbeat, Four Floor, 16th Shaker, Ride 8th, Son Clave 3-2 | im Plugin |

Ein Groove-Bank-Groove ist eine Rhythmuslinie mit bis zu zwei Varianten. Was in der Hauptlinie steht,
wird wahrscheinlich (Gewicht 8, +1 je Variante, die mitspielt), was nur in den Varianten steht, selten
(2 je Variante). `A`/`x`/`g` legen die Stufe 127/80/30 fest.

## NGEN-Templates auf die Force kopieren

Die NGEN-Ressourcen stehen unter keiner Lizenz, deshalb liegen die Templates **nicht** in diesem
Repo. DrumGen liest sie beim Start aus `/sdcard/vst/drumgen/`. Den Ordner legt das Plugin beim ersten
Laden selbst an (mit einer `LIESMICH.txt`).

1. Die 11 Dateien aus `NGEN-Resources/Factory Content/DRUMGEN/` auf den PC holen (BOOMBAP.HEX,
   BOSSA.hex, …, TECHNO.hex).
2. Im Terminal in den Ordner mit den Dateien wechseln und kopieren (IP der Force einsetzen):

   ```
   ssh root@192.168.x.y "mkdir -p /sdcard/vst/drumgen"
   scp *.hex *.HEX root@192.168.x.y:/sdcard/vst/drumgen/
   ```
3. MPC neu starten (oder DrumGen aus allen Tracks entfernen und neu einsetzen).

Eigene Templates aus dem NGEN DrumGen Template Editor funktionieren genauso: höchstens 16 Dateien,
192 Bytes, alphabetisch sortiert, der Dateiname ist der Style-Name. Ohne Templates laufen die
Groove-Bank- und Basis-Spuren trotzdem; STATUS zeigt dann `No templates in vst/drumgen`.

## Auf der Force einsetzen

1. **DrumGen FX** als Insert auf einen beliebigen Track legen (z. B. den Drum-Track selbst oder den
   Master). Er reicht das Audio unverändert durch.
2. Einmalig: Preferences → MIDI → beim Port `DrumGen FX` **Track** einschalten.
3. Beim Drum-Track als MIDI-Eingang `DrumGen FX` wählen, Kanal 10 oder All.
4. Play drücken. Standard-Noten sind General MIDI (36 Kick, 38 Snare, 42 Hat, 37 Perc, 39 Clap,
   46 Open Hat, 45 Tom, 51 Ride), alles auf Kanal 10. Note und Kanal sind pro Spur einstellbar,
   so lassen sich auch einzelne Spuren an andere Tracks oder Plugins schicken (z. B. an 8W8).

### Seiten und Q-Links

| Seite | Inhalt | Q-Links (nur Regler, keine Schalter) |
|---|---|---|
| MAIN | Style, Generate, Variate, Random, Swing, Gate, Auto, Status, alle 8 Patterns als Text | Style, Random, Swing, Gate, Density 1–8 |
| LANES 1-4 / 5-8 | pro Spur: On, Source, Pattern, Density, Length, Note, Channel, Lock, Dice | Pattern, Density, Length, Note der vier Spuren |
| FX | Remix, Echo, Glitch (siehe unten) | alle Effekt-Regler |
| PRESETS | Preset 1–32, Load, Save, Control Ch, Status | Preset, Control Ch |

Skin-Vorschau (ohne Live-Werte): `docs/skin-main.png`, `docs/skin-lanes.png`, `docs/skin-fx.png`, `docs/skin-presets.png`.

Die Pattern-Zeilen auf MAIN zeigen die Steps der jeweiligen Länge: `X` = 127, `x` = 80, `-` = 30,
`.` = Pause, in Vierergruppen. Ein Step-Raster zum Antippen gibt es nicht, weil Plugin-Skins in MPC OS
keine frei gezeichneten Elemente kennen (mpc-vst-plugins `docs/NOTES.md`).

## MIDI-Effekte (Seite FX)

Drei Effekte, die nur auf die **Ausgabe** wirken: die Patterns und die Zeilen auf MAIN bleiben
unverändert, ausschalten bringt den Groove genau zurück. Jeder Effekt hat einen eigenen **ON**-Schalter
und ein **TARGET**: alle Spuren, eine Spur, `1-4`, `5-8`, `ALL BUT 1` (alles außer Kick),
`2+5` (Snare + Clap) oder `3+6` (beide Hats).

**REMIX** nach Yamahas *Real Time Loop Remix* (RS7000, Motif/MOXF). Im jeweils letzten Takt von
EVERY (1/2/4/8 Takte) wird der Takt umgebaut, wie Slices eines Loops; alle Ziel-Spuren gleich:

| MODE | Wirkung |
|---|---|
| NORMAL | setzt Teile des Takts an andere Stellen (TYPE 1–4 Viertel, 5–8 Achtel, ab 9 Sechzehntel, ab 12 auch rückwärts) |
| BREAK | schneidet Teile heraus, Stop-and-go (der erste Schlag bleibt immer) |
| ROLL | macht einzelne Schläge zu Wirbeln mit Crescendo (2, 3 oder 4 Schläge pro 16tel) |
| FILL | baut das Taktende zum Fill um, zurück zum Anfang (TYPE 1–4 letzter Schlag … 13–16 ganzer Takt) |

TYPE 1–16 ist die Komplexität. Wie beim Yamaha ist das Ergebnis reproduzierbar: gleiche Einstellungen
ergeben immer denselben Remix. STATUS zeigt `REMIX`, solange ein Takt umgebaut wird. FILL mit
EVERY 4 oder 8 BARS ergibt einen automatischen Fill am Ende jeder Phrase.

**ECHO** nach NGEN *Echoes*: MIDI-Delay im Tempo (TIME 1/32 bis 1/4, auch triolisch und
punktiert), REPEATS 1–8, PROBABILITY (Anteil der Schläge mit Echo), FALLOFF (Velocity-Abnahme je
Wiederholung).

**GLITCH** nach NGEN *Glitch*: zufällige Ratchets. REPEATS (höchstens 2–8 Schläge pro Step),
GATE (Länge der Ratchets), PROBABILITY, RANDOM (zufällige Anzahl und Velocity).

Q-Links der Seite FX: alle Regler (Mode, Type, Every, Time, Repeats, Probability, Falloff, Gate,
Random); Schalter und Ziele liegen nur auf dem Display.

## Steuerung per MIDI-CC (beide Varianten)

Wie beim Euclidier: zweiter MIDI-Port „Control In“, der sich selbst mit allen Hardware-MIDI-Eingängen
verbindet (auch später eingesteckten). Ausgewertet werden nur CCs auf **Control Ch** (OFF, 1–16;
Voreinstellung 16; wird mit dem Projekt gespeichert). CCs, die an den Track geschickt werden, zählen
ebenfalls.

CC-Nummer = 10 × Spur + Funktion, also z. B. Spur 3 Density = CC 33:

| Endziffer | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|---|
| Funktion | On | Source | Pattern | Density | Length | Note | Channel | Lock | Dice |

Global: CC 100 Style, 101 Generate, 102 Variate, 103 Random, 104 Swing, 105 Gate, 106 Auto,
107 Preset, 108 Load, 109 Save.

Effekte: CC 90 Remix On, 91 Mode, 92 Type, 93 Every, 94 Remix Target, 95 Echo On, 96 Time,
97 Repeats, 98 Probability, 99 Falloff, 110 Echo Target, 111 Glitch On, 112 Repeats, 113 Gate,
114 Probability, 115 Random, 116 Glitch Target. Der Wertebereich 0–127 wird auf den Parameter verteilt; Schalter
schalten ab 64 ein, Tasten (Generate, Variate, Dice, Load, Save) lösen ab 64 aus. Ein Faderfox EC4
mit acht Density-Reglern (CC 13, 23 … 83) ist damit ein direkter NGEN-Ersatz für die Hände.

## Presets und Projekt

Das Projekt speichert alle Einstellungen **und** die Würfelwerte jedes Steps, ein geladenes Projekt
spielt also genau dieselben Patterns. Presets (32 Plätze) speichern dasselbe ohne Control Ch;
Datei: `/sdcard/vst/drumgen_BANK.txt`.

## Build

GitHub Actions → „VST release (draft)“ bzw. „VST release FX (draft)“ → Run workflow (wie beim
Euclidier; `dry_run` baut nur das ZIP als Artefakt). Lokal: `vst/build.sh` bzw. `vst-fx/build.sh`
(Docker, armhf), Offline-Test `vst/test.sh` / `vst-fx/test.sh` (x86, ASan/UBSan, eigene
Test-Templates). Parameterliste und Skin-Layout erzeugt `vst/gen_layout.py`; `vst/captions.py` zeichnet
nach dem Skin-Bau alle Beschriftungen (Rahmentitel, Labels über Anzeigen und Listen, Tasten,
Listeneinträge, Schalter) in Titillium Web in der Größe der Knob-Namen statt in der kleinen
Pixelschrift; die Reihenfolge der
Parameter ist der VST-Index und darf sich nie ändern (nur hinten anhängen).

Diagnose: `/tmp/drumgen_vst.log`, alle 5 s eine Zeile (Pulse, Noten, empfangene CCs).

## Lizenzen

- DrumGen: MIT (`LICENSE`).
- Groove-Bank-Rhythmen (`vst/grooves.h`): © 2026 mission-minnow, MIT (`LICENSE-groovebank`).
- Schrift Titillium Web (`vst/fonts/`): SIL Open Font License 1.1 (`vst/fonts/OFL.txt`).
- NGEN und DRUMGEN sind Produkte von Spektro Audio; dieses Projekt ist davon unabhängig und enthält
  keine Inhalte von Spektro Audio. Das Template-Format folgt deren öffentlichem
  `NGEN_DrumGenTemplate.py`.

## Hinweis

Entwickelt mit Unterstützung von Claude (Anthropic)
