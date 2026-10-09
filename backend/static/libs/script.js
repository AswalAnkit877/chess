const socket = io('http://127.0.0.1:5000');
const chess = new Chess();
let board = null;
let playerColor = 'white';
let playerId = Math.floor(Math.random() * 10000);
let isMyTurn = false;
let playingBot = false;
let botDifficulty = '';
let maxBotTime = 10;
let soundMuted = false;

let playerName = '';
let playerSkill = 1600;

// Initialize the chessboard
function initializeBoard() {
  board = Chessboard('board', {
    draggable: true,
    position: 'start',
    orientation: playerColor,
    pieceTheme: '/static/img/{piece}.png',

    onMouseoverSquare: (square, piece) => {
      if (!isMyTurn || chess.game_over()) return;
      if ((playerColor === 'white' && piece && piece.startsWith('b')) ||
          (playerColor === 'black' && piece && piece.startsWith('w'))) return;

      const moves = chess.moves({ square, verbose: true });
      if (moves.length === 0) return;

      const squaresToHighlight = moves.map(m => m.to);
      highlightSquares([square, ...squaresToHighlight]);
    },

    onMouseoutSquare: (square, piece) => {
      removeHighlights();
    },

    onDragStart: (source, piece) => {
      if (!isMyTurn || chess.game_over()) return false;
      if ((playerColor === 'white' && piece.startsWith('b')) || (playerColor === 'black' && piece.startsWith('w')))
        return false;

      const clickSound = document.getElementById('clickSound');
      if (clickSound) {
        clickSound.currentTime = 0;
        clickSound.play().catch(err => console.error('Click sound error:', err));
      }
    },

    onDrop: (source, target) => {
      removeHighlights();

      const move = chess.move({ from: source, to: target, promotion: 'q' });
      if (move === null) {
        playSound('invalidSound');
        return 'snapback';
      }

      board.position(chess.fen());
      updateStatus();
      isMyTurn = false;

      if (move && move.captured) playSound('captureSound');
      else playSound('moveSound');

      if (playingBot) {
        setTimeout(makeBotMove, 500);
      } else {
        socket.emit('move', {
          player_id: playerId,
          move: { from: move.from, to: move.to, promotion: move.promotion }
        });
      }
    },

    onSnapEnd: () => {
      board.position(chess.fen());
    }
  });

  playSound('startSound');
  updateStatus();
}

function highlightSquares(squares) {
  removeHighlights();
  squares.forEach(square => {
    const squareEl = $('#board .square-' + square);
    squareEl.append('<div class="move-dot"></div>');
  });
}

function removeHighlights() {
  $('#board .move-dot').remove();
}

function playSound(id) {
  if (!soundMuted) {
    const audio = document.getElementById(id);
    if (audio) audio.play();
  }
}

document.getElementById('muteBtn')?.addEventListener('click', () => {
  soundMuted = !soundMuted;
  document.getElementById('muteBtn').innerText = soundMuted ? '🔇 Unmute' : '🔊 Mute';
});

function makeBotMove() {
  if (!botDifficulty) {
    alert("Please select bot difficulty!");
    return;
  }

  switch (botDifficulty) {
    case 'easy': maxBotTime = 5; break;
    case 'medium': maxBotTime = 10; break;
    case 'hard': maxBotTime = 15; break;
    case 'expert': maxBotTime = 20; break;
    default: maxBotTime = 10; break;
  }

  $('#botTime').show();
  $('#progressBarContainer').show();
  $('#countdown').text(`${maxBotTime} seconds left`);
  $('#progressBar').css('width', '100%');
  $('#progressBar').css('transition', 'none');

  let timeRemaining = maxBotTime;
  const timerInterval = setInterval(() => {
    timeRemaining--;
    const percentage = (timeRemaining / maxBotTime) * 100;
    $('#progressBar').css('width', `${percentage}%`);
    $('#countdown').text(`${timeRemaining} seconds left`);
    if (timeRemaining <= 0) clearInterval(timerInterval);
  }, 1000);

  const startTime = Date.now();
  socket.emit('get_ai_move', {
    player_id: playerId,
    fen: chess.fen(),
    difficulty: botDifficulty
  });

  socket.once('ai_move', (data) => {
    clearInterval(timerInterval);
    const moveResult = chess.move(data.move);
    board.position(chess.fen());
    isMyTurn = true;

    if (moveResult?.captured) playSound('captureSound');
    else playSound('moveSound');

    updateStatus();
    const endTime = Date.now();
    const duration = ((endTime - startTime) / 1000).toFixed(2);
    $('#botTime').text(`🤖 Bot moved in ${duration}s (Max: ${maxBotTime}s)`);

    const thinkingTime = data.thinking_time || 10;
    $('#progressBar').css({
      width: '100%',
      transition: `width ${thinkingTime}s linear`
    });
  });
}

function updateStatus() {
  let status = '';
  if (chess.in_checkmate()) {
    const winner = chess.turn() === 'w' ? 'Black' : 'White';
    status = `🎉 Game over. ${winner} wins by checkmate!`;
    playSound('checkmateSound');
    celebrateWinner(winner);
  } else if (chess.in_draw()) {
    status = 'Game over. Draw!';
    playSound('drawSound');
  } else {
    status = `${chess.turn() === 'w' ? 'White' : 'Black'} to move`;
    if (chess.in_check()) {
      status += ' (check)';
      playSound('checkSound');
    }
  }
  $('#status').text(status);
}

function celebrateWinner(winnerColor) {
  playSound('checkmateSound');
  setTimeout(() => playSound('winSound'), 1000);
  $('#status').text(`🎉 ${winnerColor} wins!`);
  confetti({ particleCount: 250, spread: 90, origin: { y: 0.6 } });
  $('body').css('background-color', '#d1e7dd');
  setTimeout(() => $('body').css('background-color', '#f0f2f5'), 1500);
}

$('.difficulty-btn').click(function () {
  botDifficulty = $(this).data('difficulty');
  if (!botDifficulty) {
    alert("Please select a valid difficulty!");
    return;
  }
  $('#difficultySelection').hide();
  $('#status').text(`🤖 Playing bot (${botDifficulty})`);
  playSound('clickSound');
  initializeBoard();
  isMyTurn = true;
   $('#newGameBtn').show();
});

function startBotGame() {
  $('#modeSelect').hide();
  $('#difficultySelection').show();
  $('#status').text("🤖 Select bot difficulty");
  playingBot = true;
  playerColor = 'white';
  playSound('clickSound');
}

function startOnlineGame() {
  $('#modeSelect').hide();

  const nameInput = prompt("Enter your name:");
  if (nameInput === null) {
    $('#modeSelect').show();
    return; // User cancelled name input
  }

  const skillInput = prompt("Enter your skill rating (default 1600):", "1600");
  if (skillInput === null) {
    $('#modeSelect').show();
    return; // User cancelled skill input
  }

  playerName = nameInput.trim() || `Player${playerId}`;
  playerSkill = parseInt(skillInput) || 1600;

  socket.emit('add_player', {
    player_id: playerId,
    name: playerName,
    skill: playerSkill
  });

  playSound('clickSound');
  $('#status').text("👤 Waiting for match...");
}


socket.on('start_game', (data) => {
  playerColor = data.color;
  isMyTurn = (playerColor === 'white');
  console.log("✅ Received match data:", data);

  let whiteName, blackName, whiteSkill, blackSkill;

  if (playerColor === 'white') {
    whiteName = playerName;
    whiteSkill = playerSkill;
    blackName = data.opponent;
    blackSkill = data.opponent_skill;
  } else {
    blackName = playerName;
    blackSkill = playerSkill;
    whiteName = data.opponent;
    whiteSkill = data.opponent_skill;
  }

  $('#matchup').text(`${whiteName} (${whiteSkill}) vs ${blackName} (${blackSkill})`);
  $('#status').text(`🎯 Match found! You are ${playerColor} vs ${data.opponent} (${data.opponent_skill})`);
  $('#newGameBtn').show();
  playSound('matchFoundSound');

  setTimeout(() => {
    initializeBoard();
  }, 2000);
});

socket.on('opponent_move', (move) => {
  const moveResult = chess.move(move);
  if (moveResult) {
    board.position(chess.fen());
    isMyTurn = true;
    if (moveResult.captured) playSound('captureSound');
    else playSound('moveSound');
    updateStatus();
  } else {
    console.error('Invalid move from opponent:', move);
  }
});

socket.on('ai_move', (data) => {
  const moveResult = chess.move(data.move);
  if (moveResult) {
    board.position(chess.fen());
    isMyTurn = true;
    if (moveResult.captured) playSound('captureSound');
    else playSound('moveSound');
  } else {
    console.error('Invalid AI move:', data.move);
  }
  updateStatus();
});

socket.on('disconnect', () => {
  $('#status').text("❌ Disconnected from server");
  playSound('disconnectSound');
});
