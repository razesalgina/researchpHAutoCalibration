

// Arduino-compatible port of ESP32 pH code
// Assumptions: Arduino Uno/Nano (10-bit ADC, Vref ~5.0V).
// If you use a 3.3V board, change VREF_MV to 3300.

#include <EEPROM.h>
#include <DFRobot_PH.h>
// If you have a generic DFRobot pH library for Arduino, include it.
// Replace with the exact library name you installed, e.g. <DFRobot_PH.h>
DFRobot_PH ph;
// ADC / voltage reference (adjust if your board uses 3.3V)
#define ADC_RESOLUTION 1023.0   // 10-bit ADC
#define VREF_MV 5000.0          // reference voltage in mV (5V Arduino)

// Pins
const int PH_PIN = A1;        // analog pin for pH probe
const int TEMP_PIN = A0;      // analog pin for temp sensor (LM35). Optional.

// Temperature handling
#define USE_TEMP_SENSOR true   // set false to use fixed temperature (temperatureDefault)
float temperatureDefault = 25.0; // used when USE_TEMP_SENSOR == false

// EEPROM addresses for calibration
const int EEPROM_ADDR_SLOPE = 0;      // float (4 bytes)
const int EEPROM_ADDR_INTERCEPT = 4;  // float

// Two-point calibration params
float calSlope = 0.0;
float calIntercept = 0.0;
bool haveCalibration = false;

// working variables
float voltage_mV = 0.0;
float phValue = 0.0;
float temperatureC = 25.0;

void setup() {
  Serial.begin(115200);
  delay(50);
  ph.begin();

  // load calibration from EEPROM
  EEPROM.get(EEPROM_ADDR_SLOPE, calSlope);
  EEPROM.get(EEPROM_ADDR_INTERCEPT, calIntercept);

  if (isfinite(calSlope) && isfinite(calIntercept)) {
    if (calIntercept > -20 && calIntercept < 40) {
      haveCalibration = true;
      Serial.println("Loaded two-point calibration from EEPROM:");
      Serial.print("  slope = "); Serial.println(calSlope, 6);
      Serial.print("  intercept = "); Serial.println(calIntercept, 6);
    }
  }
  Serial.println("Ready. Commands: CAL2, CAL3, SHOWCAL, CLEARCAL, CAL3 <mode>, CAP, SAVE, IGNORE, CANCEL");
}

// Read temperature from LM35 on TEMP_PIN
float readTemperature() {
#if USE_TEMP_SENSOR
  // LM35: 10mV per degC. analogRead->mV conversion then /10
  int raw = analogRead(TEMP_PIN);
  float mv = (raw / ADC_RESOLUTION) * VREF_MV;
  float tempC = mv / 10.0;
  // basic sanity clamp
  if (tempC < -40 || tempC > 150) return temperatureDefault;
  return tempC;
#else
  return temperatureDefault;
#endif
}

// Helper: averaged voltage in millivolts from PH_PIN
float readPHVoltage(int samples = 10, int delayMs = 10) {
  long sum = 0;
  for (int i = 0; i < samples; ++i) {
    sum += analogRead(PH_PIN);
    delay(delayMs);
  }
  float avg = (float)sum / samples;
  return (avg / ADC_RESOLUTION) * VREF_MV; // mV    
}

void doTwoPointCalibration() {
  Serial.println("Starting two-point calibration.");
  Serial.println("Place probe in pH 4.01 buffer, type CAP and Enter to capture.");
  while (true) {
    if (Serial.available()) {
      String s = Serial.readStringUntil('\n'); s.trim();
      if (s.equalsIgnoreCase("CAP")) break;
      if (s.equalsIgnoreCase("CANCEL")) { Serial.println("Calibration cancelled."); return; }
    }
    delay(10);
  }
  float v1 = readPHVoltage();
  Serial.print("Captured V at pH 4.01: "); Serial.print(v1, 4); Serial.println(" mV");

  Serial.println("Rinse probe, place in pH 9.18 buffer, type CAP and Enter to capture.");
  while (true) {
    if (Serial.available()) {
      String s = Serial.readStringUntil('\n'); s.trim();
      if (s.equalsIgnoreCase("CAP")) break;
      if (s.equalsIgnoreCase("CANCEL")) { Serial.println("Calibration cancelled."); return; }
    }
    delay(10);
  }
  float v2 = readPHVoltage();
  Serial.print("Captured V at pH 9.18: "); Serial.print(v2, 4); Serial.println(" mV");

  if (fabs(v2 - v1) < 1e-6) {
    Serial.println("Error: captured voltages are too close. Aborting.");
    return;
  }

  const float pH1 = 4.01;
  const float pH2 = 9.18;
  calSlope = (pH2 - pH1) / (v2 - v1);
  calIntercept = pH1 - calSlope * v1;

  EEPROM.put(EEPROM_ADDR_SLOPE, calSlope);
  EEPROM.put(EEPROM_ADDR_INTERCEPT, calIntercept);

  haveCalibration = true;
  Serial.println("Two-point calibration saved:");
  Serial.print("  slope = "); Serial.println(calSlope, 6);
  Serial.print("  intercept = "); Serial.println(calIntercept, 6);
}

void handleSerialCommands() {
    if (!Serial.available()) return;
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.equalsIgnoreCase("CAL2")) {
        doTwoPointCalibration();
    } else if (cmd.equalsIgnoreCase("CAL3")) {
        // Simple 3-point calibration: capture one sample each from 4.01, 6.86, 9.18 (in that order)
        Serial.println("Starting simple 3-point calibration (4.01, 6.86, 9.18).");
        float buffers[3] = {4.01, 6.86, 9.18};
        float v[3];
        for (int i = 0; i < 3; ++i) {
            Serial.print("Place probe in pH "); Serial.print(buffers[i]); Serial.println(" buffer and type CAP to capture");
            while (true) {
                if (Serial.available()) {
                    String s = Serial.readStringUntil('\n'); s.trim();
                    if (s.equalsIgnoreCase("CAP")) break;
                    if (s.equalsIgnoreCase("CANCEL")) { Serial.println("Calibration cancelled."); return; }
                }
                delay(10);
            }
            v[i] = readPHVoltage();
            Serial.print("Captured V: "); Serial.print(v[i], 4); Serial.print(" mV for pH "); Serial.println(buffers[i]);
        }
        // compute linear fit using three points
        float sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
        int n = 3;
        for (int i = 0; i < n; ++i) {
            float x = v[i];
            float y = buffers[i];
            sumX += x; sumY += y; sumXY += x * y; sumX2 += x * x;
        }
        float denom = n * sumX2 - sumX * sumX;
        if (fabs(denom) < 1e-9) { Serial.println("Calibration failed: insufficient variation."); return; }
        calSlope = (n * sumXY - sumX * sumY) / denom;
        calIntercept = (sumY - calSlope * sumX) / n;
        EEPROM.put(EEPROM_ADDR_SLOPE, calSlope);
        EEPROM.put(EEPROM_ADDR_INTERCEPT, calIntercept);
        haveCalibration = true;
        Serial.println("Three-point calibration saved:");
        Serial.print("  slope = "); Serial.println(calSlope, 6);
        Serial.print("  intercept = "); Serial.println(calIntercept, 6);
    } else if (cmd.startsWith("CAL3 ") || cmd.equalsIgnoreCase("BERULANG") || cmd.equalsIgnoreCase("BERUBAH NAIK") || cmd.equalsIgnoreCase("BERUBAH TURUN")) {
        // testing modes: take 9 samples according to mode but do NOT automatically save calibration
        String mode;
        if (cmd.length() > 4) {
            mode = cmd.substring(4);
            mode.trim();
        } else {
            Serial.println("CAL3 <mode> where mode is: berulang | naik | turun");
            return;
        }
        String m = mode;
        m.toLowerCase();
        bool isBerulang = (m.indexOf("berulang") >= 0);
        bool isNaik = (m.indexOf("naik") >= 0 || m.indexOf("berubah naik") >= 0);
        bool isTurun = (m.indexOf("turun") >= 0 || m.indexOf("berubah turun") >= 0);
        if (!isBerulang && !isNaik && !isTurun) { Serial.println("Unknown mode"); return; }
        float bufA = 6.86, bufB = 4.01, bufC = 9.18;
        const int totalSamples = 9;
        float samplesV[totalSamples];
        float samplesPH[totalSamples];
        int idx = 0;
        if (isBerulang) {
            float order[3] = {bufB, bufA, bufC};
            for (int b = 0; b < 3; ++b) for (int rep = 0; rep < 3; ++rep) {
                Serial.print("Place probe in pH "); Serial.print(order[b]); Serial.println(" buffer and type CAP to capture");
                while (true) { if (Serial.available()) { String s = Serial.readStringUntil('\n'); s.trim(); if (s.equalsIgnoreCase("CAP")) break; if (s.equalsIgnoreCase("CANCEL")) { Serial.println("Calibration cancelled."); return; } } delay(10); }
                float v = readPHVoltage(); samplesV[idx] = v; samplesPH[idx] = order[b]; Serial.print("Captured V: "); Serial.print(v,4); Serial.print(" mV for actual pH "); Serial.print(order[b]); Serial.print(" with pH calculated: "); Serial.println(calSlope * v + calIntercept ,4); idx++; }
        } else if (isNaik) {
            float order[3] = {bufB, bufA, bufC};
            for (int rep = 0; rep < 3; ++rep) for (int b = 0; b < 3; ++b) {
                Serial.print("Place probe in pH "); Serial.print(order[b]); Serial.println(" buffer and type CAP to capture");
                while (true) { if (Serial.available()) { String s = Serial.readStringUntil('\n'); s.trim(); if (s.equalsIgnoreCase("CAP")) break; if (s.equalsIgnoreCase("CANCEL")) { Serial.println("Calibration cancelled."); return; } } delay(10); }
                float v = readPHVoltage(); samplesV[idx] = v; samplesPH[idx] = order[b]; Serial.print("Captured V: "); Serial.print(v,4); Serial.print(" mV for actual pH "); Serial.print(order[b]); Serial.print(" with pH calculated: "); Serial.println(calSlope * v + calIntercept ,4); idx++; }
        } else if (isTurun) {
            float order[3] = {bufC, bufA, bufB};
            for (int rep = 0; rep < 3; ++rep) for (int b = 0; b < 3; ++b) {
                Serial.print("Place probe in pH "); Serial.print(order[b]); Serial.println(" buffer and type CAP to capture");
                while (true) { if (Serial.available()) { String s = Serial.readStringUntil('\n'); s.trim(); if (s.equalsIgnoreCase("CAP")) break; if (s.equalsIgnoreCase("CANCEL")) { Serial.println("Calibration cancelled."); return; } } delay(10); }
                float v = readPHVoltage(); samplesV[idx] = v; samplesPH[idx] = order[b]; Serial.print("Captured V: "); Serial.print(v,4); Serial.print(" mV for actual pH "); Serial.print(order[b]); Serial.print(" with pH calculated: "); Serial.println(calSlope * v + calIntercept ,4); idx++; }
        }
        // compute linear fit but DO NOT store—print results and offer to save
        float sumX=0,sumY=0,sumXY=0,sumX2=0; int n=idx;
        for(int i=0;i<n;i++){ float x=samplesV[i], y=samplesPH[i]; sumX+=x; sumY+=y; sumXY+=x*y; sumX2+=x*x; }
        float denom = n*sumX2 - sumX*sumX; if (fabs(denom) < 1e-9) { Serial.println("Fit failed"); return; }
        float slope = (n*sumXY - sumX*sumY)/denom; float intercept = (sumY - slope*sumX)/n;
        Serial.println("Calibration FIT (TEST MODE):"); Serial.print(" slope="); Serial.println(slope,6); Serial.print(" intercept="); Serial.println(intercept,6);
        Serial.println("Type SAVE to store these values as calibration, or IGNORE to discard.");
        // wait for SAVE/IGNORE
        while(true){ if (Serial.available()){ String r = Serial.readStringUntil('\n'); r.trim(); if (r.equalsIgnoreCase("SAVE")){ calSlope=slope; calIntercept=intercept; EEPROM.put(EEPROM_ADDR_SLOPE, calSlope); EEPROM.put(EEPROM_ADDR_INTERCEPT, calIntercept); haveCalibration=true; Serial.println("Calibration saved from test fit."); break; } else if (r.equalsIgnoreCase("IGNORE")){ Serial.println("Calibration discarded."); break; } } delay(10); }
    } else if (cmd.equalsIgnoreCase("SHOWCAL")) {
        if (haveCalibration) {
            Serial.println("Calibration parameters:");
            Serial.print("  slope = "); Serial.println(calSlope, 6);
            Serial.print("  intercept = "); Serial.println(calIntercept, 6);
        } else {
            Serial.println("No calibration stored.");
        }
    } else if (cmd.equalsIgnoreCase("CLEARCAL")) {
        calSlope = 0.0; calIntercept = 0.0; haveCalibration = false;
        float z = 0.0;
        EEPROM.put(EEPROM_ADDR_SLOPE, z);
        EEPROM.put(EEPROM_ADDR_INTERCEPT, z);
        Serial.println("Calibration cleared.");
    }
}

void loop() {
  static unsigned long timepoint = millis();
  if (millis() - timepoint > 1000U) {
    timepoint = millis();

    voltage_mV = readPHVoltage();
    Serial.print("voltage (mV): ");
    Serial.println(voltage_mV, 4);

    temperatureC = readTemperature();
    Serial.print("temperature (C): ");
    Serial.println(temperatureC, 1);

    if (haveCalibration) {
      phValue = calSlope * voltage_mV + calIntercept;
    } else {
      phValue = ph.readPH(voltage_mV, temperatureC);
    }
    Serial.print("pH: ");
    Serial.println(phValue, 4);
  }

  handleSerialCommands();
}
