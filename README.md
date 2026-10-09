# Online Chess Matchmaking

A real-time multiplayer chess game with a matchmaking system and Stockfish AI bot integration. Built with Python, Flask, Socket.IO, and a custom C extension for fast matchmaking.

## Features
- Real-time multiplayer chess using WebSockets.
- Matchmaking system backed by a custom C extension.
- Play against a Stockfish AI bot with varying difficulties (Easy, Medium, Hard, Expert).
- Dynamic board with sounds and animations.

## Setup and Installation

### Prerequisites
- Python 3
- Native C compiler (for building the matchmaking library, pre-built DLLs are included for Windows/Mac)
- Stockfish executable (managed in the background for bots)

### Running the Server
1. Navigate to the `backend` directory.
2. Install dependencies:
   ```bash
   pip install -r requirements.txt
   ```
3. Run the application:
   ```bash
   python app.py
   ```
4. Open your browser and navigate to `http://127.0.0.1:5000`.

## Architecture
- **Backend:** Flask & Flask-SocketIO in Python.
- **Frontend:** HTML/JS/CSS with chessboard.js and chess.js libraries.
- **Matchmaking:** Native C library (`matchmaking.c`) loaded via `ctypes` for high-performance player matching.
