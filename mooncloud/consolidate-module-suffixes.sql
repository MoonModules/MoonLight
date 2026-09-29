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
-- DE-DUPLICATION IS LEFT ALONE. Folding `NetworkSend-2` into `NetworkSend` can leave the same
-- name twice in one row, and SQLite has no split to collapse that cheaply. The read path asks
-- whether a row CONTAINS a name rather than counting occurrences, so a repeat changes no answer.

-- Before a comma, and before a script's slash.
UPDATE reports SET modules =
  replace(replace(replace(replace(
  replace(replace(replace(replace(
  replace(replace(replace(replace(
  replace(replace(replace(replace(modules,
    '-2,', ','), '-3,', ','), '-4,', ','), '-5,', ','),
    '-6,', ','), '-7,', ','), '-8,', ','), '-9,', ','),
    '-2/', '/'), '-3/', '/'), '-4/', '/'), '-5/', '/'),
    '-6/', '/'), '-7/', '/'), '-8/', '/'), '-9/', '/')
WHERE modules GLOB '*-[2-9],*' OR modules GLOB '*-[2-9]/*';

-- And at the end of the list, where there is no delimiter to match on.
UPDATE reports SET modules = substr(modules, 1, length(modules) - 2)
  WHERE modules GLOB '*-[2-9]';
