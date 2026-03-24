from flask import Flask, request, jsonify, render_template
from flask_cors import CORS
import xgboost as xgb
import pandas as pd
import numpy as np
from datetime import datetime

app = Flask(__name__)
CORS(app) # Enables the Dashboard to fetch data without security blocks

# --- 1. LOAD ML MODELS ---
# We use two models: one for Generation (Solar) and one for Demand (Load)
gen_model = xgb.XGBRegressor()
load_model = xgb.XGBRegressor()

try:
    gen_model.load_model('solar_model_5min.json')
    load_model.load_model('load_model_5min.json') # Trained on NITW demand data
    print("✅ Digital Twin Models Loaded Successfully")
except Exception as e:
    print(f"⚠️ Model Load Warning: {e}. Ensure .json files are in the directory.")

# --- 2. DATA STORAGE ---
# Stores the last 50 readings to provide history for the dashboard charts
telemetry_history = []

def calculate_soc(v_batt):
    """Converts 3S Li-ion Voltage (10.5V - 12.6V) to Percentage"""
    percentage = ((v_batt - 10.5) / (12.6 - 10.5)) * 100
    return round(max(0, min(100, percentage)), 1)

# --- 3. API ENDPOINTS ---

@app.route('/update', methods=['POST'])
def update_telemetry():
    """
    Endpoint for ESP32. 
    Expected JSON: {"vBatt": 12.1, "vSolar": 13.2, "ldr": 800, "curr": 0.05, "time": "10:30"}
    """
    data = request.json
    if not data:
        return jsonify({"status": "error", "message": "No data received"}), 400

    try:
        # A. Process Time for ML Features
        time_obj = datetime.strptime(data['time'], "%H:%M")
        hour_val = time_obj.hour + time_obj.minute / 60.0
        
        # B. Prepare Input for Models
        # Features: [Hour, Solar_Voltage, LDR_Intensity]
        features = pd.DataFrame([[hour_val, data['vSolar'], data['ldr']]], 
                                columns=['Hour', 'Solar_V', 'LDR'])
        
        # C. Generate 5-Minute Predictions
        pred_gen_val = float(gen_model.predict(features)[0])
        pred_load_val = float(load_model.predict(features)[0])
        
        # D. Logic to Determine Power Source (Matches your ESP32 Relay Logic)
        mode = "GRID"
        if data['ldr'] < 1500: # If it's Day
            if data['vSolar'] > 11.0:
                mode = "SOLAR"
            else:
                mode = "BATTERY"
        
        # E. Calculate Current Metrics
        actual_p_gen = round(data['vSolar'] * data['curr'], 3)
        actual_p_load = 0.65 # Constant LED prototype load (approx)
        soc_now = calculate_soc(data['vBatt'])
        
        # F. Create Unified Record
        record = {
            "time": data['time'],
            "vBatt": round(data['vBatt'], 2),
            "vSolar": round(data['vSolar'], 2),
            "ldr": data['ldr'],
            "actual_gen": actual_p_gen,
            "actual_load": actual_p_load,
            "pred_gen": round(pred_gen_val, 3),
            "pred_load": round(pred_load_val, 3),
            "soc": soc_now,
            "mode": mode
        }
        
        telemetry_history.append(record)
        
        # Keep only the last 50 data points to prevent memory overflow
        if len(telemetry_history) > 50:
            telemetry_history.pop(0)
            
        print(f"📊 Data Received: {data['time']} | Mode: {mode} | Gen: {actual_p_gen}W")
        return jsonify({"status": "success", "next_gen": record["pred_gen"]})

    except Exception as e:
        print(f"❌ Error processing data: {e}")
        return jsonify({"status": "error", "message": str(e)}), 500

@app.route('/data', methods=['GET'])
def get_history():
    """Endpoint for the Dashboard to fetch chart data"""
    return jsonify(telemetry_history)

@app.route('/')
def index():
    """Serves the dashboard HTML file"""
    return render_template('index.html')

# --- 4. START SERVER ---
if __name__ == '__main__':
    # This MUST be 0.0.0.0 to allow the ESP32 to connect!
    app.run(host='0.0.0.0', port=5000, debug=True)