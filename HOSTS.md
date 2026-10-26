# Tested hosts

Which hosts Sineless has been tested in, how, and with what result: the list of hosts each release
was checked in. Other hosts that load VST3 plugins, or on macOS Audio Units, should work but have
not been tested; a report through the host compatibility issue template adds them. Add a row
whenever a host is tested; update a row when a new version of Sineless or of the host is tested
again, saying which test ran: the smoke test or the full protocol.

Every release passes the automated checks in CONTRIBUTING.md (the VST3 validator, pluginval,
auval, and the installer checks on GitHub's runners) before any host is opened. In hosts, each
release gets the smoke test below; the full protocol runs occasionally ("Releasing" in
CONTRIBUTING.md says when).

## Matrix

| Host | Version | OS | Format | Plugin process | Sineless | Date | Result | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Ableton Live Standard | 12.2.7 | macOS 27.0.1 | VST3 | Shared (only mode) | 0.5.2 | 2026-10-17 | Pass | Release installer (checksum and attestation verified), opened as downloaded: Open Anyway, then installed over 0.5.1 in ~/Library/Audio/Plug-Ins/VST3 without a password and with no quarantine flag; Live found it at once. The 0.5.1 test set (3/4, 96 kHz) opened with every setting: Compare with Kick checked; an inverted Pluck reads "0.0 ms Ø · −1.00" and the sum −∞ dB, Preview fix +3.0 dB; level cursors dragged, then turned off: no line left on screen (#146); saved and reopened: mode, time base and span restored, Compare back on the first other track. The protocol at 48 kHz and 256 samples, in 4/4 at 120 BPM: Single in Sec holds a 49 Hz tone still at 50 ms (Trig'd); a jump while looping keeps the sweep on the bar lines; Auto-freeze on stop, live again on play; 140 BPM and then 3/4 while playing, the grid follows (three beats a bar); the editor at its minimum size and enlarged, nothing cropped; 26 reopens while playing, Live's CPU meter 3% open and closed, its memory flat over ten (705 to 704 MB); Multi with group A and B (Kick Off and Sub in B), lanes in track order with their colours, stacked lanes fitting their labels; a track renamed and recoloured, its lane follows at once. Multi and Single at 44.1 kHz/64, 88.2 kHz/1024 and 96 kHz/1024. Load: six tracks over four bars at 96 kHz, CPU meter 4%; fifteen tracks over sixteen bars at 60 BPM and 96 kHz: the window grows to fit every lane, the sweep fills over its first pass, CPU meter about 4%, Live +370 MB including its own nine new tracks. Live gives every track a stereo bus, so a mono track cannot be tried here. |
| Ableton Live Standard | 12.4.6 (x64, emulated) | Windows 11 ARM64 | VST3 | Shared (only mode) | 0.4.0 development | 2026-10-09 | Partial | Scan, Single, Multi with two tracks, keys to Live and to fields, 50 editor reopens without memory growth, two editors at once; the editor appears and draws sharp with Auto-Scale on and off at 100 and 125 %. Protocol steps 3, 4 and 7 to 10 not yet run on Windows. |
| REAPER | 7.82 (ARM64EC; loads the x64 binary) | Windows 11 ARM64 | VST3 | Shared, and "Dedicated process" | 0.4.0 development | 2026-10-09 | Pass (scripted) | `scripts/windows/Test-Reaper.ps1`: four tracks of tone, editors captured, 50 editor reopens while playing with REAPER's memory and handles flat, state restored after reopening the project, Space reaches the transport; in both process modes. |
| REAPER | — | — | VST3 | Shared by default ("Native"); "Dedicated process" isolates | — | — | Not tested | Multi shows each instance alone when isolated. |
| Bitwig Studio | — | — | VST3 | "Together" by default; "Individually" isolates | — | — | Not tested | As REAPER. |
| Cubase / Nuendo | — | — | VST3 | Shared | — | — | Not tested | |
| Studio One | — | — | VST3 | Shared | — | — | Not tested | |
| FL Studio | — | — | VST3 | Shared unless bridged | — | — | Not tested | |
| Logic Pro | — | — | AU | Out of process (AUHostingService); a project's instances share one process | — | — | Not tested | |
| GarageBand | — | — | AU | Out of process (AUHostingService); a project's instances share one process | — | — | Not tested | |
| Ableton Live | — | — | AU | Shared, in Live's own process | — | — | Not tested | A VST3 and an AU instance of Sineless never see each other in Multi or Compare. |
| Pro Tools | — | — | — | — | — | — | Not applicable | Loads AAX only. |

## Smoke test

About five minutes, with the release's own installer, at the host's usual rate and buffer:

1. **Install and scan.** Install from the release's installer as downloaded (the draft's, before a
   release); the host finds Sineless under Orizon Audio, at the new version.
2. **Play.** Insert on two tracks with audio and play: the trace follows and the readouts move, in
   Beats (the sweep on the bar lines) and in Sec (a steady tone held still).
3. **Editor.** Open, resize and close the editor a few times while playing: it draws at once and
   nothing is cropped.
4. **Multi and Compare.** Multi shows both tracks as lanes with their names, in order; in Compare,
   checking the other track shows its match and the sum.
5. **Transport.** With Auto-freeze on, stop: the display freezes; play: it is live again.
6. **State.** Save, close and reopen the project: every setting is restored.

## Protocol

The full protocol, run when "Releasing" in CONTRIBUTING.md says it is due. Run every step with a
Release build installed from the release (installer or zip), at 48 kHz and a 256-sample buffer
unless a step says otherwise. Note anything unexpected in the row's notes.

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
