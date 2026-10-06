const CONFIG = {
    SERVER_HOST: '127.0.0.1',
    SERVER_PORT: 1984,
    HEALTH_CHECK_INTERVAL_MS: 30000,
};

let healthCheckIntervalId = null;

chrome.runtime.onInstalled.addListener((details) => {
    if (details.reason === 'install') {
        console.log('[Gygax Portal] Extension installed');
        startHealthCheck();
    } else if (details.reason === 'update') {
        console.log('[Gygax Portal] Extension updated');
        startHealthCheck();
    }
});

chrome.runtime.onSuspend.addListener(() => {
    stopHealthCheck();
    console.log('[Gygax Portal] Service worker suspending');
});

function startHealthCheck() {
    if (healthCheckIntervalId) return;

    console.log('[Gygax Portal] Starting health checks');

    checkServerHealth();

    healthCheckIntervalId = setInterval(checkServerHealth, CONFIG.HEALTH_CHECK_INTERVAL_MS);
}

function stopHealthCheck() {
    if (healthCheckIntervalId) {
        clearInterval(healthCheckIntervalId);
        healthCheckIntervalId = null;
        console.log('[Gygax Portal] Health checks stopped');
    }
}

async function checkServerHealth() {
    const url = `http://${CONFIG.SERVER_HOST}:${CONFIG.SERVER_PORT}/status`;

    try {
        const controller = new AbortController();
        const timeoutId = setTimeout(() => controller.abort(), 3000);

        const { gygaxToken } = await chrome.storage.local.get('gygaxToken');
        const response = await fetch(url, {
            method: 'GET',
            headers: gygaxToken ? { Authorization: `Bearer ${gygaxToken}` } : {},
            signal: controller.signal,
        });

        clearTimeout(timeoutId);

        if (response.ok) {
            const data = await response.json();
            updateBadge('online', '✓');
            logHealthStatus('Server is online', true);
            return true;
        } else {
            updateBadge('offline', '✗');
            logHealthStatus(`Server responded with ${response.status}`, false);
            return false;
        }
    } catch (error) {
        updateBadge('offline', '✗');
        logHealthStatus(`Health check failed: ${error.message}`, false);
        return false;
    }
}

function updateBadge(status, text) {
    const colors = {
        online: [0, 255, 136, 255],
        offline: [255, 85, 85, 255],
        pending: [255, 170, 0, 255],
    };

    chrome.action.setBadgeBackgroundColor({ color: colors[status] || colors.offline });
    chrome.action.setBadgeText({ text: text || '' });
}

function logHealthStatus(message, isHealthy) {
    const timestamp = new Date().toLocaleTimeString();
    const prefix = isHealthy ? '[✓]' : '[✗]';
    console.log(`${prefix} [${timestamp}] ${message}`);
}

chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
    if (request.action === 'checkHealth') {
        checkServerHealth().then(result => {
            sendResponse({ success: result });
        }).catch(error => {
            console.error('Health check error:', error);
            sendResponse({ success: false, error: error.message });
        });
        return true;
    }
});

startHealthCheck();
