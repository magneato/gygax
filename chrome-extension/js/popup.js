const CONFIG = {
    SERVER_HOST: '127.0.0.1',
    SERVER_PORT: 1984,
    POLL_INTERVAL_MS: 500,
    MAX_POLL_ATTEMPTS: 20,
};

const PRESETS = {
    robotron: 'Create a Tron-inspired 2.5D Robotron game page with neon aesthetics. Use the Daggorath vector graphics engine. Include controls for WASD movement, arrow key shooting, and Q/E for depth layers (in/out of screen).',
    tron: 'Design a TRON-themed grid interface with cyan, magenta, and yellow neon colors. Create a retro glow effect with scanlines. Include an embedded game canvas and agent status sidebar.',
};

class GygaxPortal {
    constructor() {
        this.isConnected = false;
        this.currentDirective = null;
        this.lastResponse = null;
        this.pollTimeoutId = null;

        this.elements = {
            serverStatus: document.getElementById('serverStatus'),
            statusDetails: document.getElementById('statusDetails'),
            statusText: null,
            statusDot: null,
            directiveSelect: document.getElementById('directiveSelect'),
            directiveText: document.getElementById('directiveText'),
            sendButton: document.getElementById('sendButton'),
            errorMessage: document.getElementById('errorMessage'),
            successMessage: document.getElementById('successMessage'),
            responseContainer: document.getElementById('responseContainer'),
            openButton: document.getElementById('openButton'),
        };

        this.elements.statusText = this.elements.serverStatus.querySelector('.status-text');
        this.elements.statusDot = this.elements.serverStatus.querySelector('.status-dot');

        this.init();
    }

    init() {
        this.setupEventListeners();
        this.checkServerConnection();
    }

    setupEventListeners() {
        this.elements.directiveSelect.addEventListener('change', (e) => {
            if (e.target.value === 'custom') {
                this.elements.directiveText.focus();
            } else if (e.target.value) {
                this.elements.directiveText.value = PRESETS[e.target.value] || '';
                this.elements.directiveSelect.value = '';
            }
        });

        this.elements.sendButton.addEventListener('click', () => this.sendDirective());
        this.elements.openButton.addEventListener('click', () => this.openPageInNewTab());

        this.elements.directiveText.addEventListener('keydown', (e) => {
            if (e.ctrlKey && e.key === 'Enter') {
                this.sendDirective();
            }
        });
    }

    async checkServerConnection() {
        try {
            const response = await this.fetchWithTimeout('/status', { method: 'GET' }, 2000);
            if (response.ok) {
                this.setServerStatus(true);
                return true;
            }
        } catch (error) {
            console.warn('Server connection check failed:', error);
        }
        this.setServerStatus(false);
        return false;
    }

    setServerStatus(connected) {
        this.isConnected = connected;
        if (connected) {
            this.elements.statusDot.className = 'status-dot online';
            this.elements.statusText.textContent = 'Server ONLINE';
            this.elements.statusDetails.style.display = 'block';
            this.elements.sendButton.disabled = false;
        } else {
            this.elements.statusDot.className = 'status-dot offline';
            this.elements.statusText.textContent = 'Server OFFLINE';
            this.elements.statusDetails.style.display = 'none';
            this.elements.sendButton.disabled = true;
            this.showError('Gygax server not running on http://127.0.0.1:1984');
        }
    }

    async sendDirective() {
        const directive = this.elements.directiveText.value.trim();

        if (!directive) {
            this.showError('Please enter a directive');
            return;
        }

        if (!this.isConnected) {
            this.showError('Server is not connected');
            return;
        }

        this.currentDirective = directive;
        this.setUILoading(true);
        this.clearMessages();

        try {
            const sendResponse = await this.fetchWithTimeout(
                '/directive',
                {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ directive }),
                },
                5000
            );

            if (!sendResponse.ok) {
                throw new Error(`Server responded with ${sendResponse.status}`);
            }

            this.showSuccess('Directive accepted. Agents constructing page...');

            await this.pollForResponse();

        } catch (error) {
            console.error('Directive send failed:', error);
            this.showError(`Failed to send directive: ${error.message}`);
        } finally {
            this.setUILoading(false);
        }
    }

    async pollForResponse() {
        this.elements.statusDot.className = 'status-dot pending';
        this.elements.statusText.textContent = 'Constructing page...';

        let attempt = 0;
        while (attempt < CONFIG.MAX_POLL_ATTEMPTS) {
            try {
                await this.delay(CONFIG.POLL_INTERVAL_MS);

                const response = await this.fetchWithTimeout('/status', { method: 'GET' }, 3000);
                if (!response.ok) {
                    throw new Error(`Status check failed: ${response.status}`);
                }

                const data = await response.json();

                if (data.latest_html &&
                    data.latest_html !== '<h1>No Layout Generated Yet</h1>' &&
                    data.latest_html.length > 100) {

                    this.lastResponse = data;
                    this.displayResponse(data);
                    this.showSuccess('Page generated successfully!');
                    this.elements.openButton.style.display = 'block';
                    this.elements.statusDot.className = 'status-dot online';
                    this.elements.statusText.textContent = 'Page Ready';
                    return;
                }

                attempt++;
            } catch (error) {
                console.warn(`Poll attempt ${attempt + 1} failed:`, error);
                attempt++;
            }
        }

        this.showError('Page generation timed out. Check if Gygax orchestrator is running.');
        this.elements.statusDot.className = 'status-dot offline';
        this.elements.statusText.textContent = 'Timeout';
    }

    displayResponse(data) {
        const container = this.elements.responseContainer;

        const htmlPreview = data.latest_html
            ? data.latest_html.substring(0, 500) + (data.latest_html.length > 500 ? '...' : '')
            : 'No HTML content';

        const titleMatch = data.latest_html.match(/<title>([^<]+)<\/title>/);
        const title = titleMatch ? titleMatch[1] : 'Generated Page';

        container.innerHTML = `
            <h3>📄 ${title}</h3>
            <p><strong>Status:</strong> Page successfully generated by agent hierarchy.</p>
            <details>
                <summary style="cursor: pointer; color: #00ffaa; margin: 8px 0;">View HTML Preview</summary>
                <pre>${escapeHtml(htmlPreview)}</pre>
            </details>
            <p style="margin-top: 10px; font-size: 0.8em; color: #666688;">
                Click "Open in New Tab" to view the full page.
            </p>
        `;
    }

    async openPageInNewTab() {
        if (!this.lastResponse || !this.lastResponse.latest_html) {
            this.showError('No page to display');
            return;
        }

        try {
            const html = this.lastResponse.latest_html;
            const blob = new Blob([html], { type: 'text/html' });
            const url = URL.createObjectURL(blob);

            chrome.tabs.create({ url });

            setTimeout(() => URL.revokeObjectURL(url), 10000);
        } catch (error) {
            console.error('Failed to open page:', error);
            this.showError(`Failed to open page: ${error.message}`);
        }
    }

    showError(message) {
        this.clearMessages();
        this.elements.errorMessage.textContent = message;
        this.elements.errorMessage.style.display = 'block';
    }

    showSuccess(message) {
        this.clearMessages();
        this.elements.successMessage.textContent = message;
        this.elements.successMessage.style.display = 'block';
    }

    clearMessages() {
        this.elements.errorMessage.style.display = 'none';
        this.elements.successMessage.style.display = 'none';
    }

    setUILoading(loading) {
        const btnText = this.elements.sendButton.querySelector('.btn-text');
        const btnSpinner = this.elements.sendButton.querySelector('.btn-spinner');

        this.elements.sendButton.disabled = loading;
        this.elements.directiveText.disabled = loading;
        this.elements.directiveSelect.disabled = loading;

        if (loading) {
            btnText.style.opacity = '0.5';
            btnSpinner.style.display = 'inline-block';
        } else {
            btnText.style.opacity = '1';
            btnSpinner.style.display = 'none';
        }
    }

    fetchWithTimeout(endpoint, options = {}, timeoutMs = 5000) {
        const url = `http://${CONFIG.SERVER_HOST}:${CONFIG.SERVER_PORT}${endpoint}`;

        const controller = new AbortController();
        const timeoutId = setTimeout(() => controller.abort(), timeoutMs);

        return chrome.storage.local.get('gygaxToken').then(({ gygaxToken }) => {
            const headers = { ...(options.headers || {}) };
            if (gygaxToken) headers['Authorization'] = `Bearer ${gygaxToken}`;
            return fetch(url, {
                ...options,
                headers,
                signal: controller.signal,
            });
        }).finally(() => clearTimeout(timeoutId));
    }

    delay(ms) {
        return new Promise(resolve => setTimeout(resolve, ms));
    }
}

function escapeHtml(text) {
    const map = {
        '&': '&amp;',
        '<': '&lt;',
        '>': '&gt;',
        '"': '&quot;',
        "'": '&#039;',
    };
    return text.replace(/[&<>"']/g, char => map[char]);
}

document.addEventListener('DOMContentLoaded', () => {
    new GygaxPortal();
});

document.addEventListener('DOMContentLoaded', async () => {
    const input = document.getElementById('tokenInput');
    const save = document.getElementById('saveToken');
    if (!input || !save) return;
    const { gygaxToken } = await chrome.storage.local.get('gygaxToken');
    if (gygaxToken) input.value = gygaxToken;
    save.addEventListener('click', async () => {
        await chrome.storage.local.set({ gygaxToken: input.value.trim() });
        save.textContent = 'Saved';
        setTimeout(() => { save.textContent = 'Save token'; }, 1200);
    });
});
