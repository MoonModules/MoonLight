-- MoonCloud Stats: the whole database.
--
-- One table. There is no address column and no precise timestamp, because neither is collected:
-- the country arrives already derived at the edge, and the date is stored to the DAY, since a
-- timestamp to the second plus a country is a fingerprint.

CREATE TABLE IF NOT EXISTS reports (
  installationId  TEXT NOT NULL,
  event           TEXT NOT NULL DEFAULT 'install',
  version         TEXT NOT NULL DEFAULT '',
  previousVersion TEXT NOT NULL DEFAULT '',
  chip            TEXT NOT NULL DEFAULT '',
  flash           TEXT NOT NULL DEFAULT '',
  psram           TEXT NOT NULL DEFAULT '',
  sdk             TEXT NOT NULL DEFAULT '',
  deviceModel     TEXT NOT NULL DEFAULT '',
  modules         TEXT NOT NULL DEFAULT '',
  country         TEXT NOT NULL DEFAULT '??',
  receivedAt      TEXT NOT NULL,
  -- 1 when the firmware was built locally rather than published by CI. Every figure counts every
  -- report, and the `Build` breakdown is what shows the split, so a reader sees the distinction
  -- rather than a total that quietly left one side out. `?dev=0` narrows to released installs for
  -- a caller that wants only those. Recorded from the first report because a flag added later
  -- cannot classify rows already stored.
  dev             INTEGER NOT NULL DEFAULT 0,
  -- The build and the silicon that decides it: which firmware a board runs, its revision and its package (the classic's).
  firmware        TEXT NOT NULL DEFAULT '',
  chipRevision    TEXT NOT NULL DEFAULT '',
  chipPackage     TEXT NOT NULL DEFAULT '',

  -- Raw numbers, bucketed into ranges by the server on read: storing a bucket would freeze every
  -- row at today's boundaries, and a range that turns out wrong could never be re-cut.
  totalHeap       INTEGER NOT NULL DEFAULT 0,   -- internal + PSRAM capacity
  freeHeap        INTEGER NOT NULL DEFAULT 0,   -- free at report time
  lightCount      INTEGER NOT NULL DEFAULT 0,   -- physical lights driven (Layer::physicalLightCount)
  fps             INTEGER NOT NULL DEFAULT 0,   -- system render rate at report time (Scheduler::fps)

  -- One row per installation per version. A device that re-reports the same upgrade overwrites its
  -- row rather than adding one, so a count of rows is a count of installations rather than of
  -- retries.
  PRIMARY KEY (installationId, version)
);

CREATE INDEX IF NOT EXISTS idx_reports_version ON reports(version);
CREATE INDEX IF NOT EXISTS idx_reports_chip    ON reports(chip);

-- MoonTalk: a public message board between devices, in the shape Meshtastic's channel chat has.
--
-- Every message is readable by everyone, so nothing here is private and the table holds only what a
-- sender chose to publish. There is no auth and no per-user table: `sender` is the same installation
-- id Stats uses, which makes messages from one device groupable without naming anyone.
CREATE TABLE IF NOT EXISTS messages (
  id        INTEGER PRIMARY KEY AUTOINCREMENT,
  sender    TEXT NOT NULL,             -- installation id, or its first 8 chars as a display handle
  name      TEXT NOT NULL DEFAULT '',  -- device name, ONLY when the sender consented to share it
  text      TEXT NOT NULL,
  country   TEXT NOT NULL DEFAULT '??',
  sentAt    TEXT NOT NULL              -- ISO 8601 to the SECOND: a chat needs ordering, where a
                                       -- report only needed a day
);

-- Newest first is the only query the board makes.
CREATE INDEX IF NOT EXISTS idx_messages_id ON messages(id DESC);

-- Every report as it arrived, append-only, for trends over time.
--
-- SEPARATE from `reports` because the two answer different questions and cannot share a shape.
-- `reports` is CURRENT STATE: one row per installation per version, overwritten when a device
-- re-reports, so it answers "what is running now". This table is HISTORY: one row per report ever
-- received, never updated, so it answers "what happened in March".
--
-- It exists now rather than when someone wants a chart, because history cannot be reconstructed
-- afterwards: `reports` overwrites `receivedAt` on every conflict, so the moment a device
-- re-reports, when it first reported is gone. A table added in six months starts empty.
--
-- No installation id is stored here. A dated row per installation is a movement profile, and
-- nothing a trend asks needs one: "how many upgrades in March" counts rows.
CREATE TABLE IF NOT EXISTS events (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  event       TEXT NOT NULL DEFAULT 'install',
  version     TEXT NOT NULL DEFAULT '',
  previousVersion TEXT NOT NULL DEFAULT '',
  chip        TEXT NOT NULL DEFAULT '',
  deviceModel TEXT NOT NULL DEFAULT '',
  country     TEXT NOT NULL DEFAULT '??',
  day         TEXT NOT NULL,  -- YYYY-MM-DD, the same resolution the trend charts need
  dev         INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_events_day ON events(day);
