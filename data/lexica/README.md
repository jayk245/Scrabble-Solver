# Lexica

Word lists are not checked in. Fetch them with `scripts/fetch_lexicon.sh`:

| Command | File | List | Terms |
|---|---|---|---|
| `scripts/fetch_lexicon.sh` | `enable1.txt` | ENABLE (172,823 entries) | Public domain. Used by the tests. |
| `scripts/fetch_lexicon.sh wow24` | `wow24.txt` | [WGPO Official Words 2024](https://wordgameplayers.org/wgpo-official-words/) (195,383 words) | Free for non-commercial use; do not sell it or include it in a commercial product. Never commit it. |

Both downloads are pinned by SHA-256. Compiled GADDAG caches (`*.gaddag`)
are generated next to each list on first load and are ignored by git.

WOW24 is a tournament-style list close to the NASPA Word List 2023 (NWL2023)
but not identical: WGPO lists about 1,385 NWL2023 additions that WOW24 does
not include and 13 NWL2023 deletions that WOW24 keeps. Analyse a game with
the lexicon it was played under. The official NWL2023 and Collins (CSW)
files are licensed and must be supplied separately; any one-word-per-line
file works with `--lexicon`.
