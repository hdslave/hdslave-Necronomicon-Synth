# Third-party pieces

Necronomicon's own code is in `src/` (except where noted below). The borrowed pieces:

| Piece | Where | Licence |
| --- | --- | --- |
| CLAP headers (the plug-in API) | `third_party/clap/` | MIT, © Alexandre Bique — text in `third_party/clap/LICENSE` |
| Departure Mono by Helena Zhang (the screen font's glyphs) | `src/ui/font.h` | SIL Open Font License — text in `licenses/OFL-departure-mono.txt` |

`src/melodee_screen.h` is hdslave's own header for Melodee's screen extension; it carries its own
MIT notice so any plug-in may copy it.
