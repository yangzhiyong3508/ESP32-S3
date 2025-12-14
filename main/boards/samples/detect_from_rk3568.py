#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import asyncio
import websockets
import numpy as np
import cv2
import threading
import time
import torch
import warnings
from queue import Queue, Empty
from objdetector import Detector
from WebsocketMessageSender import WebSocketMessenger
from DirectionMessenger import DirectionMessenger
from objtracker import reset_tracker

np.float = float
np.int = int
np.bool = bool

WIDTH, HEIGHT = 640, 480
MAX_MSG_SIZE = 50 * 1024 * 1024
FRAME_RATE = 30
SHOW_WINDOW = True
warnings.filterwarnings("ignore", category=FutureWarning, module="torch")

latest_frame_queue = Queue(maxsize=1)
latest_frame_to_send = None
frame_lock = threading.Lock()

def decode_frame(raw: bytes):
    L = len(raw)
    if L >= 3 and raw[0:3] == b'\xff\xd8\xff':
        arr = np.frombuffer(raw, dtype=np.uint8)
        return cv2.imdecode(arr, cv2.IMREAD_COLOR)
    elif L == WIDTH * HEIGHT * 4:
        arr = np.frombuffer(raw, dtype=np.uint8).reshape((HEIGHT, WIDTH, 4))
        return cv2.cvtColor(arr, cv2.COLOR_RGBA2BGR)
    elif L == WIDTH * HEIGHT * 3:
        arr = np.frombuffer(raw, dtype=np.uint8).reshape((HEIGHT, WIDTH, 3))
        return cv2.cvtColor(arr, cv2.COLOR_RGB2BGR)
    return None

def encode_frame(frame):
    success, encoded = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 80])
    if success:
        return encoded.tobytes()
    return None

def update_frame_to_send(frame):
    global latest_frame_to_send
    with frame_lock:
        latest_frame_to_send = frame.copy() if frame is not None else None

def get_frame_to_send():
    global latest_frame_to_send
    with frame_lock:
        return latest_frame_to_send.copy() if latest_frame_to_send is not None else None


# ============================================================
# 摄像头推流客户端
# ============================================================
async def handle_client(websocket):
    client = websocket.remote_address
    print(f"[WS] New camera client connected: {client}")

    try:
        reset_tracker()
        print("[Tracker] Tracker reset. IDs start from 1.")
    except Exception as e:
        print(f"[Tracker] Reset error: {e}")

    try:
        async for data in websocket:
            if isinstance(data, bytes):
                img = decode_frame(data)
                if img is not None:
                    try:
                        if latest_frame_queue.full():
                            latest_frame_queue.get_nowait()
                        latest_frame_queue.put_nowait(img)
                        update_frame_to_send(img)
                    except:
                        pass
    except websockets.exceptions.ConnectionClosed:
        print(f"[WS] Camera client disconnected: {client}")
        reset_tracker()
        print("[Tracker] Tracker reset after camera disconnection.")
    except Exception as e:
        print(f"[WS] Client error {client}: {e}")
        reset_tracker()

async def websocket_server():
    print("[WS] Starting camera video server ws://0.0.0.0:8765")
    async with websockets.serve(handle_client, "0.0.0.0", 8765, max_size=MAX_MSG_SIZE):
        await asyncio.Future()


# ============================================================
# 转发端客户端（前端查看者）
# ============================================================
class FrameBroadcaster:
    def __init__(self):
        self.connections = set()
        self.lock = asyncio.Lock()

    async def register(self, websocket):
        async with self.lock:
            self.connections.add(websocket)
            print(f"[Forward] New viewer connected. Total: {len(self.connections)}")

    async def unregister(self, websocket):
        async with self.lock:
            self.connections.remove(websocket)
            print(f"[Forward] Viewer disconnected. Total: {len(self.connections)}")

    async def broadcast_frame(self, frame_data):
        if not frame_data:
            return
        disconnected = set()
        async with self.lock:
            for connection in self.connections:
                try:
                    await connection.send(frame_data)
                except websockets.exceptions.ConnectionClosed:
                    disconnected.add(connection)
                except Exception as e:
                    print(f"[Forward] Send error: {e}")
                    disconnected.add(connection)
            for connection in disconnected:
                self.connections.remove(connection)
            if disconnected:
                print(f"[Forward] Removed {len(disconnected)} closed connections. Current: {len(self.connections)}")

frame_broadcaster = FrameBroadcaster()

async def handle_forward_client(websocket):
    client = websocket.remote_address
    print(f"[Forward] New forward client connected: {client}")

    # 当新前端连接时重置跟踪器
    try:
        reset_tracker()
        print("[Tracker] Tracker reset triggered by forward client connection. IDs start from 1.")
    except Exception as e:
        print(f"[Tracker] Reset error (forward client): {e}")

    await frame_broadcaster.register(websocket)
    try:
        await websocket.wait_closed()
    finally:
        await frame_broadcaster.unregister(websocket)
        try:
            reset_tracker()
            print("[Tracker] Tracker reset after forward client disconnection.")
        except Exception as e:
            print(f"[Tracker] Reset error after forward disconnection: {e}")

async def forward_server():
    print("[Forward] Starting video forward server ws://0.0.0.0:8766")
    async with websockets.serve(handle_forward_client, "0.0.0.0", 8766, max_size=MAX_MSG_SIZE):
        await asyncio.Future()


# ============================================================
# 视频转发线程
# ============================================================
def frame_forward_thread():
    print("[Forward] Frame forward thread started")
    last_frame_time = 0
    forward_fps = 30
    while True:
        current_time = time.time()
        if current_time - last_frame_time < 1.0 / forward_fps:
            time.sleep(0.001)
            continue
        frame = get_frame_to_send()
        if frame is not None:
            frame_data = encode_frame(frame)
            if frame_data:
                asyncio.run_coroutine_threadsafe(
                    frame_broadcaster.broadcast_frame(frame_data),
                    forward_loop
                )
        last_frame_time = current_time
        time.sleep(0.001)


# ============================================================
# 检测线程
# ============================================================
def detection_thread():
    det = Detector()
    print(f"[Detection] Using device: {det.device}")

    ws_sender = WebSocketMessenger(port=8910)
    ws_sender.start()
    ws_sender.client_connected_event.wait()
    print("[Direction] Control client connected")
    direction_controller = DirectionMessenger(ws_sender)

    last_time = 0
    last_fps_time = time.time()
    fps_counter = 0
    current_fps = 0

    if SHOW_WINDOW:
        window_name = "Detection Preview"
        cv2.namedWindow(window_name, cv2.WINDOW_AUTOSIZE)

    while True:
        try:
            frame = latest_frame_queue.get(timeout=0.05)
        except Empty:
            time.sleep(0.001)
            continue

        now = time.time()
        if now - last_time < 1.0 / FRAME_RATE:
            continue
        last_time = now

        with torch.no_grad():
            with torch.amp.autocast(device_type='cuda'):
                result_dict = det.feedCap(frame)

        result_frame = result_dict['frame']
        obj_bboxes = result_dict['obj_bboxes']
        update_frame_to_send(result_frame)

        target_angle = None
        target_id = None
        if obj_bboxes and len(obj_bboxes) > 0:
            first_bbox = obj_bboxes[0]
            x1, y1, x2, y2, label, conf, *rest = first_bbox
            target_angle = direction_controller.get_angle(x1, x2, result_frame.shape[1])
            target_angle = round(target_angle, 2)
            target_id = rest[0] if len(rest) > 0 else -1
            direction_controller.send_direction_by_bbox(obj_bboxes, result_frame.shape[1])

        fps_counter += 1
        if time.time() - last_fps_time >= 1.0:
            current_fps = fps_counter
            fps_counter = 0
            last_fps_time = time.time()

        if SHOW_WINDOW:
            current_time_str = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
            text_line1 = f"{current_time_str}"
            text_line2 = f"Angle: {target_angle if target_angle is not None else '---'} | ID: {target_id if target_id is not None else '---'}"
            text_line3 = f"FPS: {current_fps}"

            cv2.putText(result_frame, text_line1, (10, 30),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 0), 2)
            cv2.putText(result_frame, text_line2, (10, 60),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 255), 2)
            cv2.putText(result_frame, text_line3, (10, 90),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 0), 2)

            cv2.imshow(window_name, result_frame)
            if cv2.waitKey(1) == 27:
                break

    if SHOW_WINDOW:
        cv2.destroyAllWindows()


# ============================================================
# 主入口
# ============================================================
if __name__ == "__main__":
    reset_tracker()
    forward_loop = asyncio.new_event_loop()

    def run_forward_server():
        asyncio.set_event_loop(forward_loop)
        forward_loop.run_until_complete(forward_server())

    forward_thread = threading.Thread(target=run_forward_server, daemon=True)
    forward_thread.start()

    threading.Thread(target=frame_forward_thread, daemon=True).start()
    threading.Thread(target=detection_thread, daemon=True).start()

    asyncio.run(websocket_server())