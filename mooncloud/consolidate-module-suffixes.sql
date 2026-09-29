-- Fold instance suffixes out of the stored module lists, so one driver counts once.
--
-- A device names a second module of the same type `NetworkSend-2`, and firmware before the type
-- fix reported that instance name rather than the type. The statistics then showed NetworkSend,
-- NetworkSend-2, NetworkSend-3 and NetworkSend-4 as four different drivers.
--
-- `worker.js` strips the suffix on ingest, which stops new reports carrying it. This is the other
-- half: the rows already stored, from devices that reported before the fix and may never report
-- again. Run once against the live database.
--
--   wrangler d1 execute <db> --remote --file mooncloud/consolidate-module-suffixes.sql
--
-- The list is comma-joined TEXT, so a suffix sits in one of three places: before the next comma,
-- before a script's `/`, or at the end of the string. The first two are a plain replace; the
-- third needs an anchor, because a bare `-2` replaced blindly would also cut one out of a name.
-- Nine levels, because the live data carries SingleRow-6 and a tenth instance has never been seen.
--
-- The `/` form is LEFT ALONE, so a scripted entry keeps whatever suffix it carries. A blind
-- `-2/` -> `/` renames a user's own directory: `effect:MoonLive/folder-2/x.mle` loses the 2 from
-- a folder nobody else chose, and SQLite has no split to tell a type's suffix from a path's.
-- `worker.js` splits on the first slash and strips only the type, which is where the rule belongs;
-- this file handles the rows that predate it, and a scripted instance among them has never been
-- seen. Over-correcting one would be silent, where leaving it counts one scripted effect twice.
--
-- DE-DUPLICATION IS LEFT ALONE. Folding `NetworkSend-2` into `NetworkSend` can leave the same
-- name twice in one row, and SQLite has no split to collapse that cheaply. The read path asks
-- whether a row CONTAINS a name rather than counting occurrences, so a repeat changes no answer.

-- Before the next comma, which is every instance suffix a non-final entry can carry.
UPDATE reports SET modules =
  replace(replace(replace(replace(
  replace(replace(replace(replace(modules,
    '-2,', ','), '-3,', ','), '-4,', ','), '-5,', ','),
    '-6,', ','), '-7,', ','), '-8,', ','), '-9,', ',')
WHERE modules GLOB '*-[2-9],*';

-- And at the end of the list, where there is no delimiter to match on.
-- The last entry is skipped when IT carries a `/`, for the reason above: a suffix there belongs to a
-- script path, not to an instance, and `effect:MoonLive/folder-2` would otherwise lose the 2 from a
-- directory its owner named. The test has to isolate that final entry, since a slash in an EARLIER
-- one says nothing about this suffix: `effect:MoonLive/a.mle,driver:NetworkSend-2` ends in a real
-- instance. `rtrim(modules, replace(modules, ',', ''))` keeps trimming every non-comma character off
-- the right until the last comma, leaving the prefix; replacing that prefix with nothing leaves the
-- final entry alone to match against.
UPDATE reports SET modules = substr(modules, 1, length(modules) - 2)
  WHERE modules GLOB '*-[2-9]'
    AND replace(modules, rtrim(modules, replace(modules, ',', '')), '') NOT GLOB '*/*';
