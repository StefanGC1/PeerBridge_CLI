import asyncio
import websockets
import json
import uuid

class User:
    def __init__(self, websocket, username, ip, port):
        self.websocket = websocket
        self.username = username
        self.ip = ip
        self.port = port
        self.pending_chat_request = None

# Store connected users
connected_users = {}
# Map usernames to users for faster lookups
username_to_user = {}

async def handler(websocket):
    print(f"[+] New connection from {websocket.remote_address}")
    user = None
    client_id = str(uuid.uuid4())[:8]

    try:
        async for message in websocket:
            print(f"[Client {client_id}] -> [Server] {message}")
            try:
                data = json.loads(message)
            except json.JSONDecodeError:
                print(f"[!] Error: Invalid JSON received from client {client_id}")
                continue

            msg_type = data.get("type")

            if msg_type == "greeting":
                # Respond to greeting message
                await websocket.send(json.dumps({
                    "type": "greet-back",
                    "message": "Hello from the signaling server!"
                }))
            
            elif msg_type == "register":
                username = data.get("username")
                ip = data.get("ip")
                port = data.get("port")
                
                # Check if username already exists
                if username in username_to_user:
                    await websocket.send(json.dumps({
                        "type": "error",
                        "message": f"Username '{username}' is already taken."
                    }))
                    continue
                
                if username and ip and port:
                    user = User(websocket, username, ip, port)
                    connected_users[websocket] = user
                    username_to_user[username] = user
                    print(f"[+] Registered {username} @ {ip}:{port}")
                    await websocket.send(json.dumps({
                        "type": "register-ack",
                        "message": f"Registered as {username}"
                    }))
            
            elif msg_type == "get-name":
                if user:
                    await websocket.send(json.dumps({
                        "type": "your-name",
                        "username": user.username
                    }))
                else:
                    await websocket.send(json.dumps({
                        "type": "error",
                        "message": "You are not registered yet."
                    }))
            
            elif msg_type == "get-peer":
                target = data.get("username")
                peer = username_to_user.get(target)
                
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
            
            elif msg_type == "start-chat":
                if not user:
                    await websocket.send(json.dumps({
                        "type": "error",
                        "message": "You must register before starting a chat."
                    }))
                    continue
                
                target_username = data.get("target")
                target_user = username_to_user.get(target_username)
                
                if not target_user:
                    await websocket.send(json.dumps({
                        "type": "error",
                        "message": f"User '{target_username}' not found or not online."
                    }))
                    continue
                
                # Send chat request to target user
                try:
                    await target_user.websocket.send(json.dumps({
                        "type": "chat-request",
                        "from": user.username
                    }))
                    
                    # Store pending request
                    target_user.pending_chat_request = user.username
                    
                    print(f"[+] Chat request from {user.username} to {target_username}")
                except Exception as e:
                    print(f"[!] Error sending chat request: {e}")
                    await websocket.send(json.dumps({
                        "type": "error",
                        "message": f"Failed to send chat request to {target_username}."
                    }))
            
            elif msg_type == "chat-accept":
                if not user or not user.pending_chat_request:
                    await websocket.send(json.dumps({
                        "type": "error",
                        "message": "No pending chat request to accept."
                    }))
                    continue
                
                requester_username = user.pending_chat_request
                requester = username_to_user.get(requester_username)
                
                if not requester:
                    await websocket.send(json.dumps({
                        "type": "error",
                        "message": f"User '{requester_username}' is no longer online."
                    }))
                    user.pending_chat_request = None
                    continue
                
                # Send connection info to requester
                await requester.websocket.send(json.dumps({
                    "type": "chat-init",
                    "username": user.username,
                    "ip": user.ip,
                    "port": user.port
                }))
                
                # Send connection info to accepter
                await user.websocket.send(json.dumps({
                    "type": "chat-init",
                    "username": requester.username,
                    "ip": requester.ip,
                    "port": requester.port
                }))
                
                print(f"[+] Chat initialized between {user.username} and {requester_username}")
                user.pending_chat_request = None
            
            elif msg_type == "chat-decline":
                if not user or not user.pending_chat_request:
                    await websocket.send(json.dumps({
                        "type": "error",
                        "message": "No pending chat request to decline."
                    }))
                    continue
                
                requester_username = user.pending_chat_request
                requester = username_to_user.get(requester_username)
                
                if requester:
                    await requester.websocket.send(json.dumps({
                        "type": "error",
                        "message": f"{user.username} declined your chat request."
                    }))
                
                print(f"[-] Chat request from {requester_username} to {user.username} declined")
                user.pending_chat_request = None

    except websockets.exceptions.ConnectionClosed:
        print(f"[-] Disconnected: {websocket.remote_address}")
    finally:
        if websocket in connected_users:
            user = connected_users[websocket]
            print(f"[-] Removing user {user.username}")
            
            # Clean up user data
            if user.username in username_to_user:
                del username_to_user[user.username]
            del connected_users[websocket]

async def main():
    async with websockets.serve(handler, "0.0.0.0", 8080):
        print("WebSocket Signaling Server running on port 8080...")
        await asyncio.Future()  # Run forever

if __name__ == "__main__":
    asyncio.run(main())