# The launcher's fonts

Three faces drawn inside `tmntnet-launcher.exe` (they are in its resources: nothing is installed
on the player's PC). All three are under the **SIL Open Font License 1.1**, which allows bundling
and redistribution; each licence, with its authors' copyright line, lies next to the files.

| Files | Face | Used for | Licence |
|---|---|---|---|
| `DelaGothicOne-Regular.ttf` | Dela Gothic One | titles, the big button, numbers of the progress | `OFL-DelaGothicOne.txt` |
| `Onest-Regular.ttf`, `Onest-SemiBold.ttf`, `Onest-Bold.ttf` | Onest | text | `OFL-Onest.txt` |
| `JetBrainsMono-Medium.ttf`, `JetBrainsMono-Bold.ttf` | JetBrains Mono | versions, paths, addresses | `OFL-JetBrainsMono.txt` |

The files are **subsets**: Latin, Russian and the punctuation the launcher writes, as Google
Fonts serves them for a given set of letters. `tools/fetch_launcher_fonts.py` fetches them (and
the licences) again; a letter outside the set is drawn by DirectWrite with one of Windows' own
fonts.
