document.addEventListener('DOMContentLoaded', () => {
  // Mobile Navigation Menu Toggle
  const menuButton = document.querySelector('.menu-toggle');
  const nav = document.querySelector('.nav-links');
  if (menuButton && nav) {
    menuButton.addEventListener('click', () => {
      const isOpen = nav.classList.toggle('open');
      menuButton.setAttribute('aria-expanded', String(isOpen));
    });
    nav.querySelectorAll('a').forEach((link) => {
      link.addEventListener('click', () => {
        nav.classList.remove('open');
        menuButton.setAttribute('aria-expanded', 'false');
      });
    });
  }

  // Safety & Installation Tab Menu Switcher
  const safetyTabBtns = document.querySelectorAll('.safety-tab-btn');
  const safetyTabPanels = document.querySelectorAll('.safety-tab-panel');

  safetyTabBtns.forEach((btn) => {
    btn.addEventListener('click', () => {
      const targetId = btn.getAttribute('data-tab');

      // Update button states
      safetyTabBtns.forEach((b) => {
        b.classList.remove('active');
        b.setAttribute('aria-selected', 'false');
      });
      btn.classList.add('active');
      btn.setAttribute('aria-selected', 'true');

      // Update panel visibility
      safetyTabPanels.forEach((panel) => {
        if (panel.id === targetId) {
          panel.classList.add('active');
        } else {
          panel.classList.remove('active');
        }
      });
    });
  });

  // FAQ Accordion Interaction
  document.querySelectorAll('.faq-btn').forEach((button) => {
    button.addEventListener('click', () => {
      const item = button.closest('.faq-item');
      const wasOpen = item.classList.contains('open');
      document.querySelectorAll('.faq-item').forEach((other) => {
        other.classList.remove('open');
        const otherBtn = other.querySelector('.faq-btn');
        if (otherBtn) otherBtn.setAttribute('aria-expanded', 'false');
      });
      if (!wasOpen) {
        item.classList.add('open');
        button.setAttribute('aria-expanded', 'true');
      }
    });
  });

  // Profile Selector Interaction
  let activeProfile = 'gaming';
  const profileChips = document.querySelectorAll('.profile-chips .chip');
  const specBars = document.querySelectorAll('.spectrum-bars .spec-bar');

  profileChips.forEach((chip) => {
    chip.addEventListener('click', () => {
      profileChips.forEach((c) => c.classList.remove('active'));
      chip.classList.add('active');
      activeProfile = chip.getAttribute('data-profile') || 'gaming';
    });
  });

  // Interactive SmartScreen Simulation Demo
  const moreInfoBtn = document.getElementById('simMoreInfoBtn');
  const appDetails = document.getElementById('simAppDetails');
  const runBtn = document.getElementById('simRunBtn');
  const successMsg = document.getElementById('simSuccessMsg');

  if (moreInfoBtn && appDetails && runBtn) {
    moreInfoBtn.addEventListener('click', () => {
      appDetails.classList.toggle('show');
      runBtn.classList.toggle('show');
    });

    runBtn.addEventListener('click', () => {
      if (successMsg) {
        successMsg.style.display = 'block';
        setTimeout(() => {
          successMsg.style.display = 'none';
        }, 3500);
      }
    });
  }

  // Live Stereo Scope Waveform Canvas
  const scopeCanvas = document.getElementById('scopeCanvas');
  if (scopeCanvas) {
    const ctx = scopeCanvas.getContext('2d');
    let phase = 0;

    const renderScope = () => {
      phase += (activeProfile === 'gaming' ? 0.1 : 0.06);
      const w = scopeCanvas.width;
      const h = scopeCanvas.height;
      const mid = h / 2;

      ctx.clearRect(0, 0, w, h);

      // Center baseline
      ctx.strokeStyle = '#1e293b';
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(0, mid);
      ctx.lineTo(w, mid);
      ctx.stroke();

      // Amplitude modifier based on active profile
      const ampL = activeProfile === 'hifi' ? 18 : (activeProfile === 'cinema' ? 20 : 14);
      const ampR = activeProfile === 'hifi' ? 14 : (activeProfile === 'cinema' ? 16 : 11);

      // Channel Left (Cyan)
      ctx.strokeStyle = '#38bdf8';
      ctx.lineWidth = 1.6;
      ctx.beginPath();
      for (let x = 0; x < w; x++) {
        const y = mid + Math.sin(x * 0.08 + phase) * ampL * Math.sin(x * 0.025);
        if (x === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.stroke();

      // Channel Right (Indigo)
      ctx.strokeStyle = '#818cf8';
      ctx.lineWidth = 1.2;
      ctx.beginPath();
      for (let x = 0; x < w; x++) {
        const y = mid + Math.cos(x * 0.075 + phase * 0.9) * ampR * Math.cos(x * 0.03);
        if (x === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.stroke();

      // Organic bounce for spectrum bars
      specBars.forEach((bar, idx) => {
        const base = Math.sin(phase * 0.6 + idx * 0.8) * 0.25 + 0.55;
        const noise = Math.sin(phase * 1.4 + idx * 1.5) * 0.1;
        const pct = Math.min(95, Math.max(12, (base + noise) * 100));
        bar.style.height = `${pct}%`;
      });

      requestAnimationFrame(renderScope);
    };

    renderScope();
  }
});
