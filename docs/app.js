document.addEventListener('DOMContentLoaded', () => {
    // 1. Theme Toggle Logic
    const themeToggleBtn = document.getElementById('theme-toggle');
    const body = document.body;
    
    // Check local storage or defaults
    const savedTheme = localStorage.getItem('theme') || 'dark';
    if (savedTheme === 'light') {
        body.classList.remove('dark-theme');
        body.classList.add('light-theme');
        themeToggleBtn.innerHTML = '<i class="fa-solid fa-sun"></i>';
    }

    themeToggleBtn.addEventListener('click', () => {
        if (body.classList.contains('dark-theme')) {
            body.classList.remove('dark-theme');
            body.classList.add('light-theme');
            themeToggleBtn.innerHTML = '<i class="fa-solid fa-sun"></i>';
            localStorage.setItem('theme', 'light');
        } else {
            body.classList.remove('light-theme');
            body.classList.add('dark-theme');
            themeToggleBtn.innerHTML = '<i class="fa-solid fa-moon"></i>';
            localStorage.setItem('theme', 'dark');
        }
    });

    // 2. Copy to Clipboard Command Logic
    const copyBtn = document.getElementById('copy-btn');
    const commandText = "wget -qO- https://raw.githubusercontent.com/UnDadFeated/piTrove/main/install.sh | sudo bash";

    copyBtn.addEventListener('click', async () => {
        try {
            await navigator.clipboard.writeText(commandText);
            
            // Visual feedback transition
            copyBtn.innerHTML = '<i class="fa-solid fa-check" style="color: #10b981;"></i>';
            copyBtn.setAttribute('aria-label', 'Command copied!');
            
            setTimeout(() => {
                copyBtn.innerHTML = '<i class="fa-regular fa-copy"></i>';
                copyBtn.setAttribute('aria-label', 'Copy installation command');
            }, 2000);
        } catch (err) {
            console.error('Failed to copy command: ', err);
        }
    });
    // 3. Play/Pause button interaction inside simulated HUD
    const playBtn = document.querySelector('.hud-ctrl-btn.play');
    let isPlaying = true;
    
    playBtn.addEventListener('click', () => {
        isPlaying = !isPlaying;
        if (isPlaying) {
            playBtn.innerHTML = '<i class="fa-solid fa-play"></i>';
            playBtn.style.background = 'var(--bg-accent)';
        } else {
            playBtn.innerHTML = '<i class="fa-solid fa-pause"></i>';
            playBtn.style.background = '#64748b';
        }
    });
});
