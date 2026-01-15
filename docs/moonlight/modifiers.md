# Modifiers

A modifier (💎) is an effect on an effect. It can change the size of the effect layer, and moves effect effect pixels around (e.g. rotate) or expand to other dimensions, e.g. a line can become a circle.

!!! tip

    Multiple modifiers can be defined per layer. The order of multipliers matter e.g. rotate then multiply is different then multiply then rotate.

## Modifier 💎 Nodes

Below is a list of Modifiers in MoonLight. 
Want to add a Modifier to MoonLight, see [develop](https://moonmodules.org/MoonLight/develop/overview/). See also [Live scripts](https://moonmodules.org/MoonLight/moonlight/livescripts/) to add runtime Modifiers.

The following Modifiers are defined in MoonLight. Some found there origin in WLED and WLED-MM (Expand1D):

| Name | Preview | Controls | Remarks
| ---- | ----- | ---- | ---- |
| Multiply 🧊 | ![Multiply](https://github.com/user-attachments/assets/fcb3e4a7-2c28-4f68-b216-5ca004d46c53) | <img width="320" alt="Multiply" src="https://github.com/user-attachments/assets/26606bca-ed3d-41a4-b34e-b40e48d1f524" /> | |
| Mirror 🧊 | ![Mirror](https://github.com/user-attachments/assets/f0f7dfa8-cc4d-4aa9-9c93-bd430d24f6df) | <img width="320" alt="Mirror" src="https://github.com/user-attachments/assets/896fccab-55e2-4083-93d5-a8ffa3cd0c75" /> | |
| Transpose 🧊 | ![Transpose](https://github.com/user-attachments/assets/5a8c74d0-3ce9-407a-8076-4ba708558b9b) | <img width="320" alt="Transpose" src="https://github.com/user-attachments/assets/fdb8e88d-6578-4a3d-a65c-8defbe5e3453" /> | |
| Circle | ![Circle](https://github.com/user-attachments/assets/c5308217-0bd4-49ad-adf8-f17b7062671f) | | |
| Rotate | ![Rotate](https://github.com/user-attachments/assets/c622a9df-318a-4f83-81c0-f5a5c7bafb7b) | <img width="320" alt="Rotate" src="https://github.com/user-attachments/assets/7273a625-5e14-4814-92e8-8f7f65cfc02e" /> | |
| Checkerboard | ![Checkerboard](https://github.com/user-attachments/assets/54970267-35af-406c-9558-c1f4219a71c0) | <img width="320" alt="Checkerboard" src="https://github.com/user-attachments/assets/66d51dc7-b816-4ca7-b1e3-57b067566516" /> | |
| Pinwheel 🧊 | ![PinWheel](https://github.com/user-attachments/assets/e5dbadbe-eeb1-41e5-b197-ec4bd5366aea) | <img width="320" alt="PinWheel" src="https://github.com/user-attachments/assets/46585cea-d301-4221-9af2-65f8054543da" /> | Projects 1D/2D effects onto 2D/3D layouts in a pinwheel pattern.<br>**Swirl**: bend the pinwheel<br>**Rotation Symmetry**: rotational symmetry of the pattern<br>**Petals** Virtual width<br>**Ztwist** twist the pattern along the z-axis<br>Height: distance from center to corner |
| RippleXZ 🧊 | ![RippleXZ](https://github.com/user-attachments/assets/0918efac-6367-420f-b0e3-d796d9551953) | <img width="320" alt="RippleXZ" src="https://github.com/user-attachments/assets/90ecf22c-c4c1-4ee9-8096-fd5613fbb1a7" /> | 1D/2D effect will be rippled to 2D/3D (🚨)<br>Shrink: shrinks the original size towards X and Z, towardsX: copies Y into X, towardsZ: copies XY into Z |

🚨: some effects already do this theirselves e.g. FreqMatrix runs on 1D but copies to 2D and 3D if size allows.
