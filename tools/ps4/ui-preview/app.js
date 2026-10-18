(() => {
  const cards = [...document.querySelectorAll('[data-card]')];
  let selected = 0;
  const cardsContainer = document.querySelector('.cards');
  function select(index) {
    selected = (index + cards.length) % cards.length;
    const active = cards[selected].dataset.card;
    cardsContainer.dataset.active = active;
    cards.forEach((card, i) => {
      card.classList.toggle('is-selected', i === selected);
      card.classList.toggle('is-expanded', i === selected);
      card.setAttribute('aria-expanded', i === selected ? 'true' : 'false');
    });
  }
  const initialCard = cards.findIndex(card => card.dataset.card === location.hash.slice(1));
  select(initialCard >= 0 ? initialCard : 0);
  cards.forEach((card, index) => {
    card.addEventListener('mouseenter', () => select(index));
    card.addEventListener('click', () => select(index));
    card.addEventListener('focus', () => select(index));
  });
  document.addEventListener('keydown', event => {
    if (event.key === 'ArrowRight' || event.key === 'Tab' && !event.shiftKey) { event.preventDefault(); select(selected + 1); }
    if (event.key === 'ArrowLeft' || event.key === 'Tab' && event.shiftKey) { event.preventDefault(); select(selected - 1); }
    if (event.key === 'Escape') document.querySelector('.overlay').classList.toggle('is-closing');
    if (event.key.toLowerCase() === 'm') document.querySelector('#muteButton').click();
  });
  const slider = document.querySelector('#volume');
  const value = document.querySelector('#volumeValue');
  const speaker = document.querySelector('#speaker');
  const muteLabel = document.querySelector('#muteLabel');
  const muteIcon = document.querySelector('#muteIcon');
  const muteButton = document.querySelector('#muteButton');
  const volumeSummary = document.querySelector('#volumeSummary');
  let previousVolume = Number(slider.value);
  function setVolume(number) {
    const volume = Math.max(0, Math.min(100, number));
    slider.value = volume;
    value.innerHTML = `${volume}<small>%</small>`;
    slider.style.background = `linear-gradient(90deg,var(--green) 0%,var(--green) ${volume}%,#ffffff2b ${volume}%)`;
    speaker.textContent = volume === 0 ? '◖×' : volume < 35 ? '◖)' : '◖))';
    muteIcon.textContent = volume === 0 ? '◖×' : '◖))';
    muteLabel.textContent = volume === 0 ? 'Ativar áudio' : 'Silenciar áudio';
    volumeSummary.textContent = `${volume}% · ${volume === 0 ? 'áudio silenciado' : 'áudio ativo'}`;
  }
  slider.addEventListener('input', () => { const v = Number(slider.value); if (v > 0) previousVolume = v; setVolume(v); });
  muteButton.addEventListener('click', () => setVolume(Number(slider.value) ? 0 : previousVolume || 80));
  document.querySelectorAll('[data-setting]').forEach(button => button.addEventListener('click', () => {
    const valueNode = button.querySelector('b');
    const choices = {
      resolution: ['720p', '1080p'], fps: ['60 FPS', '30 FPS'],
      bitrate: ['3 Mbps', '10 Mbps', '20 Mbps', '30 Mbps'],
      filter: ['Bilinear', 'FSR · EASU + RCAS']
    }[button.dataset.setting];
    const current = valueNode.childNodes[0].textContent.trim();
    const next = choices[(choices.indexOf(current) + 1) % choices.length];
    valueNode.childNodes[0].textContent = `${next} `;
  }));
  document.querySelector('.close').addEventListener('click', () => document.querySelector('.overlay').classList.toggle('is-closing'));
  setVolume(Number(slider.value));
  // Live CSS refresh: edit style.css and the browser applies it without losing the preview state.
  let styleStamp = '';
  let scriptStamp = '';
  setInterval(async () => {
    try {
      const response = await fetch('/__version', { cache: 'no-store' });
      const versions = await response.json();
      if (scriptStamp && versions.js !== scriptStamp) location.reload();
      scriptStamp = versions.js;
      if (styleStamp && versions.css !== styleStamp) {
        const oldLink = document.querySelector('link[rel="stylesheet"]');
        const newLink = oldLink.cloneNode();
        newLink.href = `style.css?v=${versions.css}`;
        newLink.onload = () => oldLink.remove();
        oldLink.after(newLink);
      }
      styleStamp = versions.css;
    } catch (_) { /* Server closed or preview opened without serve.py. */ }
  }, 700);
})();
