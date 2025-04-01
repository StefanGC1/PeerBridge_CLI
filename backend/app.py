import asyncio
import websockets
import json

class User:
    def __init__(self, websocket, username, ip, port):
        self.websocket = websocket
        self.username = username
        self.ip = ip
        self.port = port


connected_users = {}

async def handler(websocket):
    print(f"[+] New connection from {websocket.remote_address}")
    user = None

    try:
        async for message in websocket:
            print(f"[Client] -> [Server] {message}")
            try:
                data = json.loads(message)
            except json.JSONDecodeError:
                continue

            msg_type = data.get("type")

            if  msg_type == "register":
                    username = data.get("username")
                    ip = data.get("ip")
                    port = data.get("port")
                    if username and ip and port:
                        user = User(websocket, username, ip, port)
                        connected_users[websocket] = user
                        print(f"[+] Registered {username} @ {ip}:{port}")
                        await websocket.send(json.dumps({
                            "type": "register-ack",
                            "message": f"Registered as {username}"
                        }))

            elif msg_type == "get-peer":
                target = data.get("username")
                peer = next((u for u in connected_users.values() if u.username == target), None)
                if peer:
                    await websocket.send(json.dumps({
                        "type": "peer-info",
                        "username": peer.username,
                        "ip": peer.ip,
                        "port": peer.port
                    }))
                else:
                    await websocket.send(json.dumps({
                        "type": "error",
                        "message": f"User '{target}' not found or not online."
                    }))

    except websockets.exceptions.ConnectionClosed:
        print(f"[-] Disconnected: {websocket.remote_address}")
    finally:
        if websocket in connected_users:
            print(f"[-] Removing user {connected_users[websocket].username}")
            del connected_users[websocket]

async def main():
    async with websockets.serve(handler, "0.0.0.0", 5000):
        print("WebSocket Server running on port 5000...")
        await asyncio.Future()  # Run forever

if __name__ == "__main__":
    asyncio.run(main())

