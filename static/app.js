let lastLogId = -1;
let logLines = [];
let selInvSlot = -1;
let lastInventory = null;

async function fetchJSON(url) {
  try {
    const r = await fetch(url);
    const t = await r.text();
    return JSON.parse(t);
  } catch (e) {
    console.error('fetchJSON error:', url, e);
    return null;
  }
}

async function postJSON(url, data) {
  try {
    const r = await fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(data)
    });
    return await r.json();
  } catch (e) {
    console.error('postJSON error:', url, e);
    return null;
  }
}

function escapeHtml(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

function addItemLog(text) {
  logLines.push(text);
  if (logLines.length > 500) logLines.shift();
  const el = document.getElementById('log');
  const div = document.createElement('div');
  div.textContent = text;
  el.appendChild(div);
  while (el.children.length > 500) el.removeChild(el.firstChild);
  el.scrollTop = el.scrollHeight;
}

async function pollStatus() {
  const j = await fetchJSON('/api/status');
  if (j) {
    const connected = j.player_name && j.player_name.length > 0;
    document.getElementById('status').textContent =
      'бот: ' + (j.player_name || '?') + (connected ? ' (подключён)' : ' (не подключён)')
      + ' | X:' + Math.round(j.x) + ' Y:' + Math.round(j.y) + ' Z:' + Math.round(j.z)
      + ' | HP:' + Math.round(j.health) + ' Food:' + j.food;
  }
}

async function pollChat() {
  const msgs = await fetchJSON('/api/chat');
  if (!msgs || !Array.isArray(msgs)) return;
  for (const m of msgs) {
    const line = (m.sender ? '<' + m.sender + '> ' : '') + m.text;
    const id = 'chat_' + m.ts;
    if (!document.getElementById(id)) {
      const div = document.createElement('div');
      div.id = id;
      div.textContent = line;
      document.getElementById('log').appendChild(div);
    }
  }
  const el = document.getElementById('log');
  while (el.children.length > 500) el.removeChild(el.firstChild);
  el.scrollTop = el.scrollHeight;
}

async function pollInventory() {
  const j = await fetchJSON('/api/inventory');
  if (!j || !Array.isArray(j)) return;
  lastInventory = j;

  const hotbar = await fetchJSON('/api/hotbar');
  const status = await fetchJSON('/api/status');
  const selected = status ? status.hotbar_index : 0;

  const handSlot = j.find(s => s.index >= 36 && s.index <= 44 && s.index - 36 === selected);
  const handName = handSlot && handSlot.present ? ('item#' + handSlot.item_id + ' x' + handSlot.count) : '—';
  document.getElementById('handinfo').textContent = 'В руке: ' + handName;

  const invItems = j.filter(s => s.present && s.index >= 9 && s.index <= 35);
  document.getElementById('invlist').textContent =
    invItems.length === 0 ? 'Инвентарь пуст' : 'Предметы: ' + invItems.map(s => '#' + s.item_id + ' x' + s.count).join(', ');

  renderHotbar(hotbar, selected);
  renderInvGrid(j);
}

function renderHotbar(hb, selected) {
  const el = document.getElementById('hotbar');
  el.innerHTML = '';
  if (!hb || !Array.isArray(hb)) return;
  for (let p = 0; p < 9; ++p) {
    const d = document.createElement('div');
    d.className = 'hb' + (selected === p ? ' cur' : '');
    const num = document.createElement('div');
    num.className = 'num';
    num.textContent = String(p + 1);
    d.appendChild(num);
    if (hb[p] && hb[p].present) {
      const name = document.createElement('div');
      name.className = 'slot-name';
      name.textContent = '#' + hb[p].item_id;
      d.appendChild(name);
      if (hb[p].count > 1) {
        const cnt = document.createElement('div');
        cnt.className = 'slot-count';
        cnt.textContent = 'x' + hb[p].count;
        d.appendChild(cnt);
      }
    }
    d.addEventListener('click', () => postJSON('/api/hold', { slot: String(p) }));
    el.appendChild(d);
  }
}

function renderInvGrid(slots) {
  const grid = document.getElementById('invgrid');
  grid.innerHTML = '';
  if (!slots) return;

  const mainSlots = slots.filter(s => s.index >= 9 && s.index <= 35).sort((a, b) => a.index - b.index);

  for (const s of mainSlots) {
    const d = document.createElement('div');
    d.className = 'slot' + (selInvSlot === s.index ? ' sel' : '');
    if (!s.present) {
      d.addEventListener('click', () => {
        if (selInvSlot !== -1) {
          postJSON('/api/click-slot', { slot: String(selInvSlot), window_id: '0', click_type: '0', button: '0' });
          setTimeout(() => postJSON('/api/click-slot', { slot: String(s.index), window_id: '0', click_type: '0', button: '0' }), 100);
          selInvSlot = -1;
          setTimeout(pollInventory, 300);
        }
      });
      grid.appendChild(d);
      continue;
    }

    const name = document.createElement('div');
    name.className = 'slot-name';
    name.textContent = 'item#' + s.item_id;
    d.appendChild(name);

    if (s.count > 1) {
      const cnt = document.createElement('div');
      cnt.className = 'slot-count';
      cnt.textContent = 'x' + s.count;
      d.appendChild(cnt);
    }

    const drop = document.createElement('div');
    drop.className = 'drop';
    drop.textContent = '\u2715';
    drop.title = 'Выбросить';
    drop.addEventListener('click', (e) => {
      e.stopPropagation();
      postJSON('/api/drop', { window_id: '0', slot: String(s.index) });
      setTimeout(pollInventory, 300);
    });
    d.appendChild(drop);

    d.addEventListener('click', () => {
      if (selInvSlot === -1) {
        selInvSlot = s.index;
        renderInvGrid(lastInventory);
      } else if (selInvSlot === s.index) {
        selInvSlot = -1;
        renderInvGrid(lastInventory);
      } else {
        postJSON('/api/click-slot', { slot: String(selInvSlot), window_id: '0', click_type: '0', button: '0' });
        setTimeout(() => {
          postJSON('/api/click-slot', { slot: String(s.index), window_id: '0', click_type: '0', button: '0' });
          setTimeout(() => {
            postJSON('/api/click-slot', { slot: String(selInvSlot), window_id: '0', click_type: '0', button: '0' });
            selInvSlot = -1;
            setTimeout(pollInventory, 300);
          }, 100);
        }, 100);
      }
    });
    grid.appendChild(d);
  }
}

async function pollContainer() {
  const j = await fetchJSON('/api/container');
  const info = document.getElementById('containerinfo');
  const grid = document.getElementById('container');
  if (!j || !j.is_open) {
    info.textContent = 'Сундук не открыт';
    grid.innerHTML = '';
    return;
  }
  info.textContent = 'Сундук открыт (окно ' + j.window_id + ')';
  grid.innerHTML = '';
  if (!j.slots || !Array.isArray(j.slots)) return;
  for (const s of j.slots) {
    const d = document.createElement('div');
    d.className = 'slot';
    if (!s.present) {
      d.addEventListener('click', (e) => {
        const type = e.shiftKey ? '1' : '0';
        postJSON('/api/click-slot', { slot: String(s.index), window_id: String(j.window_id), click_type: type, button: '0' });
        setTimeout(pollContainer, 300);
      });
      grid.appendChild(d);
      continue;
    }
    const name = document.createElement('div');
    name.className = 'slot-name';
    name.textContent = 'item#' + s.item_id;
    d.appendChild(name);
    if (s.count > 1) {
      const cnt = document.createElement('div');
      cnt.className = 'slot-count';
      cnt.textContent = 'x' + s.count;
      d.appendChild(cnt);
    }
    d.addEventListener('click', (e) => {
      const type = e.shiftKey ? '1' : '0';
      postJSON('/api/click-slot', { slot: String(s.index), window_id: String(j.window_id), click_type: type, button: '0' });
      setTimeout(pollContainer, 300);
    });
    grid.appendChild(d);
  }
}

async function sendChat() {
  const v = document.getElementById('chat').value;
  if (v) {
    await postJSON('/api/chat', { text: v });
    addItemLog('<WebBot> ' + v);
    document.getElementById('chat').value = '';
  }
}

async function sendCommand() {
  const v = document.getElementById('command').value;
  if (v) {
    await postJSON('/api/command', { command: v });
    addItemLog('/' + v);
    document.getElementById('command').value = '';
  }
}

async function sendBotCmd() {
  const v = document.getElementById('botcmd').value;
  if (v) {
    await postJSON('/api/command', { command: v });
    addItemLog('[bot] ' + v);
    document.getElementById('botcmd').value = '';
  }
}

function useItem() { postJSON('/api/use', {}); }
function closeContainer() { postJSON('/api/close', {}); }

async function refreshAll() {
  await Promise.all([pollStatus(), pollChat(), pollInventory(), pollContainer()]);
}

document.getElementById('chat').addEventListener('keydown', e => { if (e.key === 'Enter') sendChat(); });
document.getElementById('command').addEventListener('keydown', e => { if (e.key === 'Enter') sendCommand(); });
document.getElementById('botcmd').addEventListener('keydown', e => { if (e.key === 'Enter') sendBotCmd(); });

pollStatus();
setInterval(pollStatus, 3000);
setInterval(pollChat, 1000);
pollInventory();
setInterval(pollInventory, 1500);
pollContainer();
setInterval(pollContainer, 1000);
