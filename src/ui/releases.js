// The update rules: which release a device's channel offers, the release a version ships in, and the key a MoonBase image is named by; and
// where an image is read from: the web installer's page and the download of a release asset through its Pages copy.
// One rule set behind the status bar's badge, a node's popup and its mark on the map, on a board's page and on MoonDeck alike.

import { releaseAsset, firmwareAssetName, latestStable, releaseByTag, chipKey, mirrorUrl } from "./install-picker.js";
import { isNewer, parse } from "./semver.js";

// Whether a version is a -dev build, what the moving `latest` carries (compute_version.py); a release candidate is a release of its own.
function isDevBuild(version) {
    return parse(version)?.prerelease[0] === "dev";
}

// The release a version ships in: the moving `latest` for a -dev build, its own tag for a release or a release candidate.
export function releaseTagOf(version) {
    return isDevBuild(version) ? "latest" : `v${version}`;
}

// The release holding exactly this version, or null: `latest` moves on, so it counts only while its name, its version, is still this one.
// MoonBase is offered from it alone, since the pair of an app and its own release's MoonBase is the pair that was tested.
export function releaseOfVersion(releases, version) {
    const rel = releaseByTag(releases, releaseTagOf(version));
    return rel && (rel.tag_name !== "latest" || rel.name === version) ? rel : null;
}

// The key that names a release asset, from the device's own `firmware` control. On the app tab
// that is already the variant ("esp32s3-opi"); on the MoonBase tab the device reports its MoonBase family
// ("esp32s3oflash"), passed on as it is, or, on firmware from before the family, the chip ("ESP32-S3"),
// which the asset spells "esp32s3" after the build directory's IDF target.
//
// Null when the control has not arrived: the picker reads that as "offer everything", a full list where
// an unmatched key would leave a silently empty one.
export function moonbaseAssetKeyFrom(fw) {
    if (!fw) return null;
    return /^ESP32/.test(fw) ? chipKey(fw) : fw;
}

// The System mode that shows everything, and puts a device on the `latest` channel as a -dev build is.
const MODE_DEVELOPER = 2;

// Whether a device follows the moving `latest` build: one running a -dev build, or one its reader put in developer mode.
function followsLatest(dev) {
    return isDevBuild(dev.version) || dev.mode === MODE_DEVELOPER;
}

// The update a device's channel offers from `releases`, or null. A device on a -dev build or in developer mode follows `latest`, and is
// offered it unless the official release is newer than `latest`, as right after a release; every other device is offered a newer official
// release. A device naming its firmware is offered only a release carrying its image; one too old to name it is still behind, and its page
// or popup reads the firmware from the device before it installs. The one rule behind the status bar's badge, a node's popup and its mark
// on the map. `latest`'s version is its release name (release.yml), which GitHub's API serves cross-origin.
export function channelUpdate(dev, releases) {
    if (!dev?.version) return null;
    const ships = (rel, v) => !dev.firmware || !!releaseAsset(rel, firmwareAssetName(dev.firmware, v, dev.assetPrefix || "firmware")).url;
    const stable = latestStable(releases);
    const sv = stable?.tag_name?.replace(/^v/, "");
    const official = sv && isNewer(sv, dev.version) && ships(stable, sv) ? { rel: stable, tag: stable.tag_name, label: stable.tag_name, version: sv } : null;
    if (!followsLatest(dev)) return official;
    const latest = releaseByTag(releases, "latest");
    const lv = latest?.name;
    const onLatest = lv && isNewer(lv, dev.version) && ships(latest, lv) ? { rel: latest, tag: "latest", label: `latest (${lv})`, version: lv } : null;
    return onLatest && !(official && isNewer(sv, lv)) ? onLatest : official;
}

// The web installer's Pages site, which serves its files cross-origin where GitHub's own downloads do not.
export const INSTALLER_URL = "https://moonmodules.org/MoonLight/install/";

// A release image the browser downloads for a board lacking TLS, from the installer's Pages copy, since GitHub's own asset refuses a
// cross-origin read; `signal` cancels it.
export async function downloadImage(url, signal) {
    const source = mirrorUrl(url, INSTALLER_URL);
    const res = await fetch(source, { signal });
    if (!res.ok) throw new Error(`HTTP ${res.status} from ${source}`);
    return res.blob();
}
