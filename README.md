# ESP32-ProPresenter-Remote

Ever since the **ProPresenter API** was announced, I’ve been itching to experiment with a "million ideas".  
One of those ideas was to build a **handheld remote** using the **ESP32**.  

The concept:  
A **compact, tactile controller** with large **Next** / **Previous** buttons, a **small display**, and a few **custom-mappable buttons** (*Clear*, *Up/Down*, *Macro*, etc.). I also wanted to add a **simple on-device menu** for configuration.

---

## 🧪 Proof of Concept — [M5StickC Plus](https://shop.m5stack.com/products/m5stickc-plus-esp32-pico-mini-iot-development-kit?srsltid=AfmBOooHba48_SrFj6H0c3zdABPBUifUaSECYs5Zb0maFQ8p4b6qMayU)

The first prototype uses the **M5StickC Plus**, with the following button layout:

| Button | Function |
|:--------|:----------|
| **M5 Button** | ▶️ Next Slide |
| **Action Button** | ◀️ Previous Slide |
| **Power Button** (short press) | 1️⃣ Jump to first slide of active presentation |

---

## ⚙️ Setting up a **M5StickC Plus** as a simple ProPresenter Remote
   
If you have not setup a development environment for the M5StickC then you can learn all about it [here](https://github.com/Edinburgh-College-of-Art/m5stickc-plus-introduction?tab=readme-ov-file)   
_FWIW - I personally used the [Arduino IDE](https://github.com/Edinburgh-College-of-Art/m5stickc-plus-introduction/blob/main/examples/Getting-Started/ArduinoIDE_Setup/README.md)_  

Config is currently HARD CODED! _(Feel free to submit a PR to for a nice option to configure)_  
Scroll to the USER CONFIG section at the top of M5StickCPlusProPresenterRemote.ino and edit the WiFi details and ProPresenter IP and PORT. Save and upload to your M5StickC Plus.


NB: The screen layout is coded for the **M5StickC Plus** - If you have an old M5StickC you will need to update/fix the UI. 

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






