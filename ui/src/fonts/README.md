# Fonts

The UI's fonts, embedded so that it looks the same on every system and needs no network:

| File | Family | Use | Licence |
| --- | --- | --- | --- |
| `Geist.woff2` | Geist | text and labels | SIL Open Font License 1.1 ([OFL-Geist.txt](OFL-Geist.txt)) |
| `GeistMono.woff2` | Geist Mono | values | SIL Open Font License 1.1 ([OFL-Geist.txt](OFL-Geist.txt)) |

## Source

[vercel/geist-font](https://github.com/vercel/geist-font) release
[v1.7.2](https://github.com/vercel/geist-font/releases/tag/v1.7.2), `geist-font-v1.7.2.zip`
(SHA-256 `7fc800d2ac6b92844895196e5041aca55d814c15db70c44f79b3b83ab82b04e2`):

| File | SHA-256 |
| --- | --- |
| `geist-font/Geist/variable/Geist[wght].ttf` | `cdcc4815cbf5f9882fa74e48f8ab410a0495781a58ff7316570f664e7e987753` |
| `geist-font/GeistMono/variable/GeistMono[wght].ttf` | `0e1af3f507a1c8dfbb03d13ffad585834cd45ed7ccb78c756c7ce7873d180d30` |
| `geist-font/OFL.txt` | copied as `OFL-Geist.txt` |

## How they were made

Variable fonts cut to the weights the UI uses, then subset to Basic Latin, Latin-1, Latin
Extended-A (for track names) and the symbols in the UI, keeping every name record (copyright
and licence), with fonttools 4.60.1:

```bash
fonttools varLib.instancer "Geist[wght].ttf" wght=400:600 -o geist.ttf
fonttools varLib.instancer "GeistMono[wght].ttf" wght=400:600 -o mono.ttf
pyftsubset <each>.ttf --flavor=woff2 --name-IDs='*' --name-languages='*' \
    --layout-features='kern,liga,calt,tnum,lnum' \
    --unicodes='U+0020-007E,U+00A0-017F,U+0394,U+2013-2014,U+2018-201D,U+2022,U+2026,U+2191,U+2193,U+2212,U+221E,U+2248,U+2318,U+2325,U+2713'
```

Geist has no Δ ⌘ ⌥ ✓; those come from a system font.
