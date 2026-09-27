# Privacy policy

**Last updated: 2026-09-10**

Covers the MoonLight software (firmware, desktop application and the web interface they serve), the [web installer](https://moonmodules.org/projectMM/install/), and this documentation site.

## The rule

**MoonLight sends nothing to us unless you switch it on.** Every feature that transmits anything is opt-in, off by default, and asks in plain words before its first transmission. Declining is one click, is remembered, and sends nothing at all: not even a record that you declined. Some things do reach the internet without you switching anything on, and they are listed below: they go to GitHub, never to a server of ours.

Everything else stays on the machine you run it on: your layouts, effects, drivers, pin assignments, device name, and any credentials you entered. `%LOCALAPPDATA%\MoonLight` on Windows, `~/Library/Application Support/MoonLight` on macOS, `$XDG_DATA_HOME/MoonLight` on Linux, the device's own flash on a board. Nothing there is uploaded, synchronized or backed up by us.

Whatever you switch on, MoonLight does not transmit **your name, email or postal address, your Wi-Fi or MQTT credentials, your IP or MAC address, or the contents of files you made**.

## What reaches the internet before you opt in

Four things, each either something you asked for or something your browser does, and none of them reach a server of ours:

- **The update check**: your browser asks GitHub's public release API whether a newer version exists.
- **Downloading a MoonLive script**: your browser fetches it from `raw.githubusercontent.com`.
- **A firmware update**: your device downloads it from GitHub, when you start one.
- **The web installer**: served by GitHub Pages; flashing happens over USB, directly between your browser and the board.

These go to GitHub under [its privacy statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement). This documentation site and the release downloads are hosted by GitHub too, which receives the requests your browser makes, including your IP address. We add no analytics, no cookies of our own and no tracking.

## Your browser's local storage

The web interface stores a few conveniences in your browser: a cached update-check result, the release you last selected. This never leaves your browser and holds nothing personal.

## What you can switch on

Everything here is off until you turn it on, and each thing is a separate choice with its own setting: turning one on says nothing about the others, and one is never turned on by another.

**The device tells you what it will exchange, where you switch it on.** Each setting says on the card what it sends, and where that is a set of figures the card shows them, drawn empty until you agree. That is deliberate: the disclosure sits with the decision, on a device you own, rather than in a document you would have to come back and re-read.

Whatever the feature, now or later, these rules hold.

**Nothing is transmitted, and nothing is received, until you switch it on.** No identifier leaves the device and no request is made to a server we run, so nothing comes back either.

**It exchanges facts about the device, never about you.** What it runs on, what you configured it to do, and what you deliberately publish.

**These never leave your network, whatever is switched on**: the passwords and keys you entered (WiFi, MQTT, any service you connected), the addresses of things on your own network, and the contents of files on the device.

**You can turn it off again at any time.** Nothing further is sent, and what was already published stays published: a message on a public board cannot be recalled, and a report already counted cannot be withdrawn. So the honest advice is the simple one: leave it off if you are unsure, turn it on when you want to take part, and turn it off if you change your mind.

**MoonCloud** is the name for anything MoonLight does with a server we run, and it is one self-contained part of the software rather than something woven through it. It sits on its own card, its members are the only code that talks to a server of ours, and nothing else in MoonLight goes through it. So this is not a promise about scattered behavior you would have to take on trust: it is one place, and you can switch off what is in it.

Each member is a separate setting on that card, off by default, and each says on the card what it exchanges. Members are added over time; the rules above apply to every one of them, including any added after you read this.

## The installation id

The one part of a MoonCloud transmission that is about you rather than your hardware.

It is a SHA-256 hash of an internal address together with a fixed salt. On a device that address is the chip's MAC; on a desktop or in Docker it is a random value generated on first run and stored with your configuration, derived from nothing about your machine. The address itself is never sent, and the salt is unique to MoonCloud, so the id cannot be matched against the device name or MQTT topics that same address produces on your own network.

**We call it pseudonymous rather than anonymous.** It is the same each time, so reports carrying it came from one installation. That is deliberate: it is what distinguishes an upgrade from a new install. It also means the id is personal data under the GDPR, and **your consent is the lawful basis**: nothing is generated, stored or sent until you answer yes.

What follows from it being stable:

- On a device it survives a factory reset and a reflash. Our salt is published in our source code and a MAC address is a short number, so someone determined could work backwards from an id to a MAC. We do not do this and store nothing that would help, but we will not claim it is impossible.
- On a desktop or in Docker it cannot be worked backwards to anything. Deleting your configuration folder gives you a new one.
- It is kept indefinitely, alongside the aggregates.
- **A transmission already sent cannot practically be withdrawn**, because finding it would mean you supplying your id. To ask anyway, use the contact details below.

## The IP address

Any server receiving a request sees the address it came from. Ours derives a country from it at the network edge and **never writes the address to storage**. What is stored is a country, never an address: beside the figures a report contributes, or beside the text of a message and the time it was sent.

## Systems you connect MoonLight to

An MQTT broker, Home Assistant, Art-Net or E1.31 consoles: MoonLight speaks to these when you configure it to, and those connections go where you point them. **Whatever you connect it to is governed by that service's own privacy policy**, which is worth knowing if you point it at a cloud-hosted broker.

## Changes to this policy

**A new feature that transmits anything is documented here before it ships, and asks for its own consent.** We may later offer to collect things this policy does not describe today, such as which effects are used or automatic crash reports. Each would be a separate opt-in choice, described here first, and switched off until you turn it on.

Revisions are made in the open: the policy lives in the [project repository](https://github.com/MoonModules/projectMM/blob/main/docs/legal/privacy-policy.md), so every change is a commit you can read.

## Contact

Questions, or anything here you would like to verify: an [issue on the repository](https://github.com/MoonModules/projectMM/issues) or the [Discord](https://discord.gg/TC8NSUSCdV). MoonLight is free and open-source software, and the network calls described above are the only ones in it.
