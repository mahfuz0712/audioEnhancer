/* =========================
   C++ BRIDGE (WebView2)
   JS -> C++ : plain strings
       "ready" | "output|<id>" | "effect|<name>" | "power|1/0"
       "volume|<0..100>" | "bass|<-10..10>" | "treble|<-10..10>"
   C++ -> JS : JSON objects   {type: "devices" | "state" | "power" | "status", ...}
========================= */

const bridge =
    window.chrome && window.chrome.webview
        ? window.chrome.webview
        : null;

function send(message) {

    if (bridge) {
        bridge.postMessage(message);
    } else {
        console.log("[to C++]", message);
    }

}


const selects = document.querySelectorAll(".custom-select");

const outputSelect = document.getElementById("outputSelect");
const outputMenu = outputSelect.querySelector(".dropdown-menu");

const outputValue = document.getElementById("outputValue");
const effectValue = document.getElementById("effectValue");

const statusDot = document.getElementById("statusDot");
const statusTitle = document.getElementById("statusTitle");
const statusText = document.getElementById("statusText");

const powerToggle = document.getElementById("powerToggle");


/* =========================
   OPEN / CLOSE DROPDOWNS
========================= */

selects.forEach(select => {

    const button = select.querySelector(".select-button");

    button.addEventListener("click", (event) => {

        event.stopPropagation();

        selects.forEach(other => {

            if (other !== select) {
                other.classList.remove("open");
            }

        });

        select.classList.toggle("open");

    });

});


document.addEventListener("click", () => {

    selects.forEach(select => {
        select.classList.remove("open");
    });

});


/* =========================
   AUDIO EFFECT (static items)
========================= */

const effectSelect = document.getElementById("effectSelect");

const effectItems = effectSelect.querySelectorAll(".dropdown-item");

function setEffectUI(value) {

    effectItems.forEach(i =>
        i.classList.toggle("active", i.dataset.value === value)
    );

    effectValue.textContent = value;

}

effectItems.forEach(item => {

    item.addEventListener("click", () => {

        const value = item.dataset.value;

        setEffectUI(value);

        send("effect|" + value);

        effectSelect.classList.remove("open");

    });

});


/* =========================
   SLIDERS: volume / bass / treble
========================= */

const sliders = [
    { id: "volume", format: v => v + "%" },
    { id: "bass",   format: v => (v > 0 ? "+" : "") + v + " dB" },
    { id: "treble", format: v => (v > 0 ? "+" : "") + v + " dB" }
].map(cfg => ({
    ...cfg,
    input: document.getElementById(cfg.id),
    label: document.getElementById(cfg.id + "Value")
}));


/* Fill the track: from the left for volume, from the centre for bass / treble */
function paintRange(input) {

    const min = Number(input.min);
    const max = Number(input.max);
    const value = Number(input.value);

    const pct = (value - min) / (max - min) * 100;
    const origin = min < 0 ? 50 : 0;

    const from = Math.min(origin, pct);
    const to = Math.max(origin, pct);

    input.style.background =
        `linear-gradient(to right,
            #d1d1d6 0%, #d1d1d6 ${from}%,
            #007aff ${from}%, #007aff ${to}%,
            #d1d1d6 ${to}%, #d1d1d6 100%)`;

}

function refreshSlider(slider) {

    slider.label.textContent = slider.format(Number(slider.input.value));

    paintRange(slider.input);

}

function setSlider(id, value) {

    const slider = sliders.find(s => s.id === id);

    if (!slider || typeof value !== "number") {
        return;
    }

    slider.input.value = value;

    refreshSlider(slider);

}

sliders.forEach(slider => {

    refreshSlider(slider);

    slider.input.addEventListener("input", () => {

        refreshSlider(slider);

        send(slider.id + "|" + slider.input.value);

    });

    /* Double-click = reset to default */
    slider.input.addEventListener("dblclick", () => {

        slider.input.value = slider.input.dataset.default;

        refreshSlider(slider);

        send(slider.id + "|" + slider.input.value);

    });

});


/* =========================
   POWER TOGGLE
========================= */

powerToggle.addEventListener("change", () => {

    send("power|" + (powerToggle.checked ? "1" : "0"));

});


/* =========================
   STATUS
========================= */

function setStatus(state, title, text) {

    statusDot.className = "status-indicator " + (state || "idle");

    statusTitle.textContent = title || "";

    statusText.textContent = text || "";

}


/* =========================
   C++ → JAVASCRIPT
   UPDATE AUDIO DEVICES

   updateAudioDevices([
       { id: "...", name: "Speakers", description: "Realtek Audio", icon: "🔊" },
       { id: "...", name: "WH-1000XM5", description: "Bluetooth Audio", kind: "headphones" }
   ], selectedId);
========================= */

function iconFor(device) {

    if (device.icon) {
        return device.icon;
    }

    switch (device.kind) {
        case "headphones": return "🎧";
        case "display":    return "🖥️";
        default:           return "🔊";
    }

}


function updateAudioDevices(devices, selectedId) {

    outputMenu.innerHTML = "";

    if (!devices || devices.length === 0) {

        outputValue.textContent = "No output device";

        const empty = document.createElement("div");
        empty.className = "dropdown-empty";
        empty.textContent = "No audio output devices found";
        outputMenu.appendChild(empty);

        return;

    }

    const selected =
        devices.find(d => d.id === selectedId) || devices[0];

    outputValue.textContent = selected.name;


    devices.forEach(device => {

        const item = document.createElement("div");

        item.className = "dropdown-item";

        item.dataset.value = device.id || device.name;

        if (device === selected) {
            item.classList.add("active");
        }


        const icon = document.createElement("span");
        icon.className = "device-icon";
        icon.textContent = iconFor(device);

        const info = document.createElement("div");

        const name = document.createElement("strong");
        name.textContent = device.name;

        const description = document.createElement("small");
        description.textContent = device.description || "Audio Device";

        info.appendChild(name);
        info.appendChild(description);

        const check = document.createElement("span");
        check.className = "check";
        check.textContent = "✓";

        item.appendChild(icon);
        item.appendChild(info);
        item.appendChild(check);


        item.addEventListener("click", () => {

            outputMenu
                .querySelectorAll(".dropdown-item")
                .forEach(i => i.classList.remove("active"));

            item.classList.add("active");

            outputValue.textContent = device.name;

            outputSelect.classList.remove("open");

            send("output|" + (device.id || device.name));

        });


        outputMenu.appendChild(item);

    });

}


/* =========================
   MESSAGES FROM C++
========================= */

function handleNative(msg) {

    if (!msg || !msg.type) {
        return;
    }

    switch (msg.type) {

        case "devices":
            updateAudioDevices(msg.devices, msg.selected);
            break;

        case "state":
            setEffectUI(msg.effect || "Normal");
            setSlider("volume", msg.volume);
            setSlider("bass", msg.bass);
            setSlider("treble", msg.treble);
            break;

        case "power":
            powerToggle.checked = !!msg.value;
            break;

        case "status":
            setStatus(msg.state, msg.title, msg.text);
            break;

    }

}


if (bridge) {

    bridge.addEventListener("message", event => handleNative(event.data));

    // Tell C++ the page is ready (it answers with state, devices, power and status)
    send("ready");

} else {

    /* Browser preview (no C++): show demo data */

    updateAudioDevices([
        { id: "1", name: "Speakers (Realtek Audio)", description: "Speakers", kind: "speakers" },
        { id: "2", name: "Headphones (QCY Melobuds ANC)", description: "Headphones", kind: "headphones" },
        { id: "3", name: "LG UltraGear (HDMI)", description: "HDMI / DisplayPort", kind: "display" }
    ], "2");

    powerToggle.checked = true;

    setStatus("active", "Audio Enhancer Active", "Headphones (QCY Melobuds ANC) · Normal");

}