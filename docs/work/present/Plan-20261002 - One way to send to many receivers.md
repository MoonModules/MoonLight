# Plan: one way to send to many receivers

## Why

Several network services send the same packet to more than one receiver, and each one decides how in its own code: discovery has a `wledCompatible` toggle, the network driver hides multicast inside its protocol list, audio sync joins and sends to its group by hand, and OSC feedback reaches one address only. Where multicast silently does not arrive, nothing notices. One desk driving several boards, the case that started this, has no clean answer today. And a destination is an address only: [issue #115](https://github.com/MoonModules/MoonLight/issues/115) asks for names such as `panel-01.local`, so a rig survives DHCP moving a panel.

There are three ways to reach many receivers, and each one fails somewhere:

| | One packet reaches | Fails when | Cost on WiFi |
|---|---|---|---|
| **Unicast list** | each listed address, one copy per receiver | never on the network's side; the addresses must be known and typed | one frame per receiver, sent at full speed and retried by the WiFi link |
| **Multicast** | every device that joined the group | a switch snoops IGMP without a querier and forgets the group after a few minutes; a consumer access point or mesh filters it between WiFi and wired | one frame at the slowest basic rate, never retried |
| **Broadcast** | every device on the subnet, wanted or not | rarely; only client isolation and guest networks, which block multicast too | one frame at the slowest basic rate, never retried, and every phone and laptop takes the interrupt |

A unicast list is not multicast. It is N separate sends, each delivered like a single unicast, so it works on every network. For a handful of boards on WiFi it is also the cheapest in airtime, since one fast retried frame per board beats one frame at the slowest rate. Multicast wins with many receivers, or when their addresses are not known.

## What exists

| Service | Sends to many | Today | Who owns the protocol |
|---|---|---|---|
| Device discovery (`DevicesModule`) | presence, every 10 s | multicast `239.255.77.77` always, plus broadcast when `wledCompatible` is on; every device listens for both | us, with WLED apps listening on broadcast |
| OSC feedback (`OscModule`) | surface values | unicast to `feedbackTo`, or to whoever wrote last when it is empty | us |
| Network output (`NetworkSendDriver`) | pixels | unicast to the `ips` list; E1.31 multicast as a fourth `protocol` option; Art-Net broadcast by typing a broadcast address into `ips` | Art-Net, E1.31 and DDP |
| WLED audio sync (`AudioService`) | audio frames | multicast `239.0.0.1` only | WLED |
| E1.31 receive (`NetworkReceiveEffect`) | | unicast only; the group join is backlogged | E1.31 |

The pieces already in core: `parseIpList` (`core/util/IpList.h`) reads a typed list with ranges such as `192.168.1.60-70`, and `UdpSocket` sends to a broadcast address and joins a multicast group on both the desktop and the ESP32.

## The rule

One control, named the same in every service that sends to many receivers, with the standard words:

- **`addressing`**, the textbook name for the choice, a select offering only the modes that service allows: `unicast` (a separate copy to each entry in `hosts`, one or many), `multicast`, `multicast + broadcast`, `broadcast`. A service that allows one mode shows no control.
- **`hosts`**, the unicast list, read by `parseIpList` and shown only for `unicast`. It replaces the network driver's `ips`, and `lightsPerIp` becomes `lightsPerHost` beside it. One list with one entry is the single-receiver case, so "one address" and "several addresses" are one option rather than two.
- **Receivers always listen for every mode the service offers**: the port bound, the group joined. A mixed fleet keeps working whatever each device sends, as discovery already does.
- **`hosts` takes names as well as addresses** ([issue #115](https://github.com/MoonModules/MoonLight/issues/115)): `192.168.1.10-20, panel-01.local` mixes freely. Because every service reads the list through the same parser and helper, a name works in every one of them, not only in the network driver the issue names.
- **One helper in core** owns the rule, so no service builds its own send loop. A service declares the modes it allows, which is where a protocol's limits live; the helper takes the mode, the list, the group and the port, and sends through a send function the caller passes in, which is also what makes it testable without a socket.
- **A network that drops multicast is detected once, for the whole device.** Discovery is the one service where every device both sends and receives, so it is the probe: a peer heard over broadcast but never over multicast proves the group does not arrive. That sets one flag in core, and from then every `multicast` send through the helper adds a broadcast copy, in every service. Silence proves nothing, since a device alone on the network hears nothing either; only that asymmetry counts.

Each service then offers:

| Service | Allowed modes | Default | Note |
|---|---|---|---|
| Device discovery | `multicast`, `multicast + broadcast` | `multicast` | replaces `wledCompatible`; the help explains that broadcast is what WLED apps listen to |
| OSC feedback | `the sender`, `unicast`, `multicast`, `multicast + broadcast` | `the sender` | `the sender` is today's empty `feedbackTo`; `unicast` with two addresses is one desk driving two boards |
| Network output | `unicast` for every protocol, `multicast` for E1.31, `broadcast` for Art-Net | `unicast` | `protocol` loses its `E1.31 multicast` option and keeps the three protocols; the group is computed per universe as today |
| WLED audio sync | `multicast` only, to `239.0.0.1` | | WLED's protocol fixes it, so no control shows |

**What comes out:** `wledCompatible`, `feedbackTo`, the `E1.31 multicast` protocol option, discovery's own broadcast branch, the network driver's own group sending, and audio sync's own group join and send, all replaced by the helper and one control name. The backlog items on the missing multicast fallback and on names in the host list are deleted.

## Steps

### 1. The helper in core ✅

- An `Addressing` mode, a service's allowed set, and a function that sends one packet the way the mode says: each entry in `hosts`, the group, the group plus the broadcast address, or the broadcast address.
- The receive half: bind and join the group when the service allows multicast.
- Unit tests: each mode reaches exactly the addresses it names; an empty or malformed list sends nothing and says so; a list with a range expands.

### 2. Names in the unicast list (issue #115) ✅

The design is the one worked out in [the issue's comment](https://github.com/MoonModules/MoonLight/issues/115); in short:

- **Resolved asynchronously, into a cache.** The send path only ever reads an address, so nothing on the render thread waits for a network answer. Per frame is a DNS round trip at the frame rate, and at `prepare()` a typo stalls the render thread for the resolver's timeout.
- **`parseIpList` grows a second entry kind.** An entry that parses as dotted quads stays exactly what it is today, ranges and last-octet shorthand included, which stay numeric only; anything else is a name. Built test-first, changing no existing behavior.
- **A resolver cache in the platform layer:** ask for a name, get its last known address or nothing, the lookup running off the caller's thread. A `.local` name goes to mDNS through the responder already there for discovery, anything else to the platform resolver, chosen by the suffix rather than a setting.
- **A name with no answer yet** is skipped until a lookup returns, and the card says so; the other destinations keep sending, since one unresolved name must not black out a rig.
- **A name that stops resolving** keeps its last known address with a warning, and changes only when a lookup returns something different: a DNS server briefly away is far more common than a panel that moved.
- **Re-resolved about once a minute**, on a fixed schedule rather than a TTL, to catch a DHCP move between shows.
- **Each destination's state on the card**: resolved, waiting, or stale.

### 3. Device discovery and audio sync ✅

- `addressing` replaces `wledCompatible`, and the help page explains the WLED case.
- A `migrate.js` map entry (`wledCompatible: true` becomes `multicast + broadcast`) and a MIGRATING entry.
- Audio sync sends and joins through the helper, with `multicast` as its only mode; nothing changes on the wire or on the card.

### 4. OSC feedback ✅

- `addressing` and `hosts` replace `feedbackTo`; a `group` text sets the group address for `multicast`, and a receiving board joins the same group.
- A `migrate.js` map entry (a non-empty `feedbackTo` becomes `unicast` with that address) and a MIGRATING entry.
- The how-to's "One desk for a board elsewhere" becomes "one desk for several boards", with `unicast` and two addresses as the first example.

### 5. Network output ✅

- `addressing` beside `protocol`, its options following the protocol: the group address computed per universe as today, broadcast offered for Art-Net only.
- `ips` becomes `hosts` and `lightsPerIp` becomes `lightsPerHost`.
- `migrate.js` map entries (`protocol: E1.31 multicast` becomes `E1.31` with `addressing: multicast`, and the two renames) and a MIGRATING entry.

### 6. The multicast fallback ✅

The design from the backlog item on the missing multicast fallback, now deleted, placed in core so every service gains it:

- **After boot, discovery announces on both** multicast and broadcast for a bounded window, since two devices that each wait for evidence never produce any.
- **Every device listens on both**, and notes for each peer which way its presence arrived.
- **A peer heard over broadcast and never over multicast** sets the core flag: this network drops multicast. From then the helper adds a broadcast copy to every `multicast` send, and discovery keeps announcing on both.
- **Otherwise the window ends** and discovery settles to multicast alone.
- **The Network card says** which way the device sends once the flag is set, so a user can see why their LAN carries broadcast.
- Unit tests of the evidence logic: the asymmetry sets the flag, silence never does, a peer heard both ways never does. A simulated drop on the desktop, discarding received multicast, stands in for a network that drops it.

### 7. Verify on the bench

- The desktop app with the QCon drives the S3 and the P4 together: first `unicast` with both addresses, then `multicast`.
- Discovery with each mode, both boards and the desktop finding each other.
- An Art-Net and an E1.31 multicast output still lighting their receivers.
- A destination given as `<board>.local`: it lights once resolved, keeps lighting while the name stops answering, and one unresolved name leaves the others running.
- Audio sync between two boards, unchanged.
- The fallback: with the desktop's multicast receive dropped, the boards and the desktop still find each other, and the desktop's Network card says it sends broadcast too.

## Not in this plan

- **Per-destination ports**, which the issue leaves out as well.
- **E1.31 multicast receive**, the per-universe group join in `NetworkReceiveEffect`, stays in the backlog until a multicast-only sender is on the bench; the helper's receive half is what it will use.

## Decided

- **The control is `addressing`**, the textbook term; its values are the standard unicast, multicast and broadcast.
- **The list is `hosts`**, the standard term for an entry that is an address or a name: RFC 3986 defines a URL's host as either, and OSC apps such as TouchOSC label the field Host. Unicast to one or to many is one option.
- **The help text for `unicast`:** "A separate copy to each address or name in `hosts`: one or many, separated by commas, with ranges such as `192.168.1.10-20` and names such as `panel-01.local`. It works on every network, retried by WiFi, and suits a known set of receivers. Multicast reaches any number of receivers with one send, without knowing their addresses."
- **OSC offers `multicast + broadcast`** as well, at no cost since the code is shared.
- **WLED audio sync and network output use the helper** with the modes their protocols allow, so their own send code goes.
- **The fallback is in this plan**, since the helper is where it belongs.
