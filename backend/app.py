# backend/app.py
import os
import sys
import ctypes
from flask import Flask, send_from_directory, request, render_template
from flask_socketio import SocketIO, emit, join_room, leave_room
from stockfish import Stockfish
import chess
from dataclasses import dataclass
import time
import threading
import random
import uuid

# ==========================
# Initialize Flask & SocketIO
# ==========================
app = Flask(__name__)
# Use eventlet/gevent if installed; Flask-SocketIO will pick a working async mode automatically.
socketio = SocketIO(app, cors_allowed_origins="*")

# ---------------- Dynamic native lib loader ----------------
# Prefer macOS (.dylib), Linux (.so), then Windows (.dll)
if sys.platform == "darwin":
    lib_candidates = ["matchmaking.dylib", "matchmaking.so", "matchmaking.dll"]
elif sys.platform.startswith("linux"):
    lib_candidates = ["matchmaking.so", "matchmaking.dylib", "matchmaking.dll"]
else:
    lib_candidates = ["matchmaking.dll", "matchmaking.so", "matchmaking.dylib"]

dll_path = None
base = os.path.dirname(__file__)
for name in lib_candidates:
    p = os.path.join(base, name)
    if os.path.exists(p):
        dll_path = p
        break

if not dll_path:
    raise FileNotFoundError(
        f"No native library found. Checked: {', '.join(os.path.join(base, n) for n in lib_candidates)}"
    )

# Load the library
try:
    matchmaking = ctypes.CDLL(dll_path)
except OSError as e:
    raise RuntimeError(f"Failed to load native library {dll_path}: {e}")

print(f"🔧 Loaded native library: {dll_path}")

# Constants matching the C file
UUID_LEN = 64

# ctypes struct that mirrors the C `Match`
class CMatch(ctypes.Structure):
    _fields_ = [
        ('player1', ctypes.c_char * UUID_LEN),
        ('player2', ctypes.c_char * UUID_LEN),
        ('match_id', ctypes.c_int)
    ]

# --- Function signatures (based on matchmaking.c) ---
# init_engine
if hasattr(matchmaking, "init_engine"):
    matchmaking.init_engine.restype = None
    matchmaking.init_engine.argtypes = []
# set_max_rating_diff (optional)
if hasattr(matchmaking, "set_max_rating_diff"):
    matchmaking.set_max_rating_diff.restype = None
    matchmaking.set_max_rating_diff.argtypes = [ctypes.c_int]
# set_shard (optional)
if hasattr(matchmaking, "set_shard"):
    matchmaking.set_shard.restype = None
    matchmaking.set_shard.argtypes = [ctypes.c_int, ctypes.c_int]
# add_player
if hasattr(matchmaking, "add_player"):
    matchmaking.add_player.restype = None
    matchmaking.add_player.argtypes = [ctypes.c_char_p, ctypes.c_int]
# add_bot
if hasattr(matchmaking, "add_bot"):
    matchmaking.add_bot.restype = None
    matchmaking.add_bot.argtypes = [ctypes.c_char_p, ctypes.c_int]
# remove_player
if hasattr(matchmaking, "remove_player"):
    matchmaking.remove_player.restype = None
    matchmaking.remove_player.argtypes = [ctypes.c_char_p]
# get_match
if hasattr(matchmaking, "get_match"):
    matchmaking.get_match.restype = ctypes.c_int
    matchmaking.get_match.argtypes = [ctypes.POINTER(CMatch)]
# get_all_matches
if hasattr(matchmaking, "get_all_matches"):
    matchmaking.get_all_matches.restype = ctypes.c_int
    matchmaking.get_all_matches.argtypes = [ctypes.POINTER(CMatch), ctypes.c_int]
# register_match_callback (optional)
CALLBACK_TYPE = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int)
if hasattr(matchmaking, "register_match_callback"):
    matchmaking.register_match_callback.restype = None
    matchmaking.register_match_callback.argtypes = [CALLBACK_TYPE]
# free_engine (optional)
if hasattr(matchmaking, "free_engine"):
    matchmaking.free_engine.restype = None
    matchmaking.free_engine.argtypes = []

# Initialize engine (call once) if function exists
try:
    if hasattr(matchmaking, "init_engine"):
        matchmaking.init_engine()
        # set a sane default
        if hasattr(matchmaking, "set_max_rating_diff"):
            matchmaking.set_max_rating_diff(200)
except Exception as e:
    print("Warning: init_engine failed:", e)

# ------- Helper wrappers for Pythonic use ----------
def add_player(uuid_str: str, rating: int):
    if hasattr(matchmaking, "add_player"):
        matchmaking.add_player(uuid_str.encode('utf-8'), int(rating))
    else:
        raise RuntimeError("Native add_player not available")

def add_bot(uuid_str: str, rating: int):
    if hasattr(matchmaking, "add_bot"):
        matchmaking.add_bot(uuid_str.encode('utf-8'), int(rating))
    else:
        raise RuntimeError("Native add_bot not available")

def remove_player(uuid_str: str):
    if hasattr(matchmaking, "remove_player"):
        matchmaking.remove_player(uuid_str.encode('utf-8'))
    else:
        raise RuntimeError("Native remove_player not available")

def try_get_match():
    if not hasattr(matchmaking, "get_match"):
        raise RuntimeError("Native get_match not available")
    m = CMatch()
    ok = matchmaking.get_match(ctypes.byref(m))
    if not ok:
        return None
    p1 = bytes(m.player1).split(b'\x00',1)[0].decode('utf-8')
    p2 = bytes(m.player2).split(b'\x00',1)[0].decode('utf-8')
    return {'player1': p1, 'player2': p2, 'match_id': int(m.match_id)}

def get_all_matches(limit=100):
    if not hasattr(matchmaking, "get_all_matches"):
        raise RuntimeError("Native get_all_matches not available")
    arr_type = CMatch * limit
    arr = arr_type()
    count = matchmaking.get_all_matches(arr, limit)
    res = []
    for i in range(count):
        mm = arr[i]
        p1 = bytes(mm.player1).split(b'\x00',1)[0].decode('utf-8')
        p2 = bytes(mm.player2).split(b'\x00',1)[0].decode('utf-8')
        res.append({'match_id': int(mm.match_id), 'player1': p1, 'player2': p2})
    return res

# Optional: register Python callback to receive async match logs/events from the DLL logger thread.
_py_cb_ref = None  # keep reference so GC doesn't collect it
def register_match_callback(py_func):
    global _py_cb_ref
    if not hasattr(matchmaking, "register_match_callback"):
        raise RuntimeError("Native register_match_callback not available")
    def _cb(p1, p2, match_id, r1, r2):
        s1 = p1.decode('utf-8') if p1 else ''
        s2 = p2.decode('utf-8') if p2 else ''
        try:
            py_func(s1, s2, int(match_id), int(r1), int(r2))
        except Exception as e:
            print("Callback error:", e)
    _py_cb_ref = CALLBACK_TYPE(_cb)
    try:
        matchmaking.register_match_callback(_py_cb_ref)
    except Exception as e:
        print("register_match_callback failed:", e)

# ==========================
# Stockfish config (fixed path)
# ==========================
stockfish_path = "stockfish_win/stockfish/stockfish-windows-x86-64-avx2.exe"
difficulty_settings = {
    'easy':    {'depth': 5,  'skill': 5,  'threads': 1, 'thinking_time': 1000},
    'medium':  {'depth': 10, 'skill': 10, 'threads': 2, 'thinking_time': 2000},
    'hard':    {'depth': 15, 'skill': 15, 'threads': 3, 'thinking_time': 3000},
    'expert':  {'depth': 20, 'skill': 20, 'threads': 4, 'thinking_time': 3500},
}

def get_stockfish_instance(difficulty='expert'):
    config = difficulty_settings.get(difficulty, difficulty_settings['expert'])
    sf = Stockfish(
        path=stockfish_path,
        parameters={
            "Threads": config['threads'],
            "Minimum Thinking Time": config['thinking_time'],
            "Skill Level": config['skill'],
            "Contempt": 0,
            "Ponder": True
        }
    )
    sf.set_depth(config['depth'])
    return sf

# ==========================
# Global variables
# ==========================
clients_lock = threading.Lock()
clients = {}        # player_id -> {'sid': ..., 'skill': ..., 'name': ..., 'wants_bot': ..., 'auto_bot_after_timeout': ...}
active_games = {}   # player_id -> room name

player_timer_threads = {}  # player_id -> threading.Timer
toss_results = {}          # room -> {'winner': player_id, 'loser': player_id}
game_colors = {}           # room -> {player_id: 'white'/'black'}

@dataclass
class GameSession:
    board: chess.Board
    engine: Stockfish
    difficulty: str
    is_game_over: bool = False
    moves_history: list = None

game_sessions = {}  # player_id -> GameSession

# ==========================
# Auto-bot timer function
# ==========================
def start_bot_timer(player_id, difficulty='expert', timeout=30):
    def assign_bot():
        with clients_lock:
            if player_id in active_games:
                print(f"⚠️ Player {player_id} already in a game, skipping bot assign")
                return
            if player_id not in clients:
                print(f"⚠️ Player {player_id} disconnected, skipping bot assign")
                return

            sf = get_stockfish_instance(difficulty)
            board = chess.Board()
            session = GameSession(board=board, engine=sf, difficulty=difficulty, moves_history=[])
            game_sessions[player_id] = session
            room = f"bot_game_{player_id}"
            join_room(room, sid=clients[player_id]['sid'])
            active_games[player_id] = room

            print(f"🤖 Auto-bot assigned to {player_id} | Room: {room}")
            socketio.emit('start_game', {'color': 'white', 'opponent': 'Bot', 'opponent_skill': 0}, to=clients[player_id]['sid'])

    t = threading.Timer(timeout, assign_bot)
    player_timer_threads[player_id] = t
    t.start()

# ==========================
# Flask routes
# ==========================
@app.route('/')
def serve_index():
    return render_template('index.html')

# ==========================
# SocketIO events
# ==========================
@socketio.on('connect')
def handle_connect():
    print("✅ Client connected:", request.sid)
    player_id = str(uuid.uuid4())
    socketio.emit('set_player_id', {'player_id': player_id}, to=request.sid)

@socketio.on('disconnect')
def handle_disconnect():
    sid = request.sid
    print("❌ Client disconnected:", sid)

    with clients_lock:
        for player_id, info in list(clients.items()):
            if info['sid'] == sid:
                print(f"Removing player {player_id} from matchmaking")
                try:
                    remove_player(player_id)
                except Exception as e:
                    print("DLL remove_player error:", e)

                # Cancel bot timer if exists
                if player_timer_threads.get(player_id):
                    try:
                        player_timer_threads[player_id].cancel()
                    except Exception:
                        pass
                    player_timer_threads.pop(player_id, None)

                clients.pop(player_id)
                room = active_games.pop(player_id, None)
                if room:
                    try:
                        leave_room(room, sid=sid)
                    except Exception:
                        pass
                game_sessions.pop(player_id, None)
                break

@socketio.on('add_player')
def handle_add_player(data):
    player_id = data['player_id']
    skill = data['skill']
    name = data.get('name', f"Player{player_id}")
    wants_bot = data.get('bot', False)
    difficulty = data.get('difficulty', 'expert')
    auto_bot_after_timeout = data.get('auto_bot_after_timeout', False)
    timeout = data.get('timeout', 30)

    with clients_lock:
        if player_id in active_games:
            print(f"⚠️ Player {name} is already in active game")
            return

        clients[player_id] = {
            'sid': request.sid,
            'skill': skill,
            'name': name,
            'wants_bot': wants_bot,
            'auto_bot_after_timeout': auto_bot_after_timeout
        }

    # Manual Bot Mode
    if wants_bot and not auto_bot_after_timeout:
        sf = get_stockfish_instance(difficulty)
        board = chess.Board()
        session = GameSession(board=board, engine=sf, difficulty=difficulty, moves_history=[])
        game_sessions[player_id] = session

        room = f"bot_game_{player_id}"
        join_room(room, sid=request.sid)
        active_games[player_id] = room

        print(f"🤖 Player {name} matched with Bot | Room: {room}")
        socketio.emit('start_game', {'color': 'white', 'opponent': 'Bot', 'opponent_skill': 0}, to=request.sid)
        return

    # Auto Bot Timer
    if wants_bot and auto_bot_after_timeout:
        start_bot_timer(player_id, difficulty, timeout)

    # Human Matchmaking
    if not wants_bot:
        print(f"📥 Player {name} ({skill}) joined matchmaking")
        try:
            # use wrapper that encodes uuid
            add_player(player_id, skill)
        except Exception as e:
            print("DLL add_player error:", e)

        # Cancel existing bot timer if any
        if player_timer_threads.get(player_id):
            try:
                player_timer_threads[player_id].cancel()
            except Exception:
                pass
            player_timer_threads.pop(player_id, None)

        # Check for match immediately
        match = CMatch()
        try:
            if matchmaking.get_match(ctypes.byref(match)):
                p1 = bytes(match.player1).split(b'\x00', 1)[0].decode('utf-8')
                p2 = bytes(match.player2).split(b'\x00', 1)[0].decode('utf-8')
                match_id = match.match_id

                # Skip manual bot players
                if clients.get(p1, {}).get('wants_bot') and not clients[p1].get('auto_bot_after_timeout'):
                    return
                if clients.get(p2, {}).get('wants_bot') and not clients[p2].get('auto_bot_after_timeout'):
                    return

                # Skip if any player already in active game
                if p1 in active_games or p2 in active_games:
                    print(f"⚠️ One of the players already in game, skipping match {match_id}")
                    return

                info1 = clients.get(p1)
                info2 = clients.get(p2)

                if info1 and info2:
                    sid1, sid2 = info1['sid'], info2['sid']
                    name1, name2 = info1['name'], info2['name']
                    skill1, skill2 = info1['skill'], info2['skill']

                    room = f"game_{match_id}"
                    join_room(room, sid=sid1)
                    join_room(room, sid=sid2)
                    active_games[p1] = room
                    active_games[p2] = room

                    print(f"🎯 Match found! {name1} ({skill1}) vs {name2} ({skill2}) | Match ID: {match_id}")

                    socketio.emit('start_game', {'color': 'white', 'opponent': name2, 'opponent_skill': skill2}, to=sid1)
                    socketio.emit('start_game', {'color': 'black', 'opponent': name1, 'opponent_skill': skill1}, to=sid2)

                    # Toss result
                    winner_id = random.choice([p1, p2])
                    loser_id = p1 if winner_id == p2 else p2
                    toss_results[room] = {'winner': winner_id, 'loser': loser_id}
                    socketio.emit('toss_result', {'winner': winner_id, 'loser': loser_id}, room=room)

        except Exception as e:
            print("DLL get_match error:", e)

# ===== Color choice event =====
@socketio.on('color_choice')
def handle_color_choice(data):
    chosen_player = data.get('player_id')
    chosen_color = data.get('color')

    if chosen_player is None or chosen_color not in ('white', 'black'):
        print("Invalid color_choice data:", data)
        return

    # Find room
    found_room = None
    for room, tr in toss_results.items():
        if tr.get('winner') == chosen_player:
            found_room = room
            break

    if not found_room:
        print("No toss_result room found for winner:", chosen_player)
        return

    tr = toss_results[found_room]
    winner_id = tr['winner']
    loser_id = tr['loser']

    winner_color = chosen_color
    loser_color = 'black' if winner_color == 'white' else 'white'
    game_colors[found_room] = {winner_id: winner_color, loser_id: loser_color}

    with clients_lock:
        winner_sid = clients.get(winner_id, {}).get('sid')
        loser_sid = clients.get(loser_id, {}).get('sid')

    if winner_sid:
        socketio.emit('assign_color', {'color': winner_color}, to=winner_sid)
    if loser_sid:
        socketio.emit('assign_color', {'color': loser_color}, to=loser_sid)

    print(f"Colors assigned in {found_room}: {winner_id}->{winner_color}, {loser_id}->{loser_color}")
    toss_results.pop(found_room, None)

# ===== Move events =====
@socketio.on('move')
def handle_move(data):
    player_id = data.get("player_id")
    move = data.get("move")
    room = active_games.get(player_id)

    if room:
        emit("opponent_move", move, room=room, include_self=False)
        socketio.emit("play_move_sound", {}, room=room)
        print(f"♟️ Move from {player_id} in room {room}: {move}")
    else:
        print(f"⚠️ No active room for player {player_id}")

# ===== AI Move =====
@socketio.on('get_ai_move')
def handle_ai_move(data):
    player_id = data.get('player_id')
    difficulty = data.get('difficulty', 'expert')
    fen = data.get('fen')

    if not player_id or not fen:
        emit('error', {'message': 'Invalid request'})
        return

    depth = difficulty_settings.get(difficulty, difficulty_settings['expert'])['depth']

    if player_id not in game_sessions:
        board = chess.Board(fen)
        engine = get_stockfish_instance(difficulty)
        session = GameSession(board=board, engine=engine, difficulty=difficulty, moves_history=[])
        game_sessions[player_id] = session
    else:
        session = game_sessions[player_id]
        session.board.set_fen(fen)

    if session.is_game_over:
        emit('ai_move', {'move': None, 'status': 'Game is already over'}, to=request.sid)
        return

    session.engine.set_fen_position(session.board.fen())
    session.engine.set_depth(depth)

    start_time = time.time()
    best_move = session.engine.get_best_move()
    end_time = time.time()
    thinking_time = round(end_time - start_time, 2)

    if not best_move:
        emit('ai_move', {'move': None}, to=request.sid)
        return

    uci_move = chess.Move.from_uci(best_move)
    if not session.board.is_legal(uci_move):
        emit('ai_move', {'move': None, 'status': 'Invalid move'}, to=request.sid)
        return

    session.board.push(uci_move)
    session.moves_history.append(best_move)

    move = {
        'from': best_move[:2],
        'to': best_move[2:4],
        'promotion': best_move[4] if len(best_move) > 4 else None
    }

    # Game over checks
    if session.board.is_checkmate():
        session.is_game_over = True
        emit('ai_move', {'move': move, 'status': 'checkmate', 'winner': 'ai', 'thinking_time': thinking_time}, to=request.sid)
        emit("play_win_sound", to=request.sid)
        with clients_lock:
            game_sessions.pop(player_id, None)
            active_games.pop(player_id, None)
        return
    elif session.board.is_stalemate() or session.board.is_insufficient_material():
        session.is_game_over = True
        emit('ai_move', {'move': move, 'status': 'draw', 'thinking_time': thinking_time}, to=request.sid)
        with clients_lock:
            game_sessions.pop(player_id, None)
            active_games.pop(player_id, None)
        return

    emit('ai_move', {'move': move, 'thinking_time': thinking_time}, to=request.sid)

# ===== Get all matches =====
@socketio.on('get_all_matches')
def handle_get_all_matches():
    matches_array = (CMatch * 1000)()
    try:
        count = matchmaking.get_all_matches(matches_array, 1000)
        result = []
        for i in range(count):
            m = matches_array[i]
            p1 = bytes(m.player1).split(b'\x00', 1)[0].decode('utf-8')
            p2 = bytes(m.player2).split(b'\x00', 1)[0].decode('utf-8')
            result.append({'match_id': m.match_id, 'player1_id': p1, 'player2_id': p2})
        emit('all_matches', result)
    except Exception as e:
        print("DLL get_all_matches error:", e)
        emit('all_matches', [])

# ==========================
# Run server
# ==========================
if __name__ == '__main__':
    print(f"🔧 Loaded native: {dll_path}")
    socketio.run(app, debug=True, port=5000)
