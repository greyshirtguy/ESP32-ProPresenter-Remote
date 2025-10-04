# ESP32-ProPresenter-Remote

Ever since the **ProPresenter API** was announced, I’ve been itching to experiment with a "million ideas".  
One of those ideas was to build a **handheld remote** using the **ESP32**.  

The concept:  
A **compact, tactile controller** with large **Next** / **Previous** buttons, a **small display**, and a few **custom-mappable buttons** (*Clear*, *Up/Down*, *Macro*, etc.). I also wanted to add a **simple on-device menu** for configuration.

---

## 🧪 Proof of Concept — *M5StickC Plus*

The first prototype uses the **M5StickC Plus**, with the following button layout:

| Button | Function |
|:--------|:----------|
| **M5 Button** | ▶️ Next Slide |
| **Action Button** | ◀️ Previous Slide |
| **Power Button** (short press) | 1️⃣ Jump to first slide of active presentation |

---

## 🔋 Battery & Power Notes

The internal battery in the **M5StickC Plus** won’t last long — the combination of Wi-Fi and a backlit screen draws a fair bit of power.  

For better runtime, use the **M5StickC 18650C battery case**:  
it not only provides far more capacity but also makes the device more comfortable to hold.

---

## ✋ Ergonomics

The prototype runs in **portrait mode**, which feels a little unconventional at first but still works well for **one-handed operation**.  
You can easily view the screen while pressing the **main M5 button** for *Next Slide*. The **18650C battery case** would also improve grip and balance.

---






