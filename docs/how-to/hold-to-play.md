# Playing an effect while a button is held

A button that shows an effect for as long as it is held, and drops back to a standby effect when it is let go.
A magician's quick-change tricks, a foot pedal on a stage, a "press for sparkle" button on a sculpture: each is the same setup, done with configuration only.

> New here? Start with **[Install & first light](../gettingstarted.md)**. What follows assumes MoonLight is running and the buttons are wired to GPIOs.

---

## How it works

Three pieces, each already on every board:

- **Layers stack.** A bottom layer, Standby, always shows. Above it, one layer per trick has its `opacity` at 0, so it shows nothing until its opacity rises.
- **The [Control](../moonmodules/core/system.md#control) card's faders** each drive one control. Fader 1 drives `Trick1.opacity`, fader 2 `Trick2.opacity`, and so on.
- **The [Button](../moonmodules/core/services.md#button) service** turns a GPIO into a surface control. A row of kind `set` writes its `value` while the button is held and 0 when it is released.

Held, button 1 puts 255 on fader 1, so Trick1 covers Standby. Released, fader 1 goes to 0 and Standby shows again. Every effect keeps running and stays adjustable on the fly, so nothing is timed the way a playlist is.

---

## Setting it up

The whole setup is one state document, sent once with `PATCH /api/state`, or built by hand in the UI.
This one has two tricks on GPIO 4 and 5. The effects, the pins and the number of tricks are yours to change, up to the eight faders the Control card has.

```json
{
  "Effects": {
    "$patch": "replace",
    "Standby": { "type": "Layer", "Noise": { "type": "NoiseEffect" } },
    "Trick1": { "type": "Layer", "opacity": 0, "Spiral": { "type": "SpiralEffect" } },
    "Trick2": { "type": "Layer", "opacity": 0, "Fire": { "type": "FireEffect" } }
  },
  "Control": { "fader1Target": "Trick1.opacity", "fader2Target": "Trick2.opacity" },
  "Services": {
    "Buttons": {
      "type": "ButtonService",
      "buttons": [
        { "pin": 4, "activeLow": true, "target": "Control.fader1", "kind": "set", "value": 255 },
        { "pin": 5, "activeLow": true, "target": "Control.fader2", "kind": "set", "value": 255 }
      ]
    }
  }
}
```

```bash
curl -X PATCH -H "Content-Type: application/json" -d @hold-to-play.json http://<device>/api/state
```

`activeLow` is on for a button wired to ground; the Button card's `pressed` readout shows each press before anything is bound to it.

---

## Choosing the buttons

| What you hold | Works | Why |
|---|---|---|
| Wired buttons or a foot pedal | yes | each is a contact on a GPIO, with a press and a release |
| A wireless RF remote whose receiver has momentary outputs, such as a 433 MHz module with 4 or 8 channels | yes | the receiver's outputs are wired to the GPIOs and act as buttons |
| An infrared remote | no | it reports a press but no release, so nothing would clear the trick |

---

## Going further

- **A softer change:** a `value` below 255 blends the trick over Standby instead of covering it.
- **Logic of its own**, such as another standby after a certain trick, or a sequence on one press: a [MoonLive service](../moonmodules/core/services.md#moonliveservice) script drives the same controls.
