/* =========================
   C++ BRIDGE (WebView2)
   JS -> C++ : plain strings
       "ready" | "output|<id>" | "effect|<name>" | "power|1/0"
       "volume|<0..100>" | "bass|<-10..10>" | "treble|<-10..10>"
       "theme|<id>" | "mode|system/light/dark"
       "autostart|1/0" | "checkupdates|1/0"
       "update|check" | "update|install" | "openurl|<https url>"
   C++ -> JS : JSON objects
       {type: "devices" | "state" | "power" | "status" | "settings" | "update", ...}
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

const root = document.documentElement;


/* =========================
   THEMES
   (colours live in style.css: [data-theme="<id>"][data-mode="light|dark"])
========================= */

const THEMES = [
    { id: "default",    name: "Default" },
    { id: "dracula",    name: "Dracula" },
    { id: "white",      name: "White" },
    { id: "nord",       name: "Nord" },
    { id: "midnight",   name: "Midnight" },
    { id: "red-velvet", name: "Red Velvet" },
    { id: "amber",      name: "Amber" },
    { id: "ocean",      name: "Ocean" },
    { id: "forest",     name: "Forest" }
];

const settings = { theme: "default", mode: "system" };

/* The last choice is also cached here, so the right theme shows from the very first paint */
try {
    const t = localStorage.getItem("ae.theme");
    const m = localStorage.getItem("ae.mode");
    if (THEMES.some(x => x.id === t)) settings.theme = t;
    if (["system", "light", "dark"].includes(m)) settings.mode = m;
} catch (e) { /* storage not available */ }

const colorScheme = window.matchMedia("(prefers-color-scheme: dark)");

function effectiveMode() {

    if (settings.mode === "system") {
        return colorScheme.matches ? "dark" : "light";
    }

    return settings.mode;

}

function applyTheme() {

    const mode = effectiveMode();

    root.dataset.theme = settings.theme;
    root.dataset.mode = mode;

    /* selected theme / mode highlight, and the previews follow the active mode */
    document.querySelectorAll(".theme-card").forEach(card => {
        card.classList.toggle("active", card.dataset.id === settings.theme);
        card.querySelector(".preview").dataset.mode = mode;
    });

    document.querySelectorAll("#modeSeg button").forEach(b =>
        b.classList.toggle("active", b.dataset.mode === settings.mode)
    );

    sliders.forEach(paintSlider);

}

colorScheme.addEventListener("change", () => {

    if (settings.mode === "system") {
        applyTheme();
    }

});

function cacheTheme() {

    try {
        localStorage.setItem("ae.theme", settings.theme);
        localStorage.setItem("ae.mode", settings.mode);
    } catch (e) { /* ignore */ }

}

function chooseTheme(id) {

    settings.theme = id;
    applyTheme();
    cacheTheme();
    send("theme|" + id);

}

function chooseMode(mode) {

    settings.mode = mode;
    applyTheme();
    cacheTheme();
    send("mode|" + mode);

}


/* =========================
   ELEMENTS
========================= */

const selects = document.querySelectorAll(".custom-select");

const outputSelect = document.getElementById("outputSelect");
const outputMenu = outputSelect.querySelector(".dropdown-menu");

const outputValue = document.getElementById("outputValue");
const effectValue = document.getElementById("effectValue");

const statusDot = document.getElementById("statusDot");
const statusTitle = document.getElementById("statusTitle");
const statusText = document.getElementById("statusText");

const powerToggle = document.getElementById("powerToggle");

const viewMain = document.getElementById("viewMain");
const viewSettings = document.getElementById("viewSettings");

const menuWrap = document.getElementById("menuWrap");
const menu = document.getElementById("menu");
const menuBadge = document.getElementById("menuBadge");


/* =========================
   OPEN / CLOSE DROPDOWNS
========================= */

selects.forEach(select => {

    const button = select.querySelector(".select-button");

    button.addEventListener("click", (event) => {

        event.stopPropagation();

        menu.classList.remove("open");

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

    menu.classList.remove("open");

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


/* Fill the track: from the left for volume, from the centre for bass / treble.
   Colours come from the active theme (--track / --accent). */
function paintSlider(slider) {

    const input = slider.input;

    const min = Number(input.min);
    const max = Number(input.max);
    const value = Number(input.value);

    const pct = (value - min) / (max - min) * 100;
    const origin = min < 0 ? 50 : 0;

    const from = Math.min(origin, pct);
    const to = Math.max(origin, pct);

    const css = getComputedStyle(root);
    const track = css.getPropertyValue("--track").trim() || "#d1d1d6";
    const accent = css.getPropertyValue("--accent").trim() || "#007aff";

    input.style.background =
        `linear-gradient(to right,
            ${track} 0%, ${track} ${from}%,
            ${accent} ${from}%, ${accent} ${to}%,
            ${track} ${to}%, ${track} 100%)`;

}

function refreshSlider(slider) {

    slider.label.textContent = slider.format(Number(slider.input.value));

    paintSlider(slider);

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
   HAMBURGER MENU + SETTINGS PAGE
========================= */

document.getElementById("menuBtn").addEventListener("click", (event) => {

    event.stopPropagation();

    selects.forEach(s => s.classList.remove("open"));

    menu.classList.toggle("open");

});

menu.addEventListener("click", (event) => event.stopPropagation());

const sectionIds = {
    appearance: "secAppearance",
    general: "secGeneral",
    updates: "secUpdates",
    about: "secAbout"
};

function showSettings(section) {

    menu.classList.remove("open");

    viewMain.hidden = true;
    viewSettings.hidden = false;

    const target = sectionIds[section] && document.getElementById(sectionIds[section]);

    if (target) {
        target.scrollIntoView({ block: "start" });
    } else {
        window.scrollTo(0, 0);
    }

}

function showMain() {

    menu.classList.remove("open");

    viewSettings.hidden = true;
    viewMain.hidden = false;

    window.scrollTo(0, 0);

}

menu.querySelectorAll("button").forEach(button => {

    button.addEventListener("click", () => {

        const go = button.dataset.go;

        showSettings(go === "top" ? null : go);

        if (go === "updates") {
            send("update|check");
        }

    });

});

document.getElementById("backBtn").addEventListener("click", showMain);

document.addEventListener("keydown", (event) => {

    if (event.key !== "Escape") {
        return;
    }

    if (menu.classList.contains("open")) {
        menu.classList.remove("open");
    } else if (!viewSettings.hidden) {
        showMain();
    }

});


/* =========================
   SETTINGS PAGE CONTROLS
========================= */

/* Theme cards (each preview carries its own data-theme, so it shows the real colours) */
const themeGrid = document.getElementById("themeGrid");

THEMES.forEach(theme => {

    const card = document.createElement("button");
    card.type = "button";
    card.className = "theme-card";
    card.dataset.id = theme.id;

    const preview = document.createElement("div");
    preview.className = "preview";
    preview.dataset.theme = theme.id;
    preview.dataset.mode = effectiveMode();
    preview.innerHTML = '<span class="pv-card"></span><span class="pv-bar"></span><span class="pv-dot"></span>';

    const name = document.createElement("span");
    name.className = "theme-name";
    name.textContent = theme.name;

    const tick = document.createElement("span");
    tick.className = "theme-tick";
    tick.textContent = "✓";

    card.appendChild(preview);
    card.appendChild(name);
    card.appendChild(tick);

    card.addEventListener("click", () => chooseTheme(theme.id));

    themeGrid.appendChild(card);

});

document.querySelectorAll("#modeSeg button").forEach(button => {

    button.addEventListener("click", () => chooseMode(button.dataset.mode));

});

const autostartToggle = document.getElementById("autostartToggle");
const checkUpdatesToggle = document.getElementById("checkUpdatesToggle");

autostartToggle.addEventListener("change", () => {
    send("autostart|" + (autostartToggle.checked ? "1" : "0"));
});

checkUpdatesToggle.addEventListener("change", () => {
    send("checkupdates|" + (checkUpdatesToggle.checked ? "1" : "0"));
});

/* every element with data-url opens that link in the normal browser (C++ only allows known links) */
document.querySelectorAll("[data-url]").forEach(el => {

    el.addEventListener("click", () => send("openurl|" + el.dataset.url));

});

function setVersion(version) {

    document.getElementById("curVersion").textContent = "v" + version;
    document.getElementById("aboutVersion").textContent = version;
    document.getElementById("footerVersion").textContent = "v" + version;

}

function handleSettings(msg) {

    if (THEMES.some(t => t.id === msg.theme)) {
        settings.theme = msg.theme;
    }

    if (["system", "light", "dark"].includes(msg.mode)) {
        settings.mode = msg.mode;
    }

    autostartToggle.checked = !!msg.autostart;
    checkUpdatesToggle.checked = !!msg.checkUpdates;

    if (msg.version) {
        setVersion(msg.version);
    }

    applyTheme();
    cacheTheme();

}


/* =========================
   UPDATES
   states from C++: checking | latest | available | noasset | downloading | installing | error
========================= */

const updStatus = document.getElementById("updStatus");
const updProgress = document.getElementById("updProgress");
const updProgressBar = document.getElementById("updProgressBar");
const updNotes = document.getElementById("updNotes");
const btnCheck = document.getElementById("btnCheck");
const btnInstall = document.getElementById("btnInstall");
const btnReleases = document.getElementById("btnReleases");
const updateBanner = document.getElementById("updateBanner");

function formatSize(bytes) {

    if (!bytes) {
        return "";
    }

    return (bytes / (1024 * 1024)).toFixed(1) + " MB";

}

function handleUpdate(msg) {

    const busy = msg.state === "checking" || msg.state === "downloading" || msg.state === "installing";

    btnCheck.disabled = busy;
    btnInstall.disabled = busy;

    updProgress.hidden = !(msg.state === "downloading" || msg.state === "checking" || msg.state === "installing");
    updProgress.classList.toggle("busy", msg.state !== "downloading");
    updProgressBar.style.width = msg.state === "downloading" ? (msg.progress || 0) + "%" : "";

    if (msg.state !== "available") {
        btnReleases.hidden = msg.state !== "noasset";
    }

    switch (msg.state) {

        case "checking":
            updStatus.textContent = "Checking for updates…";
            break;

        case "latest":
            updStatus.textContent = "You're up to date.";
            btnInstall.hidden = true;
            updNotes.hidden = true;
            updateBanner.hidden = true;
            menuBadge.hidden = true;
            break;

        case "available": {

            const size = formatSize(msg.size);

            updStatus.textContent = "Version " + msg.latest + " is available" + (size ? " (" + size + ")" : "") + ".";

            btnInstall.hidden = false;
            btnReleases.hidden = true;

            updNotes.hidden = !msg.notes;
            updNotes.textContent = msg.notes || "";

            document.getElementById("updateBannerText").textContent =
                "Version " + msg.latest + " is available";

            updateBanner.hidden = false;
            menuBadge.hidden = false;
            break;

        }

        case "noasset":
            updStatus.textContent =
                "Version " + msg.latest + " exists, but no installer for this system is attached to it yet.";
            btnInstall.hidden = true;
            break;

        case "downloading":
            updStatus.textContent = "Downloading the update… " + (msg.progress || 0) + "%";
            break;

        case "installing":
            updStatus.textContent = "Starting the installer. Audio Enhancer will close now.";
            break;

        case "error":
            updStatus.textContent = msg.message || "Something went wrong while checking for updates.";
            break;

    }

}

btnCheck.addEventListener("click", () => send("update|check"));
btnInstall.addEventListener("click", () => send("update|install"));

document.getElementById("updateBannerBtn").addEventListener("click", () => showSettings("updates"));


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

        case "settings":
            handleSettings(msg);
            break;

        case "update":
            handleUpdate(msg);
            break;

    }

}


applyTheme();

if (bridge) {

    bridge.addEventListener("message", event => handleNative(event.data));

    // Tell C++ the page is ready (it answers with state, devices, power, status and settings)
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

    handleSettings({ theme: settings.theme, mode: settings.mode, autostart: false, checkUpdates: true, version: "1.0.0" });

    /* fake update check, so the Updates card can be tried in the browser */
    btnCheck.addEventListener("click", () => {

        handleUpdate({ state: "checking" });

        setTimeout(() => handleUpdate({
            state: "available", latest: "1.0.1", size: 2411724,
            notes: "- New themes\n- Bug fixes"
        }), 900);

    });

}