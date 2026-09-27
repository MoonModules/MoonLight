// MoonLight semver — a tiny, dependency-free Semantic Versioning compare.
//
// One home for version comparison (the firmware-update check needs it; the install picker can adopt
// it for its dropdown ordering later). Implements the parts of https://semver.org we use:
//   §2  version core: MAJOR.MINOR.PATCH (numeric)
//   §9  pre-release: a `-` then dot-separated identifiers (e.g. 2.1.0-dev, 1.0.0-rc.2)
//   §10 build metadata: a `+` then identifiers — IGNORED for precedence (per spec)
//   §11 precedence: compare core numerically; a version WITH a pre-release is LOWER than the same
//       core without one; otherwise compare pre-release identifiers left-to-right (numeric < non-
//       numeric, numerics compared as numbers, alphanumerics by ASCII, longer set wins if all equal).
//
// Our own code (not a port of `compare-versions`/`semver` npm) — carry the idea forward, write it
// fresh against our needs. Tested in test/js/semver.test.mjs.

// Parse "v?MAJOR.MINOR.PATCH(-prerelease)?(+build)?" → {major, minor, patch, prerelease: string[]}.
// Returns null if it isn't a parseable version core (so callers can treat junk as "can't compare").
// A leading `v` is tolerated (git tags). Build metadata is dropped (it has no precedence).
export function parse(version) {
    if (typeof version !== "string") return null;
    const s = version.trim().replace(/^v/, "");
    // core (-prerelease)? (+build)?
    const m = /^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$/.exec(s);
    if (!m) return null;
    return {
        major: Number(m[1]),
        minor: Number(m[2]),
        patch: Number(m[3]),
        prerelease: m[4] ? m[4].split(".") : [],
    };
}

// Compare two pre-release identifier arrays per §11. Returns -1/0/1.
function comparePrerelease(a, b) {
    // A version with NO pre-release outranks one WITH a pre-release (e.g. 1.0.0 > 1.0.0-rc.1).
    if (a.length === 0 && b.length === 0) return 0;
    if (a.length === 0) return 1;   // a is the release, b is a pre-release → a is higher
    if (b.length === 0) return -1;
    const len = Math.min(a.length, b.length);
    for (let i = 0; i < len; i++) {
        const ai = a[i], bi = b[i];
        const an = /^\d+$/.test(ai), bn = /^\d+$/.test(bi);
        if (an && bn) {                                  // both numeric → compare as numbers
            const d = Number(ai) - Number(bi);
            if (d !== 0) return d < 0 ? -1 : 1;
        } else if (an !== bn) {                          // numeric identifiers are LOWER than alphanumeric
            return an ? -1 : 1;
        } else if (ai !== bi) {                          // both alphanumeric → ASCII order
            return ai < bi ? -1 : 1;
        }
    }
    // All shared identifiers equal → the LONGER set is higher (more identifiers = more specific).
    return a.length === b.length ? 0 : (a.length < b.length ? -1 : 1);
}

// Compare two version strings. Returns -1 (a<b), 0 (a==b), 1 (a>b).
// An unparseable side is treated as the LOWEST (so a comparison against junk never claims an update).
export function compare(a, b) {
    const pa = parse(a), pb = parse(b);
    if (!pa && !pb) return 0;
    if (!pa) return -1;
    if (!pb) return 1;
    for (const k of ["major", "minor", "patch"]) {
        if (pa[k] !== pb[k]) return pa[k] < pb[k] ? -1 : 1;
    }
    return comparePrerelease(pa.prerelease, pb.prerelease);
}

// True iff `candidate` is a strictly newer version than `current` (the update-available test).
export function isNewer(candidate, current) {
    return compare(candidate, current) === 1;
}
