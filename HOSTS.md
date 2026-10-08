# Tested hosts

Which hosts O-Scope has been tested in, how, and with what result. Add a row whenever a host is
tested; update a row when a new version of O-Scope or of the host is tested again. A host is
"supported" only once it has a row with a passing result for the current release.

## Matrix

| Host | Version | macOS | Format | Plugin process | O-Scope | Date | Result | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Ableton Live Standard | 12.2.7 | 27.0.1 | VST3 | Shared (only mode) | 0.3.0 + development | 2026-10-08 | Pass | Single, Multi and Compare, groups, freeze, cursors, saved state. Track names arrive in three partial reports; merged. |
| REAPER | — | — | VST3 | Shared by default ("Native"); "Dedicated process" isolates | — | — | Not tested | Multi shows each instance alone when isolated. |
| Bitwig Studio | — | — | VST3 | "Together" by default; "Individually" isolates | — | — | Not tested | As REAPER. |
| Cubase / Nuendo | — | — | VST3 | Shared | — | — | Not tested | |
| Studio One | — | — | VST3 | Shared | — | — | Not tested | |
| FL Studio | — | — | VST3 | Shared unless bridged | — | — | Not tested | |
| Logic Pro | — | — | — | — | — | — | Not applicable | Loads Audio Units only; O-Scope is VST3. |
| Pro Tools | — | — | — | — | — | — | Not applicable | Loads AAX only. |

## Protocol

Run every step with a Release build installed from the release zip, at 48 kHz and a 256-sample
buffer unless a step says otherwise. Note anything unexpected in the row's notes.

1. **Install and scan.** The host finds O-Scope under Orizon Audio without warnings, and a rescan
   is quick.
2. **Single.** Insert on a stereo track and on a mono track. Play: the trace follows, readouts
   update, Beats and Sec both work, the trigger holds a steady tone still in Sec mode.
3. **Transport.** Stop, play, loop a region and jump around: Beats mode stays on the bar lines;
   Auto-freeze freezes on stop and resumes on play.
4. **Tempo and time signature.** Change the tempo while playing and use a 3/4 bar: the grid and
   beat span follow.
5. **Editor.** Open and close the editor many times while playing, resize it to the minimum and
   larger: nothing is cropped, and the host's CPU returns to where it was once it is closed.
6. **Multi.** Insert on five or more tracks in group A and two in group B: lanes show the right
   names and colours, in order, and line up in time. Rename and recolour a track: its lane
   follows. In hosts with plugin isolation, check both modes and note what Multi shows.
7. **Compare.** Check and uncheck tracks, an inverted copy reads Ø and cancels, Preview fix.
8. **State.** Save the project, close and reopen it: every setting is restored; an older
   project opens.
9. **Rates and buffers.** Repeat steps 2 and 6 briefly at 44.1 and 96 kHz, and at 64 and 1024
   samples.
10. **Load.** With the editor open on seven tracks over four bars, the host's CPU meter stays
    calm and playback has no dropouts.
