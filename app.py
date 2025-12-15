# app.py
"""
Complete backend:
- Flask + Flask-SocketIO (threading)
- Serial read thread (Arduino) emits sensor_update events
- Fake sensor emitter if no serial
- Socket handlers: toggle_relay, set_auto, clear_manual
- /predict uses a Hugging Face image classification model (local inference)
"""

import os
import time
import json
import io
import threading
import traceback

from flask import Flask, render_template, request, jsonify
from flask_socketio import SocketIO, emit

# Serial (pyserial)
try:
    import serial
    import serial.tools.list_ports
except Exception as e:
    serial = None
    print("[IMPORT] pyserial not available:", e)

# Hugging Face / Torch
try:
    import torch
    from transformers import AutoImageProcessor, AutoModelForImageClassification
    from PIL import Image
except Exception as e:
    torch = None
    AutoImageProcessor = None
    AutoModelForImageClassification = None
    Image = None
    print("[IMPORT] HF/torch not available:", e)

# ---------------- CONFIG ----------------
SERIAL_PORT = "COM5"   # preferred port (change for your system or leave to auto-detect)
BAUDRATE = 115200
HF_MODEL_ID = "linkanjarad/mobilenet_v2_1.0_224-plant-disease-identification"
# HF_MODEL_ID can be changed to any HF image classification model ID
APP_HOST = "0.0.0.0"
APP_PORT = 5000
REPORT_INTERVAL_MS = 1000
# ----------------------------------------

app = Flask(__name__, static_folder="static", template_folder="templates")
socketio = SocketIO(app, cors_allowed_origins="*", async_mode="threading", logger=False, engineio_logger=False)

ser = None
hf_processor = None
hf_model = None
hf_device = None

# ---------------- HF model load ----------------
def load_hf_model():
    """Load HF image classification model (AutoImageProcessor + AutoModelForImageClassification)."""
    global hf_processor, hf_model, hf_device
    if AutoImageProcessor is None or AutoModelForImageClassification is None or torch is None:
        print("[HF] transformers/torch not installed; skipping HF model load.")
        return
    try:
        hf_device = "cuda" if torch.cuda.is_available() else "cpu"
        print(f"[HF] Loading model {HF_MODEL_ID} on device {hf_device} ... this may take a while.")
        hf_processor = AutoImageProcessor.from_pretrained(HF_MODEL_ID)
        hf_model = AutoModelForImageClassification.from_pretrained(HF_MODEL_ID)
        hf_model.to(hf_device)
        print("[HF] Model loaded. num_labels:", len(hf_model.config.id2label))
    except Exception as e:
        print("[HF] Failed to load HF model:", e)
        traceback.print_exc()
        hf_processor = None
        hf_model = None
        hf_device = None

# Call load once on startup
load_hf_model()

# ---------------- Serial helpers ----------------
def list_serial_ports():
    if serial is None:
        return []
    return list(serial.tools.list_ports.comports())

def try_open_port(port):
    if serial is None:
        return None
    try:
        s = serial.Serial(port, BAUDRATE, timeout=1)
        time.sleep(2)
        print(f"[SERIAL] Opened {port}")
        return s
    except Exception as e:
        print(f"[SERIAL] Could not open {port}:", e)
        return None

def init_serial_connection():
    global ser
    if serial is None:
        print("[SERIAL] pyserial not installed; skipping serial init.")
        return

    ports = list_serial_ports()
    if ports:
        print("[SERIAL] Available ports:")
        for p in ports:
            print(" ", p.device, "-", p.description)
    else:
        print("[SERIAL] No serial ports found.")

    if SERIAL_PORT:
        print(f"[SERIAL] Trying preferred port: {SERIAL_PORT}")
        ser = try_open_port(SERIAL_PORT)

    if ser is None and serial is not None:
        for p in serial.tools.list_ports.comports():
            if p.device == SERIAL_PORT:
                continue
            print(f"[SERIAL] Trying {p.device} ...")
            ser = try_open_port(p.device)
            if ser:
                break

    if ser:
        print("[SERIAL] Serial connection established.")
    else:
        print("[SERIAL] No serial connection. Running without hardware.")

# ---------------- Serial read thread ----------------
def read_serial_thread():
    """Read lines from serial, expect JSON lines, emit sensor_update or serial_line."""
    global ser
    if ser is None:
        print("[SERIAL] No serial to read from.")
        return
    print("[SERIAL] Serial read thread starting.")
    while True:
        try:
            raw = ser.readline().decode("utf-8", errors="ignore")
            if not raw:
                time.sleep(0.02)
                continue
            line = raw.strip()
            if not line:
                continue
            print("[SERIAL RAW] >", line)
            # try parse JSON
            try:
                data = json.loads(line)
                expected = {'mq','soil','temp','hum','relay'}
                if expected.issubset(set(data.keys())):
                    socketio.emit('sensor_update', data)
                else:
                    # emit raw if keys differ
                    socketio.emit('serial_line', {'line': line})
            except json.JSONDecodeError:
                socketio.emit('serial_line', {'line': line})
        except Exception as e:
            print("[SERIAL] Exception in read loop:", e)
            traceback.print_exc()
            time.sleep(1)

# ---------------- Fake sensor thread ----------------
def fake_sensor_thread():
    import random
    print("[FAKE] Starting fake sensor emitter.")
    while True:
        data = {
            "mq": random.randint(300, 700),
            "soil": random.randint(20, 80),
            "temp": round(random.uniform(22.0, 30.0), 1),
            "hum": round(random.uniform(35.0, 70.0), 1),
            "relay": 0
        }
        socketio.emit('sensor_update', data)
        time.sleep(1)

# ---------------- SocketIO handlers ----------------
@socketio.on('toggle_relay')
def handle_toggle(data):
    global ser
    try:
        state = int(data.get('state', 0))
    except Exception:
        state = 0
    cmd = f"RELAY:{1 if state else 0}\n"
    if ser:
        try:
            ser.write(cmd.encode('utf-8'))
            print("[SERIAL WRITE] ->", cmd.strip())
            emit('relay_ack', {'sent': state})
        except Exception as e:
            print("[SERIAL WRITE ERROR]", e)
            emit('relay_ack', {'error': str(e)})
    else:
        print("[SERIAL WRITE] No serial; would send:", cmd.strip())
        emit('relay_ack', {'error': 'serial not connected', 'would_send': cmd.strip()})

@socketio.on('set_auto')
def handle_set_auto(data):
    global ser
    try:
        state = int(data.get('state', 0))
    except Exception:
        state = 0
    cmd = f"AUTO:{1 if state else 0}\n"
    if ser:
        try:
            ser.write(cmd.encode('utf-8'))
            print("[SERIAL WRITE] ->", cmd.strip())
            emit('auto_ack', {'sent': state})
        except Exception as e:
            print("[SERIAL WRITE ERROR]", e)
            emit('auto_ack', {'error': str(e)})
    else:
        print("[SERIAL WRITE] No serial; would send:", cmd.strip())
        emit('auto_ack', {'error': 'serial not connected', 'would_send': cmd.strip()})

@socketio.on('clear_manual')
def handle_clear_manual():
    global ser
    cmd = "MANUAL:0\n"
    if ser:
        try:
            ser.write(cmd.encode('utf-8'))
            print("[SERIAL WRITE] ->", cmd.strip())
            emit('manual_cleared', {'ok': True})
        except Exception as e:
            print("[SERIAL WRITE ERROR]", e)
            emit('manual_cleared', {'error': str(e)})
    else:
        print("[SERIAL WRITE] No serial; would send:", cmd.strip())
        emit('manual_cleared', {'error': 'serial not connected', 'would_send': cmd.strip()})

# ---------------- Flask routes ----------------
@app.route('/')
def index():
    # Assumes templates/index.html exists (your dashboard UI)
    return render_template('index.html')

# ---------------- Predict endpoint using HF model ----------------
@app.route('/predict', methods=['POST'])
def predict():
    global hf_processor, hf_model, hf_device
    if hf_model is None or hf_processor is None:
        return jsonify({'error': 'HF model not loaded on server'}), 500
    if 'file' not in request.files:
        return jsonify({'error': 'no file part'}), 400
    f = request.files['file']
    if f.filename == '':
        return jsonify({'error': 'no selected file'}), 400

    # Read image bytes
    try:
        img_bytes = f.read()
        img = Image.open(io.BytesIO(img_bytes)).convert("RGB")
    except Exception as e:
        return jsonify({'error': f'invalid image: {e}'}), 400

    try:
        # Preprocess with HF processor (handles resizing / normalization)
        inputs = hf_processor(images=img, return_tensors="pt")
        # Move tensors to device
        if hf_device == "cuda":
            inputs = {k: v.cuda() for k, v in inputs.items()}
        else:
            inputs = {k: v for k, v in inputs.items()}

        with torch.no_grad():
            outputs = hf_model(**inputs)
            logits = outputs.logits
            probs = torch.softmax(logits, dim=-1)[0].cpu().numpy()

        top_idx = int(probs.argmax())
        top_conf = float(probs[top_idx])
        label = hf_model.config.id2label.get(top_idx, str(top_idx))

        # Optionally return top-k probabilities (small)
        # topk = sorted([(i, float(p)) for i, p in enumerate(probs)], key=lambda x: x[1], reverse=True)[:5]
        return jsonify({'label': label, 'confidence': top_conf, 'all_probs': probs.tolist()})
    except Exception as e:
        traceback.print_exc()
        return jsonify({'error': f'prediction error: {e}'}), 500

# ---------------- Startup helpers ----------------
def start_background_threads():
    init_serial_connection()
    if ser:
        thr = threading.Thread(target=read_serial_thread, daemon=True)
        thr.start()
        print("[MAIN] Serial read thread started.")
    else:
        thr = threading.Thread(target=fake_sensor_thread, daemon=True)
        thr.start()
        print("[MAIN] Fake sensor thread started.")

# ---------------- Main ----------------
if __name__ == '__main__':
    print("[MAIN] Starting app...")
    start_background_threads()
    # start Flask + SocketIO server
    socketio.run(app, host=APP_HOST, port=APP_PORT)
