#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <istream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Eine Produktionsanlage schreibt Statusmeldungen ihrer Maschinen in ein Log.
// Jede Zeile sieht so aus: <timestamp_sekunden>;<maschinen_id>;<status>
// status ist RUNNING, IDLE oder ERROR. Beispiel:
// 100;M1;RUNNING
// 130;M2;RUNNING
// 160;M1;ERROR
// 200;M1;RUNNING
// 210;M2;IDLE
// 250;M1;IDLE

namespace tst {

enum class Status { Running, Idle, Error };

struct Event {
  std::int64_t time;
  Status status;
};

// ---------------------------------------------------------------------------
// Parsing: reine Funktionen ohne Zustand, einzeln testbar
// ---------------------------------------------------------------------------

namespace parse {

inline std::string_view trim(std::string_view s) {
  constexpr std::string_view ws = " \t\r\n";
  const auto first = s.find_first_not_of(ws);
  if (first == std::string_view::npos)
    return {};
  const auto last = s.find_last_not_of(ws);
  // why last-first+1? because substr takes length, not last index
  // why + 1? because last is inclusive, but substr length is exclusive
  return s.substr(first, last - first + 1);
}

// Zerlegt s an delim in genau N Felder; nullopt bei abweichender Feldanzahl.
template <std::size_t N>
std::optional<std::array<std::string_view, N>> split(std::string_view s,
                                                     char delim) {
  std::array<std::string_view, N> fields;
  std::size_t n = 0;
  for (;;) {
    if (n == N)
      return std::nullopt; // zu viele Felder
    const auto pos = s.find(delim);
    fields[n++] = trim(s.substr(0, pos));
    if (pos == std::string_view::npos)
      break;
    s.remove_prefix(pos + 1);
  }
  if (n != N)
    return std::nullopt; // zu wenige Felder
  return fields;
}

inline std::optional<std::int64_t> timestamp(std::string_view s) {
  std::int64_t value{};
  const auto end = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(s.data(), end, value);
  if (s.empty() || ec != std::errc{} || ptr != end || value < 0)
    return std::nullopt;
  return value;
}

inline std::optional<Status> status(std::string_view s) {
  if (s == "RUNNING")
    return Status::Running;
  if (s == "IDLE")
    return Status::Idle;
  if (s == "ERROR")
    return Status::Error;
  return std::nullopt;
}

struct Entry {
  std::string_view machine; // zeigt in die übergebene Zeile
  Event event;
};

inline std::optional<Entry> line(std::string_view text) {
  const auto fields = split<3>(text, ';');
  if (!fields)
    return std::nullopt;
  const auto &[tsStr, id, stStr] = *fields;

  const auto ts = timestamp(tsStr);
  const auto st = status(stStr);
  if (!ts || id.empty() || !st)
    return std::nullopt;
  return Entry{id, {*ts, *st}};
}

} // namespace parse

// ---------------------------------------------------------------------------

class MachineLog {
public:
  struct Durations {
    std::int64_t running = 0;
    std::int64_t idle = 0;
    std::int64_t error = 0;

    void add(Status s, std::int64_t duration) {
      switch (s) {
      case Status::Running:
        running += duration;
        break;
      case Status::Idle:
        idle += duration;
        break;
      case Status::Error:
        error += duration;
        break;
      }
    }
  };

  // Liest alle Zeilen ein, gibt die Anzahl übersprungener (fehlerhafter) Zeilen
  // zurück. Leerzeilen werden ignoriert und nicht als Fehler gezählt.
  std::size_t read(std::istream &in) {
    std::size_t skipped = 0;
    std::string text;
    while (std::getline(in, text)) {
      if (parse::trim(text).empty())
        continue;
      if (const auto entry = parse::line(text))
        events_[std::string(entry->machine)].push_back(entry->event);
      else
        ++skipped;
    }
    // Einmal hier sortieren statt bei jeder Auswertung.
    // stable: bei gleichem Zeitstempel gewinnt die spätere Zeile im Log
    for (auto &[id, events] : events_)
      std::stable_sort(
          events.begin(), events.end(),
          [](const Event &a, const Event &b) { return a.time < b.time; });
    return skipped;
  }

  std::unordered_map<std::string, Durations>
  durations(std::int64_t tEnd) const {
    std::unordered_map<std::string, Durations> result;
    result.reserve(events_.size());
    for (const auto &[id, events] : events_) {
      Durations d;
      for (std::size_t i = 0; i < events.size() && events[i].time < tEnd; ++i) {
        const std::int64_t end =
            (i + 1 < events.size()) ? std::min(events[i + 1].time, tEnd) : tEnd;
        d.add(events[i].status, end - events[i].time);
      }
      result.emplace(id, d);
    }
    return result;
  }

  // Bei Gleichstand gewinnt die lexikographisch kleinste ID (deterministisch,
  // unabhängig von der Iterationsreihenfolge der unordered_map).
  std::optional<std::string> machineWithMostError(std::int64_t tEnd) const {
    std::optional<std::string> best;
    std::int64_t bestTime = 0;
    for (const auto &[id, d] : durations(tEnd)) {
      if (d.error > bestTime || (d.error == bestTime && best && id < *best)) {
        bestTime = d.error;
        best = id;
      }
    }
    return best; // nullopt, wenn keine Maschine je in ERROR war
  }

private:
  std::unordered_map<std::string, std::vector<Event>> events_;
};

// ---------------------------------------------------------------------------
// Test
// ---------------------------------------------------------------------------

inline void testMachineLog() {
  std::istringstream in("100;M1;RUNNING\n"
                        "130;M2;RUNNING\n"
                        "160;M1;ERROR\n"
                        "\n"
                        "kaputt\n"
                        "200;M1;RUNNING\n"
                        "210;M2;IDLE\n"
                        "250;M1;IDLE\n");
  MachineLog log;
  const auto skipped = log.read(in);
  assert(skipped == 1);

  const auto d = log.durations(300);
  const auto &m1 = d.at("M1");
  const auto &m2 = d.at("M2");
  assert(m1.running == 110 && m1.error == 40 && m1.idle == 50);
  assert(m2.running == 80 && m2.error == 0 && m2.idle == 90);
  assert(log.machineWithMostError(300) == "M1");

  std::cout << "[MachineLog] skipped=" << skipped << '\n';
  for (const auto &[id, dur] : d)
    std::cout << "  " << id << ": running=" << dur.running
              << " idle=" << dur.idle << " error=" << dur.error << '\n';
  std::cout << "  most error: " << log.machineWithMostError(300).value_or("-")
            << '\n';
}

} // namespace tst
