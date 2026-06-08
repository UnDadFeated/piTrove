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

    // Helper function to handle copy to clipboard
    const setupClipboardButton = (buttonId, textToCopy) => {
        const copyBtn = document.getElementById(buttonId);
        if (!copyBtn) return;

        copyBtn.addEventListener('click', async () => {
            try {
                await navigator.clipboard.writeText(textToCopy);
                
                // Visual feedback transition
                const originalIcon = copyBtn.innerHTML;
                copyBtn.innerHTML = '<i class="fa-solid fa-check" style="color: #10b981;"></i>';
                copyBtn.setAttribute('aria-label', 'Copied!');
                
                setTimeout(() => {
                    copyBtn.innerHTML = originalIcon;
                    copyBtn.setAttribute('aria-label', 'Copy to clipboard');
                }, 2000);
            } catch (err) {
                console.error('Failed to copy text: ', err);
            }
        });
    };

    // 2. Copy to Clipboard for Bootstrap Command
    const bootstrapCommand = "wget -qO- https://raw.githubusercontent.com/UnDadFeated/piTrove/main/install.sh | sudo bash";
    setupClipboardButton('copy-btn', bootstrapCommand);

    // 3. Copy to Clipboard for Organizer Command
    const organizerCommand = "sudo ./install.sh --organize /path/to/media";
    setupClipboardButton('copy-organize-btn', organizerCommand);
});
