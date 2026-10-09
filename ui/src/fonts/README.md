# Fonts

The UI's fonts, embedded so that it looks the same on every system and needs no network:

| File | Family | Use | Licence |
| --- | --- | --- | --- |
| `RobotoCondensed.woff2` | Roboto Condensed | labels | SIL Open Font License 1.1 ([OFL-Roboto.txt](OFL-Roboto.txt)) |
| `RobotoMono.woff2` | Roboto Mono | values | SIL Open Font License 1.1 ([OFL-RobotoMono.txt](OFL-RobotoMono.txt)) |
| `Roboto.woff2` | Roboto | text | SIL Open Font License 1.1 ([OFL-Roboto.txt](OFL-Roboto.txt)) |

Roboto Condensed's own metadata still names its earlier licence, Apache 2.0; Google Fonts
distributes it under the OFL. Either allows embedding.

## Source

[google/fonts](https://github.com/google/fonts) at commit
`2eb0b48d5f760f62e286216f0859a8c540dbc1bd`:

| File | SHA-256 |
| --- | --- |
| `ofl/robotocondensed/RobotoCondensed[wght].ttf` | `dace262afcee68a5276f200d8026c57221735c0118ab5fda8c2c0d3dc409a8d0` |
| `ofl/robotomono/RobotoMono[wght].ttf` | `66a80e79d17e4c7cabd162e2916578a4cc08fd19eef6e2a643305eae9c567b2b` |
| `ofl/roboto/Roboto[wdth,wght].ttf` | `d7598e12c5dbef095ff8272cfc55da0250bd07fbdecbac8a530b9b277872a134` |

## How they were made

Variable fonts cut to the weights the UI uses, then subset to Basic Latin, Latin-1, Latin
Extended-A (for track names) and the symbols in the UI, keeping every name record (copyright
and licence), with fonttools 4.60.1:

```bash
fonttools varLib.instancer "Roboto[wdth,wght].ttf" wdth=100 wght=400:700 -o roboto.ttf
fonttools varLib.instancer "RobotoCondensed[wght].ttf" wght=400:700 -o condensed.ttf
fonttools varLib.instancer "RobotoMono[wght].ttf" wght=400:600 -o mono.ttf
pyftsubset <each>.ttf --flavor=woff2 --name-IDs='*' --name-languages='*' \
    --layout-features='kern,liga,calt,tnum,lnum' \
    --unicodes='U+0020-007E,U+00A0-017F,U+0394,U+2013-2014,U+2018-201D,U+2022,U+2026,U+2191,U+2193,U+2212,U+221E,U+2248,U+2318,U+2325,U+2713'
```

Roboto has no ↑ ↓ ⌘ ⌥ ✓; those come from a system font.
