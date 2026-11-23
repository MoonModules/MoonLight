/// Other projects' firmwares, as other-firmwares.json lists them: each entry's builds and version read live, and the choices for a
/// detected board. Pure apart from fetch, so the installer page, the bench script (moondeck/build/install_other_firmware.py) and the
/// tests share this one copy.

/// Read each entry's chips and version live, in place: a project's own manifest (its builds and `version`), or the latest GitHub
/// release (its tag, and the assets a WLED-family entry's rules pick from). An entry that fails to load keeps its name and gets `error`.
export async function resolveOtherFirmwares(list) {
  await Promise.all(list.map(async (entry) => {
    try {
      if (entry.manifest) {
        const res = await fetch(entry.manifest);
        if (!res.ok) throw new Error(`${new URL(entry.manifest).host} answered ${res.status}`);
        const manifest = await res.json();
        entry.builds = (manifest.builds || []).filter((build) => Array.isArray(build.parts) && build.parts.length);
        entry.version = otherFirmwareVersion(entry, manifest);
      } else if (entry.release) {
        const res = await fetch(`https://api.github.com/repos/${entry.release.repo}/releases/latest`);
        if (!res.ok) throw new Error(`GitHub answered ${res.status}`);   // 403 once the API's hourly limit per address runs out
        entry.releaseData = await res.json();
        entry.version = otherFirmwareVersion(entry, entry.releaseData);
      }
    } catch (e) {
      console.warn(`[install] ${entry.name}:`, e);
      entry.error = `did not load (${e.message}): reload the page later`;
    }
    entry.builds = entry.builds || [];
  }));
  return list;
}

/// What an other firmware's label names as its version: its project manifest's `version`, or its latest GitHub release's tag.
export function otherFirmwareVersion(entry, data) {
  return (entry.manifest ? data.version : data.tag_name) || "";
}

/// The builds a WLED-family entry's GitHub release offers for `board` ({ family, flashMB, psramMode } from Detect): each asset an
/// entry rule matches for the board's chip, its flash size no larger than the board's and its PSRAM mode the board's, as an ESP
/// Web Tools manifest with the boot files from the project's own installer. A bootloader built for another flash size is marked
/// to be re-flagged (patchBootloaderFlashSize); the app comes through the project's CORS mirror, since GitHub's downloads send none.
export function releaseManifests(entry, release, board) {
  const offered = [];
  for (const asset of (release && release.assets) || []) {
    if (!asset.name.endsWith(".bin")) continue;
    const rule = entry.assets.find((r) => new RegExp(r.match).test(asset.name));
    if (!rule || rule.chipFamily !== board.family) continue;
    const named = rule.flash || (/(\d+)MB/.exec(asset.name) || [])[0];
    if (named && board.flashMB && parseInt(named, 10) > board.flashMB) continue;
    if (rule.psram && rule.psram !== board.psramMode) continue;
    const set = entry.bootSets[rule.boot];
    let parts;
    if (set.parts) {
      parts = set.parts.map(([path, offset]) => ({ path: entry.boot + path, offset }));
    } else {
      // The largest table the flash holds, or the smallest while the flash size did not read, as WLED's installer defaults to 4 MB.
      const sizes = Object.keys(set.partitions).filter((size) => !board.flashMB || parseInt(size, 10) <= board.flashMB)
                          .sort((a, b) => parseInt(a, 10) - parseInt(b, 10));
      const size = rule.flash || (board.flashMB ? sizes[sizes.length - 1] : sizes[0]);
      if (!size || !set.partitions[size]) continue;
      const [path, offset, native] = set.bootloader;
      parts = [{ path: entry.boot + path, offset, ...(native === size ? {} : { flashSize: size }) },
               { path: entry.boot + set.partitions[size], offset: 0x8000 }];
    }
    parts.push({ path: entry.download + new URL(asset.browser_download_url).pathname, offset: 0x10000 });
    const short = asset.name.replace(/^WLED(MM)?_[^_]+_/, "").replace(/\.bin$/, "");
    offered.push({ short, manifest: { name: `${entry.name} ${short}`, version: release.tag_name, builds: [{ chipFamily: rule.chipFamily, parts }] } });
  }
  return offered;
}

/// The dropdown's choices for `board`: an entry with a build for the board's chip, and each WLED-family build releaseManifests
/// offers for it. Before a Detect every entry is listed with its chips and none is installable, since the build depends on the board;
/// an entry that did not load is listed with its reason.
export function otherFirmwareOptions(catalog, board) {
  const options = [];
  for (const entry of catalog) {
    const label = entry.version && !entry.assets ? `${entry.name} · ${entry.version}` : entry.name;
    if (entry.error) {
      options.push({ label: `${entry.name} · ${entry.error}` });
    } else if (!board) {
      const chips = [...new Set((entry.assets || entry.builds).map((build) => build.chipFamily))];
      options.push({ label: `${label} · for ${chips.join(", ") || "its chips"}: Detect the board first` });
    } else if (entry.assets) {
      for (const { short, manifest } of releaseManifests(entry, entry.releaseData, board)) {
        options.push({ label: `${entry.name} ${manifest.version} · ${short}`, name: manifest.name, version: manifest.version, home: entry.home,
                       accessPoint: entry.accessPoint, source: { manifest } });
      }
    } else if (entry.builds.some((build) => build.chipFamily === board.family)) {
      options.push({ label, name: entry.name, version: entry.version, home: entry.home, accessPoint: entry.accessPoint,
                     source: entry.manifest ? { manifestUrl: entry.manifest } : { manifest: entry } });   // a project's manifest resolves its own relative paths
    }
  }
  return options;
}
