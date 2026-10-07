// Конфигурация графика в реальном времени
let chart;
const maxDataPoints = 30;
const chartData = {
    labels: [],
    datasets: [
        {
            label: 'Сэкономлено памяти (MB)',
            borderColor: '#238636',
            backgroundColor: 'rgba(35, 134, 84, 0.1)',
            borderWidth: 2,
            fill: true,
            data: [],
            yAxisID: 'y'
        },
        {
            label: 'Загрузка RAM хоста (%)',
            borderColor: '#58a6ff',
            backgroundColor: 'transparent',
            borderWidth: 2,
            borderDash: [5, 5],
            data: [],
            yAxisID: 'y1'
        }
    ]
};

function initChart() {
    const ctx = document.getElementById('savingsChart').getContext('2d');
    chart = new Chart(ctx, {
        type: 'line',
        data: chartData,
        options: {
            responsive: true,
            maintainAspectRatio: false,
            interaction: {
                mode: 'index',
                intersect: false,
            },
            scales: {
                x: {
                    grid: { color: '#21262d' },
                    ticks: { color: '#8b949e' }
                },
                y: {
                    type: 'linear',
                    display: true,
                    position: 'left',
                    grid: { color: '#21262d' },
                    ticks: { color: '#8b949e' },
                    title: {
                        display: true,
                        text: 'Экономия (MB)',
                        color: '#8b949e'
                    }
                },
                y1: {
                    type: 'linear',
                    display: true,
                    position: 'right',
                    grid: { drawOnChartArea: false },
                    ticks: { color: '#8b949e' },
                    min: 0,
                    max: 100,
                    title: {
                        display: true,
                        text: 'Использование (%)',
                        color: '#8b949e'
                    }
                }
            },
            plugins: {
                legend: {
                    labels: { color: '#c9d1d9' }
                }
            }
        }
    });
}

// Добавление новых точек на график
function updateChart(label, savedMb, usagePercent) {
    if (chart.data.labels.length >= maxDataPoints) {
        chart.data.labels.shift();
        chart.data.datasets[0].data.shift();
        chart.data.datasets[1].data.shift();
    }

    chart.data.labels.push(label);
    chart.data.datasets[0].data.push(savedMb);
    chart.data.datasets[1].data.push(usagePercent);
    chart.update();
}

// Запросы к REST API
async function fetchStatus() {
    try {
        const response = await fetch('/api/status');
        if (!response.ok) throw new Error('API request failed');
        const data = await response.json();

        updateUI(data);
    } catch (error) {
        console.error('Ошибка получения статуса системы:', error);
    }
}

function updateUI(data) {
    // 1. Статус-бейдж
    const badge = document.getElementById('ksm-global-badge');
    if (!data.ksmSupported) {
        badge.innerText = 'KSM: НЕ ПОДДЕРЖИВАЕТСЯ';
        badge.className = 'status-badge inactive';
    } else if (data.ksmRunning) {
        badge.innerText = 'KSM: АКТИВЕН';
        badge.className = 'status-badge active';
    } else {
        badge.innerText = 'KSM: ОТКЛЮЧЕН';
        badge.className = 'status-badge';
    }

    // 2. Карточки KPI
    document.getElementById('val-saved-mem').innerText = data.savedMemoryMb.toFixed(2) + ' MB';

    // Процент от общей памяти хоста
    const totalSystemMemoryMb = data.systemTotalMemory / 1024 / 1024;
    const percentOfTotal = totalSystemMemoryMb > 0 ? (data.savedMemoryMb / totalSystemMemoryMb * 100) : 0;
    document.getElementById('val-saved-percent').innerText = percentOfTotal.toFixed(2) + '% от всей памяти хоста';

    document.getElementById('val-sys-usage').innerText = data.systemMemoryUsagePercent.toFixed(1) + '%';
    const totalGb = data.systemTotalMemory / 1024 / 1024 / 1024;
    const availGb = data.systemAvailableMemory / 1024 / 1024 / 1024;
    const usedGb = totalGb - availGb;
    document.getElementById('val-sys-details').innerText = usedGb.toFixed(2) + ' GB / ' + totalGb.toFixed(2) + ' GB';

    // 3. Переключатели и поля ввода
    if (!document.activeElement || document.activeElement.id !== 'btn-toggle-ksm') {
        document.getElementById('btn-toggle-ksm').checked = data.ksmRunning;
    }
    if (!document.activeElement || document.activeElement.id !== 'btn-toggle-autotune') {
        document.getElementById('btn-toggle-autotune').checked = data.autoTune;
    }

    const manualFields = document.getElementById('manual-tuning-fields');
    if (data.autoTune) {
        manualFields.classList.add('disabled-tuning');
    } else {
        manualFields.classList.remove('disabled-tuning');
    }

    if (!document.activeElement || document.activeElement.id !== 'input-pages-to-scan') {
        document.getElementById('input-pages-to-scan').value = data.pagesToScan;
    }
    if (!document.activeElement || document.activeElement.id !== 'input-sleep-ms') {
        document.getElementById('input-sleep-ms').value = data.sleepMillisecs;
    }
    if (!document.activeElement || document.activeElement.id !== 'input-zero-pages') {
        document.getElementById('input-zero-pages').checked = data.useZeroPages;
    }

    // 4. Обновление графика
    const now = new Date();
    const timeStr = now.getHours().toString().padStart(2, '0') + ':' +
                    now.getMinutes().toString().padStart(2, '0') + ':' +
                    now.getSeconds().toString().padStart(2, '0');
    updateChart(timeStr, data.savedMemoryMb, data.systemMemoryUsagePercent);

    // 5. Таблица контейнеров
    const tableBody = document.getElementById('containers-table-body');
    tableBody.innerHTML = '';

    if (data.containers && data.containers.length > 0) {
        data.containers.forEach(c => {
            const row = document.createElement('tr');
            const ramMb = c.memoryUsageBytes / 1024 / 1024;

            row.innerHTML = `
                <td><strong>${c.name}</strong></td>
                <td><code>${c.image}</code></td>
                <td>${ramMb.toFixed(1)} MB</td>
                <td><small style="color: #8b949e;">.../${c.id.substring(0, 12)}/memory.current</small></td>
            `;
            tableBody.appendChild(row);
        });
    } else {
        tableBody.innerHTML = `
            <tr>
                <td colspan="4" class="text-center" style="color: #8b949e;">
                    Запущенных контейнеров на хосте не обнаружено.
                </td>
            </tr>
        `;
    }
}

// Управляющие запросы требуют токен из MEMORYGUARD_API_TOKEN.
// Он хранится только в localStorage этого браузера и запрашивается при первом использовании.
function getApiToken() {
    try { return localStorage.getItem('mgApiToken') || ''; } catch (e) { return ''; }
}

async function controlRequest(url, payload) {
    for (let attempt = 0; attempt < 2; attempt++) {
        const response = await fetch(url, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Authorization': 'Bearer ' + getApiToken()
            },
            body: JSON.stringify(payload)
        });
        if (response.status === 401 && attempt === 0) {
            const token = prompt('Токен API MemoryGuard (MEMORYGUARD_API_TOKEN):');
            if (!token) break;
            try { localStorage.setItem('mgApiToken', token); } catch (e) { /* private mode */ }
            continue;
        }
        if (!response.ok) {
            const body = await response.json().catch(() => ({}));
            throw new Error(body.error || ('HTTP ' + response.status));
        }
        return response;
    }
    throw new Error('Нет авторизации');
}

// Слушатели событий интерфейса
document.getElementById('btn-toggle-ksm').addEventListener('change', async (e) => {
    try {
        await controlRequest('/api/ksm/toggle', { run: e.target.checked });
        fetchStatus();
    } catch (err) {
        console.error('Не удалось переключить KSM:', err);
        alert('Не удалось переключить KSM: ' + err.message);
        fetchStatus();
    }
});

document.getElementById('btn-toggle-autotune').addEventListener('change', async (e) => {
    try {
        await controlRequest('/api/ksm/tune', { autoTune: e.target.checked });
        fetchStatus();
    } catch (err) {
        console.error('Не удалось переключить автоподстройку:', err);
        alert('Не удалось переключить автоподстройку: ' + err.message);
        fetchStatus();
    }
});

document.getElementById('btn-apply-tune').addEventListener('click', async () => {
    const pagesToScan = parseInt(document.getElementById('input-pages-to-scan').value);
    const sleepMillisecs = parseInt(document.getElementById('input-sleep-ms').value);
    const useZeroPages = document.getElementById('input-zero-pages').checked;

    try {
        await controlRequest('/api/ksm/tune', {
            pagesToScan: pagesToScan,
            sleepMillisecs: sleepMillisecs,
            useZeroPages: useZeroPages
        });
        alert('Параметры оптимизации успешно применены!');
        fetchStatus();
    } catch (err) {
        console.error('Не удалось применить параметры:', err);
        alert('Не удалось применить параметры: ' + err.message);
    }
});

// Инициализация при загрузке
window.addEventListener('load', () => {
    initChart();
    fetchStatus();
    // Опрос метрик каждые 5 секунд
    setInterval(fetchStatus, 5000);
});
