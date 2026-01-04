// Firebase configuration
const firebaseConfig = {
  apiKey: "AIzaSyAbeFhSuXYh_8RGIkL8AYQFuPILRgM9e_E",
  authDomain: "project2-cd9c9.firebaseapp.com",
  databaseURL: "https://project2-cd9c9-default-rtdb.firebaseio.com",
  projectId: "project2-cd9c9",
  storageBucket: "project2-cd9c9.firebasestorage.app",
  messagingSenderId: "72637977704",
  appId: "1:72637977704:web:3d21a61e5ba1105677d7dd"
};

firebase.initializeApp(firebaseConfig);
const database = firebase.database();

const PATH = "/Var";

// ===== Top 2x2 status =====
const soilStat = document.getElementById("soilStat");
const volumeStat = document.getElementById("volumeStat");
const thresholdStat = document.getElementById("thresholdStat");
const modeStat = document.getElementById("modeStat");

// ===== Controls =====
const modeManual = document.getElementById("mode_manual");
const modeAuto = document.getElementById("mode_auto");
const modeSchedule = document.getElementById("mode_schedule");

const manualSwitch = document.getElementById("manual_switch");

const scheduleDate = document.getElementById("schedule_date");
const scheduleTime = document.getElementById("schedule_time");

const thresholdSlider = document.getElementById("threshold_slider");
const thresholdDisplay = document.getElementById("threshold_display");
const pumpSecondsInput = document.getElementById("pump_seconds");
const saveBtn = document.getElementById("save_settings");

// ===== Helpers =====
function modeToText(m) {
  if (m === 0) return "MANUAL";
  if (m === 1) return "AUTO";
  if (m === 2) return "SCHEDULE";
  return "N/A";
}

function setModeUI(m) {
  modeManual.checked = (m === 0);
  modeAuto.checked = (m === 1);
  modeSchedule.checked = (m === 2);

  // Manual switch usable only in MANUAL
  manualSwitch.disabled = (m !== 0);

  modeStat.textContent = modeToText(m);
}

// ===== Realtime read: sensors =====
database.ref(PATH + "/Soid").on("value", function(snapshot) {
  let v = snapshot.val();
  soilStat.innerHTML = (v !== null) ? Number(v).toFixed(0) : "--";
});

database.ref(PATH + "/Volume").on("value", function(snapshot) {
  let v = snapshot.val();
  volumeStat.innerHTML = (v !== null) ? Number(v).toFixed(0) : "--";
});

// ===== Realtime read: settings/mode =====
database.ref(PATH + "/Threshold").on("value", function(snapshot) {
  let v = snapshot.val();
  if (v !== null) {
    thresholdStat.innerHTML = Number(v).toFixed(0);
    thresholdSlider.value = Number(v);
    thresholdDisplay.innerHTML = `${Number(v).toFixed(0)}%`;
  }
});

database.ref(PATH + "/PumpSeconds").on("value", function(snapshot) {
  let v = snapshot.val();
  if (v !== null) pumpSecondsInput.value = Number(v);
});

database.ref(PATH + "/Mode").on("value", function(snapshot) {
  let m = snapshot.val();
  if (m === null) m = 0;
  setModeUI(Number(m));
});

database.ref(PATH + "/ManualSwitch").on("value", function(snapshot) {
  let v = snapshot.val();
  manualSwitch.checked = (v === 1);
});

database.ref(PATH + "/Schedule/Date").on("value", function(snapshot) {
  let v = snapshot.val();
  if (v) scheduleDate.value = v;
});

database.ref(PATH + "/Schedule/Time").on("value", function(snapshot) {
  let v = snapshot.val();
  if (v) scheduleTime.value = v;
});

// ===== Write: Mode =====
modeManual.addEventListener("change", function() {
  if (modeManual.checked) database.ref(PATH).update({ Mode: 0 });
});

modeAuto.addEventListener("change", function() {
  if (modeAuto.checked) database.ref(PATH).update({ Mode: 1 });
});

modeSchedule.addEventListener("change", function() {
  if (modeSchedule.checked) database.ref(PATH).update({ Mode: 2 });
});

// ===== Write: Manual switch =====
// NOTE: PumpSeconds applies to MANUAL (mode 0) + SCHEDULE (mode 2) on ESP side.
manualSwitch.addEventListener("change", function() {
  let state = manualSwitch.checked ? 1 : 0;
  database.ref(PATH).update({
    Mode: 0,
    ManualSwitch: state
  });
});

// ===== Write: Schedule =====
scheduleDate.addEventListener("change", function() {
  database.ref(PATH + "/Schedule").update({ Date: scheduleDate.value });
});

scheduleTime.addEventListener("change", function() {
  database.ref(PATH + "/Schedule").update({ Time: scheduleTime.value });
});

// ===== Threshold slider display =====
// Threshold applies to AUTO only on ESP side.
thresholdSlider.addEventListener("input", function() {
  thresholdDisplay.innerHTML = `${thresholdSlider.value}%`;
});

thresholdSlider.addEventListener("change", function() {
  database.ref(PATH).update({ Threshold: Number(thresholdSlider.value) });
});

// ===== Save settings =====
saveBtn.addEventListener("click", function() {
  const th = Number(thresholdSlider.value);
  const ps = Number(pumpSecondsInput.value);

  database.ref(PATH).update({
    Threshold: th,
    PumpSeconds: ps
  });
});
