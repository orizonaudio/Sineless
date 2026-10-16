# Tested hosts

Which hosts Sineless has been tested in, how, and with what result. Add a row whenever a host is
tested; update a row when a new version of Sineless or of the host is tested again. A host is
"supported" only once it has a row with a passing result for the current release.

## Matrix

| Host | Version | OS | Format | Plugin process | Sineless | Date | Result | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Ableton Live Standard | 12.2.7 | macOS 27.0.1 | VST3 | Shared (only mode) | 0.5.0 | 2026-10-14 | Partial | Release zip, on a set saved with 0.4.0, which opens with every setting. Single, Beats and Sec, Auto trigger; Freeze, and Auto-freeze on stop and play; tempo changed to 90 BPM and a 3/4 bar, the grid follows; editor closed and reopened about ten times while playing, settings kept, Live +15 MB; Multi with 6 tracks in group A and 2 in group C, then 15 in group A, names and colours, Overlay and Stack, the window grows to fit 15 lanes; Compare: an inverted copy reads "0.0 ms Ø · −1.00", the sum −∞ dB, Preview fix +3.0 dB; settings restored after saving and reopening; 44.1 and 96 kHz; fifteen tracks over sixteen bars at 60 BPM and 96 kHz: Live's CPU meter 8 %, Live 1.2 GB in all, flat over a minute. Not run: 64- and 1024-sample buffers (512 only), loop and jumps, renaming and recolouring a track, the minimum window size. Live's tracks are stereo, so no mono track. Frozen, Compare kept matches from before the freeze; fixed for the next release. |
| Ableton Live Standard | 12.4.6 (x64, emulated) | Windows 11 ARM64 | VST3 | Shared (only mode) | 0.4.0 development | 2026-10-09 | Partial | Scan, Single, Multi with two tracks, keys to Live and to fields, 50 editor reopens without memory growth, two editors at once; the editor appears and draws sharp with Auto-Scale on and off at 100 and 125 %. Protocol steps 3, 4 and 7 to 10 not yet run on Windows. |
| REAPER | 7.82 (ARM64EC; loads the x64 binary) | Windows 11 ARM64 | VST3 | Shared, and "Dedicated process" | 0.4.0 development | 2026-10-09 | Pass (scripted) | `scripts/windows/Test-Reaper.ps1`: four tracks of tone, editors captured, 50 editor reopens while playing with REAPER's memory and handles flat, state restored after reopening the project, Space reaches the transport; in both process modes. |
| REAPER | — | — | VST3 | Shared by default ("Native"); "Dedicated process" isolates | — | — | Not tested | Multi shows each instance alone when isolated. |
| Bitwig Studio | — | — | VST3 | "Together" by default; "Individually" isolates | — | — | Not tested | As REAPER. |
| Cubase / Nuendo | — | — | VST3 | Shared | — | — | Not tested | |
| Studio One | — | — | VST3 | Shared | — | — | Not tested | |
| FL Studio | — | — | VST3 | Shared unless bridged | — | — | Not tested | |
| Logic Pro | — | — | — | — | — | — | Not applicable | Loads Audio Units only; Sineless is VST3. |
| Pro Tools | — | — | — | — | — | — | Not applicable | Loads AAX only. |

## Protocol

Run every step with a Release build installed from the release zip, at 48 kHz and a 256-sample
buffer unless a step says otherwise. Note anything unexpected in the row's notes.

1. **Install and scan.** The host finds Sineless under Orizon Audio without warnings, and a rescan
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
   names and colours, in order, and line up in time, and the window grows so that every stacked
   lane fits its label. Rename and recolour a track: its lane follows. In hosts with plugin
   isolation, check both modes and note what Multi shows.
7. **Compare.** Check and uncheck tracks, an inverted copy reads Ø and cancels, Preview fix. Close and reopen the editor: the checks remain. Save and reopen the project: the first other track is checked again.
8. **State.** Save the project, close and reopen it: every setting is restored; an older
   project opens.
9. **Rates and buffers.** Repeat steps 2 and 6 briefly at 44.1 and 96 kHz, and at 64 and 1024
   samples.
10. **Load.** With the editor open on seven tracks over four bars, the host's CPU meter stays
    calm and playback has no dropouts. Then the worst case: fifteen tracks over sixteen bars at
    60 BPM and 96 kHz. The sweep fills in over its first pass ("Filling"), the CPU meter stays
    calm, and the host reports well under a gigabyte more memory.
