#pragma once

// What hosts report of their transport for one block, whatever the plugin format: each format's
// tests turn a case into its host API (a VST3 process context, an Audio Unit's host callbacks)
// and check that the scope follows the transport as the host reports it.

#include <ostream>
#include <vector>

namespace orizon::engine {

// What a host knows for one block. A field the host does not know still holds a plausible value,
// as hosts leave one there: a format that read it regardless would be caught.
struct TransportCase
{
    const char* name = "";
    bool playing = true;
    bool positionKnown = true;
    double quarterNotes = 0.0; // Project position at the block start.
    bool tempoKnown = true;
    double tempo = 120.0;
    bool timeSignatureKnown = true;
    int numerator = 4;
    int denominator = 4;
    bool samplePositionKnown = true;
    double samplePosition = 0.0; // Samples from the project start, a whole number.
};

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
inline void PrintTo(const TransportCase& c, std::ostream* out)
{
    *out << c.name;
}

inline std::vector<TransportCase> transportCases()
{
    return {
        {.name = "Playing44", .quarterNotes = 17.25, .tempo = 128.0, .samplePosition = 352800.0},
        {.name = "PlayingThreeFour",
         .quarterNotes = 6.0,
         .tempo = 90.0,
         .numerator = 3,
         .samplePosition = 176400.0},
        // A bar counted in eighths: its length depends on the denominator as well.
        {.name = "PlayingSevenEight",
         .quarterNotes = 10.5,
         .tempo = 96.0,
         .numerator = 7,
         .denominator = 8,
         .samplePosition = 264600.0},
        {.name = "Stopped", .playing = false, .quarterNotes = 8.0, .tempo = 100.0},
        {.name = "PreRoll", .quarterNotes = -2.0, .samplePosition = -44100.0},
        {.name = "NoBeatAndTempo",
         .positionKnown = false,
         .quarterNotes = 4.0,
         .tempoKnown = false,
         .tempo = 140.0},
        {.name = "NoMusicalTime", .timeSignatureKnown = false, .numerator = 7, .denominator = 8},
        {.name = "NoTransport", .quarterNotes = 3.0, .samplePositionKnown = false},
        // A position without a tempo, and the other way round: VST3 flags each on its own, an
        // Audio Unit's host reports both through one callback or neither.
        {.name = "PositionWithoutTempo",
         .quarterNotes = 5.0,
         .tempoKnown = false,
         .tempo = 140.0,
         .samplePosition = 96000.0},
        {.name = "TempoWithoutPosition",
         .positionKnown = false,
         .quarterNotes = 5.0,
         .tempo = 140.0,
         .samplePosition = 96000.0},
    };
}

} // namespace orizon::engine
