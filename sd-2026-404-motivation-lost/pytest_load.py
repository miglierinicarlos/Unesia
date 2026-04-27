import socket
import struct
import time
import threading
import random

# Message types based on MessageSerializer::MessageType
MSG_HEARTBEAT = 4
MSG_REGISTER = 1
MSG_ANALYZE = 5
MSG_MALFORMED_FAKE = 99

def build_message(msg_type, msg_id, payload=b'{"node_id":"python-tester"}'):
    version = 1
    frame_body = struct.pack(">BBII", version, msg_type, msg_id, len(payload)) + payload
    prefix = struct.pack(">I", len(frame_body))
    return prefix + frame_body

def client_worker(client_id):
    msg_id = 0
    end_time = time.time() + 300  # Run exactly 30 seconds
    
    while time.time() < end_time:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.connect(('127.0.0.1', 9027))
            
            s.sendall(build_message(MSG_REGISTER, msg_id))
            msg_id += 1
            time.sleep(0.1)

            # Spam 10 messages before recreating to avoid constant broken pipes
            for _ in range(10):
                if time.time() > end_time: break
                
                roll = random.random()
                m_type = MSG_HEARTBEAT if roll < 0.7 else (MSG_ANALYZE if roll < 0.9 else MSG_MALFORMED_FAKE)
                    
                s.sendall(build_message(m_type, msg_id))
                msg_id += 1
                time.sleep(random.uniform(0.1, 0.3))
                
            s.close()
        except Exception as e:
            time.sleep(0.5)

print("Iniciando prueba de carga con 10 clientes concurrentes (duración garantizada de 30s)...")
threads = []
for i in range(10):
    t = threading.Thread(target=client_worker, args=(i,))
    t.start()
    threads.append(t)
    
for t in threads:
    t.join()
print("Prueba de carga finalizada.")
